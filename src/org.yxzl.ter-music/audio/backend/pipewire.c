/**
 * @file pipewire.c
 * @brief PipeWire audio backend — runtime dlopen, ring buffer, stream lifecycle
 *
 * Handles all PipeWire-specific functionality including dynamic loading,
 * SPA pod building, SPSC ring buffer, stream callbacks, and stream
 * lifecycle management (prepare, cleanup, write, pause, resume).
 *
 * 所有 PipeWire/SPA 结构体与常量都来自 pipewire.h 中的手写 ABI 兼容层，
 * 构建时不依赖系统 pipewire/spa 头文件。
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

/* 保护“调用 PipeWire API + 流生命周期”的互斥锁。
 * 与 pw_state_mutex 分开：process 回调会拿 pw_state_mutex，若在持有
 * pw_state_mutex 时去等 thread_loop_lock 会与循环线程互相等待。
 * 所有从非循环线程发起的 API 调用都在此锁内完成，销毁流程也持有它，
 * 从而避免“读到指针后流被销毁”的竞态。 */
static pthread_mutex_t pw_api_mutex = PTHREAD_MUTEX_INITIALIZER;

/* thread loop 是否已经 start 成功：只有启动过的 loop 才能 lock/stop，
 * 否则在失败清理路径上 thread_loop_lock 会永久阻塞。 */
static int pw_thread_running = 0;

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

/*
 * 对象布局（与 spa_pod_builder 的输出逐字节一致）：
 *   header: size(4) type(4)                      —— size 只统计 body
 *   body  : object_type(4) object_id(4)
 *   属性  : key(4) flags(4) value_pod(16)
 *           value_pod = size(4) type(4) value(4) padding(4)
 * 因此单个属性占 24 字节，5 个属性时 body = 8 + 120 = 128，总计 136 字节。
 *
 * 注意 object_id 必须是 SPA_PARAM_EnumFormat（3）而不是 SPA_PARAM_Format（4）：
 * pw_stream_connect() 的 params 表示“客户端支持的格式”，PipeWire 内部的
 * find_format() 只枚举 SPA_PARAM_EnumFormat 来决定 media_type 并创建 adapter。
 * 传 SPA_PARAM_Format 时连接虽然返回 0，但不会创建 adapter，只暴露一个原始
 * interleaved 端口，节点无人连线并永远停在 PAUSED（实测 1.6.4）。
 */
int build_audio_format_pod(void *dst, int sample_rate, int channels) {
    if (!dst) return 0;

    uint32_t *p = (uint32_t *)dst;
    const uint32_t body_size = 8 + 5 * 24;

#define POD_PUT(v) do { *p++ = (uint32_t)(v); } while (0)

    /* spa_pod header + spa_pod_object_body */
    POD_PUT(body_size);
    POD_PUT(SPA_TYPE_Object);
    POD_PUT(SPA_TYPE_OBJECT_Format);
    POD_PUT(SPA_PARAM_EnumFormat);

    /* mediaType = audio (Id) */
    POD_PUT(SPA_FORMAT_mediaType);
    POD_PUT(0);
    POD_PUT(4); POD_PUT(SPA_TYPE_Id); POD_PUT(SPA_MEDIA_TYPE_audio); POD_PUT(0);

    /* mediaSubtype = raw (Id) */
    POD_PUT(SPA_FORMAT_mediaSubtype);
    POD_PUT(0);
    POD_PUT(4); POD_PUT(SPA_TYPE_Id); POD_PUT(SPA_MEDIA_SUBTYPE_raw); POD_PUT(0);

    /* audio.format = S32_LE (Id) */
    POD_PUT(SPA_FORMAT_AUDIO_format);
    POD_PUT(0);
    POD_PUT(4); POD_PUT(SPA_TYPE_Id); POD_PUT(SPA_AUDIO_FORMAT_S32_LE); POD_PUT(0);

    /* audio.rate (Int) */
    POD_PUT(SPA_FORMAT_AUDIO_rate);
    POD_PUT(0);
    POD_PUT(4); POD_PUT(SPA_TYPE_Int); POD_PUT((uint32_t)sample_rate); POD_PUT(0);

    /* audio.channels (Int) */
    POD_PUT(SPA_FORMAT_AUDIO_channels);
    POD_PUT(0);
    POD_PUT(4); POD_PUT(SPA_TYPE_Int); POD_PUT((uint32_t)channels); POD_PUT(0);

#undef POD_PUT

    return (int)(body_size + 8);
}

