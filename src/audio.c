#include "../include/defs.h"
#include "../include/media_session.h"
#include "../include/menu_views.h"
#include "../include/progress.h"  // 新增：进度跟踪模块
#include "../include/lyrics.h"    // 新增：歌词模块
#include "../include/remote.h"
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 音频后端头文件 — 运行时通过 dlopen/dlsym 加载，不编译时硬链接
#include "dyn_pulse.h"
#include "dyn_alsa.h"
#include "dyn_pipewire.h"
#include <dlfcn.h>

// FFmpeg 头文件
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

// 确保DT_REG和DT_UNKNOWN被定义
#ifndef DT_REG
#define DT_REG 8
#endif

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif

// 函数指针表全局实例定义
struct pulseaudio_funcs P = {0};
struct alsa_funcs A = {0};
struct pipewire_funcs PW = {0};

// 音频后端全局变量 — 始终定义，运行时检查是否加载成功
static pa_mainloop *pa_ml = NULL;
static pa_context *pa_ctx = NULL;
static pa_stream *pa_s = NULL;
static pa_sample_spec pa_ss;
static int pa_connected = 0;
static snd_pcm_t *alsa_pcm = NULL;
static int alsa_ready = 0;

static int pulse_loaded = 0;  // 运行时 PA dlopen 成功标志
static int alsa_loaded = 0;   // 运行时 ALSA dlopen 成功标志
static int pipewire_loaded = 0; // 运行时 PipeWire dlopen 成功标志

/* PipeWire 运行时状态 */
static struct pw_thread_loop *pw_loop = NULL;
static struct pw_main_loop *pw_mainloop = NULL;
static struct pw_context *pw_ctx = NULL;
static struct pw_core *pw_core = NULL;
static struct pw_stream *pw_s = NULL;
static struct spa_hook pw_stream_listener;
static int pw_stream_ready = 0;
static int pw_stream_connecting = 0;  /* 阻止 process 过早回调 */
static int pw_channels = 2;
static int pw_sample_rate = 44100;
static int pw_write_underrun = 0;
static volatile int pw_stream_destroying = 0;
static pthread_mutex_t pw_state_mutex = PTHREAD_MUTEX_INITIALIZER;

int g_active_backend = AUDIO_BACKEND_AUTO;

static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_volume_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_visualizer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_play_control_cond = PTHREAD_COND_INITIALIZER;
static int g_volume_percent = 100;
static int g_pending_volume_sync = 0;
static int g_output_sample_rate = 44100;
static int g_output_channels = 2;
static int g_visualizer_levels[VISUALIZER_BAND_COUNT] = {0};
static int g_visualizer_peaks[VISUALIZER_BAND_COUNT] = {0};
static uint64_t g_visualizer_last_update_ms = 0;
static uint64_t g_visualizer_last_analysis_ms = 0;

int g_audio_sample_rate = 0;
int g_audio_bit_rate = 0;
int g_audio_bit_depth = 0;
char g_audio_codec_name[32] = "";

#define VISUALIZER_ANALYSIS_SIZE 128
#define VISUALIZER_UPDATE_INTERVAL_MS 40ULL

static const char *audio_text(const char *utf8, const char *ascii) {
    return use_english_ui() ? ascii : utf8;
}

static int codec_channel_count(const AVCodecContext *codec_ctx) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
    if (codec_ctx->ch_layout.nb_channels > 0) {
        return codec_ctx->ch_layout.nb_channels;
    }
    return 0;
#else
    if (codec_ctx->channels > 0) {
        return codec_ctx->channels;
    }
    if (codec_ctx->channel_layout) {
        return av_get_channel_layout_nb_channels(codec_ctx->channel_layout);
    }
    return 0;
#endif
}

static int init_resampler(SwrContext *swr_ctx,
                          const AVCodecContext *codec_ctx,
                          int input_channels,
                          int output_channels,
                          int output_sample_rate) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout in_ch_layout = codec_ctx->ch_layout;
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, output_channels);

    av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_ch_layout, 0);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
#else
    int64_t in_channel_layout = codec_ctx->channel_layout;
    int64_t out_channel_layout = av_get_default_channel_layout(output_channels);

    if (!in_channel_layout) {
        in_channel_layout = av_get_default_channel_layout(input_channels);
    }

    av_opt_set_channel_layout(swr_ctx, "in_channel_layout", in_channel_layout, 0);
    av_opt_set_channel_layout(swr_ctx, "out_channel_layout", out_channel_layout, 0);
#endif

    av_opt_set_int(swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", output_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S32, 0);

    return swr_init(swr_ctx);
}

static uint64_t audio_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

#define PCM_QUEUE_CAPACITY 24
#define PCM_QUEUE_MIN_PREFILL_MS 180
#define PCM_QUEUE_MAX_PREFILL_MS 420

/* PipeWire 环形缓冲区（线程安全：单生产者-单消费者） */
#define PW_RING_SIZE (1024 * 1024)          /* 1 MB 环形缓冲区 */
static uint8_t pw_ring_data[PW_RING_SIZE];
static volatile int pw_ring_write_pos = 0;
static volatile int pw_ring_read_pos = 0;
static pthread_mutex_t pw_ring_mutex = PTHREAD_MUTEX_INITIALIZER;
#define PW_RING_MASK (PW_RING_SIZE - 1)

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

static int clamp_volume_percent(int volume) {
    if (volume < 0) {
        return 0;
    }
    if (volume > 100) {
        return 100;
    }
    return volume;
}

static int get_configured_latency_ms(void) {
    int latency_ms = g_app_config.audio_latency_ms;
    if (latency_ms < 20) {
        latency_ms = 20;
    }
    if (latency_ms > 250) {
        latency_ms = 250;
    }
    return latency_ms;
}

static int get_pcm_prefill_target_ms(int sample_rate) {
    int prefill_ms = get_configured_latency_ms() * 3;
    if (prefill_ms < PCM_QUEUE_MIN_PREFILL_MS) {
        prefill_ms = PCM_QUEUE_MIN_PREFILL_MS;
    }
    if (sample_rate >= 88200 && prefill_ms < 240) {
        prefill_ms = 240;
    }
    if (sample_rate >= 176400 && prefill_ms < 320) {
        prefill_ms = 320;
    }
    if (prefill_ms > PCM_QUEUE_MAX_PREFILL_MS) {
        prefill_ms = PCM_QUEUE_MAX_PREFILL_MS;
    }
    return prefill_ms;
}

void reset_visualizer_state(void) {
    pthread_mutex_lock(&g_visualizer_mutex);
    memset(g_visualizer_levels, 0, sizeof(g_visualizer_levels));
    memset(g_visualizer_peaks, 0, sizeof(g_visualizer_peaks));
    g_visualizer_last_update_ms = audio_now_ms();
    g_visualizer_last_analysis_ms = 0;
    pthread_mutex_unlock(&g_visualizer_mutex);
}

void push_visualizer_samples(const int32_t *samples, int frame_count, int channels) {
    if (!samples || frame_count <= 0 || channels <= 0) {
        return;
    }

    uint64_t now_ms = audio_now_ms();
    pthread_mutex_lock(&g_visualizer_mutex);
    if (g_visualizer_last_analysis_ms > 0 &&
        now_ms - g_visualizer_last_analysis_ms < VISUALIZER_UPDATE_INTERVAL_MS) {
        pthread_mutex_unlock(&g_visualizer_mutex);
        return;
    }
    g_visualizer_last_analysis_ms = now_ms;
    pthread_mutex_unlock(&g_visualizer_mutex);

    static double window[VISUALIZER_ANALYSIS_SIZE];
    static int window_initialized = 0;

    if (!window_initialized) {
        for (int i = 0; i < VISUALIZER_ANALYSIS_SIZE; i++) {
            window[i] = 0.5 - 0.5 * cos((2.0 * M_PI * (double)i) / (double)(VISUALIZER_ANALYSIS_SIZE - 1));
        }
        window_initialized = 1;
    }

    double mono[VISUALIZER_ANALYSIS_SIZE];
    for (int i = 0; i < VISUALIZER_ANALYSIS_SIZE; i++) {
        int src_frame = (i * frame_count) / VISUALIZER_ANALYSIS_SIZE;
        if (src_frame >= frame_count) {
            src_frame = frame_count - 1;
        }

        double mixed = 0.0;
        for (int ch = 0; ch < channels; ch++) {
            mixed += (double)samples[src_frame * channels + ch];
        }
        mixed /= (double)channels * 2147483648.0;
        mono[i] = mixed * window[i];
    }

    double magnitudes[VISUALIZER_ANALYSIS_SIZE / 2] = {0.0};
    int useful_bins = VISUALIZER_ANALYSIS_SIZE / 2;

    for (int bin = 1; bin < useful_bins; bin++) {
        double real = 0.0;
        double imag = 0.0;
        double coeff = (2.0 * M_PI * (double)bin) / (double)VISUALIZER_ANALYSIS_SIZE;

        for (int n = 0; n < VISUALIZER_ANALYSIS_SIZE; n++) {
            double angle = coeff * (double)n;
            real += mono[n] * cos(angle);
            imag -= mono[n] * sin(angle);
        }

        double magnitude = sqrt(real * real + imag * imag) / ((double)VISUALIZER_ANALYSIS_SIZE / 2.0);
        double emphasis = 1.0 + ((double)bin / (double)(useful_bins - 1)) * 0.35;
        magnitudes[bin] = magnitude * emphasis;
    }

    pthread_mutex_lock(&g_visualizer_mutex);
    for (int i = 0; i < VISUALIZER_BAND_COUNT; i++) {
        int start_bin = 1 + (i * (useful_bins - 1)) / VISUALIZER_BAND_COUNT;
        int end_bin = 1 + ((i + 1) * (useful_bins - 1)) / VISUALIZER_BAND_COUNT;
        if (end_bin <= start_bin) {
            end_bin = start_bin + 1;
        }
        if (end_bin > useful_bins) {
            end_bin = useful_bins;
        }

        double band_energy = 0.0;
        for (int bin = start_bin; bin < end_bin; bin++) {
            if (magnitudes[bin] > band_energy) {
                band_energy = magnitudes[bin];
            }
        }

        double compressed = log1p(band_energy * 48.0) / log1p(49.0);
        int scaled_level = (int)lround(compressed * 100.0);
        if (scaled_level < 0) {
            scaled_level = 0;
        }
        if (scaled_level > 100) {
            scaled_level = 100;
        }

        int previous = g_visualizer_levels[i];
        if (scaled_level > previous) {
            g_visualizer_levels[i] = (previous + scaled_level * 3) / 4;
        } else {
            g_visualizer_levels[i] = (previous * 3 + scaled_level) / 4;
        }

        if (g_visualizer_levels[i] < 1) {
            g_visualizer_levels[i] = 0;
        }

        if (g_visualizer_levels[i] > g_visualizer_peaks[i]) {
            g_visualizer_peaks[i] = g_visualizer_levels[i];
        } else if (g_visualizer_peaks[i] > 0) {
            g_visualizer_peaks[i] -= 1;
        }
    }
    g_visualizer_last_update_ms = now_ms;
    pthread_mutex_unlock(&g_visualizer_mutex);
}

void get_visualizer_snapshot(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms) {
    if (!levels || !peaks || max_levels <= 0) {
        return;
    }

    int copy_count = max_levels < VISUALIZER_BAND_COUNT ? max_levels : VISUALIZER_BAND_COUNT;

    pthread_mutex_lock(&g_visualizer_mutex);
    for (int i = 0; i < copy_count; i++) {
        levels[i] = g_visualizer_levels[i];
        peaks[i] = g_visualizer_peaks[i];
    }
    if (last_update_ms) {
        *last_update_ms = g_visualizer_last_update_ms;
    }
    pthread_mutex_unlock(&g_visualizer_mutex);
}

static void signal_playback_thread(void) {
    pthread_cond_broadcast(&g_play_control_cond);
}

static int pcm_chunk_ensure_capacity(PCMChunk *chunk, int required_bytes) {
    if (!chunk || required_bytes <= 0) {
        return -1;
    }
    if (chunk->capacity_bytes >= required_bytes) {
        return 0;
    }

    int new_capacity = chunk->capacity_bytes > 0 ? chunk->capacity_bytes : MAX_AUDIO_BUFFER_SIZE;
    while (new_capacity < required_bytes) {
        new_capacity *= 2;
    }

    int32_t *new_data = realloc(chunk->data, (size_t)new_capacity);
    if (!new_data) {
        return -1;
    }

    chunk->data = new_data;
    chunk->capacity_bytes = new_capacity;
    return 0;
}

