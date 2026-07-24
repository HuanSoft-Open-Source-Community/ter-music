/**
 * @file pipewire.c
 * @brief PipeWire audio backend — runtime dlopen, ring buffer, stream lifecycle
 *
 * Handles all PipeWire-specific functionality including dynamic loading,
 * SPA pod building, SPSC ring buffer, stream callbacks, and stream
 * lifecycle management (prepare, cleanup, write, pause, resume).
 *
 * @author ter-music team
 */

#include "audio/audio_internal.h"
#include <ncursesw/ncurses.h>
#include "ui/ui.h"
#include "i18n/i18n.h"
#include "logger/logger.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>

/* ================================================================== */
/*  PipeWire 状态变量                                                 */
/*  （定义在此文件，audio_internal.h 中作 extern 声明）                */
/* ================================================================== */

struct pw_thread_loop *pw_loop = NULL;
struct pw_main_loop  *pw_mainloop = NULL;
struct pw_context    *pw_ctx = NULL;
struct pw_core       *pw_core = NULL;
struct pw_stream     *pw_s = NULL;
struct spa_hook       pw_stream_listener;
int pw_stream_ready = 0;
int pw_stream_connecting = 0;
int pw_channels = 2;
int pw_sample_rate = 44100;
int pw_write_underrun = 0;
volatile int pw_stream_destroying = 0;
pthread_mutex_t pw_state_mutex = PTHREAD_MUTEX_INITIALIZER;
int pipewire_loaded = 0;

/* ================================================================== */
/*  环形缓冲区（SPSC，线程安全）                                       */
/* ================================================================== */

#define PW_RING_SIZE (1024 * 1024)          /* 1 MB */
static uint8_t pw_ring_data[PW_RING_SIZE];
static volatile int pw_ring_write_pos = 0;
static volatile int pw_ring_read_pos = 0;
static pthread_mutex_t pw_ring_mutex = PTHREAD_MUTEX_INITIALIZER;
#define PW_RING_MASK (PW_RING_SIZE - 1)

static int pw_ring_bytes_writable(void) {
    int write = pw_ring_write_pos;
    int read  = pw_ring_read_pos;
    int used = write - read;
    if (used < 0) used += PW_RING_SIZE;
    return PW_RING_SIZE - 1 - used;
}

static int pw_ring_bytes_available(void) {
    int write = pw_ring_write_pos;
    int read  = pw_ring_read_pos;
    int used = write - read;
    if (used < 0) used += PW_RING_SIZE;
    return used;
}

static int pw_ring_write(const uint8_t *data, int bytes) {
    int written = 0;
    pthread_mutex_lock(&pw_ring_mutex);
    int avail = pw_ring_bytes_writable();
    if (avail <= 0) {
        pthread_mutex_unlock(&pw_ring_mutex);
        return 0;
    }
    if (bytes > avail) bytes = avail;

    int write = pw_ring_write_pos;
    int first = PW_RING_SIZE - (write & PW_RING_MASK);
    if (first > bytes) first = bytes;
    memcpy(&pw_ring_data[write & PW_RING_MASK], data, (size_t)first);
    if (first < bytes) {
        memcpy(pw_ring_data, data + first, (size_t)(bytes - first));
    }
    pw_ring_write_pos = write + bytes;
    written = bytes;
    pthread_mutex_unlock(&pw_ring_mutex);
    return written;
}

static int pw_ring_read(uint8_t *data, int bytes) {
    int read_bytes = 0;
    pthread_mutex_lock(&pw_ring_mutex);
    int avail = pw_ring_bytes_available();
    if (avail <= 0) {
        pthread_mutex_unlock(&pw_ring_mutex);
        return 0;
    }
    if (bytes > avail) bytes = avail;

    int read = pw_ring_read_pos;
    int first = PW_RING_SIZE - (read & PW_RING_MASK);
    if (first > bytes) first = bytes;
    memcpy(data, &pw_ring_data[read & PW_RING_MASK], (size_t)first);
    if (first < bytes) {
        memcpy(data + first, pw_ring_data, (size_t)(bytes - first));
    }
    pw_ring_read_pos = read + bytes;
    read_bytes = bytes;
    pthread_mutex_unlock(&pw_ring_mutex);
    return read_bytes;
}

static void pw_ring_reset(void) {
    pthread_mutex_lock(&pw_ring_mutex);
    pw_ring_write_pos = 0;
    pw_ring_read_pos = 0;
    pthread_mutex_unlock(&pw_ring_mutex);
}

