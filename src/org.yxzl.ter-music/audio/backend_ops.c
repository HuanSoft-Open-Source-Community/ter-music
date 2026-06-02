/**
 * @file backend_ops.c
 * @brief 音频后端流操作 — 准备/清理/写入/暂停/恢复/音量同步
 *
 * 从 audio.c 拆分，封装对 PulseAudio/ALSA/PipeWire 后端的流生命周期操作。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "audio/audio_internal.h"
#include "ui/ui.h"
#include "config/config.h"
#include "logger/logger.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

extern int g_active_backend;

/* ============================================================
 * Volume sync
 * ============================================================ */

static int consume_volume_sync_request(int *volume_out, int force)
{
    int should_sync = force;
    int volume = 100;

    pthread_mutex_lock(&g_volume_mutex);
    volume = g_volume_percent;
    if (g_pending_volume_sync) {
        should_sync = 1;
        g_pending_volume_sync = 0;
    }
    pthread_mutex_unlock(&g_volume_mutex);

    if (volume_out) *volume_out = volume;
    return should_sync;
}

void audio_backend_sync_volume(int force)
{
    int volume = 100;
    if (!consume_volume_sync_request(&volume, force)) return;

    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE) {
        if (!pa_ctx || !pa_ml || !pa_s || P.stream_get_state(pa_s) != PA_STREAM_READY) return;

        uint32_t stream_index = P.stream_get_index(pa_s);
        if (stream_index == PA_INVALID_INDEX) return;

        pa_cvolume cvolume;
        P.cvolume_set(&cvolume, pa_ss.channels, P.sw_volume_from_linear((double)volume / 100.0));

        pa_operation *op = P.context_set_sink_input_volume(pa_ctx, stream_index, &cvolume, NULL, NULL);
        if (!op) return;

        while (P.operation_get_state(op) == PA_OPERATION_RUNNING)
            P.mainloop_iterate(pa_ml, 1, NULL);
        P.operation_unref(op);
        return;
    }

    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        pw_sync_volume(volume);
        return;
    }

    (void)force;
    (void)volume;
}

/* ============================================================
 * Prepare stream
 * ============================================================ */

int audio_backend_prepare_stream(int sample_rate, int channels)
{
    g_output_sample_rate = sample_rate;
    g_output_channels = channels;

    unsigned int latency_usec = (unsigned int)get_configured_latency_ms() * 1000U;
    unsigned int minreq_usec = latency_usec / 4U;
    if (minreq_usec < 5000U) minreq_usec = 5000U;
    unsigned int max_latency_usec = latency_usec * 2U;

    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE) {
        if (!pa_connected || !pa_ctx) {
            update_controls_status(audio_text("PulseAudio 未连接", "PulseAudio disconnected"));
            return -1;
        }

        pa_ss.format = PA_SAMPLE_S32LE;
        pa_ss.rate = sample_rate;
        pa_ss.channels = (uint8_t)channels;

        pa_stream *new_stream = P.stream_new(pa_ctx, "playback", &pa_ss, NULL);
        if (!new_stream) {
            update_controls_status(audio_text("无法创建 PulseAudio 播放流", "Cannot create PulseAudio stream"));
            return -1;
        }

        pa_buffer_attr ba;
        memset(&ba, 0, sizeof(ba));
        ba.maxlength = P.usec_to_bytes((uint64_t)max_latency_usec, &pa_ss);
        ba.tlength   = P.usec_to_bytes((uint64_t)latency_usec, &pa_ss);
        ba.prebuf    = 0;
        ba.minreq    = P.usec_to_bytes((uint64_t)minreq_usec, &pa_ss);
        ba.fragsize  = (uint32_t)-1;

        if (P.stream_connect_playback(new_stream, NULL, &ba, PA_STREAM_ADJUST_LATENCY, NULL, NULL) < 0) {
            update_controls_status(audio_text("无法连接 PulseAudio 播放流", "Cannot connect PulseAudio stream"));
            P.stream_unref(new_stream);
            return -1;
        }

        while (P.stream_get_state(new_stream) != PA_STREAM_READY) {
            if (P.stream_get_state(new_stream) == PA_STREAM_FAILED ||
                P.stream_get_state(new_stream) == PA_STREAM_TERMINATED) {
                update_controls_status(audio_text("PulseAudio 播放流初始化失败", "PulseAudio stream init failed"));
                P.stream_unref(new_stream);
                return -1;
            }
            P.mainloop_iterate(pa_ml, 1, NULL);
        }

        pa_s = new_stream;
        return 0;
    }

    if (alsa_loaded && g_active_backend == AUDIO_BACKEND_ALSA) {
        if (!alsa_ready) {
            update_controls_status(audio_text("ALSA 后端未就绪", "ALSA backend not ready"));
            return -1;
        }

        extern char g_default_audio_device[128];
        if (A.pcm_open(&alsa_pcm, g_default_audio_device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            update_controls_status(audio_text("无法打开 ALSA 设备", "Cannot open ALSA device"));
            alsa_pcm = NULL;
            return -1;
        }

        if (A.pcm_set_params(alsa_pcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               (unsigned int)channels, (unsigned int)sample_rate, 1, latency_usec) < 0) {
            update_controls_status(audio_text("无法配置 ALSA 设备", "Cannot configure ALSA device"));
            A.pcm_close(alsa_pcm);
            alsa_pcm = NULL;
            return -1;
        }
        return 0;
    }

    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        if (pw_prepare_stream(sample_rate, channels) == 0) return 0;
        log_warn("audio", "PipeWire backend failed, falling back to next backend");
        pw_cleanup_stream_locked();
        if (pulse_loaded) {
            if (pulse_ensure_connected() < 0) {
                log_warn("audio", "PulseAudio init failed, cannot fall back");
            } else {
                g_active_backend = AUDIO_BACKEND_PULSE;
                log_info("audio", "Falling back to PulseAudio backend");
                return audio_backend_prepare_stream(sample_rate, channels);
            }
        }
        if (alsa_loaded) {
            g_active_backend = AUDIO_BACKEND_ALSA;
            alsa_ready = 1;
            return audio_backend_prepare_stream(sample_rate, channels);
        }
        return -1;
    }

    update_controls_status(audio_text("没有可用的音频后端", "No audio backend available"));
    return -1;
}

