/**
 * @file playback_thread.c
 * @brief 音频播放线程 — Segment 池、解码循环、线程函数
 *
 * 基于 m3u8 分段思路，将曲目拆成 15 秒一段的 Segment，
 * 池中最多保留当前段 + 后两段（共 3 段），曲目末尾自动预加载下一曲的前 1-2 段。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-04
 */

#include "types.h"
#include "audio/audio.h"
#include "audio/audio_internal.h"
#include "audio/karaoke_cache.h"
#include "audio/karaoke_worker.h"
#include "audio/progress/progress.h"
#include "audio/play_queue.h"
#include "audio/visualizer.h"
#include "core/core.h"
#include "queue/backend_queue.h"
#include "i18n/i18n.h"
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

#include "lyrics/lyrics.h"

/* ── Write batch size (~23ms at 44100 Hz) ── */
#define WRITE_BATCH_FRAMES 1024

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
 * Append decoded frame data into a Segment buffer
 * ============================================================ */

static void append_decoded_frame_to_segment(Segment *seg, AVFrame *frame,
                                            SwrContext *swr_ctx,
                                            int output_sample_rate,
                                            int output_channels,
                                            int use_resampler,
                                            int atempo_input_sample_rate)
{
    if (!seg || !frame) return;

    int max_extra = seg->capacity_frames - seg->frame_count;
    if (max_extra <= 0) return;

    if (!use_resampler) {
        int bytes = av_samples_get_buffer_size(NULL, output_channels,
                                               frame->nb_samples, AV_SAMPLE_FMT_S32, 1);
        if (bytes > 0) {
            int frames = frame->nb_samples;
            if (frames > max_extra) frames = max_extra;
            memcpy(seg->data + seg->frame_count * output_channels,
                   frame->data[0], (size_t)frames * output_channels * sizeof(int32_t));
            seg->frame_count += frames;
        }
    } else {
        int src_rate = (atempo_input_sample_rate > 0) ? atempo_input_sample_rate
                                                       : output_sample_rate;
        int dst_nb = av_rescale_rnd(
            swr_get_delay(swr_ctx, src_rate) + frame->nb_samples,
            output_sample_rate, src_rate, AV_ROUND_UP);
        if (dst_nb > max_extra) dst_nb = max_extra;
        if (dst_nb > 0) {
            uint8_t *planes[] = {
                (uint8_t *)(seg->data + seg->frame_count * output_channels)
            };
            int produced = swr_convert(swr_ctx, planes, dst_nb,
                                       (const uint8_t **)frame->data, frame->nb_samples);
            if (produced > 0) seg->frame_count += produced;
        }
    }
}

/* ============================================================
 * Decode one segment into a pool slot
 *
 * max_new_frames <= 0：一次填满整段（起播/seek 等本来就要暂停的路径）。
 * max_new_frames > 0：本次最多再解码这么多帧然后返回，槽保持未就绪，
 * 由主循环下一轮从 seg->frame_count 处继续（解码器/读包位置天然可续跑）。
 * 分片的意义：PipeWire 写入端按配置延时节流后，环里只压几百毫秒数据；
 * 若 15 秒一段仍在单次调用里整段解完（百毫秒级阻塞），段界处会被饿出
 * 可闻的静音。分片把阻塞摊薄到每轮几毫秒。
 * read_errors：跨调用保存的读错误熔断计数（分片续跑不得重置熔断）。
 * 返回：-1 出错；1 解到了数据；0 一段都没解到。调用方以
 * “frame_count 达标或 decoder_finished”判定填满，只有填满才 mark_ready。
 * ============================================================ */

/* 每轮主循环允许的分片解码帧数（≈93ms 音频，mp3 解码 CPU 开销约 1–2ms） */
#define DECODE_SLICE_FRAMES 4096

/* 读错误熔断计数：单播放线程内使用，track 起播时清零 */
static int g_decode_read_errors = 0;
/* 当前预加载数据来自哪个播放源（KARAOKE_SOURCE_*）；起播时用于丢弃"源已变"的预加载。 */
static int g_preload_source_kind = KARAOKE_SOURCE_ORIGINAL;

static int decode_segment_fill(AVFormatContext *fmt_ctx,
                               AVCodecContext *codec_ctx,
                               SwrContext *swr_ctx,
                               AVPacket *packet,
                               AVFrame *frame,
                               AVFrame *filtered_frame,
                               SegmentPool *pool,
                               int target_slot,
                               int target_frames,
                               int max_new_frames,
                               int audio_stream_index,
                               int output_sample_rate,
                               int output_channels,
                               int use_resampler,
                               int *decoder_draining,
                               int *decoder_finished)
{
    if (!fmt_ctx || !codec_ctx || !packet || !frame || !filtered_frame ||
        !pool || target_slot < 0 || target_slot >= SEGMENT_POOL_SIZE)
        return -1;

    Segment *seg = &pool->slots[target_slot];

    /* Make sure we don't exceed capacity */
    if (target_frames > seg->capacity_frames)
        target_frames = seg->capacity_frames;

    const int slice_start = seg->frame_count;

    while (seg->frame_count < target_frames && !*decoder_finished &&
           (max_new_frames <= 0 || seg->frame_count - slice_start < max_new_frames)) {
        /* 1. Try atempo filtered frame first */
        if (atempo_is_active()) {
            int filter_ret = atempo_receive_frame(filtered_frame);
            if (filter_ret == 0) {
                append_decoded_frame_to_segment(seg, filtered_frame,
                                                 swr_ctx, output_sample_rate,
                                                 output_channels, use_resampler,
                                                 atempo_get_input_sample_rate());
                av_frame_unref(filtered_frame);

                /* Check if segment is now full */
                if (seg->frame_count >= target_frames)
                    break;
                continue;
            }
        }

        /* 2. Try receiving decoded frame */
        int ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == 0) {
            if (atempo_is_active()) {
                if (atempo_send_frame(frame) < 0) { av_frame_unref(frame); return -1; }
                av_frame_unref(frame);
                continue;
            }

            /* No atempo: directly append */
            append_decoded_frame_to_segment(seg, frame, swr_ctx,
                                             output_sample_rate, output_channels,
                                             use_resampler, 0);
            av_frame_unref(frame);
            continue;
        }

        if (ret == AVERROR_EOF) {
            *decoder_finished = 1;
            break;
        }
        if (ret != AVERROR(EAGAIN)) {
            /* Non-EOF, non-EAGAIN error from decoder — log and try to
             * continue rather than aborting the whole track.  Corrupted
             * FLAC frames or checksum mismatches can trigger transient
             * decode errors that don't make the rest of the file unreadable. */
            log_warn("audio", "avcodec_receive_frame error %d in decode_segment_fill, trying to continue", ret);
            /* Fall through to packet reading to advance past the problem area */
        }

        /* 3. Read next packet */
        if (!*decoder_draining) {
            /* Circuit breaker: limit consecutive read errors to avoid
             * infinite spinning on a truly pathological file.  100
             * failures is well beyond any realistic transient-error
             * burst but small enough to avoid hung playback. */
            while (1) {
                ret = av_read_frame(fmt_ctx, packet);
                if (ret == AVERROR_EOF) {
                    *decoder_draining = 1;
                    ret = avcodec_send_packet(codec_ctx, NULL);
                    if (ret < 0 && ret != AVERROR_EOF) return -1;
                    break;
                }
                if (ret < 0) {
                    /* Non-EOF read error (e.g. corrupted frame, checksum
                     * mismatch).  Log and try to skip past the bad data
                     * rather than draining the decoder immediately —
                     * draining here would discard all remaining audio. */
                    log_warn("audio", "av_read_frame error %d in decode_segment_fill, attempting to skip and continue", ret);
                    av_packet_unref(packet);
                    if (++g_decode_read_errors > 100) {
                        log_error("audio", "av_read_frame: too many consecutive errors (%d), aborting segment fill",
                                  g_decode_read_errors);
                        return -1;
                    }
                    continue;
                }
                g_decode_read_errors = 0;  /* successful read resets the breaker */

                if (packet->stream_index != audio_stream_index) { av_packet_unref(packet); continue; }
                ret = avcodec_send_packet(codec_ctx, packet);
                av_packet_unref(packet);
                if (ret == AVERROR(EAGAIN)) {
                    /* Decoder not ready — drain with avcodec_receive_frame and
                     * re-read; the packet is lost, but EAGAIN is extremely rare
                     * for audio codecs (it's primarily an issue with video). */
                    break;
                }
                if (ret < 0) {
                    /* Codec rejected the packet — non-fatal, try next */
                    log_warn("audio", "avcodec_send_packet error %d in decode_segment_fill, skipping packet", ret);
                    continue;
                }
                break;
            }
        } else {
            *decoder_finished = 1;
            break;
        }
    }

    return (seg->frame_count > 0) ? 1 : 0;
}

/* ============================================================
 * Seek-in-decoder handler (segment-aware)
 * ============================================================ */

/* 普通（原声 / v1 整曲缓存）解码器定位到 target_position（子曲目相对秒）。
 * 段内偏移按输出帧精确跳过（此前从段首重播的误差 ≤15s）。 */