static void pcm_queue_reset(PCMQueue *queue) {
    if (!queue) {
        return;
    }

    queue->read_index = 0;
    queue->write_index = 0;
    queue->count = 0;
    queue->buffered_frames = 0;
    for (int i = 0; i < PCM_QUEUE_CAPACITY; i++) {
        queue->chunks[i].frame_count = 0;
        queue->chunks[i].bytes = 0;
    }
}

static int pcm_queue_init(PCMQueue *queue) {
    if (!queue) {
        return -1;
    }

    memset(queue, 0, sizeof(*queue));
    for (int i = 0; i < PCM_QUEUE_CAPACITY; i++) {
        queue->chunks[i].data = malloc(MAX_AUDIO_BUFFER_SIZE);
        if (!queue->chunks[i].data) {
            for (int j = 0; j < i; j++) {
                free(queue->chunks[j].data);
                queue->chunks[j].data = NULL;
            }
            return -1;
        }
        queue->chunks[i].capacity_bytes = MAX_AUDIO_BUFFER_SIZE;
    }

    pcm_queue_reset(queue);
    return 0;
}

static void pcm_queue_destroy(PCMQueue *queue) {
    if (!queue) {
        return;
    }

    for (int i = 0; i < PCM_QUEUE_CAPACITY; i++) {
        free(queue->chunks[i].data);
        queue->chunks[i].data = NULL;
    }
    pcm_queue_reset(queue);
}

static PCMChunk *pcm_queue_write_slot(PCMQueue *queue) {
    if (!queue || queue->count >= PCM_QUEUE_CAPACITY) {
        return NULL;
    }
    return &queue->chunks[queue->write_index];
}

static void pcm_queue_commit_write(PCMQueue *queue, int frame_count, int bytes) {
    if (!queue || queue->count >= PCM_QUEUE_CAPACITY) {
        return;
    }

    queue->chunks[queue->write_index].frame_count = frame_count;
    queue->chunks[queue->write_index].bytes = bytes;
    queue->write_index = (queue->write_index + 1) % PCM_QUEUE_CAPACITY;
    queue->count++;
    queue->buffered_frames += frame_count;
}

static PCMChunk *pcm_queue_peek(PCMQueue *queue) {
    if (!queue || queue->count <= 0) {
        return NULL;
    }
    return &queue->chunks[queue->read_index];
}

static int pcm_queue_buffered_ms(const PCMQueue *queue, int sample_rate) {
    if (!queue || sample_rate <= 0 || queue->buffered_frames <= 0) {
        return 0;
    }
    return (queue->buffered_frames * 1000) / sample_rate;
}

static void pcm_queue_consume(PCMQueue *queue) {
    if (!queue || queue->count <= 0) {
        return;
    }

    int consumed_frames = queue->chunks[queue->read_index].frame_count;
    queue->chunks[queue->read_index].frame_count = 0;
    queue->chunks[queue->read_index].bytes = 0;
    queue->read_index = (queue->read_index + 1) % PCM_QUEUE_CAPACITY;
    queue->count--;
    queue->buffered_frames -= consumed_frames;
    if (queue->buffered_frames < 0) {
        queue->buffered_frames = 0;
    }
}

static void apply_volume_to_samples(int32_t *samples, int sample_count) {
    if (g_active_backend == AUDIO_BACKEND_PULSE ||
        g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        (void)samples;
        (void)sample_count;
        return;
    }
    if (!samples || sample_count <= 0) {
        return;
    }

    int volume = get_volume_percent();
    if (volume >= 100) {
        return;
    }
    if (volume <= 0) {
        memset(samples, 0, (size_t)sample_count * sizeof(int32_t));
        return;
    }

    for (int i = 0; i < sample_count; i++) {
        samples[i] = (int32_t)(((int64_t)samples[i] * volume) / 100);
    }
}

static int consume_volume_sync_request(int *volume_out, int force) {
    int should_sync = force;
    int volume = 100;

    pthread_mutex_lock(&g_volume_mutex);
    volume = g_volume_percent;
    if (g_pending_volume_sync) {
        should_sync = 1;
        g_pending_volume_sync = 0;
    }
    pthread_mutex_unlock(&g_volume_mutex);

    if (volume_out) {
        *volume_out = volume;
    }
    return should_sync;
}

/* Forward declarations for PipeWire backend functions */
static void pw_sync_volume(int volume);

