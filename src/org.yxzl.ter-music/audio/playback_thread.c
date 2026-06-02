/**
 * @file playback_thread.c
 * @brief 音频播放线程 — PCM 队列、解码循环、线程函数
 *
 * 从 audio.c 拆分，负责音频文件的 FFmpeg 解码、重采样、atempo 滤镜
 * 处理和 PCM 数据输出。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "audio/audio_internal.h"
#include "audio/progress/progress.h"
#include "playlist/playlist.h"
#include "ui/ui.h"
#include "config/config.h"
#include "logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>

/* ── FFmpeg version dependent channel count ── */
int codec_channel_count(const AVCodecContext *codec_ctx)
{
#if LIBAVUTIL_VERSION_MAJOR >= 57
    if (codec_ctx->ch_layout.nb_channels > 0) return codec_ctx->ch_layout.nb_channels;
    return 0;
#else
    if (codec_ctx->channels > 0) return codec_ctx->channels;
    if (codec_ctx->channel_layout) return av_get_channel_layout_nb_channels(codec_ctx->channel_layout);
    return 0;
#endif
}

/* ============================================================
 * Resampler init
 * ============================================================ */

static int init_resampler(SwrContext *swr_ctx,
                          const AVCodecContext *codec_ctx,
                          int input_channels,
                          int output_channels,
                          int output_sample_rate)
{
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout in_ch_layout = codec_ctx->ch_layout;
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, output_channels);
    av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_ch_layout, 0);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
#else
    int64_t in_channel_layout = codec_ctx->channel_layout;
    int64_t out_channel_layout = av_get_default_channel_layout(output_channels);
    if (!in_channel_layout) in_channel_layout = av_get_default_channel_layout(input_channels);
    av_opt_set_channel_layout(swr_ctx, "in_channel_layout", in_channel_layout, 0);
    av_opt_set_channel_layout(swr_ctx, "out_channel_layout", out_channel_layout, 0);
#endif
    av_opt_set_int(swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", output_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S32, 0);
    return swr_init(swr_ctx);
}

/* ============================================================
 * PCM Queue
 * ============================================================ */

#define PCM_QUEUE_CAPACITY 24
#define PCM_QUEUE_MIN_PREFILL_MS 180
#define PCM_QUEUE_MAX_PREFILL_MS 420

typedef struct {
    int32_t *data;
    int frame_count;
    int bytes;
    int capacity_bytes;
} PCMChunk;

typedef struct {
    PCMChunk chunks[PCM_QUEUE_CAPACITY];
    int read_index;
    int write_index;
    int count;
    int buffered_frames;
} PCMQueue;

static int pcm_chunk_ensure_capacity(PCMChunk *chunk, int required_bytes)
{
    if (!chunk || required_bytes <= 0) return -1;
    if (chunk->capacity_bytes >= required_bytes) return 0;
    int new_capacity = chunk->capacity_bytes > 0 ? chunk->capacity_bytes : MAX_AUDIO_BUFFER_SIZE;
    while (new_capacity < required_bytes) new_capacity *= 2;
    int32_t *new_data = realloc(chunk->data, (size_t)new_capacity);
    if (!new_data) return -1;
    chunk->data = new_data;
    chunk->capacity_bytes = new_capacity;
    return 0;
}

static void pcm_queue_reset(PCMQueue *queue)
{
    if (!queue) return;
    queue->read_index = 0;
    queue->write_index = 0;
    queue->count = 0;
    queue->buffered_frames = 0;
    for (int i = 0; i < PCM_QUEUE_CAPACITY; i++) {
        queue->chunks[i].frame_count = 0;
        queue->chunks[i].bytes = 0;
    }
}

static int pcm_queue_init(PCMQueue *queue)
{
    if (!queue) return -1;
    memset(queue, 0, sizeof(*queue));
    for (int i = 0; i < PCM_QUEUE_CAPACITY; i++) {
        queue->chunks[i].data = malloc(MAX_AUDIO_BUFFER_SIZE);
        if (!queue->chunks[i].data) {
            for (int j = 0; j < i; j++) { free(queue->chunks[j].data); queue->chunks[j].data = NULL; }
            return -1;
        }
        queue->chunks[i].capacity_bytes = MAX_AUDIO_BUFFER_SIZE;
    }
    pcm_queue_reset(queue);
    return 0;
}

static void pcm_queue_destroy(PCMQueue *queue)
{
    if (!queue) return;
    for (int i = 0; i < PCM_QUEUE_CAPACITY; i++) {
        free(queue->chunks[i].data);
        queue->chunks[i].data = NULL;
    }
    pcm_queue_reset(queue);
}