/* ============================================================
 * Backend stream lifecycle
 * ============================================================ */

void audio_backend_cleanup_stream(void)
{
    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE && pa_s) {
        if (P.stream_is_corked(pa_s)) P.stream_cork(pa_s, 0, NULL, NULL);
        P.stream_disconnect(pa_s);
        P.stream_unref(pa_s);
        pa_s = NULL;
        return;
    }
    if (alsa_loaded && g_active_backend == AUDIO_BACKEND_ALSA && alsa_pcm) {
        A.pcm_drop(alsa_pcm);
        A.pcm_close(alsa_pcm);
        alsa_pcm = NULL;
        return;
    }
    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        pw_cleanup_stream();
        return;
    }
}

void audio_backend_flush_stream(void)
{
    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE && pa_s && pa_ml && pa_ctx) {
        int state = P.stream_get_state(pa_s);
        if (state == PA_STREAM_READY) {
            pa_operation *op = P.stream_flush(pa_s, NULL, NULL);
            if (op) {
                while (P.operation_get_state(op) == PA_OPERATION_RUNNING)
                    P.mainloop_iterate(pa_ml, 1, NULL);
                P.operation_unref(op);
            }
        }
        return;
    }
    if (alsa_loaded && g_active_backend == AUDIO_BACKEND_ALSA && alsa_pcm) {
        A.pcm_drop(alsa_pcm);
        A.pcm_prepare(alsa_pcm);
        return;
    }
    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        pw_flush_stream();
        return;
    }
}

int audio_backend_write_samples(const int32_t *samples, int frame_count)
{
    if (!samples || frame_count <= 0) return 0;

    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE) {
        size_t bytes = (size_t)frame_count * pa_ss.channels * sizeof(int32_t);
        if (!pa_s || P.stream_get_state(pa_s) != PA_STREAM_READY) return -1;

        while (pa_s && P.stream_get_state(pa_s) == PA_STREAM_READY &&
               P.stream_writable_size(pa_s) < bytes) {
            extern int g_play_thread_running;
            if (!g_play_thread_running) return 0;
            P.mainloop_iterate(pa_ml, 0, NULL);
            usleep(1000);
        }
        if (!pa_s || P.stream_get_state(pa_s) != PA_STREAM_READY) return -1;
        return P.stream_write(pa_s, samples, bytes, NULL, 0, PA_SEEK_RELATIVE);
    }

    if (alsa_loaded && g_active_backend == AUDIO_BACKEND_ALSA) {
        int written = 0;
        int wait_timeout_ms = get_configured_latency_ms();
        if (wait_timeout_ms < 20) wait_timeout_ms = 20;

        while (written < frame_count) {
            extern int g_play_thread_running;
            if (!g_play_thread_running) return 0;
            if (A.pcm_wait(alsa_pcm, wait_timeout_ms) < 0) A.pcm_prepare(alsa_pcm);
            snd_pcm_sframes_t ret = A.pcm_writei(alsa_pcm,
                                                   samples + (written * g_output_channels),
                                                   (snd_pcm_uframes_t)(frame_count - written));
            if (ret > 0) { written += (int)ret; continue; }
            if (ret == -EAGAIN) continue;
            if (ret == -EPIPE || ret == -ESTRPIPE) { A.pcm_prepare(alsa_pcm); continue; }
            return -1;
        }
        return 0;
    }

    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE)
        return pw_write_samples(samples, frame_count);

    return -1;
}

void audio_backend_pause_stream(void)
{
    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE && pa_s && pa_ml && pa_ctx) {
        int state = P.stream_get_state(pa_s);
        if (state == PA_STREAM_READY && !P.stream_is_corked(pa_s)) {
            pa_operation *op = P.stream_cork(pa_s, 1, NULL, NULL);
            if (op) P.operation_unref(op);
        }
        return;
    }
    if (alsa_loaded && g_active_backend == AUDIO_BACKEND_ALSA && alsa_pcm) {
        A.pcm_pause(alsa_pcm, 1);
        return;
    }
    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        pw_pause_stream();
        return;
    }
}

void audio_backend_resume_stream(void)
{
    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE && pa_s && pa_ml && pa_ctx) {
        int state = P.stream_get_state(pa_s);
        if (state == PA_STREAM_READY && P.stream_is_corked(pa_s)) {
            pa_operation *op = P.stream_cork(pa_s, 0, NULL, NULL);
            if (op) P.operation_unref(op);
        }
        return;
    }
    if (alsa_loaded && g_active_backend == AUDIO_BACKEND_ALSA && alsa_pcm) {
        A.pcm_pause(alsa_pcm, 0);
        return;
    }
    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        pw_resume_stream();
        return;
    }
}
