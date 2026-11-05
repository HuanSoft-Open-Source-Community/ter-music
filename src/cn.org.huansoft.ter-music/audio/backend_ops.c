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
#include "i18n/i18n.h"
#include "core/core.h"
#include "config/config.h"
#include "logger/logger.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

extern int g_active_backend;

/* ============================================================
 * Backend candidates & capability probing
 * ============================================================ */

/* ALSA 设备探测参数：与建流时的格式保持一致，仅用于确认设备真的可用 */
#define ALSA_PROBE_CHANNELS    2
#define ALSA_PROBE_SAMPLE_RATE 48000

/* ALSA 写入等待预算（毫秒）。
 * writei 返回 0 / -EAGAIN / -EPIPE 都不是致命错误，只表示“设备此刻写不进”：
 * 在预算内继续等待设备可写；预算耗尽仍写不进才判定为真实写失败。 */
#define ALSA_WRITE_BUDGET_MS      1000
#define ALSA_WRITE_RETRY_SLEEP_US 2000
/* 暂停态等待“播放恢复”的单次等待上限（毫秒） */
#define ALSA_WRITE_PAUSE_WAIT_MS  100

/* ALSA 应用层暂停标志：snd_pcm_pause() 可能不被设备支持（-ENOSYS/-EBADFD），
 * 此时以应用层语义为准——写入循环等待“恢复播放或线程被要求停止”，
 * 而不是把暂停态下的 0 返回当成写失败。 */
static int alsa_paused = 0;

const char *audio_backend_display_name(int backend)
{
    switch (backend) {
        case AUDIO_BACKEND_PIPEWIRE: return i18n_get("audio.backend.name.pipewire");
        case AUDIO_BACKEND_PULSE:    return i18n_get("audio.backend.name.pulseaudio");
        case AUDIO_BACKEND_ALSA:     return i18n_get("audio.backend.name.alsa");
        case AUDIO_BACKEND_AUTO:     return i18n_get("general.auto");
        default:                     return "?";
    }
}

/* 语言包占位符风格为 {key}（见 cli/help.c、ui/help_view.c 的用法）：
 * 这里做最简替换，把 {from} / {to} / {reason} 换成实际值。 */
static void replace_placeholder(char *buf, size_t buf_size,
                                const char *token, const char *value)
{
    if (!buf || buf_size == 0) return;
    if (!value) value = "";
    char *pos = strstr(buf, token);
    if (!pos) return;
    char merged[1024];
    int n = snprintf(merged, sizeof(merged), "%.*s%s%s",
                     (int)(pos - buf), buf, value, pos + strlen(token));
    if (n < 0) return;
    snprintf(buf, buf_size, "%s", merged);
}

void audio_backend_format_message(char *out, size_t out_size, const char *tmpl,
                                   const char *from, const char *to,
                                   const char *reason)
{
    char work[512];
    snprintf(work, sizeof(work), "%s", tmpl ? tmpl : "");
    replace_placeholder(work, sizeof(work), "{from}", from);
    replace_placeholder(work, sizeof(work), "{to}", to);
    replace_placeholder(work, sizeof(work), "{reason}", reason);
    snprintf(out, out_size, "%s", work);
}

/* ALSA 错误码 → 可读文本 */
static const char *alsa_error_text(int err)
{
    if (A.strerror) {
        const char *text = A.strerror(err);
        if (text) return text;
    }
    return "unknown error";
}

/* ALSA 设备能力探测：真实打开 PCM 并配置参数，失败即视为该后端不可用。
 * 成功时 alsa_ready = 1，表示“探测通过”（而非“库能加载”）。 */
static int alsa_probe_device(const char **reason_out)
{
    snd_pcm_t *pcm = NULL;
    int err = A.pcm_open(&pcm, g_default_audio_device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        if (reason_out) *reason_out = alsa_error_text(err);
        return -1;
    }

    unsigned int latency_usec = (unsigned int)get_configured_latency_ms() * 1000U;
    err = A.pcm_set_params(pcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           ALSA_PROBE_CHANNELS, ALSA_PROBE_SAMPLE_RATE, 1, latency_usec);
    if (err < 0) {
        if (reason_out) *reason_out = alsa_error_text(err);
        A.pcm_close(pcm);
        return -1;
    }

    A.pcm_close(pcm);
    return 0;
}

