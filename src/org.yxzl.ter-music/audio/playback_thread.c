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
#include "audio/progress/progress.h"
#include "audio/play_queue.h"
#include "playlist/playlist.h"
#include "ui/ui.h"
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
 * Decode one full segment (15s) into a pool slot
 * ============================================================ */

static int decode_segment_fill(AVFormatContext *fmt_ctx,
                               AVCodecContext *codec_ctx,
                               SwrContext *swr_ctx,
                               AVPacket *packet,
                               AVFrame *frame,
                               AVFrame *filtered_frame,
                               SegmentPool *pool,
                               int target_slot,
                               int target_frames,
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

    while (seg->frame_count < target_frames && !*decoder_finished) {
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
        if (ret != AVERROR(EAGAIN)) return -1;

        /* 3. Read next packet */
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
            break;
        }
    }

    return (seg->frame_count > 0) ? 1 : 0;
}

/* ============================================================
 * Seek-in-decoder handler (segment-aware)
 * ============================================================ */

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
                                          int cue_offset  /* CUE sub-track offset; added to target_position for absolute file seek */)
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
        int64_t target_ts = av_rescale_q(target_position + cue_offset, (AVRational){1, 1}, time_base);
        int ret = av_seek_frame(fmt_ctx, audio_stream_index, target_ts, 0);
        if (ret < 0) {
            update_controls_status(i18n_get("audio.err.seek_failed"));
        } else {
            avcodec_flush_buffers(codec_ctx);
            if (swr_ctx) swr_init(swr_ctx);
            if (packet) av_packet_unref(packet);

            /* Reset segment pool to target segment */
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

            /* Immediately decode the target segment into slot 0 */
            int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
            decode_segment_fill(fmt_ctx, codec_ctx, swr_ctx, packet, frame, filtered_frame,
                                pool, pool->current_slot, target_frames,
                                audio_stream_index, output_sample_rate, output_channels,
                                use_resampler, decoder_draining, decoder_finished);

            /* Mark the decoded segment as ready — was missing, causing
             * segment_pool_current() to return NULL and skip playback */
            int is_last = (*decoder_finished) || (target_seg >= pool->total_segments - 1);
            segment_pool_mark_ready(pool, pool->current_slot,
                                    pool->slots[pool->current_slot].frame_count,
                                    target_seg, is_last);

            char msg[64];
            snprintf(msg, sizeof(msg),
                     i18n_get("status.seek_fmt"),
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

    /* Determine next track index (need lock for queue access) */
    int next_index = -1;
    pthread_mutex_lock(&g_play_mutex);
    next_index = play_queue_peek_next(&g_play_queue, g_play_mode);
    pthread_mutex_unlock(&g_play_mutex);
    if (next_index < 0 || next_index == current_track_index) {
        log_debug("segment", "preload: no valid next track (idx=%d)", next_index);
        return;
    }

    /* Skip preload for CUE tracks — preloaded PCM from position 0 would be
     * at the wrong offset. The playback thread handles CUE offset seeking. */
    if (next_index >= 0 && next_index < MAX_TRACKS &&
        g_playlist.cue_offsets[next_index] > 0) {
        log_debug("segment", "preload: skipping (CUE track with offset)");
        return;
    }

    char file_path[MAX_PATH_LEN];
    if (playlist_get_track_path(next_index, file_path, sizeof(file_path)) != 0) {
        log_warn("segment", "preload: cannot get path for next track idx=%d", next_index);
        return;
    }

    /* Use cached remote path if available */
    {
        extern char g_cached_audio_path[256];
        if (g_cached_audio_path[0])
            strncpy(file_path, g_cached_audio_path, MAX_PATH_LEN - 1);
    }

    log_info("segment", "preload: attempting next track idx=%d path='%s'", next_index, file_path);

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

    /* Decode up to PRELOAD_SEGMENT_COUNT segments */
    int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
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
            if (rret != AVERROR(EAGAIN)) break;

            if (!drain) {
                int pret = av_read_frame(next_fmt, next_pkt);
                if (pret < 0) {
                    drain = 1;
                    avcodec_send_packet(next_codec, NULL);
                    break;
                }
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

    /* ── Segment pool ── */
    SegmentPool seg_pool;
    memset(&seg_pool, 0, sizeof(seg_pool));
    int pool_initialized = 0;
    int preload_attempted = 0;

    if (avformat_open_input(&fmt_ctx, file_path, NULL, NULL) != 0) {
        log_error("audio", "avformat_open_input failed for '%s' (index=%d)", file_path, index);
        update_controls_status(i18n_get("audio.err.cannot_open_file"));
        goto cleanup;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        log_error("audio", "avformat_find_stream_info failed for '%s'", file_path);
        update_controls_status(i18n_get("audio.err.cannot_read_stream"));
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

    /* Cap duration for CUE sub-tracks */
    if (cue_offset > 0) {
        int next_offset = cue_find_next_offset(index);
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

    request_ui_refresh(UI_DIRTY_CONTROLS);

    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }
    if (audio_stream_index == -1) {
        log_error("audio", "No audio stream found in '%s'", file_path);
        update_controls_status(i18n_get("audio.err.no_stream"));
        goto cleanup;
    }

    AVCodecParameters *codec_par = fmt_ctx->streams[audio_stream_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        log_error("audio", "Unsupported codec in '%s' (codec_id=%d)", file_path, codec_par->codec_id);
        update_controls_status(i18n_get("audio.err.unsupported_codec"));
        goto cleanup;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) { update_controls_status(i18n_get("audio.err.alloc_codec_ctx")); goto cleanup; }
    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) { update_controls_status(i18n_get("audio.err.copy_codec_params")); goto cleanup; }
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) { update_controls_status(i18n_get("audio.err.open_codec")); goto cleanup; }

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

    use_resampler = (codec_ctx->sample_fmt != AV_SAMPLE_FMT_S32 || input_channels != output_channels);

    /* ── Segment pool init ── */
    if (segment_pool_init(&seg_pool, output_sample_rate, output_channels) < 0) {
        update_controls_status(i18n_get("audio.err.alloc_segment_buf"));
        goto cleanup;
    }
    pool_initialized = 1;

    /* Set total segments for this track */
    seg_pool.total_segments = segment_pool_total_for_duration(g_total_duration);
    log_debug("audio", "Total segments for track: %d (%d sec)",
              seg_pool.total_segments, g_total_duration);

    if (use_resampler) {
        swr_ctx = swr_alloc();
        if (!swr_ctx) { update_controls_status(i18n_get("audio.err.alloc_resampler")); goto cleanup; }
        if (init_resampler(swr_ctx, codec_ctx, input_channels, output_channels, output_sample_rate) < 0) {
            update_controls_status(i18n_get("audio.err.init_resampler")); goto cleanup;
        }
    }

    if (init_atempo_filter(codec_ctx, g_playback_speed) < 0) {
        update_controls_status(i18n_get("audio.err.init_speed_filter")); goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    filtered_frame = av_frame_alloc();
    if (!packet || !frame || !filtered_frame) {
        update_controls_status(i18n_get("audio.err.alloc_decode"));
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
    if (g_preload_data.valid && g_preload_data.track_index == index &&
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
            dst->is_last = 0;   /* will be corrected by decoder in Phase 3 fill */
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
    if (!used_preload && cue_offset == 0) {
        int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
        for (int s = 0; s < SEGMENT_POOL_SIZE && !decoder_finished; s++) {
            int seg_id = s;
            if (seg_id >= seg_pool.total_segments) break;

            int ret = decode_segment_fill(fmt_ctx, codec_ctx, swr_ctx,
                                          packet, frame, filtered_frame,
                                          &seg_pool, s, target_frames,
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

            audio_backend_flush_stream();
            g_current_position = 0;
            progress_tracker_seek(0);
            decoder_draining = 0;
            decoder_finished = 0;

            /* Re-decode first segment from seeked position */
            int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
            decode_segment_fill(fmt_ctx, codec_ctx, swr_ctx, packet, frame, filtered_frame,
                                &seg_pool, seg_pool.current_slot, target_frames,
                                audio_stream_index, output_sample_rate, output_channels,
                                use_resampler, &decoder_draining, &decoder_finished);

            segment_pool_mark_ready(&seg_pool, seg_pool.current_slot,
                                    seg_pool.slots[seg_pool.current_slot].frame_count,
                                    seg_id,
                                    (decoder_finished || seg_id >= seg_pool.total_segments - 1));

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
        if (handle_seek_request_in_decoder(fmt_ctx, codec_ctx, swr_ctx,
                                           packet, frame, filtered_frame,
                                           &seg_pool, audio_stream_index,
                                           output_sample_rate, output_channels,
                                           use_resampler,
                                           &decoder_draining, &decoder_finished,
                                           cue_offset)) {
            if (g_current_position >= g_total_duration) { g_play_thread_running = 0; break; }
            continue;
        }

        /* ── Phase 3: Fill empty slots ── */
        if (!playback_error) {
            int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
            int free_slot;
            while ((free_slot = segment_pool_free_slot(&seg_pool)) >= 0 && !decoder_finished) {
                int next_seg_id = seg_pool.current_segment_id;
                /* Find the global segment id for this free slot */
                /* It's the slot farthest from current, so its global id =
                 * current_segment_id + (offset from current) */
                int offset = (free_slot - seg_pool.current_slot + SEGMENT_POOL_SIZE) % SEGMENT_POOL_SIZE;
                int global_seg = seg_pool.current_segment_id + offset;
                if (global_seg >= seg_pool.total_segments) break;

                int ret = decode_segment_fill(fmt_ctx, codec_ctx, swr_ctx,
                                              packet, frame, filtered_frame,
                                              &seg_pool, free_slot, target_frames,
                                              audio_stream_index, output_sample_rate,
                                              output_channels, use_resampler,
                                              &decoder_draining, &decoder_finished);
                if (ret < 0) { playback_error = 1; break; }

                segment_pool_mark_ready(&seg_pool, free_slot,
                                        seg_pool.slots[free_slot].frame_count,
                                        global_seg,
                                        (decoder_finished || global_seg >= seg_pool.total_segments - 1));
                if (ret == 0 && seg_pool.slots[free_slot].frame_count == 0) break;
            }
        }

        if (playback_error) break;

        /* ── Phase 4: Advance if current segment is done ── */
        if (segment_pool_current_done(&seg_pool)) {
            if (segment_pool_is_last(&seg_pool)) {
                reached_end_of_stream = 1;
                break;
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
                int next_global = seg_pool.current_segment_id + 2; /* the new farthest slot */
                if (next_global < seg_pool.total_segments) {
                    int target_frames = output_sample_rate * SEGMENT_DURATION_SEC;
                    int ret = decode_segment_fill(fmt_ctx, codec_ctx, swr_ctx,
                                                  packet, frame, filtered_frame,
                                                  &seg_pool, free_slot, target_frames,
                                                  audio_stream_index, output_sample_rate,
                                                  output_channels, use_resampler,
                                                  &decoder_draining, &decoder_finished);
                    if (ret < 0) { playback_error = 1; break; }
                    segment_pool_mark_ready(&seg_pool, free_slot,
                                            seg_pool.slots[free_slot].frame_count,
                                            next_global,
                                            (decoder_finished || next_global >= seg_pool.total_segments - 1));
                }
            }
            continue;
        }

        /* ── Phase 5: Write batch from current segment ── */
        Segment *cur = segment_pool_current(&seg_pool);
        if (!cur) {
            if (decoder_finished) { reached_end_of_stream = 1; break; }
            /* Current slot invalid but decoder not finished: try to recover by
             * advancing to the next valid slot if one exists. This prevents
             * getting stuck when a slot was never filled (e.g. decoder_finished
             * triggered during Phase 3 fill before all slots were ready). */
            {   /* scan for any valid slot other than current */
                int recovered = 0;
                for (int s = 0; s < SEGMENT_POOL_SIZE; s++) {
                    if (seg_pool.slots[s].is_valid && s != seg_pool.current_slot) {
                        log_debug("audio", "Recovering: advancing from invalid slot %d to valid slot %d",
                                  seg_pool.current_slot, s);
                        seg_pool.current_slot = s;
                        seg_pool.current_segment_id = seg_pool.slots[s].segment_id;
                        recovered = 1;
                        break;
                    }
                }
                if (!recovered) {
                    log_debug("audio", "Recovery failed: no valid slots — waiting for decode");
                }
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
            update_controls_status(i18n_get("audio.err.device_write"));
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
    drain_atempo_filter(filtered_frame);
    audio_backend_cleanup_stream();

    if (pool_initialized) segment_pool_destroy(&seg_pool);
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