/* ================================================================== */
/*  dlopen / dlsym 宏                                                 */
/* ================================================================== */

#define PIPEWIRE_LOAD(name) do { \
    *(void **)(&PW.name) = dlsym(PW.handle, "pw_" #name); \
    if (!PW.name) { \
        fprintf(stderr, "dlsym(pw_" #name ") failed: %s\n", dlerror()); \
        dlclose(PW.handle); \
        PW.handle = NULL; \
        return -1; \
    } \
} while(0)

/* ================================================================== */
/*  Load / Unload                                                     */
/* ================================================================== */

int pipewire_load(void) {
    if (PW.loaded) return 0;
    PW.handle = dlopen(PIPEWIRE_SONAME, RTLD_LAZY | RTLD_LOCAL);
    if (!PW.handle) return -1;

    PIPEWIRE_LOAD(init);
    PIPEWIRE_LOAD(deinit);

    PIPEWIRE_LOAD(main_loop_new);
    PIPEWIRE_LOAD(main_loop_destroy);
    PIPEWIRE_LOAD(main_loop_run);
    PIPEWIRE_LOAD(main_loop_quit);

    PIPEWIRE_LOAD(thread_loop_new);
    PIPEWIRE_LOAD(thread_loop_destroy);
    PIPEWIRE_LOAD(thread_loop_start);
    PIPEWIRE_LOAD(thread_loop_stop);
    PIPEWIRE_LOAD(thread_loop_lock);
    PIPEWIRE_LOAD(thread_loop_unlock);
    PIPEWIRE_LOAD(thread_loop_signal);
    PIPEWIRE_LOAD(thread_loop_get_loop);

    PIPEWIRE_LOAD(context_new);
    PIPEWIRE_LOAD(context_destroy);
    PIPEWIRE_LOAD(context_connect);

    PIPEWIRE_LOAD(core_disconnect);

    PIPEWIRE_LOAD(properties_new);
    PIPEWIRE_LOAD(properties_free);
    PIPEWIRE_LOAD(properties_set);
    PIPEWIRE_LOAD(properties_get);

    PIPEWIRE_LOAD(stream_new);
    PIPEWIRE_LOAD(stream_new_simple);
    PIPEWIRE_LOAD(stream_destroy);
    PIPEWIRE_LOAD(stream_connect);
    PIPEWIRE_LOAD(stream_disconnect);
    PIPEWIRE_LOAD(stream_dequeue_buffer);
    PIPEWIRE_LOAD(stream_queue_buffer);
    PIPEWIRE_LOAD(stream_set_active);
    PIPEWIRE_LOAD(stream_set_control);
    PIPEWIRE_LOAD(stream_flush);
    PIPEWIRE_LOAD(stream_get_state);
    PIPEWIRE_LOAD(stream_add_listener);
    PIPEWIRE_LOAD(stream_set_error);
    PIPEWIRE_LOAD(stream_update_params);

    PW.init(NULL, NULL);
    PW.loaded = 1;
    return 0;
}

void pipewire_unload(void) {
    if (PW.loaded) {
        PW.deinit();
        PW.loaded = 0;
    }
    if (PW.handle) {
        dlclose(PW.handle);
        PW.handle = NULL;
    }
}

/* ================================================================== */
/*  SPA Pod Builder (inline)                                          */
/* ================================================================== */

int build_audio_format_pod(void *dst, int sample_rate, int channels) {
    uint32_t *p = (uint32_t *)dst;
    uint32_t body_size = 128;
    uint32_t audio_format = SPA_AUDIO_FORMAT_S32;

#define POD_SET(w)   do { *p++ = (uint32_t)(w); } while(0)

    POD_SET(body_size);
    POD_SET(SPA_TYPE_Object);
    POD_SET(SPA_PARAM_Format);
    POD_SET(0);

    POD_SET(SPA_FORMAT_mediaType);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Id);  POD_SET(SPA_MEDIA_TYPE_audio);  POD_SET(0);

    POD_SET(SPA_FORMAT_mediaSubtype);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Id);  POD_SET(SPA_MEDIA_SUBTYPE_raw);  POD_SET(0);

    POD_SET(SPA_FORMAT_AUDIO_format);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Id);  POD_SET(audio_format);  POD_SET(0);

    POD_SET(SPA_FORMAT_AUDIO_rate);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Int);  POD_SET((uint32_t)sample_rate);  POD_SET(0);

    POD_SET(SPA_FORMAT_AUDIO_channels);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Int);  POD_SET((uint32_t)channels);  POD_SET(0);