static PCMChunk *pcm_queue_write_slot(PCMQueue *queue)
{
    if (!queue || queue->count >= PCM_QUEUE_CAPACITY) return NULL;
    return &queue->chunks[queue->write_index];
}

static void pcm_queue_commit_write(PCMQueue *queue, int frame_count, int bytes)
{
    if (!queue || queue->count >= PCM_QUEUE_CAPACITY) return;
    queue->chunks[queue->write_index].frame_count = frame_count;
    queue->chunks[queue->write_index].bytes = bytes;
    queue->write_index = (queue->write_index + 1) % PCM_QUEUE_CAPACITY;
    queue->count++;
    queue->buffered_frames += frame_count;
}

static PCMChunk *pcm_queue_peek(PCMQueue *queue)
{
    if (!queue || queue->count <= 0) return NULL;
    return &queue->chunks[queue->read_index];
}

static int pcm_queue_buffered_ms(const PCMQueue *queue, int sample_rate)
{
    if (!queue || sample_rate <= 0 || queue->buffered_frames <= 0) return 0;
    return (queue->buffered_frames * 1000) / sample_rate;
}

static void pcm_queue_consume(PCMQueue *queue)
{
    if (!queue || queue->count <= 0) return;
    int consumed_frames = queue->chunks[queue->read_index].frame_count;
    queue->chunks[queue->read_index].frame_count = 0;
    queue->chunks[queue->read_index].bytes = 0;
    queue->read_index = (queue->read_index + 1) % PCM_QUEUE_CAPACITY;
    queue->count--;
    queue->buffered_frames -= consumed_frames;
    if (queue->buffered_frames < 0) queue->buffered_frames = 0;
}

/* ============================================================
 * Seek-in-decoder handler
 * ============================================================ */

static int handle_seek_request_in_decoder(AVFormatContext *fmt_ctx,
                                          AVCodecContext *codec_ctx,
                                          SwrContext *swr_ctx,
                                          AVPacket *packet,
                                          PCMQueue *queue,
                                          int audio_stream_index,
                                          int *decoder_draining,
                                          int *decoder_finished)
{
    extern pthread_mutex_t g_seek_mutex;
    extern int g_seek_request;
    extern int g_seek_position;
    extern int g_current_position;

    int handled = 0;
    pthread_mutex_lock(&g_seek_mutex);
    if (g_seek_request && fmt_ctx && codec_ctx) {
        int target_position = g_seek_position;
        g_seek_request = 0;
        handled = 1;

        AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;
        int64_t target_ts = av_rescale_q(target_position, (AVRational){1, 1}, time_base);
        int ret = av_seek_frame(fmt_ctx, audio_stream_index, target_ts, 0);
        if (ret < 0) {
            update_controls_status(audio_text("跳转失败", "Seek failed"));
        } else {
            avcodec_flush_buffers(codec_ctx);
            if (swr_ctx) swr_init(swr_ctx);
            if (packet) av_packet_unref(packet);
            if (queue) pcm_queue_reset(queue);
            audio_backend_flush_stream();
            progress_tracker_seek(target_position);
            g_current_position = target_position;
            if (decoder_draining) *decoder_draining = 0;
            if (decoder_finished) *decoder_finished = 0;

            char msg[64];
            snprintf(msg, sizeof(msg),
                     use_english_ui() ? "Seek to %02d:%02d" : "已跳转到 %02d:%02d",
                     target_position / 60, target_position % 60);
            update_controls_status(msg);
        }
    }
    pthread_mutex_unlock(&g_seek_mutex);
    return handled;
}

/* ============================================================
 * Wait while paused
 * ============================================================ */

static int wait_while_paused(void)
{
    extern PlayState g_play_state;
    extern int g_play_thread_running;
    extern pthread_cond_t g_play_control_cond;

    pthread_mutex_lock(&g_play_mutex);
    while (g_play_state == PLAY_STATE_PAUSED && g_play_thread_running)
        pthread_cond_wait(&g_play_control_cond, &g_play_mutex);
    int still_running = g_play_thread_running;
    pthread_mutex_unlock(&g_play_mutex);
    return still_running;
}

/* ============================================================
 * Decode next PCM chunk
 * ============================================================ */