static int decoder_seek_to(AVFormatContext *fmt_ctx,
                           AVCodecContext *codec_ctx,
                           SwrContext *swr_ctx,
                           AVPacket *packet,
                           AVFrame *frame,
                           AVFrame *filtered_frame,
                           SegmentPool *pool,
                           int audio_stream_index,
                           int output_sample_rate,
                           int output_channels,
                           int use_resampler,
                           int *decoder_draining,
                           int *decoder_finished,
                           int target_position,
                           int cue_offset)
{
    extern int g_current_position;
    extern float g_playback_speed;

    AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;
    int64_t target_ts = av_rescale_q(target_position + cue_offset,
                                     (AVRational){1, 1}, time_base);
    int ret = av_seek_frame(fmt_ctx, audio_stream_index, target_ts, 0);
    if (ret < 0) {
        core_status_push(i18n_get("audio.err.seek_failed"));
        return -1;
    }
    avcodec_flush_buffers(codec_ctx);
    if (swr_ctx) swr_init(swr_ctx);
    if (packet) av_packet_unref(packet);

    int target_seg = segment_id_from_position(target_position);
    int saved_total = pool->total_segments;
    segment_pool_seek_to(pool, target_seg);
    pool->total_segments = saved_total;

    /* Reset atempo filter to clear stale pre-seek samples */
    if (atempo_is_active()) {
        float speed = atempo_get_speed();
        cleanup_atempo_filter();
        if (speed > 0.01f)
            init_atempo_filter(codec_ctx, speed);
    }

    audio_backend_flush_stream();
    progress_tracker_seek(target_position);
    g_current_position = target_position;

    if (decoder_draining) *decoder_draining = 0;
    if (decoder_finished) *decoder_finished = 0;

    int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
    int fill_ret = decode_segment_fill(fmt_ctx, codec_ctx, swr_ctx, packet, frame,
                                       filtered_frame, pool, pool->current_slot,
                                       target_frames, 0 /*整段*/,
                                       audio_stream_index, output_sample_rate,
                                       output_channels, use_resampler,
                                       decoder_draining, decoder_finished);
    if (fill_ret < 0) {
        log_error("audio", "handle_seek_request: decode_segment_fill failed, aborting seek");
        core_status_push(i18n_get("audio.err.seek_failed"));
        return -1;
    }

    /* 段内偏移：从 target_seg 的段首跳过 offset 秒对应的输出帧。 */
    Segment *cur = &pool->slots[pool->current_slot];
    int offset_sec = target_position - target_seg * SEGMENT_DURATION_SEC;
    if (offset_sec > 0 && cur->frame_count > 0) {
        long long skip = (long long)((double)offset_sec * output_sample_rate *
                                     (double)g_playback_speed + 0.5);
        if (skip >= cur->frame_count)
            skip = cur->frame_count > 0 ? cur->frame_count - 1 : 0;
        cur->consumed_frames = (int)skip;
    }

    int is_last = (*decoder_finished) || (target_seg >= pool->total_segments - 1);
    segment_pool_mark_ready(pool, pool->current_slot, cur->frame_count, target_seg,
                            is_last);

    char msg[64];
    snprintf(msg, sizeof(msg), i18n_get("status.seek_fmt"),
             target_position / 60, target_position % 60);
    core_status_push(msg);
    return 0;
}