/* ================================================================== */
/*  Stream Callbacks                                                   */
/* ================================================================== */

static const char *pw_state_name(int state) {
    switch (state) {
        case PW_STREAM_STATE_ERROR:       return "error";
        case PW_STREAM_STATE_UNCONNECTED: return "unconnected";
        case PW_STREAM_STATE_CONNECTING:  return "connecting";
        case PW_STREAM_STATE_PAUSED:      return "paused";
        case PW_STREAM_STATE_STREAMING:   return "streaming";
        default:                          return "unknown";
    }
}

static void pw_stream_on_state_changed(void *userdata,
                                        enum pw_stream_state old,
                                        enum pw_stream_state state,
                                        const char *error) {
    (void)userdata;
    (void)old;

    pthread_mutex_lock(&pw_state_mutex);
    if (state == PW_STREAM_STATE_STREAMING) {
        if (!pw_stream_ready)
            log_info("audio", "PipeWire stream is streaming");
        pw_stream_ready = 1;
        pw_stream_connecting = 0;
    } else if (state == PW_STREAM_STATE_PAUSED) {
        /* PAUSED 只表示节点未激活，环形缓冲仍可继续填充 */
        pw_stream_ready = 1;
        pw_stream_connecting = 0;
    } else if (state == PW_STREAM_STATE_ERROR) {
        log_warn("audio", "PipeWire stream error: %s", error ? error : "unknown");
        pw_stream_ready = 0;
        pw_stream_connecting = 0;
    } else if (state == PW_STREAM_STATE_UNCONNECTED) {
        pw_stream_ready = 0;
        pw_stream_connecting = 0;
    }
    pthread_mutex_unlock(&pw_state_mutex);
}

static void pw_stream_on_process(void *userdata) {
    (void)userdata;

    pthread_mutex_lock(&pw_state_mutex);
    int destroying = pw_stream_destroying;
    struct pw_stream *s = pw_s;
    int channels = pw_channels;
    pthread_mutex_unlock(&pw_state_mutex);

    if (destroying || !s) return;

    /* 1.x 中 dequeue 直接返回 pw_buffer*；失败返回 NULL */
    struct pw_buffer *buf = PW.stream_dequeue_buffer(s);
    if (!buf) return;

    struct spa_buffer *spa_buf = buf->buffer;
    if (!spa_buf || spa_buf->n_datas < 1 || !spa_buf->datas) {
        PW.stream_queue_buffer(s, buf);
        return;
    }

    struct spa_data *data = &spa_buf->datas[0];
    uint8_t *dst = (uint8_t *)data->data;
    if (!dst || !data->chunk || data->maxsize == 0) {
        PW.stream_queue_buffer(s, buf);
        return;
    }

    uint32_t frame_bytes = (uint32_t)((channels > 0 ? channels : 2) * (int)sizeof(int32_t));
    uint32_t max_bytes = data->maxsize;
    /* 只填满完整的帧，避免半帧错位 */
    uint32_t capacity = max_bytes - (max_bytes % frame_bytes);

    int copied = pw_ring_read(dst, (int)capacity);
    if (copied < 0) copied = 0;

    /* 数据不足的部分补静音，避免残留上一轮数据造成爆音 */
    if ((uint32_t)copied < max_bytes)
        memset(dst + copied, 0, (size_t)(max_bytes - (uint32_t)copied));

    data->chunk->offset = 0;
    data->chunk->size = (uint32_t)copied;
    data->chunk->stride = (int32_t)frame_bytes;
    data->chunk->flags = 0;

    /* size 用于 PipeWire 的时间统计：按“帧数”填写 */
    buf->size = (uint64_t)((uint32_t)copied / frame_bytes);

    PW.stream_queue_buffer(s, buf);

    if ((uint32_t)copied < capacity)
        pw_write_underrun = 1;
}