static int decode_next_pcm_chunk(AVFormatContext *fmt_ctx,
                                 AVCodecContext *codec_ctx,
                                 SwrContext *swr_ctx,
                                 AVPacket *packet,
                                 AVFrame *frame,
                                 AVFrame *filtered_frame,
                                 PCMQueue *queue,
                                 int audio_stream_index,
                                 int output_sample_rate,
                                 int output_channels,
                                 int use_resampler,
                                 int *decoder_draining,
                                 int *decoder_finished)
{
    if (!fmt_ctx || !codec_ctx || !packet || !frame || !filtered_frame || !queue) return -1;

    while (!*decoder_finished) {
        // Try atempo filtered frame first
        if (atempo_is_active()) {
            int filter_ret = atempo_receive_frame(filtered_frame);
            if (filter_ret == 0) {
                PCMChunk *slot = pcm_queue_write_slot(queue);
                if (!slot) { av_frame_unref(filtered_frame); return 0; }

                int produced_frames = 0, produced_bytes = 0;
                if (!use_resampler) {
                    produced_bytes = av_samples_get_buffer_size(NULL, output_channels,
                                        filtered_frame->nb_samples, AV_SAMPLE_FMT_S32, 1);
                    if (produced_bytes > 0 && pcm_chunk_ensure_capacity(slot, produced_bytes) == 0) {
                        memcpy(slot->data, filtered_frame->data[0], (size_t)produced_bytes);
                        produced_frames = filtered_frame->nb_samples;
                    }
                } else {
                    int dst_nb_samples = av_rescale_rnd(
                        swr_get_delay(swr_ctx, atempo_get_input_sample_rate()) + filtered_frame->nb_samples,
                        output_sample_rate, atempo_get_input_sample_rate(), AV_ROUND_UP);
                    produced_bytes = av_samples_get_buffer_size(NULL, output_channels, dst_nb_samples,
                                                                AV_SAMPLE_FMT_S32, 1);
                    if (produced_bytes > 0 && pcm_chunk_ensure_capacity(slot, produced_bytes) == 0) {
                        uint8_t *output_planes[] = {(uint8_t *)slot->data};
                        produced_frames = swr_convert(swr_ctx, output_planes, dst_nb_samples,
                                                      (const uint8_t **)filtered_frame->data,
                                                      filtered_frame->nb_samples);
                        if (produced_frames > 0)
                            produced_bytes = produced_frames * output_channels * (int)sizeof(int32_t);
                    }
                }
                av_frame_unref(filtered_frame);
                if (produced_frames > 0 && produced_bytes > 0) {
                    pcm_queue_commit_write(queue, produced_frames, produced_bytes);
                    return 1;
                }
                continue;
            }
        }

        int ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == 0) {
            if (atempo_is_active()) {
                if (atempo_send_frame(frame) < 0) { av_frame_unref(frame); return -1; }
                av_frame_unref(frame);
                continue;
            }

            PCMChunk *slot = pcm_queue_write_slot(queue);
            if (!slot) { av_frame_unref(frame); return 0; }

            int produced_frames = 0, produced_bytes = 0;
            if (!use_resampler) {
                produced_bytes = av_samples_get_buffer_size(NULL, output_channels,
                                    frame->nb_samples, AV_SAMPLE_FMT_S32, 1);
                if (produced_bytes > 0 && pcm_chunk_ensure_capacity(slot, produced_bytes) == 0) {
                    memcpy(slot->data, frame->data[0], (size_t)produced_bytes);
                    produced_frames = frame->nb_samples;
                }
            } else {
                int dst_nb_samples = av_rescale_rnd(
                    swr_get_delay(swr_ctx, codec_ctx->sample_rate) + frame->nb_samples,
                    output_sample_rate, codec_ctx->sample_rate, AV_ROUND_UP);
                produced_bytes = av_samples_get_buffer_size(NULL, output_channels, dst_nb_samples,
                                                            AV_SAMPLE_FMT_S32, 1);
                if (produced_bytes > 0 && pcm_chunk_ensure_capacity(slot, produced_bytes) == 0) {
                    uint8_t *output_planes[] = {(uint8_t *)slot->data};
                    produced_frames = swr_convert(swr_ctx, output_planes, dst_nb_samples,
                                                  (const uint8_t **)frame->data, frame->nb_samples);
                    if (produced_frames > 0)
                        produced_bytes = produced_frames * output_channels * (int)sizeof(int32_t);
                }
            }
            av_frame_unref(frame);
            if (produced_frames > 0 && produced_bytes > 0) {
                pcm_queue_commit_write(queue, produced_frames, produced_bytes);
                return 1;
            }
            continue;
        }
        if (ret == AVERROR_EOF) { *decoder_finished = 1; return 0; }
        if (ret != AVERROR(EAGAIN)) return -1;

        if (!*decoder_draining) {
            while (1) {
                ret = av_read_frame(fmt_ctx, packet);
                if (ret < 0) {
                    *decoder_draining = 1;
                    ret = avcodec_send_packet(codec_ctx, NULL);
                    if (ret < 0 && ret != AVERROR_EOF) return -1;
                    break;
                }
                if (packet->stream_index != audio_stream_index) { av_packet_unref(packet); continue; }
                ret = avcodec_send_packet(codec_ctx, packet);
                av_packet_unref(packet);
                if (ret == AVERROR(EAGAIN)) break;
                if (ret < 0) return -1;
                break;
            }
        } else {
            *decoder_finished = 1;
            return 0;
        }
    }
    return 0;
}