static void audio_backend_sync_volume(int force) {
    int volume = 100;
    if (!consume_volume_sync_request(&volume, force)) {
        return;
    }

    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE) {
        if (!pa_ctx || !pa_ml || !pa_s || P.stream_get_state(pa_s) != PA_STREAM_READY) {
            return;
        }

        uint32_t stream_index = P.stream_get_index(pa_s);
        if (stream_index == PA_INVALID_INDEX) {
            return;
        }

        pa_cvolume cvolume;
        P.cvolume_set(&cvolume, pa_ss.channels, P.sw_volume_from_linear((double)volume / 100.0));

        pa_operation *op = P.context_set_sink_input_volume(pa_ctx, stream_index, &cvolume, NULL, NULL);
        if (!op) {
            return;
        }

        while (P.operation_get_state(op) == PA_OPERATION_RUNNING) {
            P.mainloop_iterate(pa_ml, 1, NULL);
        }
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

// 全局变量定义
PlayState g_play_state = PLAY_STATE_STOPPED; // 当前播放状态
int g_current_play_index = -1; // 当前播放的歌曲索引
LoopMode g_loop_mode = LOOP_OFF; // 当前循环模式
pthread_t g_play_thread; // 播放线程
int g_play_thread_running = 0; // 播放线程运行状态
pthread_mutex_t g_play_mutex = PTHREAD_MUTEX_INITIALIZER; // 播放控制互斥锁
static int g_play_thread_active = 0;
static int g_play_thread_finished = 0;
static int g_pending_playback_index = -1;

// 远程音频缓存
static char g_cached_audio_path[MAX_PATH_LEN] = "";
static char g_cached_lyrics_path[MAX_PATH_LEN] = "";

// 倍速播放相关
float g_playback_speed = 1.0f;
static float g_speed_ratios[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
static int g_speed_index = 1;
static int g_speed_count = sizeof(g_speed_ratios) / sizeof(g_speed_ratios[0]);

// atempo 滤镜相关结构体
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

// 跳转相关变量
int g_seek_request = 0; // 跳转请求标志
int g_seek_position = 0; // 目标跳转位置（秒）
int g_initial_seek_position = 0; // 重启播放时的初始跳转位置（秒）

// 进度条相关变量（保留用于 UI 兼容，实际值从 progress_tracker 获取）
int g_current_position = 0; // 当前播放位置（秒）
int g_total_duration = 0; // 当前歌曲总时长（秒）
pthread_mutex_t g_seek_mutex = PTHREAD_MUTEX_INITIALIZER; // 跳转操作互斥锁 

// 全局变量：默认音频设备名称
char g_default_audio_device[128] = "default";

/* ------------------------------------------------------------------- */
/*  PulseAudio 动态加载                                                 */
/* ------------------------------------------------------------------- */
#define PULSE_SONAME "libpulse.so.0"

#define PULSE_LOAD(name) do { \
    *(void **)(&P.name) = dlsym(P.handle, "pa_" #name); \
    if (!P.name) { \
        fprintf(stderr, "dlsym(pa_" #name ") failed: %s\n", dlerror()); \
        dlclose(P.handle); \
        P.handle = NULL; \
        return -1; \
    } \
} while(0)

int pulse_load(void) {
    if (P.loaded) return 0;
    P.handle = dlopen(PULSE_SONAME, RTLD_LAZY | RTLD_LOCAL);
    if (!P.handle) return -1;

    PULSE_LOAD(mainloop_new);
    PULSE_LOAD(mainloop_get_api);
    PULSE_LOAD(mainloop_free);
    PULSE_LOAD(mainloop_iterate);

    PULSE_LOAD(context_new);
    PULSE_LOAD(context_connect);
    PULSE_LOAD(context_get_state);
    PULSE_LOAD(context_disconnect);
    PULSE_LOAD(context_unref);
    PULSE_LOAD(context_set_sink_input_volume);

    PULSE_LOAD(stream_new);
    PULSE_LOAD(stream_connect_playback);
    PULSE_LOAD(stream_get_state);
    PULSE_LOAD(stream_get_index);
    PULSE_LOAD(stream_disconnect);
    PULSE_LOAD(stream_unref);
    PULSE_LOAD(stream_writable_size);
    PULSE_LOAD(stream_write);
    PULSE_LOAD(stream_flush);
    PULSE_LOAD(stream_is_corked);
    PULSE_LOAD(stream_cork);

    PULSE_LOAD(usec_to_bytes);

    PULSE_LOAD(operation_get_state);
    PULSE_LOAD(operation_unref);

    PULSE_LOAD(cvolume_set);
    PULSE_LOAD(sw_volume_from_linear);

    P.loaded = 1;
    return 0;
}

void pulse_unload(void) {
    if (P.handle) {
        dlclose(P.handle);
        P.handle = NULL;
    }
    P.loaded = 0;
}

/* ------------------------------------------------------------------- */
/*  ALSA 动态加载                                                      */
/* ------------------------------------------------------------------- */
#define ALSA_SONAME "libasound.so.2"

#define ALSA_LOAD(name) do { \
    *(void **)(&A.name) = dlsym(A.handle, "snd_" #name); \
    if (!A.name) { \
        fprintf(stderr, "dlsym(snd_" #name ") failed: %s\n", dlerror()); \
        dlclose(A.handle); \
        A.handle = NULL; \
        return -1; \
    } \
} while(0)

int alsa_load(void) {
    if (A.loaded) return 0;
    A.handle = dlopen(ALSA_SONAME, RTLD_LAZY | RTLD_LOCAL);
    if (!A.handle) return -1;

    ALSA_LOAD(pcm_open);
    ALSA_LOAD(pcm_set_params);
    ALSA_LOAD(pcm_writei);
    ALSA_LOAD(pcm_wait);
    ALSA_LOAD(pcm_prepare);
    ALSA_LOAD(pcm_drop);
    ALSA_LOAD(pcm_close);
    ALSA_LOAD(pcm_pause);

    A.loaded = 1;
    return 0;
}

void alsa_unload(void) {
    if (A.handle) {
        dlclose(A.handle);
        A.handle = NULL;
    }
    A.loaded = 0;
}

/* ------------------------------------------------------------------- */
/*  PipeWire 动态加载                                                   */
/* ------------------------------------------------------------------- */
#define PIPEWIRE_SONAME "libpipewire-0.3.so.0"

#define PIPEWIRE_LOAD(name) do { \
    *(void **)(&PW.name) = dlsym(PW.handle, "pw_" #name); \
    if (!PW.name) { \
        fprintf(stderr, "dlsym(pw_" #name ") failed: %s\n", dlerror()); \
        dlclose(PW.handle); \
        PW.handle = NULL; \
        return -1; \
    } \
} while(0)

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

/* ------------------------------------------------------------------- */
/*  SPA Pod Builder (inline — builds audio format without libspa)      */
/* ------------------------------------------------------------------- */

/*
   Builds a SPA_PARAM_Format object pod for S32LE audio.
   Binary layout (136 bytes for stereo 44100):

   [spa_pod: body_size=128, type=Object]            8 bytes
   [object_body: body_type=Format(4), padding=0]    8 bytes
   prop mediaType:    key=0,  flags=0, pod(Id=2), val=1  +pad => 24
   prop mediaSubtype: key=1,  flags=0, pod(Id=2), val=1  +pad => 24
   prop format:       key=2,  flags=0, pod(Id=2), val=S32 +pad => 24
   prop rate:         key=3,  flags=0, pod(Int=3), val=R  +pad => 24
   prop channels:     key=4,  flags=0, pod(Int=3), val=C  +pad => 24
   ----------------------------------------------------------------
   Total: 136 bytes, pod.body_size=128
*/
int build_audio_format_pod(void *dst, int sample_rate, int channels) {
    uint32_t *p = (uint32_t *)dst;
    /* 每属性固定 24 字节，5 属性 = 120 字节，加上 16 字节对象头 */
    uint32_t body_size = 128;  /* after spa_pod header */
    uint32_t audio_format = SPA_AUDIO_FORMAT_S32;

#define POD_SET(w)   do { *p++ = (uint32_t)(w); } while(0)

    /* spa_pod header */
    POD_SET(body_size);               /* size */
    POD_SET(SPA_TYPE_Object);         /* type */

    /* object body */
    POD_SET(SPA_PARAM_Format);        /* body_type */
    POD_SET(0);                       /* padding */

    /* prop 0: mediaType = audio */
    POD_SET(SPA_FORMAT_mediaType);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Id);  POD_SET(SPA_MEDIA_TYPE_audio);  POD_SET(0);

    /* prop 1: mediaSubtype = raw */
    POD_SET(SPA_FORMAT_mediaSubtype);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Id);  POD_SET(SPA_MEDIA_SUBTYPE_raw);  POD_SET(0);

    /* prop 2: format = S32 */
    POD_SET(SPA_FORMAT_AUDIO_format);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Id);  POD_SET(audio_format);  POD_SET(0);

    /* prop 3: sample rate */
    POD_SET(SPA_FORMAT_AUDIO_rate);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Int);  POD_SET((uint32_t)sample_rate);  POD_SET(0);

    /* prop 4: channels */
    POD_SET(SPA_FORMAT_AUDIO_channels);
    POD_SET(0);
    POD_SET(4);  POD_SET(SPA_TYPE_Int);  POD_SET((uint32_t)channels);  POD_SET(0);

#undef POD_SET

    /* Total bytes written = 34 × 4 = 136 */
    return 136;
}

/* ------------------------------------------------------------------- */
/*  PipeWire 环形缓冲区操作（线程安全，SPSC）                           */
/* ------------------------------------------------------------------- */

static int pw_ring_bytes_writable(void) {
    int write = pw_ring_write_pos;
    int read  = pw_ring_read_pos;
    int used = write - read;
    if (used < 0) used += PW_RING_SIZE;
    return PW_RING_SIZE - 1 - used;  /* 始终留一个空位 */
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

/* ------------------------------------------------------------------- */
/*  PipeWire 后端接口函数                                               */
/* ------------------------------------------------------------------- */

static void pw_cleanup_stream_locked(void);
static void pw_stream_state_change_locked(int ready);

/* Callback: state changed (called from PW thread loop). */
static void pw_stream_on_state_changed(void *userdata,
                                        enum pw_stream_state old,
                                        enum pw_stream_state state,
                                        const char *error) {
    (void)userdata;
    (void)old;
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

/* Callback: process (called from PW thread loop when graph needs data). */
static void pw_stream_on_process(void *userdata) {
    (void)userdata;

    /*
     * 安全访问 pw_s：全程持锁读取连接标志和流指针。
     * pw_cleanup_stream_locked() 在销毁流前会设置 pw_stream_destroying = 1，
     * 并调用 stream_set_active(0) 阻止新回调。已进入本回调的旧流指针 s 将在
     * 释放 pw_state_mutex 后继续使用，但此时 stream_destroy() 会等待当前回调
     * 返回后才真正释放流内存 —— 因此 s 在回调全程有效。
     */
    pthread_mutex_lock(&pw_state_mutex);
    int still_connecting = pw_stream_connecting;
    int destroying = pw_stream_destroying;
    struct pw_stream *s = pw_s;
    pthread_mutex_unlock(&pw_state_mutex);
    if (still_connecting || destroying || !s) return;

    struct pw_buffer *buf = NULL;
    if (PW.stream_dequeue_buffer(s, &buf) < 0 || !buf) {
        return;
    }

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

static void pw_stream_on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    (void)userdata; (void)id; (void)param;
}

/* PipeWire 1.6 的事件版本是 2（包含 io_changed） */
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

/*
 * 轻量探测：仅检查 PipeWire 守护进程是否可达。
 * 不创建流、不启动 RT 线程，因此可在 init_audio_device() 中安全调用，
 * 不会引发 stream_destroy 时的竞态 SIGSEGV。
 * 返回 0 表示可达，-1 表示失败。
 */
static int pw_probe_backend(void) {
    struct pw_thread_loop *loop = NULL;
    struct pw_context *ctx = NULL;
    struct pw_core *core = NULL;

    loop = PW.thread_loop_new("ter-music-probe", NULL);
    if (!loop) return -1;

    ctx = PW.context_new(PW.thread_loop_get_loop(loop), NULL, 0);
    if (!ctx) { PW.thread_loop_destroy(loop); return -1; }

    core = PW.context_connect(ctx, NULL, 0);
    if (!core) { PW.context_destroy(ctx); PW.thread_loop_destroy(loop); return -1; }

    /* 可达 — 清理局部资源后返回成功 */
    PW.core_disconnect(core);
    PW.context_destroy(ctx);
    PW.thread_loop_destroy(loop);
    return 0;
}

static int pw_prepare_stream(int sample_rate, int channels) {
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

    /* 创建线程循环 */
    pw_loop = PW.thread_loop_new("ter-music-pw", NULL);
    if (!pw_loop) {
        update_controls_status(audio_text("无法创建 PipeWire 线程循环", "Cannot create PW thread loop"));
        return -1;
    }
    pw_mainloop = PW.thread_loop_get_loop(pw_loop);

    /* 创建上下文 */
    pw_ctx = PW.context_new(pw_mainloop, NULL, 0);
    if (!pw_ctx) {
        update_controls_status(audio_text("无法创建 PipeWire 上下文", "Cannot create PW context"));
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    /* 连接到 PipeWire 守护进程 */
    pw_core = PW.context_connect(pw_ctx, NULL, 0);
    if (!pw_core) {
        update_controls_status(audio_text("无法连接到 PipeWire", "Cannot connect to PipeWire"));
        PW.context_destroy(pw_ctx); pw_ctx = NULL;
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    /* 创建流 */
    pw_s = PW.stream_new(pw_core, "ter-music", NULL);
    if (!pw_s) {
        update_controls_status(audio_text("无法创建 PipeWire 流", "Cannot create PW stream"));
        PW.core_disconnect(pw_core); pw_core = NULL;
        PW.context_destroy(pw_ctx); pw_ctx = NULL;
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    /* 注册事件回调 */
    PW.stream_add_listener(pw_s, &pw_stream_listener, &pw_stream_callbacks, NULL);

    /* 构建音频格式 SPA Pod */
    uint8_t pod_buf[256];
    int pod_len = build_audio_format_pod(pod_buf, sample_rate, channels);
    (void)pod_len;
    const struct spa_pod *params[1];
    params[0] = (const struct spa_pod *)pod_buf;

    /* 连接流 */
    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT |
                     PW_STREAM_FLAG_MAP_BUFFERS |
                     PW_STREAM_FLAG_DONT_RECONNECT;
    if (PW.stream_connect(pw_s, PW_DIRECTION_OUTPUT, 0, flags, params, 1) < 0) {
        update_controls_status(audio_text("无法连接 PipeWire 流", "Cannot connect PW stream"));
        PW.stream_destroy(pw_s); pw_s = NULL;
        PW.core_disconnect(pw_core); pw_core = NULL;
        PW.context_destroy(pw_ctx); pw_ctx = NULL;
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    /* 启动线程循环 */
    if (PW.thread_loop_start(pw_loop) < 0) {
        update_controls_status(audio_text("无法启动 PipeWire 线程", "Cannot start PW thread"));
        PW.stream_destroy(pw_s); pw_s = NULL;
        PW.core_disconnect(pw_core); pw_core = NULL;
        PW.context_destroy(pw_ctx); pw_ctx = NULL;
        PW.thread_loop_destroy(pw_loop);
        pw_loop = NULL; pw_mainloop = NULL;
        return -1;
    }

    /* 等待流就绪（带超时） */
    int timeout_ms = 5000;
    while (timeout_ms > 0) {
        pthread_mutex_lock(&pw_state_mutex);
        int ready = pw_stream_ready;
        int err = 0;
        if (!ready && !pw_stream_connecting && pw_s) {
            /* 如果不再连接且未就绪，说明流已进入错误状态 */
            const char *e = NULL;
            PW.stream_get_state(pw_s, &e);
            if (e && e[0] != '\0') err = 1;
        }
        pthread_mutex_unlock(&pw_state_mutex);
        if (ready) break;
        if (err) {
            timeout_ms = 0;
            break;
        }
        usleep(1000);
        timeout_ms--;
    }

    pthread_mutex_lock(&pw_state_mutex);
    int is_ready = pw_stream_ready;
    /* 二次验证：等待一小段时间，捕获流就绪后又迅速进入 ERROR 的竞态。
     * 在无输出节点的 PipeWire 环境中，流可能从 PAUSED 快速进入 ERROR
     * （"no target node available"），导致 probe 返回 0 但实际上不可用。 */
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
            if (st == PW_STREAM_STATE_STREAMING) {
                break;
            }
            usleep(1000);
            verify_ms--;
        }
        /* 300ms 后仍非 STREAMING -> 判定无输出节点 */
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
        update_controls_status(audio_text("PipeWire 流未就绪", "PW stream not ready"));
        goto pw_cleanup_and_fail;
    }

    return 0;

pw_cleanup_and_fail:
    /* 与 pw_cleanup_stream_locked 同样策略：只停循环、不调 PW 销毁 */
    pthread_mutex_lock(&pw_state_mutex);
    pw_stream_destroying = 1;
    pthread_mutex_unlock(&pw_state_mutex);

    if (pw_loop) {
        PW.thread_loop_lock(pw_loop);
    }
    if (pw_s) {
        PW.stream_set_active(pw_s, 0);
    }
    if (pw_loop) {
        PW.thread_loop_unlock(pw_loop);
    }
    if (pw_loop) {
        PW.thread_loop_stop(pw_loop);
    }

    pthread_mutex_lock(&pw_state_mutex);
    pw_s = NULL;
    pw_core = NULL;
    pw_ctx = NULL;
    pw_loop = NULL;
    pw_mainloop = NULL;
    pw_stream_ready = 0;
    pw_stream_connecting = 0;
    pthread_mutex_unlock(&pw_state_mutex);
    pw_ring_reset();
    return -1;
}

static void pw_cleanup_stream_locked(void) {
    if (!pw_s && !pw_core && !pw_loop) {
        /* 没有活动资源 — 仅复位标志 */
        pw_stream_ready = 0;
        pw_stream_connecting = 0;
        pw_ring_reset();
        return;
    }

    /*
     * PipeWire 1.6.0 workaround：屏蔽所有 pw_stream_destroy /
     * pw_core_disconnect / pw_context_destroy 调用。
     *
     * PipeWire 1.6.0 的 pw_stream_destroy 存在 use-after-free 缺陷：
     * 内部 impl vtbl destroy 先释放了 impl 内存，随后外层公共代码访问
     * pw_stream+0x70 处已释放的链表节点，导致 SIGSEGV。
     *
     * 无论通过什么路径（直接 pw_stream_destroy、经 pw_core_disconnect
     * → pw_proxy_destroy、或经 pw_context_destroy → 内部递归清理）
     * 最终都会走到有问题的代码路径。
     *
     * 解决：只停线程循环（保证现场回调不再触发），然后丢弃指针。
     * 不调用任何 PW 销毁函数。资源在进程退出时由 OS 回收。
     * 下一次 pw_prepare_stream() 会创建全新的 loop/ctx/core/stream。
     */
    pthread_mutex_lock(&pw_state_mutex);
    pw_stream_destroying = 1;
    pthread_mutex_unlock(&pw_state_mutex);

    if (pw_loop) {
        PW.thread_loop_lock(pw_loop);
    }
    if (pw_s) {
        PW.stream_set_active(pw_s, 0);
    }
    if (pw_loop) {
        PW.thread_loop_unlock(pw_loop);
    }

    /* 停线程循环（锁外），弃置所有 PW 资源 */
    if (pw_loop) {
        PW.thread_loop_stop(pw_loop);
    }

    pthread_mutex_lock(&pw_state_mutex);
    pw_s = NULL;
    pw_core = NULL;
    pw_ctx = NULL;
    pw_loop = NULL;
    pw_mainloop = NULL;
    pw_stream_ready = 0;
    pw_stream_connecting = 0;
    pthread_mutex_unlock(&pw_state_mutex);
    pw_ring_reset();
}

static void pw_cleanup_stream(void) {
    pw_cleanup_stream_locked();
}

static void pw_flush_stream(void) {
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    pthread_mutex_unlock(&pw_state_mutex);
    if (ready && s) {
        PW.stream_flush(s, 0);
    }
    pw_ring_reset();
}

static int pw_write_samples(const int32_t *samples, int frame_count);
static void pw_pause_stream(void);
static void pw_resume_stream(void);
static void pw_sync_volume(int volume);

static int pw_write_samples(const int32_t *samples, int frame_count) {
    if (!samples || frame_count <= 0) return 0;

    /* 检查流是否仍健康（"no target node" 等错误会清此标志） */
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
            /* 等待时再次检查流健康 */
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

static void pw_pause_stream(void) {
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    pthread_mutex_unlock(&pw_state_mutex);
    if (ready && s) PW.stream_set_active(s, 0);
}

static void pw_resume_stream(void) {
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    pthread_mutex_unlock(&pw_state_mutex);
    if (ready && s) PW.stream_set_active(s, 1);
}

static void pw_sync_volume(int volume) {
    pthread_mutex_lock(&pw_state_mutex);
    struct pw_stream *s = pw_s;
    int ready = pw_stream_ready;
    pthread_mutex_unlock(&pw_state_mutex);
    if (ready && s) {
        float vol_linear = (float)volume / 100.0f;
        PW.stream_set_control(s, PW_STREAM_CONTROL_VOLUME, vol_linear);
    }
}

static int pw_prepare_stream(int sample_rate, int channels);
static void pw_cleanup_stream(void);
static void pw_flush_stream(void);

int audio_backend_is_available(int backend) {
    if (backend == AUDIO_BACKEND_PULSE) return pulse_loaded;
    if (backend == AUDIO_BACKEND_ALSA)  return alsa_loaded;
    if (backend == AUDIO_BACKEND_PIPEWIRE) return pipewire_loaded;
    return 0;
}

// PulseAudio 状态检查宏
#define PA_CHECK_SUCCESS(op, msg) do { \
    if (!(op)) { \
        fprintf(stderr, "PulseAudio error: %s\n", msg); \
    } \
} while(0)



/**
 * 根据倍速值构建 atempo 滤镜字符串
 * atempo 滤镜的有效范围是 0.5 到 2.0，超出范围需要链式使用
 */
static void build_atempo_filter_string(char *buf, size_t buf_size, float speed) {
    if (speed == 1.0f) {
        // 1.0x 不需要滤镜
        buf[0] = '\0';
        return;
    }
    
    // atempo 滤镜范围是 0.5 到 2.0
    if (speed >= 0.5f && speed <= 2.0f) {
        snprintf(buf, buf_size, "atempo=%.2f", speed);
        return;
    }
    
    // 对于 3.0x，使用链式滤镜: atempo=2.0,atempo=1.5
    if (speed > 2.0f) {
        float remaining = speed;
        buf[0] = '\0';
        size_t offset = 0;
        int first = 1;
        
        while (remaining > 1.01f) {
            float factor = (remaining > 2.0f) ? 2.0f : remaining;
            int written = snprintf(buf + offset, buf_size - offset, 
                                   first ? "atempo=%.2f" : ",atempo=%.2f", factor);
            if (written < 0 || (size_t)written >= buf_size - offset) {
                break;
            }
            offset += written;
            remaining /= factor;
            first = 0;
        }
        return;
    }
    
    // 对于小于 0.5 的速度（如 0.25x），需要链式使用
    if (speed < 0.5f) {
        float remaining = speed;
        buf[0] = '\0';
        size_t offset = 0;
        int first = 1;
        
        while (remaining < 0.99f) {
            float factor = (remaining < 0.5f) ? 0.5f : remaining;
            int written = snprintf(buf + offset, buf_size - offset,
                                   first ? "atempo=%.2f" : ",atempo=%.2f", factor);
            if (written < 0 || (size_t)written >= buf_size - offset) {
                break;
            }
            offset += written;
            remaining /= factor;
            first = 0;
        }
        return;
    }
    
    // 默认情况
    buf[0] = '\0';
}

/**
 * 初始化 atempo 滤镜图
 */
static int init_atempo_filter(AtempoFilter *filter, const AVCodecContext *codec_ctx, float speed) {
    if (!filter || !codec_ctx || speed <= 0) {
        return -1;
    }
    
    // 如果速度是 1.0x，不需要滤镜
    if (speed == 1.0f) {
        filter->initialized = 0;
        filter->speed = 1.0f;
        return 0;
    }
    
    // 构建滤镜字符串
    char filter_str[256];
    build_atempo_filter_string(filter_str, sizeof(filter_str), speed);
    if (filter_str[0] == '\0') {
        filter->initialized = 0;
        filter->speed = 1.0f;
        return 0;
    }
    
    // 保存输入参数
    filter->speed = speed;
    filter->input_sample_rate = codec_ctx->sample_rate;
    filter->input_channels = codec_channel_count(codec_ctx);
    filter->input_sample_fmt = codec_ctx->sample_fmt;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    filter->input_channel_layout = codec_ctx->ch_layout.u.mask;
#else
    filter->input_channel_layout = codec_ctx->channel_layout;
#endif
    
    // 创建滤镜图
    filter->graph = avfilter_graph_alloc();
    if (!filter->graph) {
        return -1;
    }
    
    // 创建 buffer 源滤镜（输入）
    const AVFilter *abuffersrc = avfilter_get_by_name("abuffer");
    if (!abuffersrc) {
        avfilter_graph_free(&filter->graph);
        return -1;
    }
    
    char ch_layout_str[64];
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout ch_layout = codec_ctx->ch_layout;
    av_channel_layout_describe(&ch_layout, ch_layout_str, sizeof(ch_layout_str));
#else
    snprintf(ch_layout_str, sizeof(ch_layout_str), "0x%"PRIx64, filter->input_channel_layout);
#endif
    
    char args[512];
    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=%s:channels=%d",
             filter->input_sample_rate,
             av_get_sample_fmt_name(filter->input_sample_fmt),
             ch_layout_str,
             filter->input_channels);
    
    int ret = avfilter_graph_create_filter(&filter->src_ctx, abuffersrc, "in",
                                           args, NULL, filter->graph);
    if (ret < 0) {
        avfilter_graph_free(&filter->graph);
        return -1;
    }
    
    // 创建 buffer sink 滤镜（输出）
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) {
        avfilter_graph_free(&filter->graph);
        return -1;
    }
    
    ret = avfilter_graph_create_filter(&filter->sink_ctx, abuffersink, "out",
                                       NULL, NULL, filter->graph);
    if (ret < 0) {
        avfilter_graph_free(&filter->graph);
        return -1;
    }
    
    // 设置输出格式
    enum AVSampleFormat sample_fmts[] = { filter->input_sample_fmt, AV_SAMPLE_FMT_NONE };
    ret = av_opt_set_int_list(filter->sink_ctx, "sample_fmts", sample_fmts,
                              AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        avfilter_graph_free(&filter->graph);
        return -1;
    }
    
    // 解析滤镜字符串并连接
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&filter->graph);
        return -1;
    }
    
    outputs->name = av_strdup("in");
    outputs->filter_ctx = filter->src_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;
    
    inputs->name = av_strdup("out");
    inputs->filter_ctx = filter->sink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;
    
    ret = avfilter_graph_parse_ptr(filter->graph, filter_str, &inputs, &outputs, NULL);
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    
    if (ret < 0) {
        avfilter_graph_free(&filter->graph);
        return -1;
    }
    
    // 配置滤镜图
    ret = avfilter_graph_config(filter->graph, NULL);
    if (ret < 0) {
        avfilter_graph_free(&filter->graph);
        return -1;
    }
    
    filter->initialized = 1;
    return 0;
}

/**
 * 销毁 atempo 滤镜图
 */
static void cleanup_atempo_filter(AtempoFilter *filter) {
    if (!filter) {
        return;
    }
    
    if (filter->graph) {
        avfilter_graph_free(&filter->graph);
        filter->graph = NULL;
    }
    
    filter->src_ctx = NULL;
    filter->sink_ctx = NULL;
    filter->initialized = 0;
}

/**
 * 发送帧到 atempo 滤镜
 */
static int send_frame_to_atempo(AtempoFilter *filter, AVFrame *frame) {
    if (!filter || !filter->initialized) {
        return 0;
    }
    
    int ret = av_buffersrc_add_frame(filter->src_ctx, frame);
    if (ret < 0) {
        return -1;
    }
    return 0;
}

/**
 * 从 atempo 滤镜接收帧
 */
static int receive_frame_from_atempo(AtempoFilter *filter, AVFrame *frame) {
    if (!filter || !filter->initialized) {
        return -1;
    }
    
    int ret = av_buffersink_get_frame(filter->sink_ctx, frame);
    return ret;  // 返回 AVERROR(EAGAIN) 或 AVERROR_EOF 表示没有更多帧
}

/**
 * 刷新 atempo 滤镜（发送 NULL 帧表示输入结束）
 */
static int flush_atempo_filter(AtempoFilter *filter) {
    if (!filter || !filter->initialized) {
        return 0;
    }
    
    int ret = av_buffersrc_add_frame(filter->src_ctx, NULL);
    if (ret < 0) {
        return -1;
    }
    return 0;
}

/**
 * 自定义FFmpeg日志回调，完全禁止输出到终端
 * 防止日志输出破坏ncurses界面
 */
static void ffmpeg_log_callback(void *ptr, int level, const char *fmt, va_list vl) {
    // 完全丢弃所有日志，不输出到终端
    // 这防止了FFmpeg日志破坏ncurses界面
}

static void extract_parent_directory(const char *path, char *dest, size_t dest_size) {
    if (!dest || dest_size == 0) {
        return;
    }

    dest[0] = '\0';
    if (!path || path[0] == '\0') {
        return;
    }

    const char *slash = strrchr(path, '/');
    if (!slash) {
        return;
    }

    size_t length = (size_t)(slash - path);
    if (length >= dest_size) {
        length = dest_size - 1;
    }

    memcpy(dest, path, length);
    dest[length] = '\0';
}

void persist_playback_session_state(void) {
    int should_resume = 0;
    int play_index = -1;
    int play_position = 0;
    char track_path[MAX_PATH_LEN];
    char track_folder_path[MAX_PATH_LEN];

    reap_finished_playback_thread();

    pthread_mutex_lock(&g_play_mutex);
    if (g_play_thread_running &&
        (g_play_state == PLAY_STATE_PLAYING || g_play_state == PLAY_STATE_PAUSED) &&
        g_current_play_index >= 0) {
        should_resume = 1;
        play_index = g_current_play_index;
        play_position = g_current_position;
    }
    pthread_mutex_unlock(&g_play_mutex);

    if (!should_resume || playlist_get_track_path(play_index, track_path, sizeof(track_path)) != 0) {
        g_app_config.resume_last_playback = 0;
        g_app_config.last_played_position = 0;
        g_app_config.last_played_folder_path[0] = '\0';
        g_app_config.last_played_track_path[0] = '\0';
        save_config();
        return;
    }

    if (play_position < 0) {
        play_position = 0;
    }
    if (g_total_duration > 0 && play_position >= g_total_duration) {
        play_position = g_total_duration > 1 ? g_total_duration - 1 : 0;
    }

    g_app_config.resume_last_playback = 1;
    g_app_config.last_played_position = play_position;
    snprintf(g_app_config.last_played_track_path, sizeof(g_app_config.last_played_track_path),
             "%s", track_path);
    extract_parent_directory(track_path,
                             track_folder_path, sizeof(track_folder_path));
    snprintf(g_app_config.last_played_folder_path, sizeof(g_app_config.last_played_folder_path),
             "%s", track_folder_path);
    save_config();
}

static int pulse_ensure_connected(void) {
    if (pa_connected && pa_ml && pa_ctx) return 0;  /* 已连接 */
    if (!pulse_loaded) return -1;

    if (!pa_ml) {
        pa_ml = P.mainloop_new();
        if (!pa_ml) return -1;
    }
    if (!pa_ctx) {
        pa_ctx = P.context_new(P.mainloop_get_api(pa_ml), APP_NAME);
        if (!pa_ctx) { P.mainloop_free(pa_ml); pa_ml = NULL; return -1; }
        P.context_connect(pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
    }
    /* 等待连接就绪 */
    int timeout = 500;
    while (timeout > 0) {
        int state = P.context_get_state(pa_ctx);
        if (state == PA_CONTEXT_READY) { pa_connected = 1; return 0; }
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) break;
        P.mainloop_iterate(pa_ml, 1, NULL);
        timeout--;
    }
    /* 连接失败，清理 */
    if (pa_ctx) { P.context_disconnect(pa_ctx); P.context_unref(pa_ctx); pa_ctx = NULL; }
    if (pa_ml) { P.mainloop_free(pa_ml); pa_ml = NULL; }
    pa_connected = 0;
    return -1;
}

static int audio_backend_prepare_stream(int sample_rate, int channels) {
    g_output_sample_rate = sample_rate;
    g_output_channels = channels;

    unsigned int latency_usec = (unsigned int)get_configured_latency_ms() * 1000U;
    unsigned int minreq_usec = latency_usec / 4U;
    if (minreq_usec < 5000U) {
        minreq_usec = 5000U;
    }
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
        ba.tlength = P.usec_to_bytes((uint64_t)latency_usec, &pa_ss);
        ba.prebuf = 0;
        ba.minreq = P.usec_to_bytes((uint64_t)minreq_usec, &pa_ss);
        ba.fragsize = (uint32_t)-1;

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

        if (A.pcm_open(&alsa_pcm, g_default_audio_device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            update_controls_status(audio_text("无法打开 ALSA 设备", "Cannot open ALSA device"));
            alsa_pcm = NULL;
            return -1;
        }

        if (A.pcm_set_params(alsa_pcm,
                               SND_PCM_FORMAT_S32_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED,
                               (unsigned int)channels,
                               (unsigned int)sample_rate,
                               1,
                               latency_usec) < 0) {
            update_controls_status(audio_text("无法配置 ALSA 设备", "Cannot configure ALSA device"));
            A.pcm_close(alsa_pcm);
            alsa_pcm = NULL;
            return -1;
        }

        return 0;
    }

    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        if (pw_prepare_stream(sample_rate, channels) == 0)
            return 0;
        /* PipeWire 流未就绪（如无输出节点），退回到其他后端 */
        log_warn("audio", "PipeWire backend failed, falling back to next backend");
        pw_cleanup_stream_locked();  /* 清理 PW 侧残存资源 */
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

static void audio_backend_cleanup_stream(void) {
    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE && pa_s) {
        // Uncork stream if paused, prevents potential hang on disconnect
        if (P.stream_is_corked(pa_s)) {
            P.stream_cork(pa_s, 0, NULL, NULL);
        }
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

static void audio_backend_flush_stream(void) {
    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE && pa_s && pa_ml && pa_ctx) {
        int state = P.stream_get_state(pa_s);
        if (state == PA_STREAM_READY) {
            pa_operation *op = P.stream_flush(pa_s, NULL, NULL);
            if (op) {
                while (P.operation_get_state(op) == PA_OPERATION_RUNNING) {
                    P.mainloop_iterate(pa_ml, 1, NULL);
                }
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

static int audio_backend_write_samples(const int32_t *samples, int frame_count) {
    if (!samples || frame_count <= 0) {
        return 0;
    }

    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE) {
        size_t bytes = (size_t)frame_count * pa_ss.channels * sizeof(int32_t);

        if (!pa_s || P.stream_get_state(pa_s) != PA_STREAM_READY) {
            return -1;
        }

        while (pa_s && P.stream_get_state(pa_s) == PA_STREAM_READY &&
               P.stream_writable_size(pa_s) < bytes) {
            if (!g_play_thread_running) {
                return 0;
            }
            // 使用非阻塞模式，防止 stream 被 cork 后永久阻塞
            // cork 后无事件到达，blocking 的 mainloop_iterate 永不返回
            P.mainloop_iterate(pa_ml, 0, NULL);
            usleep(1000);
        }

        if (!pa_s || P.stream_get_state(pa_s) != PA_STREAM_READY) {
            return -1;
        }

        return P.stream_write(pa_s, samples, bytes, NULL, 0, PA_SEEK_RELATIVE);
    }

    if (alsa_loaded && g_active_backend == AUDIO_BACKEND_ALSA) {
        int written = 0;
        int wait_timeout_ms = get_configured_latency_ms();
        if (wait_timeout_ms < 20) {
            wait_timeout_ms = 20;
        }

        while (written < frame_count) {
            if (!g_play_thread_running) {
                return 0;
            }

            if (A.pcm_wait(alsa_pcm, wait_timeout_ms) < 0) {
                A.pcm_prepare(alsa_pcm);
            }
            snd_pcm_sframes_t ret = A.pcm_writei(alsa_pcm,
                                                   samples + (written * g_output_channels),
                                                   frame_count - written);
            if (ret > 0) {
                written += (int)ret;
                continue;
            }
            if (ret == -EAGAIN) {
                continue;
            }
            if (ret == -EPIPE || ret == -ESTRPIPE) {
                A.pcm_prepare(alsa_pcm);
                continue;
            }
            return -1;
        }

        return 0;
    }

    if (pipewire_loaded && g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        return pw_write_samples(samples, frame_count);
    }

    return -1;
}

static void audio_backend_pause_stream(void) {
    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE && pa_s && pa_ml && pa_ctx) {
        int state = P.stream_get_state(pa_s);
        if (state == PA_STREAM_READY && !P.stream_is_corked(pa_s)) {
            pa_operation *op = P.stream_cork(pa_s, 1, NULL, NULL);
            if (op) {
                P.operation_unref(op);
            }
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

static void audio_backend_resume_stream(void) {
    if (pulse_loaded && g_active_backend == AUDIO_BACKEND_PULSE && pa_s && pa_ml && pa_ctx) {
        int state = P.stream_get_state(pa_s);
        if (state == PA_STREAM_READY && P.stream_is_corked(pa_s)) {
            pa_operation *op = P.stream_cork(pa_s, 0, NULL, NULL);
            if (op) {
                P.operation_unref(op);
            }
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

/**
 * 初始化FFmpeg库
 */
void init_ffmpeg() {
    log_info("audio", "Initializing FFmpeg");
    avformat_network_init();
    // 禁用FFmpeg日志输出，避免干扰UI
    av_log_set_level(AV_LOG_QUIET);
    // 设置自定义日志回调，完全禁止任何输出
    av_log_set_callback(ffmpeg_log_callback);
    log_debug("audio", "FFmpeg initialized, log suppressed");
}

void audio_backend_shutdown(void) {
    if (pulse_loaded && pa_s) {
        P.stream_disconnect(pa_s);
        P.stream_unref(pa_s);
        pa_s = NULL;
    }
    if (pulse_loaded && pa_ml) {
        if (pa_ctx) {
            P.context_disconnect(pa_ctx);
            P.context_unref(pa_ctx);
            pa_ctx = NULL;
        }
        P.mainloop_free(pa_ml);
        pa_ml = NULL;
    }
    pa_connected = 0;
    if (alsa_loaded && alsa_pcm) {
        A.pcm_drop(alsa_pcm);
        A.pcm_close(alsa_pcm);
        alsa_pcm = NULL;
    }
    alsa_ready = 0;
    if (pipewire_loaded) {
        pw_cleanup_stream();
        pipewire_unload();
        pipewire_loaded = 0;
    }
}

void init_audio_device() {
    log_info("audio", "Initializing audio device");

    /* 尝试运行时加载音频后端库 */
    if (pulse_load() == 0) {
        pulse_loaded = 1;
        log_info("audio", "PulseAudio library loaded");
    } else {
        pulse_loaded = 0;
        log_info("audio", "PulseAudio library not available");
    }
    if (alsa_load() == 0) {
        alsa_loaded = 1;
        log_info("audio", "ALSA library loaded");
    } else {
        alsa_loaded = 0;
        log_info("audio", "ALSA library not available");
    }
    if (pipewire_load() == 0) {
        pipewire_loaded = 1;
        log_info("audio", "PipeWire library loaded");
    } else {
        pipewire_loaded = 0;
        log_info("audio", "PipeWire library not available");
    }

    /* 优先级策略：PipeWire > PulseAudio > ALSA */
    if (g_active_backend == AUDIO_BACKEND_AUTO || g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        if (pipewire_loaded) {
            log_info("audio", "Trying PipeWire backend");
            /* 轻量探测：只检验守护进程可达，不创建流、不启动 RT 线程 */
            if (pw_probe_backend() == 0) {
                g_active_backend = AUDIO_BACKEND_PIPEWIRE;
                log_info("audio", "PipeWire backend selected");
                printf("%s\n", audio_text("当前使用 PipeWire 音频后端", "Using PipeWire backend"));
                return;
            }
            /* 连接失败 — 清理并回退 */
            pipewire_unload();
            pipewire_loaded = 0;
            log_info("audio", "PipeWire connection failed, falling back");
        } else if (g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
            log_error("audio", "PipeWire library could not be loaded");
            return;
        }
    }

    if (g_active_backend == AUDIO_BACKEND_AUTO || g_active_backend == AUDIO_BACKEND_PULSE) {
        if (pulse_loaded) {
            log_info("audio", "Trying PulseAudio backend");
            pa_ml = P.mainloop_new();
            if (!pa_ml) {
                log_warn("audio", "Failed to create PulseAudio mainloop");
                goto pulse_failed;
            }

            pa_ctx = P.context_new(P.mainloop_get_api(pa_ml), APP_NAME);
            if (!pa_ctx) {
                log_warn("audio", "Failed to create PulseAudio context");
                P.mainloop_free(pa_ml);
                pa_ml = NULL;
                goto pulse_failed;
            }

            P.context_connect(pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
            while (P.context_get_state(pa_ctx) != PA_CONTEXT_READY) {
                if (P.context_get_state(pa_ctx) == PA_CONTEXT_FAILED ||
                    P.context_get_state(pa_ctx) == PA_CONTEXT_TERMINATED) {
                    log_warn("audio", "Failed to connect to PulseAudio server");
                    P.context_unref(pa_ctx);
                    pa_ctx = NULL;
                    P.mainloop_free(pa_ml);
                    pa_ml = NULL;
                    goto pulse_failed;
                }
                P.mainloop_iterate(pa_ml, 1, NULL);
            }

            pa_connected = 1;
            g_active_backend = AUDIO_BACKEND_PULSE;
            log_info("audio", "PulseAudio connected successfully");
            printf("%s\n", audio_text("已连接到 PulseAudio 服务", "Connected to PulseAudio"));
            return;

pulse_failed:
            pa_connected = 0;
            if (g_active_backend == AUDIO_BACKEND_PULSE) {
                log_error("audio", "PulseAudio selected but unavailable");
                return;
            }
            log_info("audio", "PulseAudio not available, will try ALSA");
        } else if (g_active_backend == AUDIO_BACKEND_PULSE) {
            log_error("audio", "PulseAudio library could not be loaded");
            return;
        }
    }

    if (g_active_backend == AUDIO_BACKEND_AUTO || g_active_backend == AUDIO_BACKEND_ALSA) {
        if (alsa_loaded) {
            alsa_ready = 1;
            g_active_backend = AUDIO_BACKEND_ALSA;
            log_info("audio", "ALSA backend ready, device='default'");
            printf("%s\n", audio_text("当前使用 ALSA 音频后端", "Using ALSA backend"));
            return;
        } else if (g_active_backend == AUDIO_BACKEND_ALSA) {
            log_error("audio", "ALSA library could not be loaded");
            return;
        }
    }

    log_error("audio", "No audio backend available");
}



int get_volume_percent(void) {
    int volume = 100;

    pthread_mutex_lock(&g_volume_mutex);
    volume = g_volume_percent;
    pthread_mutex_unlock(&g_volume_mutex);

    return volume;
}

void set_volume_percent(int volume) {
    int clamped = clamp_volume_percent(volume);
    int old_volume = g_volume_percent;
    int changed = 0;

    pthread_mutex_lock(&g_volume_mutex);
    if (g_volume_percent != clamped) {
        g_volume_percent = clamped;
        g_pending_volume_sync = 1;
        changed = 1;
    }
    pthread_mutex_unlock(&g_volume_mutex);

    if (changed) {
        log_info("audio", "Volume changed: %d%% -> %d%%", old_volume, clamped);
        g_app_config.volume_percent = clamped;
        save_config();

        char msg[64];
        snprintf(msg, sizeof(msg),
                 use_english_ui() ? "Volume: %d%%" : "音量：%d%%",
                 clamped);
        update_controls_status(msg);
        request_ui_refresh(UI_DIRTY_CONTROLS);
        signal_playback_thread();
    }
}

void adjust_volume(int delta) {
    set_volume_percent(get_volume_percent() + delta);
}

extern void render_playlist_content();
extern void render_controls();

/**
 * 获取循环模式的字符串表示
 */
const char *get_loop_mode_str() {
    switch(g_loop_mode) {
        case LOOP_OFF:
            return audio_text("关闭", "Off");
        case LOOP_SINGLE:
            return audio_text("单曲", "Single");
        case LOOP_LIST:
            return audio_text("列表", "List");
        case LOOP_RANDOM:
            return audio_text("随机", "Random");
        default:
            return audio_text("关闭", "Off");
    }
}

/**
 * 切换循环模式
 * 按顺序切换：Off -> Single -> List -> Random -> Off
 */
void toggle_loop_mode() {
    LoopMode old_mode = g_loop_mode;
    g_loop_mode = (g_loop_mode + 1) % 4;
    log_info("audio", "Loop mode changed: %d -> %d (%s)", old_mode, g_loop_mode, get_loop_mode_str());
    // 不再调用update_controls_status，避免阻塞
    render_controls();
}

/**
 * 切换倍速播放
 * 按顺序切换：0.75x -> 1.0x -> 1.25x -> 1.5x -> 2.0x -> 3.0x -> 0.75x
 * 切换倍速后重启播放以应用新的 atempo 滤镜
 */
void toggle_playback_speed(void) {
    float old_speed = g_playback_speed;
    g_speed_index = (g_speed_index + 1) % g_speed_count;
    g_playback_speed = g_speed_ratios[g_speed_index];
    log_info("audio", "Playback speed changed: %.2fx -> %.2fx", old_speed, g_playback_speed);
    g_app_config.default_playback_speed = g_playback_speed;
    save_config();
    char msg[64];
    snprintf(msg, sizeof(msg), "%s: %.2fx",
             use_english_ui() ? "Speed" : "倍速",
             g_playback_speed);
    update_controls_status(msg);
    render_controls();

    // 如果正在播放或暂停，重启播放以应用新的倍速
    if (g_play_state == PLAY_STATE_PLAYING || g_play_state == PLAY_STATE_PAUSED) {
        pthread_mutex_lock(&g_play_mutex);
        int was_paused = (g_play_state == PLAY_STATE_PAUSED);
        int current_idx = g_current_play_index;
        int current_pos = g_current_position;
        pthread_mutex_unlock(&g_play_mutex);

        // 设置重启位置，让新线程从当前位置开始播放
        g_initial_seek_position = current_pos;

        // 停止当前播放线程
        pthread_mutex_lock(&g_play_mutex);
        g_play_thread_running = 0;
        pthread_mutex_unlock(&g_play_mutex);
        signal_playback_thread();

        // 等待线程结束
        reap_finished_playback_thread();

        // 重新播放相同索引，会从 g_initial_seek_position 恢复位置
        play_audio(current_idx);

        // 如果之前是暂停状态，恢复暂停
        if (was_paused) {
            // 等待一小段时间确保播放开始
            usleep(50000);
            pause_audio();
        }
    }
}

void reap_finished_playback_thread(void) {
    pthread_t thread_to_join;
    int should_join = 0;

    pthread_mutex_lock(&g_play_mutex);
    if (g_play_thread_active && g_play_thread_finished) {
        thread_to_join = g_play_thread;
        g_play_thread_finished = 0;
        should_join = 1;
    }
    pthread_mutex_unlock(&g_play_mutex);

    if (should_join && !pthread_equal(pthread_self(), thread_to_join)) {
        pthread_join(thread_to_join, NULL);
        pthread_mutex_lock(&g_play_mutex);
        g_play_thread_active = 0;
        pthread_mutex_unlock(&g_play_mutex);
    }
}

void process_pending_playback_action(void) {
    int pending_index = -1;

    pthread_mutex_lock(&g_play_mutex);
    if (g_pending_playback_index >= 0 && !g_play_thread_active) {
        pending_index = g_pending_playback_index;
        g_pending_playback_index = -1;
    }
    pthread_mutex_unlock(&g_play_mutex);

    if (pending_index >= 0) {
        play_audio(pending_index);
    }
}

void cleanup_playback_cache(void) {
    if (g_cached_audio_path[0]) {
        log_debug("audio", "Cleaning up cached audio: %s", g_cached_audio_path);
        unlink(g_cached_audio_path);
        g_cached_audio_path[0] = '\0';
    }
    if (g_cached_lyrics_path[0]) {
        log_debug("audio", "Cleaning up cached lyrics: %s", g_cached_lyrics_path);
        unlink(g_cached_lyrics_path);
        g_cached_lyrics_path[0] = '\0';
    }
}

void wait_for_playback_thread_shutdown(void) {
    int timeout_ms = 2000;
    int waited = 0;

    while (waited < timeout_ms) {
        reap_finished_playback_thread();

        pthread_mutex_lock(&g_play_mutex);
        int is_running = g_play_thread_running;
        int is_active = g_play_thread_active;
        int is_finished = g_play_thread_finished;
        pthread_mutex_unlock(&g_play_mutex);

        if (!is_running && !is_active && !is_finished) {
            break;
        }

        signal_playback_thread();
        usleep(10000);
        waited += 10;
    }

    if (waited >= timeout_ms) {
        log_warn("audio", "Playback thread did not stop within %dms, forcing shutdown", timeout_ms);
    }
}

static int wait_while_paused(void) {
    pthread_mutex_lock(&g_play_mutex);
    while (g_play_state == PLAY_STATE_PAUSED && g_play_thread_running) {
        pthread_cond_wait(&g_play_control_cond, &g_play_mutex);
    }
    int still_running = g_play_thread_running;
    pthread_mutex_unlock(&g_play_mutex);
    return still_running;
}

static int handle_seek_request_in_decoder(AVFormatContext *fmt_ctx,
                                          AVCodecContext *codec_ctx,
                                          SwrContext *swr_ctx,
                                          AVPacket *packet,
                                          PCMQueue *queue,
                                          int audio_stream_index,
                                          int *decoder_draining,
                                          int *decoder_finished) {
    int handled = 0;

    pthread_mutex_lock(&g_seek_mutex);
    if (g_seek_request && g_play_thread_running && fmt_ctx && codec_ctx) {
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
            if (swr_ctx) {
                swr_init(swr_ctx);
            }
            if (packet) {
                av_packet_unref(packet);
            }
            if (queue) {
                pcm_queue_reset(queue);
            }
            audio_backend_flush_stream();
            progress_tracker_seek(target_position);
            g_current_position = target_position;
            if (decoder_draining) {
                *decoder_draining = 0;
            }
            if (decoder_finished) {
                *decoder_finished = 0;
            }

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

static int decode_next_pcm_chunk(AVFormatContext *fmt_ctx,
                                 AVCodecContext *codec_ctx,
                                 SwrContext *swr_ctx,
                                 AtempoFilter *atempo_filter,
                                 AVPacket *packet,
                                 AVFrame *frame,
                                 AVFrame *filtered_frame,
                                 PCMQueue *queue,
                                 int audio_stream_index,
                                 int output_sample_rate,
                                 int output_channels,
                                 int use_resampler,
                                 int *decoder_draining,
                                 int *decoder_finished) {
    if (!fmt_ctx || !codec_ctx || !packet || !frame || !filtered_frame || !queue) {
        return -1;
    }

    while (!*decoder_finished) {
        // 首先尝试从 atempo 滤镜获取已处理的帧
        if (atempo_filter && atempo_filter->initialized) {
            int filter_ret = receive_frame_from_atempo(atempo_filter, filtered_frame);
            if (filter_ret == 0) {
                // 成功获取到滤镜输出帧
                PCMChunk *slot = pcm_queue_write_slot(queue);
                if (!slot) {
                    av_frame_unref(filtered_frame);
                    return 0;
                }

                int produced_frames = 0;
                int produced_bytes = 0;

                if (!use_resampler) {
                    produced_bytes = av_samples_get_buffer_size(NULL, output_channels, filtered_frame->nb_samples,
                                                                AV_SAMPLE_FMT_S32, 1);
                    if (produced_bytes > 0 && pcm_chunk_ensure_capacity(slot, produced_bytes) == 0) {
                        memcpy(slot->data, filtered_frame->data[0], (size_t)produced_bytes);
                        produced_frames = filtered_frame->nb_samples;
                    }
                } else {
                    int dst_nb_samples = av_rescale_rnd(
                        swr_get_delay(swr_ctx, atempo_filter->input_sample_rate) + filtered_frame->nb_samples,
                        output_sample_rate, atempo_filter->input_sample_rate, AV_ROUND_UP);
                    produced_bytes = av_samples_get_buffer_size(NULL, output_channels, dst_nb_samples,
                                                                AV_SAMPLE_FMT_S32, 1);
                    if (produced_bytes > 0 && pcm_chunk_ensure_capacity(slot, produced_bytes) == 0) {
                        uint8_t *output_planes[] = {(uint8_t *)slot->data};
                        produced_frames = swr_convert(swr_ctx, output_planes, dst_nb_samples,
                                                      (const uint8_t **)filtered_frame->data, filtered_frame->nb_samples);
                        if (produced_frames > 0) {
                            produced_bytes = produced_frames * output_channels * (int)sizeof(int32_t);
                        }
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
            // 如果有 atempo 滤镜，发送帧到滤镜
            if (atempo_filter && atempo_filter->initialized) {
                if (send_frame_to_atempo(atempo_filter, frame) < 0) {
                    av_frame_unref(frame);
                    return -1;
                }
                av_frame_unref(frame);
                continue;  // 继续循环以从滤镜获取输出
            }

            // 没有 atempo 滤镜，直接处理
            PCMChunk *slot = pcm_queue_write_slot(queue);
            if (!slot) {
                av_frame_unref(frame);
                return 0;
            }

            int produced_frames = 0;
            int produced_bytes = 0;

            if (!use_resampler) {
                produced_bytes = av_samples_get_buffer_size(NULL, output_channels, frame->nb_samples,
                                                            AV_SAMPLE_FMT_S32, 1);
                if (produced_bytes > 0 && pcm_chunk_ensure_capacity(slot, produced_bytes) == 0) {
                    memcpy(slot->data, frame->data[0], (size_t)produced_bytes);
                    produced_frames = frame->nb_samples;
                } else if (produced_bytes > 0) {
                    av_frame_unref(frame);
                    return -1;
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
                    if (produced_frames > 0) {
                        produced_bytes = produced_frames * output_channels * (int)sizeof(int32_t);
                    }
                } else if (produced_bytes > 0) {
                    av_frame_unref(frame);
                    return -1;
                }
            }

            av_frame_unref(frame);

            if (produced_frames > 0 && produced_bytes > 0) {
                pcm_queue_commit_write(queue, produced_frames, produced_bytes);
                return 1;
            }
            continue;
        }

        if (ret == AVERROR_EOF) {
            *decoder_finished = 1;
            return 0;
        }

        if (ret != AVERROR(EAGAIN)) {
            return -1;
        }

        if (!*decoder_draining) {
            while (1) {
                ret = av_read_frame(fmt_ctx, packet);
                if (ret < 0) {
                    *decoder_draining = 1;
                    ret = avcodec_send_packet(codec_ctx, NULL);
                    if (ret < 0 && ret != AVERROR_EOF) {
                        return -1;
                    }
                    break;
                }

                if (packet->stream_index != audio_stream_index) {
                    av_packet_unref(packet);
                    continue;
                }

                ret = avcodec_send_packet(codec_ctx, packet);
                av_packet_unref(packet);
                if (ret == AVERROR(EAGAIN)) {
                    break;
                }
                if (ret < 0) {
                    return -1;
                }
                break;
            }
        } else {
            *decoder_finished = 1;
            return 0;
        }
    }

    return 0;
}

/**
 * 播放音频文件的线程函数
 * 负责解码音频文件并通过音频后端输出到音频设备
 */
void *play_audio_thread(void *arg) {
    int index = *((int *)arg);
    int reached_end_of_stream = 0;
    int followup_index = -1;
    free(arg);

    log_info("audio", "Playback thread started for index=%d", index);

    pthread_mutex_lock(&g_play_mutex);
    int thread_running = g_play_thread_running;
    pthread_mutex_unlock(&g_play_mutex);

    char file_path[MAX_PATH_LEN];
    int valid_index = playlist_get_track_path(index, file_path, sizeof(file_path)) == 0;

    // 如果有本地缓存的音频文件，使用缓存路径
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
    int initial_seek_position = 0;
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

    // 获取歌曲总时长（秒）
    g_total_duration = fmt_ctx->duration / AV_TIME_BASE;
    
    // 验证时长的有效性，如果无效则尝试通过音频帧数计算
    if (g_total_duration <= 0) {
        // 尝试通过音频流的 duration 和 time_base 计算时长
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
        // 如果还是无法获取有效时长，设置为一个默认值（300 秒）
        if (g_total_duration <= 0) {
            g_total_duration = 300; // 默认 5 分钟
        }
    }
    
    // 重置当前播放位置为 0
    g_current_position = 0;
    reset_visualizer_state();
    
    // 检查是否需要跳转到特定位置（在开始播放之前）
    // 如果是重启跳转，从全局变量获取目标位置
    initial_seek_position = get_and_clear_initial_seek_position(); // 获取初始跳转位置，获取后清除
    
    // 强制更新 UI 显示，确保进度条从 0% 开始
    request_ui_refresh(UI_DIRTY_CONTROLS);
    
    // 找到音频流
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
    
    // 获取解码器
    AVCodecParameters *codec_par = fmt_ctx->streams[audio_stream_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        log_error("audio", "Unsupported codec in '%s' (codec_id=%d)", file_path, codec_par->codec_id);
        update_controls_status(audio_text("当前编解码器不受支持", "Unsupported codec"));
        goto cleanup;
    }
    
    // 创建解码器上下文
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        update_controls_status(audio_text("无法分配解码器上下文", "Cannot allocate codec context"));
        goto cleanup;
    }
    
    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) {
        update_controls_status(audio_text("无法复制编解码参数", "Cannot copy codec parameters"));
        goto cleanup;
    }
    
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        update_controls_status(audio_text("无法打开编解码器", "Cannot open codec"));
        goto cleanup;
    }
    
    input_channels = codec_channel_count(codec_ctx);
    if (input_channels <= 0) {
        input_channels = 2;
    }
    output_channels = (input_channels == 1) ? 1 : 2;
    // 使用原始采样率，倍速通过 atempo 滤镜实现
    output_sample_rate = codec_ctx->sample_rate > 0 ? codec_ctx->sample_rate : 44100;

    // 提取音频技术信息供 UI 显示，覆盖旧数据
    // 使用 codec_ctx 而非 codec_par，因为 avcodec_open2 后才填充完整
    g_audio_sample_rate = codec_ctx->sample_rate > 0 ? codec_ctx->sample_rate : 0;
    g_audio_bit_rate = (codec_ctx->bit_rate > 0) ? codec_ctx->bit_rate :
                       (codec_par->bit_rate > 0) ? codec_par->bit_rate : 0;
    // FLAC 等 VBR 格式的比特率常为 0，从文件大小/时长推算平均比特率
    if (g_audio_bit_rate <= 0 && g_total_duration > 0 && fmt_ctx && fmt_ctx->pb) {
        int64_t file_size = avio_size(fmt_ctx->pb);
        if (file_size > 0) {
            g_audio_bit_rate = (int)((file_size * 8) / g_total_duration);
        }
    }
    if (codec_ctx->bits_per_raw_sample > 0) {
        g_audio_bit_depth = codec_ctx->bits_per_raw_sample;
    } else {
        int bytes = av_get_bytes_per_sample(codec_ctx->sample_fmt);
        g_audio_bit_depth = bytes > 0 ? bytes * 8 : 0;
    }
    snprintf(g_audio_codec_name, sizeof(g_audio_codec_name), "%s", codec->name);

    log_debug("audio", "Audio stream: rate=%d, channels=%d, codec=%s, duration=%ds",
              output_sample_rate, output_channels,
              codec ? codec->name : "unknown",
              g_total_duration);
    prefill_target_frames = (output_sample_rate * get_pcm_prefill_target_ms(output_sample_rate) + 999) / 1000;
    
    // 检查是否需要重采样（输出格式不是 S16 或通道数需要转换）
    use_resampler = (codec_ctx->sample_fmt != AV_SAMPLE_FMT_S32 || input_channels != output_channels);

    if (use_resampler) {
        swr_ctx = swr_alloc();
        if (!swr_ctx) {
            update_controls_status(audio_text("无法分配重采样器", "Cannot allocate resampler"));
            goto cleanup;
        }

        if (init_resampler(swr_ctx, codec_ctx, input_channels, output_channels, output_sample_rate) < 0) {
            update_controls_status(audio_text("无法初始化重采样器", "Cannot initialize resampler"));
            goto cleanup;
        }
    }
    
    // 初始化 atempo 滤镜（如果倍速不是 1.0x）
    if (init_atempo_filter(&g_atempo_filter, codec_ctx, g_playback_speed) < 0) {
        update_controls_status(audio_text("无法初始化倍速滤镜", "Cannot initialize speed filter"));
        goto cleanup;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    AVFrame *filtered_frame = av_frame_alloc();
    if (!packet || !frame || !filtered_frame) {
        update_controls_status(audio_text("无法分配解码数据结构", "Cannot allocate decode structures"));
        goto cleanup;
    }

    if (pcm_queue_init(&pcm_queue) < 0) {
        update_controls_status(audio_text("无法分配音频缓冲区", "Cannot allocate audio buffer"));
        goto cleanup;
    }
    pcm_queue_initialized = 1;

    if (audio_backend_prepare_stream(output_sample_rate, output_channels) < 0) {
        goto cleanup;
    }

    /* PipeWire 后端需要显式激活流使其从 PAUSED 进入 STREAMING。
     * PulseAudio/ALSA 不需要此步骤（它们使用 push 模型）。 */
    audio_backend_resume_stream();

    audio_backend_sync_volume(1);

    progress_tracker_init(output_sample_rate);
    progress_tracker_set_sample_rate(output_sample_rate);
    progress_tracker_set_speed(g_playback_speed);
    progress_tracker_start();

    g_play_state = PLAY_STATE_PLAYING;

    if (initial_seek_position > 0 && initial_seek_position < g_total_duration) {
        pthread_mutex_lock(&g_seek_mutex);
        g_seek_position = initial_seek_position;
        g_seek_request = 1;
        pthread_mutex_unlock(&g_seek_mutex);
    }

    while (g_play_thread_running) {
        audio_backend_sync_volume(0);

        if (!wait_while_paused()) {
            break;
        }

        if (handle_seek_request_in_decoder(fmt_ctx, codec_ctx, swr_ctx, packet, &pcm_queue,
                                           audio_stream_index, &decoder_draining, &decoder_finished)) {
            if (g_current_position >= g_total_duration) {
                g_play_thread_running = 0;
                break;
            }
            continue;
        }

        while (g_play_thread_running &&
               !decoder_finished &&
               pcm_queue.count < PCM_QUEUE_CAPACITY &&
               pcm_queue.buffered_frames < prefill_target_frames) {
            int decode_result = decode_next_pcm_chunk(fmt_ctx, codec_ctx, swr_ctx, &g_atempo_filter, 
                                                      packet, frame, filtered_frame, &pcm_queue,
                                                      audio_stream_index, output_sample_rate, output_channels,
                                                      use_resampler, &decoder_draining, &decoder_finished);
            if (decode_result < 0) {
                playback_error = 1;
                break;
            }
            if (decode_result == 0) {
                break;
            }
        }

        if (playback_error) {
            break;
        }

        PCMChunk *chunk = pcm_queue_peek(&pcm_queue);
        if (!chunk) {
            if (decoder_finished) {
                reached_end_of_stream = 1;
                break;
            }
            continue;
        }

        if (pcm_queue_buffered_ms(&pcm_queue, output_sample_rate) > get_configured_latency_ms()) {
            push_visualizer_samples(chunk->data, chunk->frame_count, output_channels);
        }
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
    // 刷新并清理 atempo 滤镜
    if (g_atempo_filter.initialized && decoder_draining) {
        flush_atempo_filter(&g_atempo_filter);
        // 消费滤镜中剩余的帧
        while (receive_frame_from_atempo(&g_atempo_filter, filtered_frame) == 0) {
            av_frame_unref(filtered_frame);
        }
    }
    cleanup_atempo_filter(&g_atempo_filter);

    audio_backend_cleanup_stream();

    if (pcm_queue_initialized) {
        pcm_queue_destroy(&pcm_queue);
    }

    av_frame_free(&filtered_frame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    progress_tracker_on_stop();
    reset_visualizer_state();

    if (!reached_end_of_stream) {
        g_current_position = 0;
    }

    int playlist_total = playlist_count();

    pthread_mutex_lock(&g_play_mutex);
    if (g_play_thread_running && reached_end_of_stream) {
        switch (g_loop_mode) {
            case LOOP_SINGLE:
                followup_index = index;
                break;
            case LOOP_LIST:
                if (playlist_total > 0) {
                    followup_index = (index + 1) % playlist_total;
                }
                break;
            case LOOP_RANDOM:
                if (playlist_total > 0) {
                    followup_index = rand() % playlist_total;
                }
                break;
            case LOOP_OFF:
            default:
                break;
        }
    }

    if (reached_end_of_stream && followup_index >= 0) {
        log_info("audio", "End of stream for index=%d, scheduling follow-up index=%d (loop=%d)",
                 index, followup_index, g_loop_mode);
    } else if (reached_end_of_stream) {
        log_info("audio", "End of stream for index=%d, no follow-up (loop=%d)", index, g_loop_mode);
    } else if (playback_error) {
        log_warn("audio", "Playback error for index=%d, stopping thread", index);
    }

    g_play_thread_running = 0;
    g_play_thread_finished = 1;
    g_play_state = PLAY_STATE_STOPPED;
    g_current_play_index = -1;

    // 清理音频技术信息（新曲目开始时由提取代码重新填充）
    g_audio_sample_rate = 0;
    g_audio_bit_rate = 0;
    g_audio_bit_depth = 0;
    g_audio_codec_name[0] = '\0';

    if (followup_index >= 0 && g_pending_playback_index < 0) {
        g_pending_playback_index = followup_index;
    }
    pthread_mutex_unlock(&g_play_mutex);

    if (followup_index < 0) {
        clear_lyrics();
        request_ui_refresh(UI_DIRTY_LYRICS);
    }
    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS);

    log_debug("audio", "Playback thread exiting for index=%d (eos=%d, err=%d)",
              index, reached_end_of_stream, playback_error);
    return NULL;
}

/**
 * 远程下载进度回调包装 — 定期刷新 ncurses 界面
 */
static void remote_progress_refresh(void) {
    refresh();
}

/**
 * 在排序激活时，将曲目索引（物理位置）转换为视觉位置（屏幕行号）
 */
static int visual_position_of(int track_index) {
    if (!g_sort_state.active) return track_index;
    for (int i = 0; i < g_playlist.count; i++) {
        if (g_sort_state.sorted_indices[i] == track_index)
            return i;
    }
    return track_index;
}

/**
 * 播放指定索引的音频文件
 * 如果已有播放线程在运行，则先停止当前播放
 */
void play_audio(int index) {
    char track_path[MAX_PATH_LEN];
    if (playlist_get_track_path(index, track_path, sizeof(track_path)) != 0) {
        log_warn("audio", "play_audio(%d): invalid track index", index);
        return;
    }
    log_info("audio", "play_audio(index=%d) track='%s'", index, track_path);

    reap_finished_playback_thread();

    pthread_mutex_lock(&g_play_mutex);

    // 允许倍速切换重启：如果有初始跳转位置（g_initial_seek_position > 0），
    // 说明是倍速切换导致的重启，允许重新播放相同索引
    int is_speed_change_restart = (g_initial_seek_position > 0);

    if (!is_speed_change_restart && g_play_state == PLAY_STATE_PLAYING && g_current_play_index == index) {
        log_debug("audio", "Skipping play: same index=%d already playing", index);
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }

    if (g_play_thread_active) {
        int was_paused = (g_play_state == PLAY_STATE_PAUSED);

        log_debug("audio", "Pending play_audio(%d) - thread already active, scheduling switch", index);
        g_pending_playback_index = index;
        g_play_thread_running = 0;
        g_seek_request = 0;
        g_play_state = PLAY_STATE_STOPPED;

        pthread_mutex_unlock(&g_play_mutex);

        // 线程可能卡在 audio_backend_write_samples 的 PulseAudio mainloop 阻塞等待中
        // (stream 被 cork 后无事件到达，mainloop_iterate 永远不返回)
        // 发送 condvar 信号对该状态无效，必须先 uncork stream 唤醒 PA mainloop
        // 注意：此问题仅 PulseAudio 有；ALSA 有超时机制，PipeWire 使用非阻塞 ring buffer
        if (g_active_backend == AUDIO_BACKEND_PULSE) {
            audio_backend_resume_stream();
        }
        signal_playback_thread();

        if (was_paused) {
            // 暂停状态的线程应能快速退出，直接等待它结束
            // 然后立即启动新曲目，避免依赖异步 pending 的重入问题
            for (int i = 0; i < 50; i++) {
                reap_finished_playback_thread();
                pthread_mutex_lock(&g_play_mutex);
                if (!g_play_thread_active) {
                    g_pending_playback_index = -1;
                    g_current_play_index = index;
                    g_play_thread_active = 1;
                    g_play_thread_running = 1;
                    g_play_thread_finished = 0;
                    g_play_state = PLAY_STATE_STOPPED;
                    pthread_mutex_unlock(&g_play_mutex);
                    goto start_playback;
                }
                pthread_mutex_unlock(&g_play_mutex);
                usleep(10000);
            }
            log_warn("audio", "Paused thread did not exit within 500ms, falling back to async switch");
        }

        request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
        return;
    }

    g_pending_playback_index = -1;
    g_current_play_index = index;
    g_play_thread_active = 1;
    g_play_thread_running = 1;
    g_play_thread_finished = 0;
    g_play_state = PLAY_STATE_STOPPED;

    pthread_mutex_unlock(&g_play_mutex);

start_playback: {
    // 远程音频缓存：下载到本地 /tmp 后再播放，确保 seek 正常
    char local_audio_path[MAX_PATH_LEN] = "";
    char local_lyrics_path[MAX_PATH_LEN] = "";
    if (remote_is_remote_path(track_path)) {
        cleanup_playback_cache();

        // 下载期间定期刷新 ncurses 界面，保持响应
        remote_set_progress_hook(remote_progress_refresh);

        // 从原始 URL 中提取扩展名
        const char *src_ext = strrchr(track_path, '.');
        char ext_buf[16] = "";
        if (src_ext) {
            size_t ext_len = strlen(src_ext);
            if (ext_len < sizeof(ext_buf))
                strncpy(ext_buf, src_ext, ext_len);
        }

        // 下载音频文件到临时路径（保留扩展名供 FFmpeg 识别格式）
        char tmp_audio[] = "/tmp/ter-music-cache-XXXXXX";
        int audio_fd = -1;
        if (ext_buf[0]) {
            char pattern[MAX_PATH_LEN];
            snprintf(pattern, sizeof(pattern), "/tmp/ter-music-cache-XXXXXX%s", ext_buf);
            audio_fd = mkstemps(pattern, (int)strlen(ext_buf));
            if (audio_fd >= 0) {
                close(audio_fd);
                strncpy(tmp_audio, pattern, sizeof(tmp_audio) - 1);
            }
        } else {
            audio_fd = mkstemp(tmp_audio);
            if (audio_fd >= 0) close(audio_fd);
        }
        if (audio_fd >= 0) {
            update_controls_status(audio_text("正在下载...", "Downloading..."));
            refresh();
            if (remote_fetch_to_file(track_path, tmp_audio) == 0) {
                strncpy(g_cached_audio_path, tmp_audio, MAX_PATH_LEN - 1);
                strncpy(local_audio_path, tmp_audio, MAX_PATH_LEN - 1);
            } else {
                unlink(tmp_audio);
            }
        }

        // 下载歌词文件（保留 .lrc 扩展名）
        char lrc_url[MAX_PATH_LEN];
        strncpy(lrc_url, track_path, MAX_PATH_LEN - 1);
        char *dot = strrchr(lrc_url, '.');
        if (dot) {
            strcpy(dot, ".lrc");
            char tmp_lrc[] = "/tmp/ter-music-cache-XXXXXX.lrc";
            int lrc_fd = mkstemps(tmp_lrc, 4);
            if (lrc_fd >= 0) {
                close(lrc_fd);
                if (remote_fetch_to_file(lrc_url, tmp_lrc) == 0) {
                    strncpy(g_cached_lyrics_path, tmp_lrc, MAX_PATH_LEN - 1);
                    strncpy(local_lyrics_path, tmp_lrc, MAX_PATH_LEN - 1);
                } else {
                    unlink(tmp_lrc);
                }
            }
        }

        // 远程下载完成，清除进度钩子
        remote_set_progress_hook(NULL);
    }

    int *index_ptr = malloc(sizeof(int));
    if (!index_ptr) {
        pthread_mutex_lock(&g_play_mutex);
        g_play_thread_active = 0;
        g_current_play_index = -1;
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    *index_ptr = index;

    if (pthread_create(&g_play_thread, NULL, play_audio_thread, index_ptr) != 0) {
        log_error("audio", "Failed to create playback thread for index=%d", index);
        free(index_ptr);
        pthread_mutex_lock(&g_play_mutex);
        g_play_thread_active = 0;
        g_play_thread_running = 0;
        g_play_state = PLAY_STATE_STOPPED;
        g_current_play_index = -1;
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }

    pthread_mutex_lock(&g_play_mutex);
    g_play_state = PLAY_STATE_PLAYING;
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();

    // 有本地缓存的歌词则使用缓存路径
    load_lyrics(local_lyrics_path[0] ? local_lyrics_path : track_path);
    render_lyrics();

    // 有本地缓存的音频则可提取封面
    update_album_cover_for_track(local_audio_path[0] ? local_audio_path : track_path);

    Track track;
    get_track_metadata(index, &track);

    char msg[64];
    snprintf(msg, sizeof(msg), "%s%s - %s",
        audio_text("正在播放：", "Playing: "),
        track.title, track.artist);
    update_controls_status(msg);
    add_history_entry(&track);
    log_info("audio", "Now playing: '%s' - '%s' (idx=%d)", track.title, track.artist, index);
    render_playlist_content();
    request_ui_refresh(UI_DIRTY_CONTROLS);
    }
}

/**
 * 暂停当前播放的音频
 * 仅在播放状态下有效
 */
void pause_audio() {
    log_debug("audio", "pause_audio() called");
    pthread_mutex_lock(&g_play_mutex);

    // 二次验证：确保仍在播放状态且播放线程运行中
    if (g_play_state != PLAY_STATE_PLAYING || !g_play_thread_running) {
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    
    // 验证当前播放索引有效
    if (g_current_play_index < 0 || g_current_play_index >= playlist_count()) {
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    
    // 设置暂停状态
    g_play_state = PLAY_STATE_PAUSED;
    
    audio_backend_pause_stream();
    
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();
    
    // 通知进度跟踪器（在锁外调用，避免死锁）
    progress_tracker_on_pause();
    render_playlist_content();
}

/**
 * 恢复已暂停的音频播放
 * 仅在暂停状态下有效
 */
void resume_audio() {
    log_debug("audio", "resume_audio() called");
    pthread_mutex_lock(&g_play_mutex);

    // 二次验证：确保仍在暂停状态且播放线程运行中
    if (g_play_state != PLAY_STATE_PAUSED || !g_play_thread_running) {
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    
    // 验证当前播放索引有效
    if (g_current_play_index < 0 || g_current_play_index >= playlist_count()) {
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    
    // 设置播放状态
    g_play_state = PLAY_STATE_PLAYING;
    
    audio_backend_resume_stream();
    
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();
    
    /* 通知进度跟踪器（在锁外调用，避免死锁） */
    progress_tracker_on_resume();
    render_playlist_content();
    update_progress_bar();   // 确保进度条立刻同步
    update_lyrics_display(); // 确保歌词从新位置高亮
}

/**
 * 停止当前播放的音频
 * 停止播放线程并重置播放状态
 */
void stop_audio() {
    log_debug("audio", "stop_audio() called");
    reap_finished_playback_thread();

    pthread_mutex_lock(&g_play_mutex);
    g_play_thread_running = 0;
    g_pending_playback_index = -1;
    g_seek_request = 0;
    g_play_state = PLAY_STATE_STOPPED;
    g_current_play_index = -1;
    g_audio_sample_rate = 0;
    g_audio_bit_rate = 0;
    g_audio_bit_depth = 0;
    g_audio_codec_name[0] = '\0';
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();

    cleanup_playback_cache();

    g_current_position = 0;
    progress_tracker_on_stop();
    clear_lyrics();
    reset_visualizer_state();
    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
}

/**
 * 播放下一曲
 * 根据当前循环模式决定下一曲的选择逻辑
 */
void next_track() {
    log_debug("audio", "next_track() called, loop=%d, current_idx=%d", g_loop_mode, g_current_play_index);
    int playlist_total = playlist_count();
    if (playlist_total == 0) {
        return;
    }
    
    int next_index;
    if (g_loop_mode == LOOP_RANDOM) {
        next_index = rand() % playlist_total;
    } else if (g_sort_state.active) {
        // 排序激活时按视觉顺序跳转
        int visual_pos = (g_current_play_index >= 0)
            ? visual_position_of(g_current_play_index)
            : g_selected_index;
        int next_visual = (visual_pos + 1) % playlist_total;
        next_index = g_sort_state.sorted_indices[next_visual];
        g_selected_index = next_visual;
    } else {
        if (g_current_play_index >= 0) {
            next_index = g_current_play_index + 1;
        } else {
            next_index = g_selected_index + 1;
        }

        if (next_index >= playlist_total) {
            next_index = 0;
        }
    }

    play_audio(next_index);
}

/**
 * 播放上一曲
 * 根据当前循环模式决定上一曲的选择逻辑
 */
void prev_track() {
    log_debug("audio", "prev_track() called, loop=%d, current_idx=%d", g_loop_mode, g_current_play_index);
    int playlist_total = playlist_count();
    if (playlist_total == 0) {
        return;
    }

    int prev_index;
    if (g_loop_mode == LOOP_RANDOM) {
        prev_index = rand() % playlist_total;
    } else if (g_sort_state.active) {
        // 排序激活时按视觉顺序跳转
        int visual_pos = (g_current_play_index >= 0)
            ? visual_position_of(g_current_play_index)
            : g_selected_index;
        int prev_visual = (visual_pos - 1 + playlist_total) % playlist_total;
        prev_index = g_sort_state.sorted_indices[prev_visual];
        g_selected_index = prev_visual;
    } else {
        if (g_current_play_index >= 0) {
            prev_index = g_current_play_index - 1;
        } else {
            prev_index = g_selected_index - 1;
        }

        if (prev_index < 0) {
            prev_index = playlist_total - 1;
        }
    }

    play_audio(prev_index);
}

/**
 * 跳转到指定位置
 * 通过停止当前播放线程再重新启动，确保从目标位置准确开始播放
 */
void seek_audio(double position) {
    log_debug("audio", "seek_audio(pos=%.1f) called, total=%d", position, g_total_duration);
    reap_finished_playback_thread();

    // 参数校验
    if (position < 0) position = 0;
    if (g_total_duration > 0 && position > g_total_duration) position = g_total_duration;
    
    int int_position = (int)position;
    
    // 如果未在播放，仅更新 UI 状态
    if (g_play_state == PLAY_STATE_STOPPED || !g_play_thread_running) {
        pthread_mutex_lock(&g_play_mutex);
        g_current_position = int_position;
        pthread_mutex_unlock(&g_play_mutex);
        
        // 同步进度跟踪器（如果已初始化）
        if (progress_tracker_is_ready()) {
            progress_tracker_seek(int_position);
        }
        
        update_progress_bar();
        render_controls();
        return;
    }

    pthread_mutex_lock(&g_seek_mutex);
    g_seek_position = int_position;
    g_current_position = int_position;
    g_seek_request = 1;
    pthread_mutex_unlock(&g_seek_mutex);
    signal_playback_thread();

    update_progress_bar();
    render_controls();
    render_playlist_content();
    update_lyrics_display();
    media_session_notify_seek((uint64_t)g_current_position * 1000ULL);
}

/**
 * 获取初始跳转位置（用于重启播放时）
 * 调用后会清除存储的位置，避免重复使用
 */
int get_and_clear_initial_seek_position(void) {
    int pos = g_initial_seek_position;
    g_initial_seek_position = 0;
    return pos;
}