static void pw_stream_on_param_changed(void *userdata, uint32_t id,
                                        const struct spa_pod *param) {
    (void)userdata; (void)id; (void)param;
}

#define PW_STREAM_EVENTS_VERSION 2

/* 顺序必须与 pipewire/stream.h 1.x 的 pw_stream_events 一致 */
static const struct pw_stream_events pw_stream_callbacks = {
    .version = PW_STREAM_EVENTS_VERSION,
    .destroy = NULL,
    .state_changed = pw_stream_on_state_changed,
    .control_info = NULL,
    .io_changed = NULL,
    .param_changed = pw_stream_on_param_changed,
    .add_buffer = NULL,
    .remove_buffer = NULL,
    .process = pw_stream_on_process,
    .drained = NULL,
    .command = NULL,
    .trigger_done = NULL,
};

/* ================================================================== */
/*  Probing                                                            */
/* ================================================================== */

int pw_probe_backend(void) {
    struct pw_thread_loop *loop = NULL;
    struct pw_loop *pw_loop_api = NULL;
    struct pw_context *ctx = NULL;
    struct pw_core *core = NULL;

    loop = PW.thread_loop_new("ter-music-probe", NULL);
    if (!loop) {
        log_warn("audio", "PipeWire probe: cannot create thread loop");
        return -1;
    }

    pw_loop_api = PW.thread_loop_get_loop(loop);
    ctx = PW.context_new(pw_loop_api, NULL, 0);
    if (!ctx) {
        log_warn("audio", "PipeWire probe: cannot create context");
        PW.thread_loop_destroy(loop);
        return -1;
    }

    core = PW.context_connect(ctx, NULL, 0);
    if (!core) {
        log_warn("audio", "PipeWire probe: cannot connect to daemon");
        PW.context_destroy(ctx);
        PW.thread_loop_destroy(loop);
        return -1;
    }

    PW.core_disconnect(core);
    PW.context_destroy(ctx);
    PW.thread_loop_destroy(loop);
    return 0;
}

/* ================================================================== */
/*  流生命周期                                                         */
/* ================================================================== */

/* Forward reference to shared helper from audio.c */
int get_configured_latency_ms(void);

/*
 * 统一销毁流程（prepare 失败清理与 pw_cleanup_stream_locked 共用）：
 *   标记 destroying → 加锁 loop 停用/断开 → 停 loop → 销毁 stream
 *   → 断开 core → 销毁 context → 销毁 thread loop → 置空
 *
 * 指针先在 pw_state_mutex 下摘除，process 回调看到 NULL 会立即返回；
 * 随后 thread_loop_stop() 会 join 循环线程，确保不再有回调在跑，
 * 因此可以安全地真正销毁对象（不再使用“只置空不销毁”的泄漏规避）。
 */