/* ============================================================
 * Atempo filter flush helper (for end-of-stream)
 * ============================================================ */

static void drain_atempo_filter(AVFrame *filtered_frame)
{
    if (!atempo_is_active()) return;
    atempo_flush();
    while (atempo_receive_frame(filtered_frame) == 0)
        av_frame_unref(filtered_frame);
    cleanup_atempo_filter();
}

/* ============================================================
 * Playback thread
 * ============================================================ */

void *play_audio_thread(void *arg)
{
    int index = *((int *)arg);
    int reached_end_of_stream = 0;
    int followup_index = -1;
    free(arg);

    extern int g_play_thread_running;
    extern PlayState g_play_state;
    extern int g_current_play_index;
    extern char g_cached_audio_path[256];
    extern char g_cached_lyrics_path[256];
    extern int g_total_duration;
    extern int g_current_position;
    extern float g_playback_speed;
    extern int g_seek_request;
    extern int g_seek_position;
    extern int g_initial_seek_position;
    extern int g_audio_sample_rate;
    extern int g_audio_bit_rate;
    extern int g_audio_bit_depth;
    extern char g_audio_codec_name[32];
    extern int g_pending_playback_index;
    extern LoopMode g_loop_mode;
    extern int g_play_thread_active;
    extern int g_play_thread_finished;

    log_info("audio", "Playback thread started for index=%d", index);

    pthread_mutex_lock(&g_play_mutex);
    int thread_running = g_play_thread_running;
    pthread_mutex_unlock(&g_play_mutex);

    char file_path[MAX_PATH_LEN];
    int valid_index = playlist_get_track_path(index, file_path, sizeof(file_path)) == 0;

    if (valid_index && g_cached_audio_path[0]) {
        strncpy(file_path, g_cached_audio_path, MAX_PATH_LEN - 1);
        file_path[MAX_PATH_LEN - 1] = '\0';
    }

    if (!valid_index || !thread_running) {
        log_warn("audio", "Playback thread: invalid index=%d or thread not running", index);
        pthread_mutex_lock(&g_play_mutex);
        g_play_thread_running = 0;
        g_play_thread_finished = 1;
        pthread_mutex_unlock(&g_play_mutex);
        return NULL;
    }
    log_debug("audio", "Playback thread file_path='%s'", file_path);

    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    SwrContext *swr_ctx = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    int audio_stream_index = -1;
    int input_channels = 2;
    int output_channels = 2;
    int output_sample_rate = 44100;
    int use_resampler = 1;
    int decoder_draining = 0;
    int decoder_finished = 0;
    int playback_error = 0;
    int prefill_target_frames = 0;
    PCMQueue pcm_queue;
    int pcm_queue_initialized = 0;
    memset(&pcm_queue, 0, sizeof(pcm_queue));

    if (avformat_open_input(&fmt_ctx, file_path, NULL, NULL) != 0) {
        log_error("audio", "avformat_open_input failed for '%s' (index=%d)", file_path, index);
        update_controls_status(audio_text("无法打开音频文件", "Cannot open audio file"));
        goto cleanup;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        log_error("audio", "avformat_find_stream_info failed for '%s'", file_path);
        update_controls_status(audio_text("无法读取音频流信息", "Cannot read stream info"));
        goto cleanup;
    }

    g_total_duration = fmt_ctx->duration / AV_TIME_BASE;
    if (g_total_duration <= 0) {
        for (int i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                AVRational time_base = fmt_ctx->streams[i]->time_base;
                int64_t stream_duration = fmt_ctx->streams[i]->duration;
                if (stream_duration > 0 && time_base.den > 0) {
                    g_total_duration = av_rescale_q(stream_duration, time_base, (AVRational){1, 1});
                    break;
                }
            }
        }
        if (g_total_duration <= 0) g_total_duration = 300;
    }

    g_current_position = 0;
    reset_visualizer_state();

    int initial_seek_position = g_initial_seek_position;
    g_initial_seek_position = 0;

    request_ui_refresh(UI_DIRTY_CONTROLS);

    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }
    if (audio_stream_index == -1) {
        log_error("audio", "No audio stream found in '%s'", file_path);
        update_controls_status(audio_text("未找到音频流", "No audio stream found"));
        goto cleanup;
    }

    AVCodecParameters *codec_par = fmt_ctx->streams[audio_stream_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        log_error("audio", "Unsupported codec in '%s' (codec_id=%d)", file_path, codec_par->codec_id);
        update_controls_status(audio_text("当前编解码器不受支持", "Unsupported codec"));
        goto cleanup;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) { update_controls_status(audio_text("无法分配解码器上下文", "Cannot allocate codec context")); goto cleanup; }
    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) { update_controls_status(audio_text("无法复制编解码参数", "Cannot copy codec parameters")); goto cleanup; }
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) { update_controls_status(audio_text("无法打开编解码器", "Cannot open codec")); goto cleanup; }

    input_channels = codec_channel_count(codec_ctx);
    if (input_channels <= 0) input_channels = 2;
    output_channels = (input_channels == 1) ? 1 : 2;
    output_sample_rate = codec_ctx->sample_rate > 0 ? codec_ctx->sample_rate : 44100;

    g_audio_sample_rate = codec_ctx->sample_rate > 0 ? codec_ctx->sample_rate : 0;
    g_audio_bit_rate = (codec_ctx->bit_rate > 0) ? codec_ctx->bit_rate :
                       (codec_par->bit_rate > 0) ? codec_par->bit_rate : 0;
    if (g_audio_bit_rate <= 0 && g_total_duration > 0 && fmt_ctx && fmt_ctx->pb) {
        int64_t file_size = avio_size(fmt_ctx->pb);
        if (file_size > 0) g_audio_bit_rate = (int)((file_size * 8) / g_total_duration);
    }
    if (codec_ctx->bits_per_raw_sample > 0)
        g_audio_bit_depth = codec_ctx->bits_per_raw_sample;
    else {
        int bytes = av_get_bytes_per_sample(codec_ctx->sample_fmt);
        g_audio_bit_depth = bytes > 0 ? bytes * 8 : 0;
    }
    snprintf(g_audio_codec_name, sizeof(g_audio_codec_name), "%s", codec->name);

    log_debug("audio", "Audio stream: rate=%d, channels=%d, codec=%s, duration=%ds",
              output_sample_rate, output_channels, codec ? codec->name : "unknown", g_total_duration);

    prefill_target_frames = (output_sample_rate * get_pcm_prefill_target_ms(output_sample_rate) + 999) / 1000;
    use_resampler = (codec_ctx->sample_fmt != AV_SAMPLE_FMT_S32 || input_channels != output_channels);

    if (use_resampler) {
        swr_ctx = swr_alloc();
        if (!swr_ctx) { update_controls_status(audio_text("无法分配重采样器", "Cannot allocate resampler")); goto cleanup; }
        if (init_resampler(swr_ctx, codec_ctx, input_channels, output_channels, output_sample_rate) < 0) {
            update_controls_status(audio_text("无法初始化重采样器", "Cannot initialize resampler")); goto cleanup;
        }
    }

    if (init_atempo_filter(codec_ctx, g_playback_speed) < 0) {
        update_controls_status(audio_text("无法初始化倍速滤镜", "Cannot initialize speed filter")); goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    AVFrame *filtered_frame = av_frame_alloc();
    if (!packet || !frame || !filtered_frame) {
        update_controls_status(audio_text("无法分配解码数据结构", "Cannot allocate decode structures"));
        goto cleanup;
    }

    if (pcm_queue_init(&pcm_queue) < 0) {
        update_controls_status(audio_text("无法分配音频缓冲区", "Cannot allocate audio buffer")); goto cleanup;
    }
    pcm_queue_initialized = 1;

    if (audio_backend_prepare_stream(output_sample_rate, output_channels) < 0) goto cleanup;

    audio_backend_resume_stream();
    audio_backend_sync_volume(1);

    progress_tracker_init(output_sample_rate);
    progress_tracker_set_sample_rate(output_sample_rate);
    progress_tracker_set_speed(g_playback_speed);
    progress_tracker_start();

    g_play_state = PLAY_STATE_PLAYING;

    if (initial_seek_position > 0 && initial_seek_position < g_total_duration) {
        g_seek_position = initial_seek_position;
        g_seek_request = 1;
    }

    while (g_play_thread_running) {
        audio_backend_sync_volume(0);

        if (!wait_while_paused()) break;

        if (handle_seek_request_in_decoder(fmt_ctx, codec_ctx, swr_ctx, packet, &pcm_queue,
                                           audio_stream_index, &decoder_draining, &decoder_finished)) {
            if (g_current_position >= g_total_duration) { g_play_thread_running = 0; break; }
            continue;
        }

        while (g_play_thread_running && !decoder_finished &&
               pcm_queue.count < PCM_QUEUE_CAPACITY &&
               pcm_queue.buffered_frames < prefill_target_frames) {
            int dr = decode_next_pcm_chunk(fmt_ctx, codec_ctx, swr_ctx, packet, frame, filtered_frame,
                                            &pcm_queue, audio_stream_index, output_sample_rate,
                                            output_channels, use_resampler, &decoder_draining, &decoder_finished);
            if (dr < 0) { playback_error = 1; break; }
            if (dr == 0) break;
        }

        if (playback_error) break;

        PCMChunk *chunk = pcm_queue_peek(&pcm_queue);
        if (!chunk) {
            if (decoder_finished) { reached_end_of_stream = 1; break; }
            continue;
        }

        if (pcm_queue_buffered_ms(&pcm_queue, output_sample_rate) > get_configured_latency_ms())
            push_visualizer_samples(chunk->data, chunk->frame_count, output_channels);

        /* Volume applied inline in main audio.c's apply_volume_to_samples — doing it here */
        extern void apply_volume_to_samples(int32_t *samples, int sample_count);
        apply_volume_to_samples(chunk->data, chunk->frame_count * output_channels);

        if (audio_backend_write_samples(chunk->data, chunk->frame_count) < 0) {
            update_controls_status(audio_text("写入音频设备失败", "Audio device write failed"));
            playback_error = 1;
            break;
        }

        progress_tracker_add_samples(chunk->frame_count);
        g_current_position = progress_tracker_get_position_seconds();
        pcm_queue_consume(&pcm_queue);
    }

cleanup:
    drain_atempo_filter(filtered_frame);
    audio_backend_cleanup_stream();

    if (pcm_queue_initialized) pcm_queue_destroy(&pcm_queue);
    av_frame_free(&filtered_frame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    progress_tracker_on_stop();
    reset_visualizer_state();

    if (!reached_end_of_stream) g_current_position = 0;

    int playlist_total = playlist_count();

    pthread_mutex_lock(&g_play_mutex);
    if (g_play_thread_running && reached_end_of_stream) {
        switch (g_loop_mode) {
            case LOOP_SINGLE:  followup_index = index; break;
            case LOOP_LIST:    if (playlist_total > 0) followup_index = (index + 1) % playlist_total; break;
            case LOOP_RANDOM:  if (playlist_total > 0) followup_index = rand() % playlist_total; break;
            case LOOP_OFF: default: break;
        }
    }
    if (reached_end_of_stream && followup_index >= 0)
        log_info("audio", "End of stream for index=%d, scheduling follow-up index=%d (loop=%d)", index, followup_index, g_loop_mode);
    else if (reached_end_of_stream)
        log_info("audio", "End of stream for index=%d, no follow-up (loop=%d)", index, g_loop_mode);
    else if (playback_error)
        log_warn("audio", "Playback error for index=%d, stopping thread", index);

    g_play_thread_running = 0;
    g_play_thread_finished = 1;
    g_play_state = PLAY_STATE_STOPPED;
    g_current_play_index = -1;

    g_audio_sample_rate = 0;
    g_audio_bit_rate = 0;
    g_audio_bit_depth = 0;
    g_audio_codec_name[0] = '\0';

    if (followup_index >= 0 && g_pending_playback_index < 0)
        g_pending_playback_index = followup_index;
    pthread_mutex_unlock(&g_play_mutex);

    if (followup_index < 0) {
        extern void clear_lyrics(void);
        clear_lyrics();
        request_ui_refresh(UI_DIRTY_LYRICS);
    }
    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS);

    log_debug("audio", "Playback thread exiting for index=%d (eos=%d, err=%d)", index, reached_end_of_stream, playback_error);
    return NULL;
}