static int handle_seek_request_in_decoder(AVFormatContext *fmt_ctx,
                                          AVCodecContext *codec_ctx,
                                          SwrContext *swr_ctx,
                                          AVPacket *packet,
                                          AVFrame *frame,
                                          AVFrame *filtered_frame,
                                          SegmentPool *pool,
                                          int audio_stream_index,
                                          int output_sample_rate,
                                          int output_channels,
                                          int use_resampler,
                                          int *decoder_draining,
                                          int *decoder_finished,
                                          int cue_offset)
{
    extern pthread_mutex_t g_seek_mutex;
    extern int g_seek_request;
    extern int g_seek_position;

    int handled = 0;
    pthread_mutex_lock(&g_seek_mutex);
    if (g_seek_request && fmt_ctx && codec_ctx) {
        int target_position = g_seek_position;
        g_seek_request = 0;
        handled = 1;
        pthread_mutex_unlock(&g_seek_mutex);
        (void)decoder_seek_to(fmt_ctx, codec_ctx, swr_ctx, packet, frame,
                              filtered_frame, pool, audio_stream_index,
                              output_sample_rate, output_channels, use_resampler,
                              decoder_draining, decoder_finished, target_position,
                              cue_offset);
        return handled;
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
 * Next-track preloading
 * ============================================================ */

static void attempt_next_track_preload(int current_track_index,
                                       SegmentPool *pool,
                                       int output_sample_rate,
                                       int output_channels)
{
    if (!pool) return;

    /* Skip preload at non-1.0x speed — preloaded segments would be at
     * original tempo while the new thread expects speed-adjusted PCM */
    extern float g_playback_speed;
    if (g_playback_speed < 0.99f || g_playback_speed > 1.01f) {
        log_debug("segment", "preload: skipping (speed=%.2f != 1.0)", (double)g_playback_speed);
        return;
    }

    /* Determine the next queue position (need lock for queue access) */
    int next_index = -1;
    BackendQueueEntry next_entry;
    int have_next_entry = 0;
    pthread_mutex_lock(&g_play_mutex);
    next_index = play_queue_peek_next(&g_play_queue, g_play_mode);
    if (next_index >= 0) {
        have_next_entry = bq_entry_at(next_index, &next_entry) == 0;
    }
    pthread_mutex_unlock(&g_play_mutex);
    if (next_index < 0 || next_index == current_track_index || !have_next_entry) {
        log_debug("segment", "preload: no valid next track (pos=%d)", next_index);
        return;
    }

    /* Skip preload for CUE tracks — preloaded PCM from position 0 would be
     * at the wrong offset. The playback thread handles CUE offset seeking. */
    if (next_entry.cue_offset > 0) {
        log_debug("segment", "preload: skipping (CUE track with offset)");
        return;
    }

    char file_path[MAX_PATH_LEN];
    snprintf(file_path, sizeof(file_path), "%s", next_entry.path);
    int preload_source_kind = KARAOKE_SOURCE_ORIGINAL;
    {
        /* 预加载同样走 cache resolve；起播时会再 resolve 一次，若两次源不同
         * 则丢弃预加载数据（仍然只在曲目起点选源，不中途切换）。 */
        KaraokeCacheConfig kcfg;
        char kroot[MAX_PATH_LEN];
        char kmodel[MAX_PATH_LEN];
        int mode = karaoke_get_mode();
        if (mode != KARAOKE_MODE_OFF &&
            audio_karaoke_cache_config(&kcfg, kroot, sizeof(kroot), kmodel,
                                       sizeof(kmodel))) {
            char resolved[MAX_PATH_LEN];
            if (karaoke_cache_resolve(&kcfg, next_entry.path, mode, resolved,
                                      sizeof(resolved), NULL, 0) == 1) {
                snprintf(file_path, sizeof(file_path), "%s", resolved);
                preload_source_kind = mode;
            }
        }
    }

    log_info("segment", "preload: attempting next track pos=%d path='%s'", next_index, file_path);

    /* Ensure preload data buffers are allocated */
    if (preload_data_ensure_init(&g_preload_data, output_sample_rate, output_channels) < 0) {
        log_warn("segment", "preload: failed to init preload buffers");
        return;
    }

    /* Open FFmpeg context for next track */
    AVFormatContext *next_fmt = NULL;
    AVCodecContext *next_codec = NULL;
    SwrContext *next_swr = NULL;
    AVPacket *next_pkt = NULL;
    AVFrame *next_frame = NULL;
    AVFrame *next_filtered = NULL;
    int audio_stream = -1;
    int drain = 0, eof = 0;
    int success = 0;
    int preloaded_count = 0;

    if (avformat_open_input(&next_fmt, file_path, NULL, NULL) != 0) {
        log_warn("segment", "preload: avformat_open_input failed for '%s'", file_path);
        goto preload_cleanup;
    }
    if (avformat_find_stream_info(next_fmt, NULL) < 0) {
        log_warn("segment", "preload: avformat_find_stream_info failed");
        goto preload_cleanup;
    }

    for (int i = 0; i < (int)next_fmt->nb_streams; i++) {
        if (next_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream = i;
            break;
        }
    }
    if (audio_stream < 0) {
        log_warn("segment", "preload: no audio stream in '%s'", file_path);
        goto preload_cleanup;
    }

    AVCodecParameters *par = next_fmt->streams[audio_stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { log_warn("segment", "preload: unsupported codec"); goto preload_cleanup; }

    next_codec = avcodec_alloc_context3(codec);
    if (!next_codec) goto preload_cleanup;
    if (avcodec_parameters_to_context(next_codec, par) < 0) goto preload_cleanup;
    if (avcodec_open2(next_codec, codec, NULL) < 0) goto preload_cleanup;

    int in_ch = codec_channel_count(next_codec);
    if (in_ch <= 0) in_ch = 2;
    int out_ch = (in_ch == 1) ? 1 : 2;
    int in_rate = next_codec->sample_rate > 0 ? next_codec->sample_rate : output_sample_rate;
    int need_resample = (next_codec->sample_fmt != AV_SAMPLE_FMT_S32 || in_ch != out_ch);

    if (need_resample) {
        next_swr = swr_alloc();
        if (!next_swr) goto preload_cleanup;
        if (init_resampler(next_swr, next_codec, in_ch, out_ch, output_sample_rate) < 0) {
            log_warn("segment", "preload: resampler init failed");
            swr_free(&next_swr);
            next_swr = NULL;
            need_resample = 0;
        }
    }

    next_pkt = av_packet_alloc();
    next_frame = av_frame_alloc();
    next_filtered = av_frame_alloc();
    if (!next_pkt || !next_frame || !next_filtered) goto preload_cleanup;

    /* Decode up to PRELOAD_SEGMENT_COUNT segments.
     * 预加载是同步跑的：开文件、探测流、解码，全在写线程里，PipeWire
     * 节流后的积压只有几百毫秒，一次解满 15 秒（重编码可达数百毫秒）
     * 会在段尾附近饿出可闻空隙。这里只预解开头 PRELOAD_COVER_SEC 秒，
     * 够接上无缝切换的班即可，剩余部分由新线程的常规分片解码补齐。 */
#define PRELOAD_COVER_SEC 5
    int target_frames = output_sample_rate * PRELOAD_COVER_SEC;
    if (target_frames > SEGMENT_DURATION_SEC * output_sample_rate)
        target_frames = SEGMENT_DURATION_SEC * output_sample_rate;
    for (int seg_idx = 0; seg_idx < PRELOAD_SEGMENT_COUNT; seg_idx++) {
        /* Abort check — user skipped track or seeked */
        {
            extern int g_play_thread_running;
            extern int g_seek_request;
            if (!g_play_thread_running || g_seek_request) {
                log_debug("segment", "preload: aborted (thread stopped or seek requested)");
                break;
            }
        }

        if (eof) break;

        Segment *dst = &g_preload_data.segments[seg_idx];
        dst->frame_count = 0;
        dst->consumed_frames = 0;
        dst->is_valid = 0;

        int frames_so_far = 0;
        int preload_read_errors = 0;  /* circuit breaker for corrupt next-track */
        while (frames_so_far < target_frames && !eof) {
            /* Try atempo (preload uses original speed, no atempo needed for preload) */
            int rret = avcodec_receive_frame(next_codec, next_frame);
            if (rret == 0) {
                int max_extra = dst->capacity_frames - dst->frame_count;
                if (max_extra <= 0) break;

                if (!need_resample) {
                    int bytes = av_samples_get_buffer_size(NULL, out_ch,
                                            next_frame->nb_samples, AV_SAMPLE_FMT_S32, 1);
                    if (bytes > 0) {
                        int f = next_frame->nb_samples;
                        if (f > max_extra) f = max_extra;
                        memcpy(dst->data + dst->frame_count * out_ch,
                               next_frame->data[0], (size_t)f * out_ch * sizeof(int32_t));
                        dst->frame_count += f;
                        frames_so_far += f;
                    }
                } else if (next_swr) {
                    int dst_nb = av_rescale_rnd(
                        swr_get_delay(next_swr, in_rate) + next_frame->nb_samples,
                        output_sample_rate, in_rate, AV_ROUND_UP);
                    if (dst_nb > max_extra) dst_nb = max_extra;
                    if (dst_nb > 0) {
                        uint8_t *planes[] = { (uint8_t *)(dst->data + dst->frame_count * out_ch) };
                        int p = swr_convert(next_swr, planes, dst_nb,
                                            (const uint8_t **)next_frame->data,
                                            next_frame->nb_samples);
                        if (p > 0) { dst->frame_count += p; frames_so_far += p; }
                    }
                }
                av_frame_unref(next_frame);
                continue;
            }

            if (rret == AVERROR_EOF) { eof = 1; break; }
            if (rret != AVERROR(EAGAIN)) {
                /* Non-EOF decode error in preload — log and try to skip */
                log_warn("audio", "preload: avcodec_receive_frame error %d, trying to continue", rret);
            }

            if (!drain) {
                int pret = av_read_frame(next_fmt, next_pkt);
                if (pret == AVERROR_EOF) {
                    drain = 1;
                    avcodec_send_packet(next_codec, NULL);
                    break;
                }
                if (pret < 0) {
                    /* Non-EOF read error in preload — try to skip */
                    av_packet_unref(next_pkt);
                    if (++preload_read_errors > 100) {
                        log_error("audio", "preload: av_read_frame too many consecutive errors (%d), aborting",
                                  preload_read_errors);
                        eof = 1;
                        break;
                    }
                    log_warn("audio", "preload: av_read_frame error %d, attempting to skip", pret);
                    continue;
                }
                preload_read_errors = 0;  /* successful read resets the breaker */
                if (next_pkt->stream_index == audio_stream) {
                    avcodec_send_packet(next_codec, next_pkt);
                }
                av_packet_unref(next_pkt);
            } else {
                eof = 1;
            }
        }

        if (dst->frame_count > 0) {
            dst->is_valid = 1;
            preloaded_count++;
            log_debug("segment", "preload: segment %d decoded (%d frames)", seg_idx, dst->frame_count);
        }
    }

    if (preloaded_count > 0) {
        pthread_mutex_lock(&g_preload_data.lock);
        g_preload_data.valid = 1;
        g_preload_source_kind = preload_source_kind;
        g_preload_data.track_index = next_index;
        g_preload_data.segment_count = preloaded_count;
        g_preload_data.sample_rate = output_sample_rate;
        g_preload_data.channels = output_channels;
        pthread_mutex_unlock(&g_preload_data.lock);
        success = 1;
        log_info("segment", "preload: successfully preloaded %d segments for track idx=%d",
                 preloaded_count, next_index);
    }

preload_cleanup:
    av_frame_free(&next_filtered);
    av_frame_free(&next_frame);
    av_packet_free(&next_pkt);
    swr_free(&next_swr);
    avcodec_free_context(&next_codec);
    avformat_close_input(&next_fmt);

    if (!success)
        log_debug("segment", "preload: no segments preloaded for track idx=%d", next_index);
}

/* ============================================================
 * v2 分段播放源（D1/D3/D4，15s 段 /tmp 会话缓存）
 *
 * 与整曲解码路径的关键差异：
 *   - 音频来自 `<session_root>/<model>/<track_hash>/segNNN.{inst,soft}.flac`，
 *     每段独立打开 FLAC 解码（44100/2ch/16bit），段内整段解完再交给段池；
 *   - swr 与 atempo 跨段复用（同一 44.1k 输入坐标），保证段边界状态连续；
 *   - 段未就绪时播放线程最多等待（首段 30s / 一般 8s），超时或 job 失败
 *     就从段边界回退原声继续播到曲终；
 *   - seek 到未处理段时先给 worker 发优先提示，再等待/回退。
 * ============================================================ */

typedef struct {
    int valid;                          /* manifest 可读 */
    int mode;                           /* KARAOKE_SOURCE_SOFT / _FULL */
    char root[KARAOKE_CACHE_PATH_CAP];
    char model_id[128];
    char model_sha256[65];
    float overlap;
    float soft_inst_mix;
    KaraokeCacheConfig cfg;             /* 指针指向上面缓冲区 */
    KaraokeSegmentPaths paths;
    KaraokeSegmentManifest manifest;
    int total;                          /* manifest 总段数（0 = 未知） */
    int complete;
} SegSource;

static void seg_source_setup(SegSource *s, const KaraokeCacheConfig *base, int mode)
{
    memset(s, 0, sizeof(*s));
    s->mode = mode;
    snprintf(s->root, sizeof(s->root), "%s", base->root);
    snprintf(s->model_id, sizeof(s->model_id), "%s", base->model_id);
    snprintf(s->model_sha256, sizeof(s->model_sha256), "%s", base->model_sha256);
    s->overlap = base->overlap;
    s->soft_inst_mix = base->soft_inst_mix;
    memset(&s->cfg, 0, sizeof(s->cfg));
    s->cfg.root = s->root;
    s->cfg.model_id = s->model_id;
    s->cfg.model_sha256 = s->model_sha256;
    s->cfg.overlap = s->overlap;
    s->cfg.soft_inst_mix = s->soft_inst_mix;
    s->cfg.limit_mb = 0;
}

/* 读 manifest / 段状态；返回 1 表示 manifest 可读。 */
static int seg_source_refresh(SegSource *s, const char *track_path)
{
    if (!s || !s->root[0] || !track_path)
        return 0;
    KaraokeSegmentPaths paths;
    KaraokeSegmentManifest m;
    if (karaoke_cache_segment_lookup(&s->cfg, track_path, &paths, &m) != 1)
        return 0;
    s->valid = 1;
    s->paths = paths;
    s->manifest = m;
    s->total = m.total_segments;
    s->complete = m.complete;
    return 1;
}

static int seg_source_ready(SegSource *s, int seg)
{
    if (!s || !s->valid || seg < 0)
        return 0;
    return karaoke_cache_segment_ready(&s->paths, &s->manifest, seg, NULL, NULL, 0);
}

/* 段播放文件路径；1=就绪并回填。 */
static int seg_source_file(SegSource *s, int seg, char *out, size_t cap)
{
    if (!seg_source_ready(s, seg))
        return 0;
    int n = snprintf(out, cap, "%s/seg%03d.%s.flac", s->paths.dir, seg,
                     s->mode == KARAOKE_SOURCE_SOFT ? "soft" : "inst");
    return (n > 0 && (size_t)n < cap) ? 1 : 0;
}

/* 轮询等待某段就绪（≤timeout_ms，100ms 粒度）。 */
static int seg_wait_ready(SegSource *s, const char *track_path, int seg,
                          int timeout_ms)
{
    extern int g_play_thread_running;
    int waited = 0;
    while (waited < timeout_ms) {
        if (!g_play_thread_running)
            return 0;
        if (seg_source_refresh(s, track_path) && seg_source_ready(s, seg))
            return 1;
        KaraokeWorkerStatus st;
        if (karaoke_worker_status(&st) == 0 && st.track_path[0] &&
            strcmp(st.track_path, track_path) == 0 &&
            (st.state == KARAOKE_JOB_FAILED || st.state == KARAOKE_JOB_CANCELLED))
            return 0;
        usleep(100 * 1000);
        waited += 100;
    }
    return 0;
}

/* D1：首次起播等待首段（target_seg 一般 0；倍速/切档重启在当前位置）。
 * 等待期间状态栏显示"正在准备伴奏"及百分比；失败/超时返回 0（回退原声）。 */
static int seg_wait_first(SegSource *s, const char *track_path, int target_seg,
                          int timeout_ms)
{
    extern int g_play_thread_running;
    core_status_push(i18n_get("karaoke.status.preparing"));
    int waited = 0;
    int last_pct = -1;
    while (waited < timeout_ms) {
        if (!g_play_thread_running)
            return 0;
        if (seg_source_refresh(s, track_path) && seg_source_ready(s, target_seg))
            return 1;
        KaraokeWorkerStatus st;
        int have_status = (karaoke_worker_status(&st) == 0);
        if (have_status && st.track_path[0] &&
            strcmp(st.track_path, track_path) == 0 &&
            (st.state == KARAOKE_JOB_FAILED || st.state == KARAOKE_JOB_CANCELLED))
            return 0;
        if (have_status && st.state == KARAOKE_JOB_RUNNING && st.percent != last_pct) {
            last_pct = st.percent;
            char msg[192];
            snprintf(msg, sizeof(msg), i18n_get("karaoke.status.preparing_fmt"),
                     last_pct);
            core_status_push(msg);
        }
        /* 没有 job 也没有 manifest：说明根本不会产出段（CUE/单声道/超长等），
         * 给 1s 宽限后直接回退，避免无意义等待。 */
        if (waited >= 1000 && (!have_status || st.state == KARAOKE_JOB_IDLE) &&
            !s->valid)
            return 0;
        usleep(100 * 1000);
        waited += 100;
    }
    return 0;
}

/* 段解码上下文：swr 跨段复用（S16 44100 2ch → S32 44100 2ch）。 */
typedef struct {
    SwrContext *swr;
    int atempo_inited;      /* atempo 是否已按 44.1k 段格式初始化 */
} SegDecodeState;

/* 把 seg 段整段解码进池槽 slot。
 * skip_output_frames：seek 时丢弃段首的输出帧（按倍速换算后的帧数）。
 * 返回 1=已解码；0=未就绪/文件不可用；-1=解码失败。 */
static int seg_decode_into_pool(SegSource *src, SegmentPool *pool, int slot, int seg,
                                SegDecodeState *dec, int out_rate, int out_ch,
                                int skip_output_frames, float speed,
                                char *why, size_t why_cap)
{
    char file[KARAOKE_CACHE_PATH_CAP];
    if (!seg_source_file(src, seg, file, sizeof(file))) {
        snprintf(why, why_cap, "段 %d 未就绪", seg);
        return 0;
    }
    AVFormatContext *fmt = NULL;
    AVCodecContext *codec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    AVFrame *filtered = NULL;
    int stream = -1;
    int result = 0;
    if (avformat_open_input(&fmt, file, NULL, NULL) != 0) {
        snprintf(why, why_cap, "无法打开段文件 %.120s", file);
        avformat_close_input(&fmt);
        return 0;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0)
        goto done;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream = (int)i;
            break;
        }
    }
    if (stream < 0)
        goto done;
    {
        AVCodecParameters *par = fmt->streams[stream]->codecpar;
        const AVCodec *c = avcodec_find_decoder(par->codec_id);
        if (!c)
            goto done;
        codec = avcodec_alloc_context3(c);
        if (!codec || avcodec_parameters_to_context(codec, par) < 0 ||
            avcodec_open2(codec, c, NULL) < 0)
            goto done;
    }
    if (!dec->swr) {
        dec->swr = swr_alloc();
        if (!dec->swr)
            goto done;
        AVChannelLayout in_layout, out_layout;
        av_channel_layout_default(&in_layout, 2);
        av_channel_layout_default(&out_layout, 2);
        av_opt_set_chlayout(dec->swr, "in_chlayout", &in_layout, 0);
        av_opt_set_chlayout(dec->swr, "out_chlayout", &out_layout, 0);
        av_opt_set_int(dec->swr, "in_sample_rate", KARAOKE_SEGMENT_SAMPLE_RATE, 0);
        av_opt_set_int(dec->swr, "out_sample_rate", out_rate, 0);
        av_opt_set_sample_fmt(dec->swr, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        av_opt_set_sample_fmt(dec->swr, "out_sample_fmt", AV_SAMPLE_FMT_S32, 0);
        if (swr_init(dec->swr) < 0) {
            swr_free(&dec->swr);
            goto done;
        }
    }
    if (speed != 1.0f && !dec->atempo_inited) {
        /* atempo 输入必须是 44.1k 段格式（不能沿用原曲采样率的滤波器）。 */
        cleanup_atempo_filter();
        if (init_atempo_filter(codec, speed) < 0) {
            snprintf(why, why_cap, "段倍速滤镜初始化失败");
            goto done;
        }
        dec->atempo_inited = 1;
    }
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    filtered = av_frame_alloc();
    if (!pkt || !frame || !filtered) {
        snprintf(why, why_cap, "段解码缓冲分配失败");
        goto done;
    }
    {
        Segment *seg_slot = &pool->slots[slot];
        seg_slot->frame_count = 0;
        seg_slot->consumed_frames = 0;
        seg_slot->is_valid = 0;
        seg_slot->segment_id = seg;
        int target = segment_pool_capacity_frames(pool);
        int draining = 0, finished = 0;
        int rc = decode_segment_fill(fmt, codec, dec->swr, pkt, frame, filtered,
                                     pool, slot, target, 0, stream, out_rate,
                                     out_ch, 1, &draining, &finished);
        if (rc < 0) {
            snprintf(why, why_cap, "段 %d 解码失败", seg);
            goto done;
        }
        if (!finished) {
            /* 段文件输出超过池容量：按容量取前一部分会丢帧，宁可回退。 */
            snprintf(why, why_cap, "段 %d 超出池容量", seg);
            seg_slot->frame_count = 0;
            goto done;
        }
        if (seg_slot->frame_count <= 0) {
            snprintf(why, why_cap, "段 %d 空", seg);
            goto done;
        }
        if (skip_output_frames > 0) {
            if (skip_output_frames >= seg_slot->frame_count) {
                seg_slot->frame_count = 0;
                snprintf(why, why_cap, "段 %d seek 偏移越界", seg);
                goto done;
            }
            memmove(seg_slot->data,
                    seg_slot->data + (size_t)skip_output_frames * out_ch,
                    (size_t)(seg_slot->frame_count - skip_output_frames) * out_ch *
                        sizeof(int32_t));
            seg_slot->frame_count -= skip_output_frames;
        }
        result = 1;
    }
done:
    av_frame_free(&filtered);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec);
    avformat_close_input(&fmt);
    return result;
}

/* R1：回退到原声时优先把 worker 对该曲目的具体失败原因带到状态行
 * （单声道/超 30 分钟/模型缺失/缓存不可写等）；没有原因才用笼统文案。 */
static void seg_push_fallback_status(const char *track_path, int position)
{
    KaraokeWorkerStatus st;
    memset(&st, 0, sizeof(st));
    const char *reason = NULL;
    if (track_path && track_path[0] && karaoke_worker_status(&st) == 0 &&
        st.track_path[0] && strcmp(st.track_path, track_path) == 0 &&
        st.error[0])
        reason = st.error;
    if (reason) {
        char msg[KARAOKE_WORKER_ERR_CAP + 128];
        snprintf(msg, sizeof(msg), i18n_get("karaoke.status.failed_fmt"), reason);
        core_status_push(msg);
        log_info("audio", "karaoke segment fallback to original at %ds: %s",
                 position, reason);
    } else {
        core_status_push(i18n_get("karaoke.status.fallback_original"));
        log_info("audio", "karaoke segment fallback to original at %ds", position);
    }
}

/* seek/段未就绪时回退原声：从 position 秒继续，后续整曲都走原声解码。 */
static void seg_fallback_to_original(int position, const char *track_path,
                                     int *seg_mode, int *source_kind)
{
    extern int g_seek_request;
    extern int g_seek_position;
    extern pthread_mutex_t g_seek_mutex;
    extern int g_karaoke_playback_source;
    pthread_mutex_lock(&g_seek_mutex);
    g_seek_position = position;
    g_seek_request = 1;
    pthread_mutex_unlock(&g_seek_mutex);
    *seg_mode = 0;
    *source_kind = KARAOKE_SOURCE_ORIGINAL;
    pthread_mutex_lock(&g_play_mutex);
    g_karaoke_playback_source = KARAOKE_SOURCE_ORIGINAL;
    pthread_mutex_unlock(&g_play_mutex);
    seg_push_fallback_status(track_path, position);
}

/* v2 分段源 seek（D4）：优先提示 worker，等待 ≤8s；就绪则从目标段精确续播，
 * 否则回退原声从目标位置继续。返回 1=已处理 seek。 */
static int handle_seek_request_segmented(AVFormatContext *fmt_ctx,
                                         AVCodecContext *codec_ctx,
                                         SwrContext *swr_ctx,
                                         AVPacket *packet,
                                         AVFrame *frame,
                                         AVFrame *filtered_frame,
                                         SegmentPool *pool,
                                         int audio_stream_index,
                                         int output_sample_rate,
                                         int output_channels,
                                         int use_resampler,
                                         int *decoder_draining,
                                         int *decoder_finished,
                                         int cue_offset,
                                         const char *track_path,
                                         SegSource *src,
                                         SegDecodeState *dec,
                                         int *seg_mode,
                                         int *source_kind)
{
    extern pthread_mutex_t g_seek_mutex;
    extern int g_seek_request;
    extern int g_seek_position;
    extern int g_current_position;
    extern float g_playback_speed;

    int target_position = -1;
    pthread_mutex_lock(&g_seek_mutex);
    if (g_seek_request) {
        target_position = g_seek_position;
        g_seek_request = 0;
    }
    pthread_mutex_unlock(&g_seek_mutex);
    if (target_position < 0)
        return 0;

    int target_seg = segment_id_from_position(target_position);
    if (target_seg < 0)
        target_seg = 0;
    (void)karaoke_worker_hint_segment(track_path, target_seg);
    (void)seg_source_refresh(src, track_path);
    if (!seg_source_ready(src, target_seg)) {
        core_status_push(i18n_get("karaoke.status.segment_wait"));
        log_info("audio", "karaoke seek to seg %d not ready, waiting up to 8s", target_seg);
        if (!seg_wait_ready(src, track_path, target_seg, 8000)) {
            log_info("audio", "karaoke seek seg %d not ready, fallback to original", target_seg);
            seg_fallback_to_original(target_position, track_path, seg_mode,
                                 source_kind);
            return handle_seek_request_in_decoder(fmt_ctx, codec_ctx, swr_ctx, packet,
                                                  frame, filtered_frame, pool,
                                                  audio_stream_index,
                                                  output_sample_rate, output_channels,
                                                  use_resampler, decoder_draining,
                                                  decoder_finished, cue_offset);
        }
    }
    int offset_sec = target_position - target_seg * SEGMENT_DURATION_SEC;
    int skip = 0;
    if (offset_sec > 0)
        skip = (int)((double)offset_sec * output_sample_rate *
                     (double)g_playback_speed + 0.5);
    int saved_total = pool->total_segments;
    segment_pool_seek_to(pool, target_seg);
    pool->total_segments = saved_total;
    char why[192] = {0};
    /* seek 后 atempo 旧位置历史必须丢弃：下次段解码按 44.1k 段格式重建。 */
    if (atempo_is_active()) {
        cleanup_atempo_filter();
        dec->atempo_inited = 0;
    }
    int rc = seg_decode_into_pool(src, pool, pool->current_slot, target_seg, dec,
                                  output_sample_rate, output_channels, skip,
                                  g_playback_speed, why, sizeof(why));
    if (rc != 1) {
        log_warn("audio", "karaoke seek seg %d decode failed (%s), fallback to original",
                 target_seg, why);
        seg_fallback_to_original(target_position, track_path, seg_mode,
                                 source_kind);
        return handle_seek_request_in_decoder(fmt_ctx, codec_ctx, swr_ctx, packet,
                                              frame, filtered_frame, pool,
                                              audio_stream_index,
                                              output_sample_rate, output_channels,
                                              use_resampler, decoder_draining,
                                              decoder_finished, cue_offset);
    }
    Segment *cur = &pool->slots[pool->current_slot];
    int is_last = (src->complete && target_seg >= pool->total_segments - 1);
    segment_pool_mark_ready(pool, pool->current_slot, cur->frame_count, target_seg,
                            is_last);
    audio_backend_flush_stream();
    progress_tracker_seek(target_position);
    g_current_position = target_position;
    char msg[64];
    snprintf(msg, sizeof(msg), i18n_get("status.seek_fmt"),
             target_position / 60, target_position % 60);
    core_status_push(msg);
    return 1;
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
    extern int g_play_thread_active;
    extern int g_play_thread_finished;
    extern int g_cue_offset;

    log_info("audio", "Playback thread started for index=%d", index);

    /* Capture and clear CUE offset for this track */
    int cue_offset = g_cue_offset;
    g_cue_offset = 0;

    pthread_mutex_lock(&g_play_mutex);
    int thread_running = g_play_thread_running;
    pthread_mutex_unlock(&g_play_mutex);

    char file_path[MAX_PATH_LEN];
    int valid_index = bq_path_at(index, file_path, sizeof(file_path)) == 0;

    if (!valid_index || !thread_running) {
        log_warn("audio", "Playback thread: invalid index=%d or thread not running", index);
        pthread_mutex_lock(&g_play_mutex);
        g_play_thread_running = 0;
        g_play_thread_finished = 1;
        pthread_mutex_unlock(&g_play_mutex);
        return NULL;
    }
    log_debug("audio", "Playback thread file_path='%s'", file_path);

    /* ── 播放源选择（只在曲目起点做一次，绝不曲中切换）──
     * off=原声；soft/full=对应缓存（命中时）。缓存打开失败会回退原声并删除
     * 坏缓存（见下面的 avformat_open_input 失败分支）。 */
    char decode_path[MAX_PATH_LEN];
    snprintf(decode_path, sizeof(decode_path), "%s", file_path);
    int source_kind = KARAOKE_SOURCE_ORIGINAL;
    KaraokeCacheConfig cache_cfg;
    int cache_cfg_ok = 0;
    int v1_source = 0;
    int seg_candidate = 0;         /* 可能走 v2 分段源（等首段就绪） */
    int seg_mode = 0;              /* 实际使用 v2 分段源 */
    SegSource seg_src;
    memset(&seg_src, 0, sizeof(seg_src));
    SegDecodeState seg_dec;
    memset(&seg_dec, 0, sizeof(seg_dec));
    {
        char cache_root[MAX_PATH_LEN];
        char model_path[MAX_PATH_LEN];
        int mode = karaoke_get_mode();
        if (mode != KARAOKE_MODE_OFF &&
            audio_karaoke_cache_config(&cache_cfg, cache_root, sizeof(cache_root),
                                       model_path, sizeof(model_path))) {
            cache_cfg_ok = 1;
            char resolved[MAX_PATH_LEN];
            if (karaoke_cache_resolve(&cache_cfg, file_path, mode, resolved,
                                      sizeof(resolved), NULL, 0) == 1) {
                /* v1 整曲缓存优先（只读兼容，行为不变，不产生 /tmp 分段）。 */
                snprintf(decode_path, sizeof(decode_path), "%s", resolved);
                source_kind = mode;
                v1_source = 1;
            } else {
                /* v2 分段候选：段文件在 /tmp 会话根下，由 worker 产出。 */
                char session_root[KARAOKE_CACHE_PATH_CAP];
                if (karaoke_cache_session_root(session_root, sizeof(session_root)) ==
                    0) {
                    KaraokeCacheConfig seg_cfg = cache_cfg;
                    seg_cfg.root = session_root;
                    seg_cfg.limit_mb = 0;
                    seg_source_setup(&seg_src, &seg_cfg, mode);
                    seg_candidate = 1;
                    source_kind = mode;
                }
            }
        }
    }
    /* CUE 子曲目不用 v2 分段源（段坐标是物理文件从 0 开始的内容轴）。 */
    if (cue_offset > 0)
        seg_candidate = 0;
    if (!v1_source)
        source_kind = KARAOKE_SOURCE_ORIGINAL;   /* v2 决策在首段等待之后 */

    /* ── FFmpeg context ── */
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    SwrContext *swr_ctx = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    AVFrame *filtered_frame = NULL;
    int audio_stream_index = -1;
    int input_channels = 2;
    int output_channels = 2;
    int output_sample_rate = 44100;
    int use_resampler = 1;
    int decoder_draining = 0;
    int decoder_finished = 0;
    int playback_error = 0;
    g_decode_read_errors = 0;   /* 每首曲子独立的读错误熔断计数 */

    /* ── Segment pool ── */
    SegmentPool seg_pool;
    memset(&seg_pool, 0, sizeof(seg_pool));
    int pool_initialized = 0;
    int preload_attempted = 0;

    if (avformat_open_input(&fmt_ctx, decode_path, NULL, NULL) != 0) {
        if (source_kind != KARAOKE_SOURCE_ORIGINAL) {
            /* 缓存损坏/打不开：删除坏缓存，回退原声（同一曲目、不打断起播）。 */
            log_warn("audio", "karaoke cache open failed, falling back to original: %s",
                     decode_path);
            if (cache_cfg_ok)
                (void)karaoke_cache_remove(&cache_cfg, file_path);
            snprintf(decode_path, sizeof(decode_path), "%s", file_path);
            source_kind = KARAOKE_SOURCE_ORIGINAL;
            pthread_mutex_lock(&g_play_mutex);
            g_karaoke_playback_source = KARAOKE_SOURCE_ORIGINAL;
            pthread_mutex_unlock(&g_play_mutex);
        }
    }
    if (fmt_ctx == NULL && avformat_open_input(&fmt_ctx, decode_path, NULL, NULL) != 0) {
        log_error("audio", "avformat_open_input failed for '%s' (index=%d)", decode_path, index);
        core_status_push(i18n_get("audio.err.cannot_open_file"));
        goto cleanup;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        log_error("audio", "avformat_find_stream_info failed for '%s'", file_path);
        core_status_push(i18n_get("audio.err.cannot_read_stream"));
        goto cleanup;
    }

    /* ── Determine track duration ──
     * Prefer stream duration (based on total samples from STREAMINFO for FLAC,
     * or decoded frame count for other formats) over container-level duration.
     * Container duration can be inflated by attached pictures, trailing
     * metadata, or bitrate-based estimation for VBR formats. */
    {
        int64_t container_duration_sec = fmt_ctx->duration / AV_TIME_BASE;
        int64_t stream_duration_sec = 0;

        /* Try stream-level duration first — more accurate for most formats */
        for (int i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                AVRational time_base = fmt_ctx->streams[i]->time_base;
                int64_t stream_dur = fmt_ctx->streams[i]->duration;
                if (stream_dur > 0 && time_base.den > 0) {
                    stream_duration_sec = av_rescale_q(stream_dur, time_base, (AVRational){1, 1});
                }
                break;
            }
        }

        if (stream_duration_sec > 0) {
            g_total_duration = (int)stream_duration_sec;
            if (container_duration_sec > 0) {
                int64_t diff = llabs(container_duration_sec - stream_duration_sec);
                if (diff > 5) {
                    log_info("audio", "Duration: container=%llds stream=%llds (diff=%llds) — using stream duration",
                             (long long)container_duration_sec, (long long)stream_duration_sec, (long long)diff);
                }
            }
        } else if (container_duration_sec > 0) {
            g_total_duration = (int)container_duration_sec;
        } else {
            g_total_duration = 300;
        }
    }

    /* Cap duration for CUE sub-tracks.
     * 旧实现靠内容列表的 `cue_find_next_offset(index)` 找同一物理文件里的
     * 下一个子轨；后端不认识内容列表，改为在**后端路径队列**里向后找同一
     * 路径且偏移更大的条目（执行顺序即队列顺序，与扫描顺序一致）。 */
    if (cue_offset > 0) {
        int next_offset = -1;
        for (int i = index + 1; i < bq_count(); i++) {
            BackendQueueEntry candidate;
            if (bq_entry_at(i, &candidate) != 0) {
                break;
            }
            if (strcmp(candidate.path, file_path) != 0) {
                continue;
            }
            if (candidate.cue_offset > cue_offset) {
                next_offset = candidate.cue_offset;
                break;
            }
        }
        int capped;
        if (next_offset > cue_offset) {
            capped = next_offset - cue_offset;
        } else {
            capped = g_total_duration - cue_offset;
        }
        if (capped > 0 && capped < g_total_duration)
            g_total_duration = capped;
    }

    g_current_position = 0;
    reset_visualizer_state();

    int initial_seek_position = g_initial_seek_position;
    g_initial_seek_position = 0;

    core_notify_state_changed();

    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }
    if (audio_stream_index == -1) {
        log_error("audio", "No audio stream found in '%s'", file_path);
        core_status_push(i18n_get("audio.err.no_stream"));
        goto cleanup;
    }

    AVCodecParameters *codec_par = fmt_ctx->streams[audio_stream_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        log_error("audio", "Unsupported codec in '%s' (codec_id=%d)", file_path, codec_par->codec_id);
        core_status_push(i18n_get("audio.err.unsupported_codec"));
        goto cleanup;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) { core_status_push(i18n_get("audio.err.alloc_codec_ctx")); goto cleanup; }
    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) { core_status_push(i18n_get("audio.err.copy_codec_params")); goto cleanup; }
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) { core_status_push(i18n_get("audio.err.open_codec")); goto cleanup; }

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

    /* ── v2 分段源决策（D1）：无 v1 缓存时等待首段就绪再起播 ──
     * 单声道 / >30min / CUE 子曲目保持原声（与 worker 的拒绝条件一致）。 */
    if (seg_candidate) {
        if (input_channels != 2 || g_total_duration <= 0 ||
            g_total_duration > KARAOKE_WORKER_MAX_SECONDS) {
            log_info("audio",
                     "karaoke segments skipped: channels=%d duration=%ds (keep original)",
                     input_channels, g_total_duration);
            seg_candidate = 0;
        }
    }
    if (seg_candidate) {
        int wait_target = initial_seek_position > 0
                              ? initial_seek_position / SEGMENT_DURATION_SEC
                              : 0;
        int wait_ms = initial_seek_position > 0 ? 8000 : 30000;
        if (seg_wait_first(&seg_src, file_path, wait_target, wait_ms)) {
            seg_mode = 1;
            source_kind = seg_src.mode;
            output_sample_rate = KARAOKE_SEGMENT_SAMPLE_RATE;
            output_channels = KARAOKE_SEGMENT_CHANNELS;
            log_info("audio", "karaoke segmented source ready (seg=%d) for '%s'",
                     wait_target, file_path);
        } else {
            seg_mode = 0;
            source_kind = KARAOKE_SOURCE_ORIGINAL;
            /* R1：首段等待失败（worker FAILED/超时）同样带出具体原因。 */
            seg_push_fallback_status(file_path, 0);
            log_info("audio",
                     "karaoke segments not ready (timeout/failed), keep original: '%s'",
                     file_path);
        }
    }
    {
        int mode_now = seg_mode ? seg_src.mode : source_kind;
        pthread_mutex_lock(&g_play_mutex);
        g_karaoke_playback_source = mode_now;
        pthread_mutex_unlock(&g_play_mutex);
        if (mode_now != KARAOKE_SOURCE_ORIGINAL) {
            log_info("audio", "playing karaoke cache source=%d%s for '%s'", mode_now,
                     seg_mode ? " (segmented)" : "", file_path);
        } else {
            /* 以原声起播的取证点（E2E 断言"off 回原声 / 缓存未就绪回退"用；
             * 只在 debug 日志里多一行，不改变播放行为）。config v12 起没有
             * feature 开关，判据就是档位本身：mode=0 表示确为 Off 档。 */
            log_info("audio", "playing original audio (karaoke mode=%d, source=original)",
                     karaoke_get_mode());
        }
    }

    log_debug("audio", "Audio stream: rate=%d, channels=%d, codec=%s, duration=%ds",
              output_sample_rate, output_channels, codec ? codec->name : "unknown", g_total_duration);

    use_resampler = (codec_ctx->sample_fmt != AV_SAMPLE_FMT_S32 || input_channels != output_channels);

    /* ── Segment pool init ── */
    if (segment_pool_init(&seg_pool, output_sample_rate, output_channels) < 0) {
        core_status_push(i18n_get("audio.err.alloc_segment_buf"));
        goto cleanup;
    }
    pool_initialized = 1;

    /* Set total segments for this track */
    seg_pool.total_segments = segment_pool_total_for_duration(g_total_duration);
    if (seg_mode && seg_source_refresh(&seg_src, file_path) && seg_src.complete &&
        seg_src.total > 0)
        seg_pool.total_segments = seg_src.total;
    log_debug("audio", "Total segments for track: %d (%d sec, seg_mode=%d)",
              seg_pool.total_segments, g_total_duration, seg_mode);

    if (use_resampler) {
        swr_ctx = swr_alloc();
        if (!swr_ctx) { core_status_push(i18n_get("audio.err.alloc_resampler")); goto cleanup; }
        if (init_resampler(swr_ctx, codec_ctx, input_channels, output_channels, output_sample_rate) < 0) {
            core_status_push(i18n_get("audio.err.init_resampler")); goto cleanup;
        }
    }

    if (seg_mode) {
        /* v2 段是 44.1k 输入：atempo 由首次段解码按段格式惰性初始化。 */
        cleanup_atempo_filter();
        seg_dec.atempo_inited = 0;
    } else if (init_atempo_filter(codec_ctx, g_playback_speed) < 0) {
        core_status_push(i18n_get("audio.err.init_speed_filter")); goto cleanup;
    }

    /* 每首曲子起播都从干净状态开始消人声：带着上一曲的延迟线出声
     * 会在前几帧冒出可闻的瞬态。 */

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    filtered_frame = av_frame_alloc();
    if (!packet || !frame || !filtered_frame) {
        core_status_push(i18n_get("audio.err.alloc_decode"));
        goto cleanup;
    }

    if (audio_backend_prepare_stream(output_sample_rate, output_channels) < 0) goto cleanup;

    audio_backend_resume_stream();
    audio_backend_sync_volume(1);

    progress_tracker_init(output_sample_rate);
    progress_tracker_set_sample_rate(output_sample_rate);
    progress_tracker_set_speed(g_playback_speed);
    progress_tracker_start();

    g_play_state = PLAY_STATE_PLAYING;

    /* ── Check if preloaded data is available for this track ── */
    /* Only use preload at 1.0x speed (preloaded PCM is not atempo-adjusted) */
    int used_preload = 0;
    pthread_mutex_lock(&g_preload_data.lock);
    if (!seg_mode && g_preload_data.valid && g_preload_data.track_index == index &&
        g_preload_source_kind == source_kind &&
        g_preload_data.sample_rate == output_sample_rate &&
        g_preload_data.channels == output_channels &&
        g_playback_speed >= 0.99f && g_playback_speed <= 1.01f) {
        /* Copy preloaded segments into pool */
        int n = g_preload_data.segment_count;
        if (n > SEGMENT_POOL_SIZE) n = SEGMENT_POOL_SIZE;

        for (int s = 0; s < n; s++) {
            Segment *src = &g_preload_data.segments[s];
            Segment *dst = &seg_pool.slots[s];
            if (src->frame_count > dst->capacity_frames)
                src->frame_count = dst->capacity_frames;
            memcpy(dst->data, src->data, (size_t)src->frame_count * output_channels * sizeof(int32_t));
            dst->frame_count = src->frame_count;
            dst->consumed_frames = 0;
            dst->segment_id = s;
            /* is_last: this is the final segment if it is the last
             * preloaded slot AND it covers the end of the track.
             * Phase 3 only decodes free slots (> current), so slot 0's
             * is_last would never be corrected if left at 0 here. */
            dst->is_last = (s == n - 1) && (s >= seg_pool.total_segments - 1);
            dst->is_valid = (src->frame_count > 0);
        }
        seg_pool.current_slot = 0;
        seg_pool.current_segment_id = 0;
        used_preload = (n > 0);

        log_info("audio", "Using preloaded data for index=%d (%d segments)", index, n);
        g_preload_data.valid = 0;
        preload_data_reset(&g_preload_data);
    } else if (g_preload_data.valid) {
        /* Preload data exists but was rejected — log why for diagnostics */
        if (g_preload_data.track_index != index)
            log_debug("audio", "Preload skipped: track_index mismatch (preloaded=%d, current=%d)",
                      g_preload_data.track_index, index);
        else if (g_preload_data.sample_rate != output_sample_rate ||
                 g_preload_data.channels != output_channels)
            log_debug("audio", "Preload skipped: format mismatch (sr=%d/%d, ch=%d/%d)",
                      g_preload_data.sample_rate, output_sample_rate,
                      g_preload_data.channels, output_channels);
        else if (g_playback_speed < 0.99f || g_playback_speed > 1.01f)
            log_debug("audio", "Preload skipped: speed=%.2f (need ~1.0)", (double)g_playback_speed);
    }
    pthread_mutex_unlock(&g_preload_data.lock);

    /* If preloaded data was consumed, advance the decoder past the preloaded
     * frames so Phase 3 doesn't re-decode the same audio from position 0.
     * Without this, the fresh decoder (opened at line 649) is at position 0
     * and would re-decode the first 15 seconds into slots 1 and 2. */
    if (used_preload) {
        int preloaded_frames = 0;
        for (int s = 0; s < SEGMENT_POOL_SIZE && seg_pool.slots[s].is_valid; s++)
            preloaded_frames += seg_pool.slots[s].frame_count;

        if (preloaded_frames > 0) {
            int skip_seconds = preloaded_frames / output_sample_rate;
            if (skip_seconds > 0 && skip_seconds < g_total_duration) {
                AVRational tb = fmt_ctx->streams[audio_stream_index]->time_base;
                int64_t ts = av_rescale_q(skip_seconds, (AVRational){1, 1}, tb);
                int seek_ret = av_seek_frame(fmt_ctx, audio_stream_index, ts, AVSEEK_FLAG_ANY);
                if (seek_ret >= 0) {
                    avcodec_flush_buffers(codec_ctx);
                    if (swr_ctx) swr_init(swr_ctx);
                    if (packet) av_packet_unref(packet);
                    decoder_draining = 0;
                    decoder_finished = 0;
                    log_debug("audio", "Advanced decoder past preloaded %d frames (%ds) for index=%d",
                              preloaded_frames, skip_seconds, index);
                } else {
                    log_warn("audio", "Failed to advance decoder past preloaded %ds for index=%d",
                             skip_seconds, index);
                }
            }
        }
    }

    /* ── If no preload, decode first segment(s) normally ── */
    /* For CUE tracks (cue_offset > 0): skip position-0 decode entirely;
     * the CUE seek block below will seek to the correct offset and decode. */
    if (seg_mode && !used_preload && cue_offset == 0 && initial_seek_position <= 0) {
        /* v2：解码段 0 进池（首段等待已保证就绪；极小窗口再补 1s）。 */
        char why[192] = {0};
        if (!seg_source_refresh(&seg_src, file_path) || !seg_source_ready(&seg_src, 0))
            (void)seg_wait_ready(&seg_src, file_path, 0, 1000);
        int rc = seg_decode_into_pool(&seg_src, &seg_pool, seg_pool.current_slot, 0,
                                      &seg_dec, output_sample_rate, output_channels,
                                      0, g_playback_speed, why, sizeof(why));
        if (rc == 1) {
            segment_pool_mark_ready(&seg_pool, seg_pool.current_slot,
                                    seg_pool.slots[seg_pool.current_slot].frame_count,
                                    0, seg_pool.total_segments <= 1);
            seg_pool.current_slot = 0;
            seg_pool.current_segment_id = 0;
        } else {
            log_warn("audio",
                     "karaoke segment 0 decode failed (%s), falling back to original",
                     why);
            seg_fallback_to_original(0, file_path, &seg_mode, &source_kind);
        }
    } else if (!used_preload && cue_offset == 0) {
        int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
        for (int s = 0; s < SEGMENT_POOL_SIZE && !decoder_finished; s++) {
            int seg_id = s;
            if (seg_id >= seg_pool.total_segments) break;

            int ret = decode_segment_fill(fmt_ctx, codec_ctx, swr_ctx,
                                          packet, frame, filtered_frame,
                                          &seg_pool, s, target_frames, 0 /*起播前整段*/,
                                          audio_stream_index, output_sample_rate,
                                          output_channels, use_resampler,
                                          &decoder_draining, &decoder_finished);
            if (ret < 0) { playback_error = 1; break; }

            segment_pool_mark_ready(&seg_pool, s,
                                    seg_pool.slots[s].frame_count, s,
                                    (decoder_finished || s == seg_pool.total_segments - 1));
            if (ret == 0 && seg_pool.slots[s].frame_count == 0) break;
        }
        seg_pool.current_slot = 0;
        seg_pool.current_segment_id = 0;
    } else if (!used_preload && cue_offset > 0) {
        /* CUE track: pool was initialised in segment_pool_init(), slots are empty.
         * current_slot/current_segment_id are already 0 from init.
         * The CUE seek below will seek and decode the first segment. */
        log_debug("audio", "CUE track index=%d offset=%ds — skipping position-0 decode, will seek directly", index, cue_offset);
    }

    /* Handle initial seek (e.g. from speed change restart) */
    /* Handle initial seek (speed-change restart, etc.) — relative position */
    if (initial_seek_position > 0 && initial_seek_position < g_total_duration) {
        g_seek_position = initial_seek_position;
        g_seek_request = 1;
    }

    /* Handle initial CUE offset seek — seek to absolute file position */
    /* Note: if g_seek_request is set (initial_seek_position > 0), the CUE seek is
     * deferred to Phase 2's handle_seek_request_in_decoder(), which also adds cue_offset.
     * This ensures CUE offset is always applied, whether via direct seek or via Phase 2. */
    if (cue_offset > 0 && !g_seek_request) {
        AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;
        int64_t target_ts = av_rescale_q(cue_offset, (AVRational){1, 1}, time_base);
        int ret = av_seek_frame(fmt_ctx, audio_stream_index, target_ts, 0);
        if (ret >= 0) {
            avcodec_flush_buffers(codec_ctx);
            if (swr_ctx) swr_init(swr_ctx);
            if (packet) av_packet_unref(packet);

            /* Reset segment pool */
            int seg_id = segment_id_from_position(0);
            segment_pool_seek_to(&seg_pool, seg_id);
            seg_pool.total_segments = segment_pool_total_for_duration(g_total_duration);

            /* Reset atempo filter */
            if (atempo_is_active()) {
                float speed = atempo_get_speed();
                cleanup_atempo_filter();
                if (speed > 0.01f)
                    init_atempo_filter(codec_ctx, speed);
            }

            /* 回到开头重新起播：清掉消人声滤波器的上一轮历史 */

            audio_backend_flush_stream();
            g_current_position = 0;
            progress_tracker_seek(0);
            decoder_draining = 0;
            decoder_finished = 0;

            /* Re-decode first segment from seeked position */
            int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
            int fill_ret = decode_segment_fill(fmt_ctx, codec_ctx, swr_ctx, packet, frame, filtered_frame,
                                &seg_pool, seg_pool.current_slot, target_frames, 0 /*起播前整段*/,
                                audio_stream_index, output_sample_rate, output_channels,
                                use_resampler, &decoder_draining, &decoder_finished);
            if (fill_ret < 0) {
                log_error("audio", "CUE seek: decode_segment_fill failed for slot %d — will retry in main loop",
                          seg_pool.current_slot);
                /* Don't mark ready; main loop Phase 3 will fill the slot */
            } else {
                segment_pool_mark_ready(&seg_pool, seg_pool.current_slot,
                                        seg_pool.slots[seg_pool.current_slot].frame_count,
                                        seg_id,
                                        (decoder_finished || seg_id >= seg_pool.total_segments - 1));
            }

            log_debug("audio", "CUE offset seek to %ds (index=%d) — decoded segment=%d frames, finished=%d",
                      cue_offset, index, seg_pool.slots[seg_pool.current_slot].frame_count, decoder_finished);
        } else {
            log_warn("audio", "CUE offset seek failed for position %d", cue_offset);
        }
    }

    /* ════════════════════════════════════════════════════════
     * Main playback loop
     * ════════════════════════════════════════════════════════ */

    while (g_play_thread_running) {
        audio_backend_sync_volume(0);

        /* ── Phase 1: Pause check ── */
        if (!wait_while_paused()) break;

        /* ── Phase 2: Seek check ── */
        int seek_handled = 0;
        if (seg_mode) {
            seek_handled = handle_seek_request_segmented(
                fmt_ctx, codec_ctx, swr_ctx, packet, frame, filtered_frame,
                &seg_pool, audio_stream_index, output_sample_rate, output_channels,
                use_resampler, &decoder_draining, &decoder_finished, cue_offset,
                file_path, &seg_src, &seg_dec, &seg_mode, &source_kind);
        } else {
            seek_handled = handle_seek_request_in_decoder(
                fmt_ctx, codec_ctx, swr_ctx, packet, frame, filtered_frame,
                &seg_pool, audio_stream_index, output_sample_rate, output_channels,
                use_resampler, &decoder_draining, &decoder_finished, cue_offset);
        }
        if (seek_handled) {
            if (g_current_position >= g_total_duration) { g_play_thread_running = 0; break; }
            continue;
        }

        /* ── Phase 3: Fill empty slots ── */
        if (seg_mode && !playback_error) {
            /* v2：从段文件整段解码进池槽（未就绪就留空，后续等待/回退）。 */
            int free_slot;
            while ((free_slot = segment_pool_free_slot(&seg_pool)) >= 0) {
                Segment *fs = &seg_pool.slots[free_slot];
                int offset = (free_slot - seg_pool.current_slot + SEGMENT_POOL_SIZE) %
                             SEGMENT_POOL_SIZE;
                int want_seg = seg_pool.current_segment_id + offset;
                if (fs->segment_id < 0)
                    fs->segment_id = want_seg;
                int global_seg = fs->segment_id;
                if (global_seg != want_seg) {
                    if (global_seg < seg_pool.current_segment_id) {
                        fs->frame_count = 0;
                        fs->consumed_frames = 0;
                        fs->segment_id = global_seg = want_seg;
                    } else {
                        break;
                    }
                }
                if (global_seg >= seg_pool.total_segments) break;
                if (!seg_src.valid)
                    (void)seg_source_refresh(&seg_src, file_path);
                char why[160] = {0};
                int rc = seg_decode_into_pool(&seg_src, &seg_pool, free_slot,
                                              global_seg, &seg_dec,
                                              output_sample_rate, output_channels, 0,
                                              g_playback_speed, why, sizeof(why));
                if (rc != 1)
                    break;   /* 段未就绪/损坏：留给 Phase 4/5 等待或回退 */
                segment_pool_mark_ready(
                    &seg_pool, free_slot, fs->frame_count, global_seg,
                    (seg_src.complete && global_seg >= seg_pool.total_segments - 1));
            }
        } else if (!playback_error) {
            int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
            int free_slot;
            while ((free_slot = segment_pool_free_slot(&seg_pool)) >= 0 && !decoder_finished) {
                Segment *fs = &seg_pool.slots[free_slot];
                /* 该槽负责的全局段号 = current_segment_id + 距 current 的偏移；
                 * 窗口推进时 current 与 offset 同变，段号不变。半截槽先“认领”
                 * 段号再续填，避免下一轮被算成别的段。 */
                int offset = (free_slot - seg_pool.current_slot + SEGMENT_POOL_SIZE) % SEGMENT_POOL_SIZE;
                int want_seg = seg_pool.current_segment_id + offset;
                if (fs->segment_id < 0)
                    fs->segment_id = want_seg;           /* 认领 */
                int global_seg = fs->segment_id;
                if (global_seg != want_seg) {
                    if (global_seg < seg_pool.current_segment_id) {
                        /* 填的是已被跳过的旧段：作废重来（病态场景，正常不发生） */
                        fs->frame_count = 0;
                        fs->consumed_frames = 0;
                        fs->segment_id = global_seg = want_seg;
                    } else {
                        break;
                    }
                }
                if (global_seg >= seg_pool.total_segments) break;

                /* 当前槽必须整段就绪才能开播（本来就没数据可写，填满为止）；
                 * 远处槽分片填，每轮主循环一片，段界不再出现整段解码的阻塞。 */
                int is_current = (free_slot == seg_pool.current_slot);
                int slice = is_current ? 0 : DECODE_SLICE_FRAMES;

                int ret = decode_segment_fill(fmt_ctx, codec_ctx, swr_ctx,
                                              packet, frame, filtered_frame,
                                              &seg_pool, free_slot, target_frames, slice,
                                              audio_stream_index, output_sample_rate,
                                              output_channels, use_resampler,
                                              &decoder_draining, &decoder_finished);
                if (ret < 0) { playback_error = 1; break; }

                int complete = (fs->frame_count >= target_frames) || decoder_finished;
                if (complete) {
                    segment_pool_mark_ready(&seg_pool, free_slot,
                                            fs->frame_count,
                                            global_seg,
                                            (decoder_finished || global_seg >= seg_pool.total_segments - 1));
                }
                if (ret == 0 && fs->frame_count == 0) break;
                /* 远槽这一片没填满：本轮到此为止，下一轮从断点继续 */
                if (!is_current && !complete) break;
            }
        }

        if (playback_error) break;

        /* ── Phase 4: Advance if current segment is done ── */
        if (segment_pool_current_done(&seg_pool)) {
            if (seg_mode) {
                /* 当前段无效 = 该段还没解码进池（切档重启/seek 之后常见：
                 * 池里只有当前段，后面的段 worker 还没产出）。旧实现把
                 * "无效当前段"当成 done 一路 advance（静默跳段），到段数
                 * 用尽就直接 EOS，播放会提前结束。按 D3 语义处理：提示
                 * worker 优先当前段、最多等 8s，就绪则解码继续；仍不就绪
                 * 则从该位置回退原声继续（后续由 audio.c 的 re-acquire 恢复）。 */
                if (!segment_pool_current(&seg_pool)) {
                    int seg = seg_pool.current_segment_id;
                    if (seg * SEGMENT_DURATION_SEC >= g_total_duration) {
                        reached_end_of_stream = 1;
                        break;
                    }
                    (void)karaoke_worker_hint_segment(file_path, seg);
                    core_status_push(i18n_get("karaoke.status.segment_wait"));
                    int okw = seg_wait_ready(&seg_src, file_path, seg, 8000);
                    if (okw) {
                        char why[160] = {0};
                        int rc = seg_decode_into_pool(&seg_src, &seg_pool,
                                                      seg_pool.current_slot, seg,
                                                      &seg_dec, output_sample_rate,
                                                      output_channels, 0,
                                                      g_playback_speed, why,
                                                      sizeof(why));
                        if (rc == 1) {
                            Segment *ns = &seg_pool.slots[seg_pool.current_slot];
                            segment_pool_mark_ready(
                                &seg_pool, seg_pool.current_slot,
                                ns->frame_count, seg,
                                (seg_src.complete &&
                                 seg >= seg_pool.total_segments - 1));
                            continue;
                        }
                        log_warn("audio", "karaoke segment %d decode failed (%s)",
                                 seg, why);
                    }
                    log_info("audio",
                             "karaoke segment %d not ready after wait, fallback original",
                             seg);
                    seg_fallback_to_original(seg * SEGMENT_DURATION_SEC, file_path,
                                             &seg_mode, &source_kind);
                    continue;
                }
                (void)seg_source_refresh(&seg_src, file_path);
                if (seg_src.complete && seg_src.total > 0)
                    seg_pool.total_segments = seg_src.total;
                int next = seg_pool.current_segment_id + 1;
                if (seg_src.complete && next >= seg_pool.total_segments) {
                    reached_end_of_stream = 1;
                    break;
                }
                if (next >= seg_pool.total_segments) {
                    /* 估算段数用尽：D3 最长等 8s 等下一段；仍无则回退原声。 */
                    if (next * SEGMENT_DURATION_SEC >= g_total_duration) {
                        reached_end_of_stream = 1;
                        break;
                    }
                    if (seg_wait_ready(&seg_src, file_path, next, 8000)) {
                        if (next + 1 > seg_pool.total_segments)
                            seg_pool.total_segments = next + 1;
                    } else {
                        seg_fallback_to_original(next * SEGMENT_DURATION_SEC, file_path,
                                                 &seg_mode, &source_kind);
                        continue;
                    }
                }
                /* PCM 预加载仍服务原声/v1 下一曲；模型分离预取由 audio.c 状态
                 * 回调（D5）负责，此处不重复。 */
                extern AppConfig g_app_config;
                if (g_app_config.seamless_preload && !preload_attempted &&
                    seg_pool.current_segment_id >= seg_pool.total_segments - 2) {
                    preload_attempted = 1;
                    attempt_next_track_preload(index, &seg_pool,
                                               output_sample_rate, output_channels);
                }
                if (segment_pool_advance(&seg_pool) < 0) {
                    reached_end_of_stream = 1;
                    break;
                }
                continue;
            }
            if (segment_pool_is_last(&seg_pool)) {
                reached_end_of_stream = 1;
                break;
            }

            /* If current slot is invalid (never filled) and decoder is
             * finished, check whether any other slot still holds
             * unconsumed data before declaring end-of-stream. */
            if (!segment_pool_current(&seg_pool) && decoder_finished) {
                int has_remaining = 0;
                for (int s = 0; s < SEGMENT_POOL_SIZE; s++) {
                    if (s == seg_pool.current_slot) continue;
                    Segment *candidate = &seg_pool.slots[s];
                    if (candidate->is_valid &&
                        candidate->frame_count > candidate->consumed_frames) {
                        has_remaining = 1;
                        break;
                    }
                }
                if (!has_remaining) {
                    reached_end_of_stream = 1;
                    break;
                }
                /* There is still data in other slots — let Phase 5
                 * recovery pick it up. */
            }

            /* Trigger next-track preload when within 2 segments of the end */
            extern AppConfig g_app_config;
            if (g_app_config.seamless_preload && !preload_attempted &&
                seg_pool.current_segment_id >= seg_pool.total_segments - 2) {
                preload_attempted = 1;
                attempt_next_track_preload(index, &seg_pool,
                                           output_sample_rate, output_channels);
            }

            int free_slot = segment_pool_advance(&seg_pool);
            if (free_slot >= 0 && !decoder_finished) {
                /* 刚腾空的槽不再整段阻塞解码：窗口推进后它距 current 的偏移
                 * 恰为 2，下一轮 Phase 3 会认领 current_segment_id+2 并分片填满，
                 * 而池里 current+1 已有整段，足够把这一片的时间撑住。 */
            } else if (free_slot < 0 && !segment_pool_current(&seg_pool) && decoder_finished) {
                /* Advance failed but current slot is still invalid and decoder
                 * is done.  If another slot has unconsumed data, jump to it
                 * directly so Phase 5 can play it out. */
                for (int s = 0; s < SEGMENT_POOL_SIZE; s++) {
                    if (s == seg_pool.current_slot) continue;
                    Segment *candidate = &seg_pool.slots[s];
                    if (candidate->is_valid &&
                        candidate->frame_count > candidate->consumed_frames) {
                        log_debug("audio", "Phase 4 recovery: advancing to slot %d with remaining data",
                                  s);
                        seg_pool.current_slot = s;
                        seg_pool.current_segment_id = candidate->segment_id;
                        break;
                    }
                }
            }
            continue;
        }

        /* ── Phase 5: Write batch from current segment ── */
        Segment *cur = segment_pool_current(&seg_pool);
        if (!cur && seg_mode) {
            /* 当前段未就绪：提示 worker + 等待 ≤8s；超时/失败回退原声（D3）。 */
            int seg = seg_pool.current_segment_id;
            if (seg * SEGMENT_DURATION_SEC >= g_total_duration) {
                reached_end_of_stream = 1;
                break;
            }
            (void)karaoke_worker_hint_segment(file_path, seg);
            core_status_push(i18n_get("karaoke.status.segment_wait"));
            int ok = seg_wait_ready(&seg_src, file_path, seg, 8000);
            if (ok) {
                char why[160] = {0};
                int rc = seg_decode_into_pool(&seg_src, &seg_pool,
                                              seg_pool.current_slot, seg, &seg_dec,
                                              output_sample_rate, output_channels, 0,
                                              g_playback_speed, why, sizeof(why));
                if (rc == 1) {
                    Segment *ns = &seg_pool.slots[seg_pool.current_slot];
                    segment_pool_mark_ready(
                        &seg_pool, seg_pool.current_slot, ns->frame_count, seg,
                        (seg_src.complete && seg >= seg_pool.total_segments - 1));
                    cur = segment_pool_current(&seg_pool);
                } else {
                    log_warn("audio", "karaoke segment %d decode failed (%s)", seg, why);
                    ok = 0;
                }
            }
            if (!cur) {
                if (!ok)
                    log_info("audio",
                             "karaoke segment %d not ready after wait, fallback original",
                             seg);
                seg_fallback_to_original(seg * SEGMENT_DURATION_SEC, file_path,
                                         &seg_mode, &source_kind);
                continue;
            }
        }
        if (!cur) {
            /* Current slot invalid: try to recover by finding another
             * valid slot in the pool that still has unconsumed data.
             * This is attempted even when decoder_finished is true,
             * because there may still be data in other slots that
             * should be played before we declare end-of-stream. */
            int recovered = 0;
            for (int s = 0; s < SEGMENT_POOL_SIZE; s++) {
                if (s == seg_pool.current_slot) continue;
                Segment *candidate = &seg_pool.slots[s];
                if (candidate->is_valid &&
                    candidate->frame_count > candidate->consumed_frames) {
                    log_debug("audio", "Recovering: advancing from invalid slot %d to valid slot %d",
                              seg_pool.current_slot, s);
                    seg_pool.current_slot = s;
                    seg_pool.current_segment_id = candidate->segment_id;
                    recovered = 1;
                    break;
                }
            }
            if (!recovered && decoder_finished) {
                reached_end_of_stream = 1;
                break;
            }
            if (!recovered) {
                log_debug("audio", "Recovery failed: no valid slots — waiting for decode");
            }
            continue;
        }

        int remaining = segment_pool_current_remaining(&seg_pool);
        int batch = (remaining < WRITE_BATCH_FRAMES) ? remaining : WRITE_BATCH_FRAMES;
        int32_t *write_ptr = cur->data + cur->consumed_frames * seg_pool.channels;

        /* Equalizer */
        if (eq_is_enabled()) {
            eq_process(write_ptr, batch, seg_pool.channels, output_sample_rate);
        }

        /* Visualizer */
        if (remaining > (output_sample_rate * get_configured_latency_ms() / 1000))
            push_visualizer_samples(write_ptr, batch, seg_pool.channels);

        /* Volume */
        extern void apply_volume_to_samples(int32_t *samples, int sample_count);
        apply_volume_to_samples(write_ptr, batch * seg_pool.channels);

        /* Write to audio backend */
        if (audio_backend_write_samples(write_ptr, batch) < 0) {
            core_status_push(i18n_get("audio.err.device_write"));
            playback_error = 1;
            break;
        }

        progress_tracker_add_samples(batch);
        segment_pool_consume(&seg_pool, batch);
        g_current_position = progress_tracker_get_position_seconds();
    }

    /* ════════════════════════════════════════════════════════
     * Cleanup
     * ════════════════════════════════════════════════════════ */