static void pw_teardown_stream(void) {
    pthread_mutex_lock(&pw_api_mutex);

    pthread_mutex_lock(&pw_state_mutex);
    struct pw_thread_loop *loop = pw_loop;
    struct pw_stream *s = pw_s;
    struct pw_core *core = pw_core;
    struct pw_context *ctx = pw_ctx;
    int loop_running = pw_thread_running;

    pw_stream_destroying = 1;
    pw_s = NULL;
    pw_core = NULL;
    pw_ctx = NULL;
    pw_loop = NULL;
    pw_mainloop = NULL;
    pw_thread_running = 0;
    pw_stream_ready = 0;
    pw_stream_connecting = 0;
    pthread_mutex_unlock(&pw_state_mutex);

    if (loop && loop_running) {
        PW.thread_loop_lock(loop);
        if (s) {
            PW.stream_set_active(s, 0);
            PW.stream_disconnect(s);
        }
        PW.thread_loop_unlock(loop);
        PW.thread_loop_stop(loop);
    }

    if (s) PW.stream_destroy(s);
    if (core) PW.core_disconnect(core);
    if (ctx) PW.context_destroy(ctx);
    if (loop) PW.thread_loop_destroy(loop);

    pw_ring_reset();
    pthread_mutex_unlock(&pw_api_mutex);
}

int pw_prepare_stream(int sample_rate, int channels) {
    /* 每次 prepare 前确保旧流已完全销毁，避免残留节点与句柄 */
    pw_teardown_stream();

    if (sample_rate <= 0) sample_rate = 44100;
    if (channels <= 0) channels = 2;

    pthread_mutex_lock(&pw_state_mutex);
    pw_channels = channels;
    pw_sample_rate = sample_rate;
    pw_stream_ready = 0;
    pw_stream_connecting = 1;
    pw_stream_destroying = 0;
    pw_write_underrun = 0;
    pthread_mutex_unlock(&pw_state_mutex);
    pw_ring_reset();

    int latency_ms = get_configured_latency_ms();
    if (latency_ms <= 0) latency_ms = 20;
    int latency_frames = sample_rate * latency_ms / 1000;
    if (latency_frames < 1) latency_frames = 1;

    char rate_str[64];
    char latency_str[64];
    snprintf(rate_str, sizeof(rate_str), "1/%d", sample_rate);
    snprintf(latency_str, sizeof(latency_str), "%d/%d", latency_frames, sample_rate);

    pw_loop = PW.thread_loop_new("ter-music-pw", NULL);
    if (!pw_loop) {
        log_warn("audio", "PipeWire prepare: cannot create thread loop");
        update_controls_status(i18n_get("audio.pw.create_loop"));
        goto fail;
    }

    struct pw_loop *loop_api = PW.thread_loop_get_loop(pw_loop);
    pw_mainloop = (struct pw_main_loop *)loop_api;

    pw_ctx = PW.context_new(loop_api, NULL, 0);
    if (!pw_ctx) {
        log_warn("audio", "PipeWire prepare: cannot create context");
        update_controls_status(i18n_get("audio.pw.create_context"));
        goto fail;
    }

    pw_core = PW.context_connect(pw_ctx, NULL, 0);
    if (!pw_core) {
        log_warn("audio", "PipeWire prepare: cannot connect to daemon");
        update_controls_status(i18n_get("audio.pw.connect"));
        goto fail;
    }

    /* properties 所有权交给 pw_stream_new()，失败路径不能再 free */
    struct pw_properties *props = PW.properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, "ter-music",
        PW_KEY_APP_NAME, "ter-music",
        PW_KEY_NODE_RATE, rate_str,
        PW_KEY_NODE_LATENCY, latency_str,
        NULL);
    if (!props) {
        log_warn("audio", "PipeWire prepare: cannot create properties");
        update_controls_status(i18n_get("audio.pw.create_stream"));
        goto fail;
    }

    pw_s = PW.stream_new(pw_core, "ter-music", props);
    if (!pw_s) {
        log_warn("audio", "PipeWire prepare: cannot create stream");
        update_controls_status(i18n_get("audio.pw.create_stream"));
        goto fail;
    }

    PW.stream_add_listener(pw_s, &pw_stream_listener, &pw_stream_callbacks, NULL);

    /* uint64_t 存储保证 8 字节对齐，满足 spa_pod 的对齐要求 */
    uint64_t pod_storage[32];
    int pod_len = build_audio_format_pod(pod_storage, sample_rate, channels);
    if (pod_len <= 0) {
        log_warn("audio", "PipeWire prepare: cannot build format pod");
        update_controls_status(i18n_get("audio.pw.connect_stream"));
        goto fail;
    }
    const struct spa_pod *params[1];
    params[0] = (const struct spa_pod *)pod_storage;

    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT |
                     PW_STREAM_FLAG_MAP_BUFFERS |
                     PW_STREAM_FLAG_DONT_RECONNECT;
    if (PW.stream_connect(pw_s, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        log_warn("audio", "PipeWire prepare: pw_stream_connect failed");
        update_controls_status(i18n_get("audio.pw.connect_stream"));
        goto fail;
    }

    if (PW.thread_loop_start(pw_loop) < 0) {
        log_warn("audio", "PipeWire prepare: cannot start thread loop");
        update_controls_status(i18n_get("audio.pw.start_thread"));
        goto fail;
    }
    pw_thread_running = 1;

    /* 只有真正进入 STREAMING 才算成功；ERROR/UNCONNECTED 立即失败 */
    enum pw_stream_state state = PW_STREAM_STATE_CONNECTING;
    const char *state_error = NULL;
    int waited_ms = 0;
    while (waited_ms < 2000) {
        state = PW.stream_get_state(pw_s, &state_error);
        if (state == PW_STREAM_STATE_STREAMING) break;
        if (state == PW_STREAM_STATE_ERROR ||
            state == PW_STREAM_STATE_UNCONNECTED) {
            break;
        }
        usleep(5000);
        waited_ms += 5;
    }

    if (state != PW_STREAM_STATE_STREAMING) {
        log_warn("audio",
                 "PipeWire prepare failed: state=%s (%d), error=%s, waited=%dms",
                 pw_state_name((int)state), (int)state,
                 state_error ? state_error : "(none)", waited_ms);
        update_controls_status(i18n_get("audio.pw.stream_not_ready"));
        goto fail;
    }

    log_info("audio",
             "PipeWire stream ready: %d Hz, %d ch, latency=%d frames, waited=%dms",
             sample_rate, channels, latency_frames, waited_ms);
    return 0;

fail:
    pw_teardown_stream();
    return -1;
}