int audio_backend_probe(int backend, const char **reason_out)
{
    if (reason_out) *reason_out = NULL;

    switch (backend) {
    case AUDIO_BACKEND_PIPEWIRE:
        if (!pipewire_loaded) {
            if (pipewire_load() != 0) {
                if (reason_out) *reason_out = "library not available";
                return -1;
            }
            pipewire_loaded = 1;
        }
        if (pw_probe_backend() != 0) {
            if (reason_out) *reason_out = "cannot connect to PipeWire daemon";
            pipewire_unload();
            pipewire_loaded = 0;
            return -1;
        }
        return 0;

    case AUDIO_BACKEND_PULSE:
        if (!pulse_loaded) {
            if (pulse_load() != 0) {
                if (reason_out) *reason_out = "library not available";
                return -1;
            }
            pulse_loaded = 1;
        }
        if (pulse_ensure_connected() != 0) {
            if (reason_out) *reason_out = "cannot connect to PulseAudio server";
            return -1;
        }
        return 0;

    case AUDIO_BACKEND_ALSA: {
        if (!alsa_loaded) {
            if (alsa_load() != 0) {
                if (reason_out) *reason_out = "library not available";
                return -1;
            }
            alsa_loaded = 1;
        }
        const char *reason = NULL;
        if (alsa_probe_device(&reason) != 0) {
            alsa_ready = 0;
            if (reason_out) *reason_out = reason ? reason : "device probe failed";
            return -1;
        }
        alsa_ready = 1;
        return 0;
    }

    default:
        if (reason_out) *reason_out = "unknown backend";
        return -1;
    }
}

/* 把候选失败原因追加到原因串（用于“无可用后端”的提示） */
static void append_probe_reason(char *buf, size_t buf_size, int backend,
                                 const char *reason)
{
    if (!buf || buf_size == 0) return;
    size_t used = strlen(buf);
    if (used + 1 >= buf_size) return;
    snprintf(buf + used, buf_size - used, "%s%s: %s",
             used ? "; " : "",
             audio_backend_display_name(backend),
             reason ? reason : "unknown");
}

/* 后端仍持有、尚未真正播出的帧数：position（进度/断点恢复）必须扣除它。
 * 目前只有 PipeWire 的 1MB 环形缓冲会累计到秒级；Pulse/ALSA 的写入路径
 * 本身受设备可写空间限制，缓冲深度很小，返回 0 即可。 */
int audio_backend_buffered_frames(void)
{
    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        return pw_ring_buffered_frames();
    }
    return 0;
}

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