#undef POD_SET
    return 136;
}

/* ================================================================== */
/*  Stream Callbacks                                                   */
/* ================================================================== */

/* Forward declarations */
static void pw_stream_state_change_locked(int ready);

static void pw_stream_on_state_changed(void *userdata,
                                        enum pw_stream_state old,
                                        enum pw_stream_state state,
                                        const char *error) {
    (void)userdata; (void)old;
    pthread_mutex_lock(&pw_state_mutex);
    if (state == PW_STREAM_STATE_PAUSED ||
        state == PW_STREAM_STATE_STREAMING) {
        pw_stream_ready = 1;
        pw_stream_connecting = 0;
    } else if (state == PW_STREAM_STATE_ERROR) {
        log_warn("audio", "PipeWire stream error: %s", error ? error : "unknown");
        pw_stream_ready = 0;
        pw_stream_connecting = 0;
    }
    pthread_mutex_unlock(&pw_state_mutex);
}

static void pw_stream_on_process(void *userdata) {
    (void)userdata;

    pthread_mutex_lock(&pw_state_mutex);
    int still_connecting = pw_stream_connecting;
    int destroying = pw_stream_destroying;
    struct pw_stream *s = pw_s;
    pthread_mutex_unlock(&pw_state_mutex);
    if (still_connecting || destroying || !s) return;

    struct pw_buffer *buf = NULL;
    if (PW.stream_dequeue_buffer(s, &buf) < 0 || !buf) return;

    struct spa_buffer *spa_buf = buf->buffer;
    if (spa_buf->n_datas < 1) {
        PW.stream_queue_buffer(s, buf);
        return;
    }

    struct spa_data *data = SPA_BUFFER_DATA(spa_buf, 0);
    uint8_t *dst = (uint8_t *)data->data;
    int max_bytes = (int)data->maxsize;

    int copied = pw_ring_read(dst, max_bytes);
    if (copied <= 0) {
        memset(dst, 0, (size_t)max_bytes);
        data->chunk->offset = 0;
        data->chunk->stride = (uint32_t)(pw_channels * (int)sizeof(int32_t));
        data->chunk->size = (uint32_t)max_bytes;
        PW.stream_queue_buffer(s, buf);
        pw_write_underrun = 1;
        return;
    }

    data->chunk->offset = 0;
    data->chunk->stride = (uint32_t)(pw_channels * (int)sizeof(int32_t));
    data->chunk->size = (uint32_t)copied;
    PW.stream_queue_buffer(s, buf);
}

static void pw_stream_on_param_changed(void *userdata, uint32_t id,
                                        const struct spa_pod *param) {
    (void)userdata; (void)id; (void)param;
}

#define PW_STREAM_EVENTS_VERSION 2

static const struct pw_stream_events pw_stream_callbacks = {
    .version = PW_STREAM_EVENTS_VERSION,
    .destroy = NULL,
    .state_changed = pw_stream_on_state_changed,
    .param_changed = pw_stream_on_param_changed,
    .process = pw_stream_on_process,
    .drained = NULL,
    .control_info = NULL,
    .io_changed = NULL,
};

/* ================================================================== */
/*  Probing                                                            */
/* ================================================================== */

int pw_probe_backend(void) {
    struct pw_thread_loop *loop = NULL;
    struct pw_context *ctx = NULL;
    struct pw_core *core = NULL;

    loop = PW.thread_loop_new("ter-music-probe", NULL);
    if (!loop) return -1;

    ctx = PW.context_new(PW.thread_loop_get_loop(loop), NULL, 0);
    if (!ctx) { PW.thread_loop_destroy(loop); return -1; }

    core = PW.context_connect(ctx, NULL, 0);
    if (!core) { PW.context_destroy(ctx); PW.thread_loop_destroy(loop); return -1; }

    PW.core_disconnect(core);
    PW.context_destroy(ctx);
    PW.thread_loop_destroy(loop);
    return 0;
}

/* ================================================================== */
/*  Stream Preparation                                                */
/* ================================================================== */

/* Forward reference to shared helper from audio.c */
int get_configured_latency_ms(void);

