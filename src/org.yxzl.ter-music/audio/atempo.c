/**
 * @file atempo.c
 * @brief atempo 倍速滤镜 — 基于 FFmpeg AVFilter 的倍速播放
 *
 * 从 audio.c 拆分，负责 atempo 滤镜图的初始化、帧处理、销毁。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include <stdio.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

/* ============================================================
 * Atempo filter structure
 * ============================================================ */

typedef struct {
    AVFilterGraph *graph;
    AVFilterContext *src_ctx;
    AVFilterContext *sink_ctx;
    int initialized;
    float speed;
    int input_sample_rate;
    int input_channels;
    uint64_t input_channel_layout;
    enum AVSampleFormat input_sample_fmt;
} AtempoFilter;

static AtempoFilter g_atempo_filter = {0};

/* ============================================================
 * Build atempo filter string
 * ============================================================ */

static void build_atempo_filter_string(char *buf, size_t buf_size, float speed)
{
    if (speed == 1.0f) { buf[0] = '\0'; return; }

    if (speed >= 0.5f && speed <= 2.0f) {
        snprintf(buf, buf_size, "atempo=%.2f", speed);
        return;
    }

    if (speed > 2.0f) {
        float remaining = speed;
        buf[0] = '\0';
        size_t offset = 0;
        int first = 1;
        while (remaining > 1.01f) {
            float factor = (remaining > 2.0f) ? 2.0f : remaining;
            int written = snprintf(buf + offset, buf_size - offset,
                                   first ? "atempo=%.2f" : ",atempo=%.2f", factor);
            if (written < 0 || (size_t)written >= buf_size - offset) break;
            offset += written;
            remaining /= factor;
            first = 0;
        }
        return;
    }

    if (speed < 0.5f) {
        float remaining = speed;
        buf[0] = '\0';
        size_t offset = 0;
        int first = 1;
        while (remaining < 0.99f) {
            float factor = (remaining < 0.5f) ? 0.5f : remaining;
            int written = snprintf(buf + offset, buf_size - offset,
                                   first ? "atempo=%.2f" : ",atempo=%.2f", factor);
            if (written < 0 || (size_t)written >= buf_size - offset) break;
            offset += written;
            remaining /= factor;
            first = 0;
        }
        return;
    }

    buf[0] = '\0';
}

/* ============================================================
 * Initialize atempo filter graph
 * ============================================================ */

int init_atempo_filter(const AVCodecContext *codec_ctx, float speed)
{
    if (!codec_ctx || speed <= 0) return -1;

    if (speed == 1.0f) {
        g_atempo_filter.initialized = 0;
        g_atempo_filter.speed = 1.0f;
        return 0;
    }

    char filter_str[256];
    build_atempo_filter_string(filter_str, sizeof(filter_str), speed);
    if (filter_str[0] == '\0') {
        g_atempo_filter.initialized = 0;
        g_atempo_filter.speed = 1.0f;
        return 0;
    }

    g_atempo_filter.speed = speed;
    g_atempo_filter.input_sample_rate = codec_ctx->sample_rate;
    g_atempo_filter.input_channels = codec_ctx->ch_layout.nb_channels > 0
        ? codec_ctx->ch_layout.nb_channels : (codec_ctx->channels > 0 ? codec_ctx->channels : 2);
    g_atempo_filter.input_sample_fmt = codec_ctx->sample_fmt;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    g_atempo_filter.input_channel_layout = codec_ctx->ch_layout.u.mask;
#else
    g_atempo_filter.input_channel_layout = codec_ctx->channel_layout;
#endif

    g_atempo_filter.graph = avfilter_graph_alloc();
    if (!g_atempo_filter.graph) return -1;

    const AVFilter *abuffersrc = avfilter_get_by_name("abuffer");
    if (!abuffersrc) { avfilter_graph_free(&g_atempo_filter.graph); return -1; }

    char ch_layout_str[64];
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout ch_layout = codec_ctx->ch_layout;
    av_channel_layout_describe(&ch_layout, ch_layout_str, sizeof(ch_layout_str));
#else
    snprintf(ch_layout_str, sizeof(ch_layout_str), "0x%"PRIx64, g_atempo_filter.input_channel_layout);
#endif

    char args[512];
    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=%s:channels=%d",
             g_atempo_filter.input_sample_rate,
             av_get_sample_fmt_name(g_atempo_filter.input_sample_fmt),
             ch_layout_str,
             g_atempo_filter.input_channels);

    int ret = avfilter_graph_create_filter(&g_atempo_filter.src_ctx, abuffersrc, "in",
                                           args, NULL, g_atempo_filter.graph);
    if (ret < 0) { avfilter_graph_free(&g_atempo_filter.graph); return -1; }

    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) { avfilter_graph_free(&g_atempo_filter.graph); return -1; }

    ret = avfilter_graph_create_filter(&g_atempo_filter.sink_ctx, abuffersink, "out",
                                       NULL, NULL, g_atempo_filter.graph);
    if (ret < 0) { avfilter_graph_free(&g_atempo_filter.graph); return -1; }

    enum AVSampleFormat sample_fmts[] = { g_atempo_filter.input_sample_fmt, AV_SAMPLE_FMT_NONE };
    ret = av_opt_set_int_list(g_atempo_filter.sink_ctx, "sample_fmts", sample_fmts,
                              AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) { avfilter_graph_free(&g_atempo_filter.graph); return -1; }

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&g_atempo_filter.graph);
        return -1;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = g_atempo_filter.src_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = g_atempo_filter.sink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    ret = avfilter_graph_parse_ptr(g_atempo_filter.graph, filter_str, &inputs, &outputs, NULL);
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    if (ret < 0) { avfilter_graph_free(&g_atempo_filter.graph); return -1; }

    ret = avfilter_graph_config(g_atempo_filter.graph, NULL);
    if (ret < 0) { avfilter_graph_free(&g_atempo_filter.graph); return -1; }

    g_atempo_filter.initialized = 1;
    return 0;
}

/* ============================================================
 * Cleanup atempo filter graph
 * ============================================================ */

void cleanup_atempo_filter(void)
{
    if (g_atempo_filter.graph) {
        avfilter_graph_free(&g_atempo_filter.graph);
        g_atempo_filter.graph = NULL;
    }
    g_atempo_filter.src_ctx = NULL;
    g_atempo_filter.sink_ctx = NULL;
    g_atempo_filter.initialized = 0;
}

/* ============================================================
 * Filter operations
 * ============================================================ */

int atempo_send_frame(AVFrame *frame)
{
    if (!g_atempo_filter.initialized) return 0;
    int ret = av_buffersrc_add_frame(g_atempo_filter.src_ctx, frame);
    return (ret < 0) ? -1 : 0;
}

int atempo_receive_frame(AVFrame *frame)
{
    if (!g_atempo_filter.initialized) return -1;
    return av_buffersink_get_frame(g_atempo_filter.sink_ctx, frame);
}

int atempo_flush(void)
{
    if (!g_atempo_filter.initialized) return 0;
    int ret = av_buffersrc_add_frame(g_atempo_filter.src_ctx, NULL);
    return (ret < 0) ? -1 : 0;
}

int atempo_is_active(void)
{
    return g_atempo_filter.initialized;
}

float atempo_get_speed(void)
{
    return g_atempo_filter.speed;
}

int atempo_get_input_sample_rate(void)
{
    return g_atempo_filter.input_sample_rate;
}