cleanup:
    /* PipeWire（或任何报告未消费帧的后端）的写端可能比播放端快：整首
     * 解码/写入完成后，后端环里仍可能有未消费的音频。若立刻 cleanup_stream() 并调度下一首，
     * 用户会听到曲目被截尾、队列提前前进；2 秒短曲在 PipeWire 上尤其明显。
     * EOS 后先等后端缓冲排空（有上限，避免设备卡死时永久等待），再拆流。 */
    if (reached_end_of_stream && !playback_error) {
        int waited_ms = 0;
        while (g_play_thread_running && waited_ms < 5000) {
            if (audio_backend_buffered_frames() <= 0) {
                break;
            }
            usleep(10000);
            waited_ms += 10;
        }
        if (waited_ms > 0) {
            log_debug("audio", "EOS: waited %dms for backend buffer drain", waited_ms);
        }
    }

    drain_atempo_filter(filtered_frame);
    audio_backend_cleanup_stream();

    if (pool_initialized) segment_pool_destroy(&seg_pool);
    if (seg_dec.swr) swr_free(&seg_dec.swr);
    av_frame_free(&filtered_frame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    /* ── Defensive duration correction ──
     * If the decoder reached end-of-stream cleanly but g_current_position
     * is less than g_total_duration, the metadata duration was likely
     * inflated (e.g. by attached pictures or bitrate estimation).
     * Adjust so the progress bar hits 100% at the true end. */
    if (reached_end_of_stream && !playback_error &&
        g_current_position > 0 && g_current_position < g_total_duration) {
        int gap = g_total_duration - g_current_position;
        log_info("audio", "Duration correction: metadata=%ds actual=%ds (gap=%ds)",
                 g_total_duration, g_current_position, gap);
        g_total_duration = g_current_position + 1;  /* +1 ensures 100% */
    }

    progress_tracker_on_stop();
    reset_visualizer_state();

    if (!reached_end_of_stream) g_current_position = 0;

    pthread_mutex_lock(&g_play_mutex);
    if (g_play_thread_running && reached_end_of_stream) {
        if (g_play_mode == PLAY_MODE_SINGLE_REPEAT) {
            followup_index = index;
        } else {
            followup_index = play_queue_peek_next(&g_play_queue, g_play_mode);
            if (followup_index >= 0)
                play_queue_advance(&g_play_queue, g_play_mode);
        }
    }
    if (reached_end_of_stream && followup_index >= 0)
        log_info("audio", "End of stream for index=%d, scheduling follow-up index=%d (mode=%d)", index, followup_index, g_play_mode);
    else if (reached_end_of_stream)
        log_info("audio", "End of stream for index=%d, no follow-up (mode=%d)", index, g_play_mode);
    else if (playback_error)
        log_warn("audio", "Playback error for index=%d, stopping thread", index);

    g_play_thread_running = 0;
    g_play_thread_finished = 1;
    g_play_state = PLAY_STATE_STOPPED;
    g_current_play_index = -1;
    g_current_play_path[0] = '\0';

    g_audio_sample_rate = 0;
    g_audio_bit_rate = 0;
    g_audio_bit_depth = 0;
    g_audio_codec_name[0] = '\0';

    if (followup_index >= 0 && g_pending_playback_index < 0)
        g_pending_playback_index = followup_index;
    pthread_mutex_unlock(&g_play_mutex);

    if (followup_index < 0) {
        clear_lyrics();
    }
    core_notify_state_changed();

    log_debug("audio", "Playback thread exiting for index=%d (eos=%d, err=%d)", index, reached_end_of_stream, playback_error);
    return NULL;
}