int pw_prepare_stream(int sample_rate, int channels) {
    pw_channels = channels;
    pw_sample_rate = sample_rate;
    pw_stream_ready = 0;
    pw_stream_connecting = 1;
    pw_stream_destroying = 0;
    pw_write_underrun = 0;
    pw_ring_reset();

    unsigned int latency_usec = (unsigned int)get_configured_latency_ms() * 1000U;
    unsigned int buf_size = (unsigned int)((uint64_t)sample_rate * (uint64_t)channels *
                                           (uint64_t)sizeof(int32_t) * latency_usec / 1000000U);
    if (buf_size < 65536) buf_size = 65536;
    if (buf_size > PW_RING_SIZE / 2) buf_size = PW_RING_SIZE / 2;

    pw_loop = PW.thread_loop_new("ter-music-pw", NULL);
    if (!pw_loop) {
        update_controls_status(i18n_get("audio.pw.create_loop"));
        return -1;
    }
    pw_mainloop = PW.thread_loop_get_loop(pw_loop);

    pw_ctx = PW.context_new(pw_mainloop, NULL, 0);
    if (!pw_ctx) {
        update_controls_status(i18n_get("audio.pw.create_context"));
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    pw_core = PW.context_connect(pw_ctx, NULL, 0);
    if (!pw_core) {
        update_controls_status(i18n_get("audio.pw.connect"));
        PW.context_destroy(pw_ctx); pw_ctx = NULL;
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    pw_s = PW.stream_new(pw_core, "ter-music", NULL);
    if (!pw_s) {
        update_controls_status(i18n_get("audio.pw.create_stream"));
        PW.core_disconnect(pw_core); pw_core = NULL;
        PW.context_destroy(pw_ctx); pw_ctx = NULL;
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    PW.stream_add_listener(pw_s, &pw_stream_listener, &pw_stream_callbacks, NULL);

    uint8_t pod_buf[256];
    int pod_len = build_audio_format_pod(pod_buf, sample_rate, channels);
    (void)pod_len;
    const struct spa_pod *params[1];
    params[0] = (const struct spa_pod *)pod_buf;

    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT |
                     PW_STREAM_FLAG_MAP_BUFFERS |
                     PW_STREAM_FLAG_DONT_RECONNECT;
    if (PW.stream_connect(pw_s, PW_DIRECTION_OUTPUT, 0, flags, params, 1) < 0) {
        update_controls_status(i18n_get("audio.pw.connect_stream"));
        PW.stream_destroy(pw_s); pw_s = NULL;
        PW.core_disconnect(pw_core); pw_core = NULL;
        PW.context_destroy(pw_ctx); pw_ctx = NULL;
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    if (PW.thread_loop_start(pw_loop) < 0) {
        update_controls_status(i18n_get("audio.pw.start_thread"));
        PW.stream_destroy(pw_s); pw_s = NULL;
        PW.core_disconnect(pw_core); pw_core = NULL;
        PW.context_destroy(pw_ctx); pw_ctx = NULL;
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    /* Wait for stream ready (with timeout) */
    int timeout_ms = 5000;
    while (timeout_ms > 0) {
        pthread_mutex_lock(&pw_state_mutex);
        int ready = pw_stream_ready;
        int err = 0;
        if (!ready && !pw_stream_connecting && pw_s) {
            const char *e = NULL;
            PW.stream_get_state(pw_s, &e);
            if (e && e[0] != '\0') err = 1;
        }
        pthread_mutex_unlock(&pw_state_mutex);
        if (ready) break;
        if (err) { timeout_ms = 0; break; }
        usleep(1000);
        timeout_ms--;
    }

    /* Post-check for race condition */
    pthread_mutex_lock(&pw_state_mutex);
    int is_ready = pw_stream_ready;
    if (is_ready && pw_s) {
        int verify_ms = 300;
        pthread_mutex_unlock(&pw_state_mutex);
        while (verify_ms > 0) {
            const char *err_str = NULL;
            enum pw_stream_state st = PW.stream_get_state(pw_s, &err_str);
            if (st == PW_STREAM_STATE_ERROR) {
                is_ready = 0;
                log_warn("audio", "PipeWire post-check: stream error: %s",
                         err_str ? err_str : "unknown");
                break;
            }
            if (st == PW_STREAM_STATE_STREAMING) break;
            usleep(1000);
            verify_ms--;
        }
        if (pw_s) {
            const char *err_str = NULL;
            enum pw_stream_state fs = PW.stream_get_state(pw_s, &err_str);
            if (fs != PW_STREAM_STATE_STREAMING && fs != PW_STREAM_STATE_ERROR) {
                log_warn("audio", "PipeWire stream stuck at PAUSED (no target node?)");
                is_ready = 0;
            }
        }
    } else {
        pthread_mutex_unlock(&pw_state_mutex);
    }

    if (!is_ready) {
        update_controls_status(i18n_get("audio.pw.stream_not_ready"));
        goto pw_cleanup_and_fail;
    }

    return 0;

pw_cleanup_and_fail:
    pthread_mutex_lock(&pw_state_mutex);
    pw_stream_destroying = 1;
    pthread_mutex_unlock(&pw_state_mutex);

    if (pw_loop) PW.thread_loop_lock(pw_loop);
    if (pw_s) PW.stream_set_active(pw_s, 0);
    if (pw_loop) PW.thread_loop_unlock(pw_loop);
    if (pw_loop) PW.thread_loop_stop(pw_loop);

    pthread_mutex_lock(&pw_state_mutex);
    pw_s = NULL; pw_core = NULL; pw_ctx = NULL;
    pw_loop = NULL; pw_mainloop = NULL;
    pw_stream_ready = 0; pw_stream_connecting = 0;
    pthread_mutex_unlock(&pw_state_mutex);
    pw_ring_reset();
    return -1;
}

/* ================================================================== */
/*  Stream Control                                                     */
/* ================================================================== */

void pw_cleanup_stream_locked(void) {
    if (!pw_s && !pw_core && !pw_loop) {
        pw_stream_ready = 0;
        pw_stream_connecting = 0;
        pw_ring_reset();
        return;
    }

    /* PipeWire 1.6.0 workaround: skip PW destroy calls (see audio.c for details) */
    pthread_mutex_lock(&pw_state_mutex);
    pw_stream_destroying = 1;
    pthread_mutex_unlock(&pw_state_mutex);

    if (pw_loop) PW.thread_loop_lock(pw_loop);
    if (pw_s) PW.stream_set_active(pw_s, 0);
    if (pw_loop) PW.thread_loop_unlock(pw_loop);
    if (pw_loop) PW.thread_loop_stop(pw_loop);

    pthread_mutex_lock(&pw_state_mutex);
    pw_s = NULL; pw_core = NULL; pw_ctx = NULL;
    pw_loop = NULL; pw_mainloop = NULL;
    pw_stream_ready = 0; pw_stream_connecting = 0;
    pthread_mutex_unlock(&pw_state_mutex);
    pw_ring_reset();
}

void pw_cleanup_stream(void) {
    pw_cleanup_stream_locked();
}

void pw_flush_stream(void) {
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    pthread_mutex_unlock(&pw_state_mutex);
    if (ready && s) PW.stream_flush(s, 0);
    pw_ring_reset();
}

int pw_write_samples(const int32_t *samples, int frame_count) {
    if (!samples || frame_count <= 0) return 0;

    pthread_mutex_lock(&pw_state_mutex);
    int stream_ok = pw_stream_ready;
    pthread_mutex_unlock(&pw_state_mutex);
    if (!stream_ok) return -1;

    int bytes = frame_count * pw_channels * (int)sizeof(int32_t);
    int total_written = 0;

    while (total_written < bytes) {
        int chunk = bytes - total_written;
        if (chunk > 65536) chunk = 65536;

        int written = pw_ring_write((const uint8_t *)samples + total_written, chunk);
        if (written <= 0) {
            if (!g_play_thread_running) return 0;
            pthread_mutex_lock(&pw_state_mutex);
            stream_ok = pw_stream_ready;
            pthread_mutex_unlock(&pw_state_mutex);
            if (!stream_ok) return -1;
            usleep(1000);
            continue;
        }
        total_written += written;
    }
    return 0;
}

void pw_pause_stream(void) {
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    pthread_mutex_unlock(&pw_state_mutex);
    if (ready && s) PW.stream_set_active(s, 0);
}

void pw_resume_stream(void) {
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    pthread_mutex_unlock(&pw_state_mutex);
    if (ready && s) PW.stream_set_active(s, 1);
}

void pw_sync_volume(int volume) {
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    pthread_mutex_unlock(&pw_state_mutex);
    if (ready && s) {
        float vol_linear = (float)volume / 100.0f;
        PW.stream_set_control(s, PW_STREAM_CONTROL_VOLUME, vol_linear);
    }
}