/* ================================================================== */
/*  Stream Control                                                     */
/* ================================================================== */

void pw_cleanup_stream_locked(void) {
    pw_teardown_stream();
}

void pw_cleanup_stream(void) {
    pw_teardown_stream();
}

void pw_flush_stream(void) {
    pthread_mutex_lock(&pw_api_mutex);
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    int destroying = pw_stream_destroying;
    pthread_mutex_unlock(&pw_state_mutex);

    if (ready && s && !destroying) PW.stream_flush(s, 0);
    pw_ring_reset();
    pthread_mutex_unlock(&pw_api_mutex);
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
    pthread_mutex_lock(&pw_api_mutex);
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    int destroying = pw_stream_destroying;
    pthread_mutex_unlock(&pw_state_mutex);

    if (ready && s && !destroying) PW.stream_set_active(s, 0);
    pthread_mutex_unlock(&pw_api_mutex);
}

void pw_resume_stream(void) {
    pthread_mutex_lock(&pw_api_mutex);
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    int destroying = pw_stream_destroying;
    pthread_mutex_unlock(&pw_state_mutex);

    if (ready && s && !destroying) PW.stream_set_active(s, 1);
    pthread_mutex_unlock(&pw_api_mutex);
}

void pw_sync_volume(int volume) {
    pthread_mutex_lock(&pw_api_mutex);
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    int destroying = pw_stream_destroying;
    pthread_mutex_unlock(&pw_state_mutex);

    if (ready && s && !destroying) {
        float vol_linear = (float)volume / 100.0f;
        int r = PW.stream_set_control(s, SPA_PROP_volume, 1, &vol_linear);
        if (r < 0)
            log_warn("audio", "PipeWire set volume failed: %d", r);
    }
    pthread_mutex_unlock(&pw_api_mutex);
}