/* 单个后端的建流（不含回退逻辑）：成功返回 0，失败返回 -1 并给出状态提示 */
static int prepare_stream_for_backend(int backend, int sample_rate, int channels)
{
    unsigned int latency_usec = (unsigned int)get_configured_latency_ms() * 1000U;
    unsigned int minreq_usec = latency_usec / 4U;
    if (minreq_usec < 5000U) minreq_usec = 5000U;
    unsigned int max_latency_usec = latency_usec * 2U;

    if (pulse_loaded && backend == AUDIO_BACKEND_PULSE) {
        if (!pa_connected || !pa_ctx) {
            core_status_push(i18n_get("audio.err.pulseaudio_disconnected"));
            return -1;
        }

        pa_ss.format = PA_SAMPLE_S32LE;
        pa_ss.rate = sample_rate;
        pa_ss.channels = (uint8_t)channels;

        pa_stream *new_stream = P.stream_new(pa_ctx, "playback", &pa_ss, NULL);
        if (!new_stream) {
            core_status_push(i18n_get("audio.err.pulseaudio_create_stream"));
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
            core_status_push(i18n_get("audio.err.pulseaudio_connect_stream"));
            P.stream_unref(new_stream);
            return -1;
        }

        while (P.stream_get_state(new_stream) != PA_STREAM_READY) {
            if (P.stream_get_state(new_stream) == PA_STREAM_FAILED ||
                P.stream_get_state(new_stream) == PA_STREAM_TERMINATED) {
                core_status_push(i18n_get("audio.err.pulseaudio_init_failed"));
                P.stream_unref(new_stream);
                return -1;
            }
            P.mainloop_iterate(pa_ml, 1, NULL);
        }

        pa_s = new_stream;
        return 0;
    }

    if (alsa_loaded && backend == AUDIO_BACKEND_ALSA) {
        /* 未探测过（或上次探测失败）时先探测一次：探测通过才允许建流 */
        if (!alsa_ready) {
            const char *reason = NULL;
            if (audio_backend_probe(AUDIO_BACKEND_ALSA, &reason) != 0) {
                log_warn("audio", "ALSA device probe failed: %s", reason ? reason : "unknown");
                core_status_push(i18n_get("audio.err.alsa_not_ready"));
                return -1;
            }
        }

        extern char g_default_audio_device[128];
        if (A.pcm_open(&alsa_pcm, g_default_audio_device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            core_status_push(i18n_get("audio.err.alsa_open"));
            alsa_pcm = NULL;
            alsa_ready = 0;
            return -1;
        }

        if (A.pcm_set_params(alsa_pcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               (unsigned int)channels, (unsigned int)sample_rate, 1, latency_usec) < 0) {
            core_status_push(i18n_get("audio.err.alsa_config"));
            A.pcm_close(alsa_pcm);
            alsa_pcm = NULL;
            alsa_ready = 0;
            return -1;
        }
        alsa_paused = 0;
        return 0;
    }

    if (pipewire_loaded && backend == AUDIO_BACKEND_PIPEWIRE) {
        if (pw_prepare_stream(sample_rate, channels) == 0) return 0;
        pw_cleanup_stream_locked();
        return -1;
    }

    return -1;
}

int audio_backend_prepare_stream(int sample_rate, int channels)
{
    g_output_sample_rate = sample_rate;
    g_output_channels = channels;

    int current = g_active_backend;
    if (current != AUDIO_BACKEND_AUTO) {
        if (prepare_stream_for_backend(current, sample_rate, channels) == 0)
            return 0;
        log_warn("audio", "Backend %s could not open the stream, looking for a fallback",
                 audio_backend_display_name(current));
    }

    /* 回退候选顺序：用户显式指定的后端优先，其余按偏好顺序 */
    int candidates[BACKEND_CANDIDATE_COUNT + 1];
    int candidate_count = 0;
    if (g_requested_backend != AUDIO_BACKEND_AUTO &&
        g_requested_backend != current)
        candidates[candidate_count++] = g_requested_backend;
    for (int i = 0; i < BACKEND_CANDIDATE_COUNT; i++) {
        int backend = g_backend_candidates[i];
        if (backend == current || backend == g_requested_backend) continue;
        candidates[candidate_count++] = backend;
    }

    char failed_reason[256];
    failed_reason[0] = '\0';

    for (int i = 0; i < candidate_count; i++) {
        int candidate = candidates[i];

        const char *reason = NULL;
        if (audio_backend_probe(candidate, &reason) != 0) {
            log_warn("audio", "Fallback candidate %s unavailable: %s",
                     audio_backend_display_name(candidate), reason ? reason : "unknown");
            append_probe_reason(failed_reason, sizeof(failed_reason), candidate, reason);
            continue;
        }

        /* 切换后端前先清理上一个后端遗留的流 */
        audio_backend_cleanup_stream();
        g_active_backend = candidate;
        if (prepare_stream_for_backend(candidate, sample_rate, channels) == 0) {
            int from = (current != AUDIO_BACKEND_AUTO) ? current : g_requested_backend;
            char msg[256];
            audio_backend_format_message(msg, sizeof(msg), i18n_get("audio.backend.fallback"),
                                         audio_backend_display_name(from),
                                         audio_backend_display_name(candidate), NULL);
            core_status_push(msg);
            log_info("audio", "Audio backend switched: %s", msg);
            return 0;
        }
        log_warn("audio", "Fallback candidate %s could not open the stream",
                 audio_backend_display_name(candidate));
    }

    /* 全部候选失败：恢复原后端状态并给出具体原因 */
    g_active_backend = current;
    char msg[256];
    audio_backend_format_message(msg, sizeof(msg), i18n_get("audio.err.backend_select_failed"),
                                 NULL, NULL,
                                 failed_reason[0] ? failed_reason : "no candidate available");
    core_status_push(msg);
    log_error("audio", "No usable audio backend: %s",
              failed_reason[0] ? failed_reason : "no candidate available");
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

/* 单调时钟毫秒数（写入等待预算用） */
static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* 等待“播放恢复或线程被要求停止”。
 * 返回 1 = 线程仍在运行（可继续写），0 = 线程已被要求停止（应正常退出）。 */
static int wait_for_playback_resume(int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&g_play_mutex);
    pthread_cond_timedwait(&g_play_control_cond, &g_play_mutex, &ts);
    int running = g_play_thread_running;
    pthread_mutex_unlock(&g_play_mutex);
    return running;
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

        /* 等待预算：设备此刻写不进（writei 返回 0 / -EAGAIN / -EPIPE）不是
         * 致命错误，预算内继续等待设备可写；预算耗尽仍写不进才判真错误。 */
        int64_t deadline = monotonic_ms() + ALSA_WRITE_BUDGET_MS;
        int paused_seen = alsa_paused;

        while (written < frame_count) {
            extern int g_play_thread_running;
            if (!g_play_thread_running) return 0;
            if (!alsa_pcm) return -1;

            if (A.pcm_wait(alsa_pcm, wait_timeout_ms) < 0) A.pcm_prepare(alsa_pcm);

            snd_pcm_sframes_t ret = A.pcm_writei(alsa_pcm,
                                                 samples + (written * g_output_channels),
                                                 (snd_pcm_uframes_t)(frame_count - written));

            /* 暂停/恢复状态变化 → 重新给一次等待预算 */
            if (alsa_paused != paused_seen) {
                paused_seen = alsa_paused;
                deadline = monotonic_ms() + ALSA_WRITE_BUDGET_MS;
            }

            if (ret > 0) {
                written += (int)ret;
                deadline = monotonic_ms() + ALSA_WRITE_BUDGET_MS;
                continue;
            }

            if (ret == 0) {
                /* PCM 处于暂停态时 writei 返回 0（不是错误码）：此时等待
                 * 播放恢复，而不是判定为写失败。 */
                if (alsa_paused) {
                    if (!wait_for_playback_resume(ALSA_WRITE_PAUSE_WAIT_MS)) return 0;
                    deadline = monotonic_ms() + ALSA_WRITE_BUDGET_MS;
                    continue;
                }
                usleep(ALSA_WRITE_RETRY_SLEEP_US);
            } else if (ret == -EAGAIN) {
                usleep(ALSA_WRITE_RETRY_SLEEP_US);
            } else if (ret == -EPIPE || ret == -ESTRPIPE) {
                A.pcm_prepare(alsa_pcm);
                usleep(ALSA_WRITE_RETRY_SLEEP_US);
            } else {
                log_error("audio", "ALSA writei failed: %s", alsa_error_text((int)ret));
                return -1;
            }

            if (monotonic_ms() >= deadline) {
                log_error("audio", "ALSA device not writable within %dms",
                          ALSA_WRITE_BUDGET_MS);
                return -1;
            }
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
    if (alsa_loaded && g_active_backend == AUDIO_BACKEND_ALSA) {
        /* 应用层暂停语义优先：设备可能不支持 snd_pcm_pause（-ENOSYS/-EBADFD），
         * 但暂停标志必须置位——写入循环据此把“写不进”当成等待恢复而非错误。 */
        alsa_paused = 1;
        if (alsa_pcm) {
            int err = A.pcm_pause(alsa_pcm, 1);
            if (err < 0)
                log_warn("audio", "ALSA pause not supported by device: %s",
                         alsa_error_text(err));
        }
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
    if (alsa_loaded && g_active_backend == AUDIO_BACKEND_ALSA) {
        alsa_paused = 0;
        if (alsa_pcm) {
            int err = A.pcm_pause(alsa_pcm, 0);
            if (err < 0)
                log_warn("audio", "ALSA resume not supported by device: %s",
                         alsa_error_text(err));
        }
        return;
    }
    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        pw_resume_stream();
        return;
    }
}
