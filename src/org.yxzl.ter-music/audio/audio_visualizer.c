/**
 * @file audio_visualizer.c
 * @brief 音频频谱分析 — FFT 可视化数据生成
 *
 * 播放线程写 PCM 时只把样本追加到一条短 PCM 环；独立工作线程按 40ms
 * 节拍、以“模拟播放游标”从环中取 128 帧分析。这样频谱更新频率不再受
 * 音频后端写入突发（PipeWire 一次排空大缓冲、播放线程随后成批补写）
 * 影响，UI 与远端 RPC 都能稳定拿到约 25 FPS 的数据。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "audio/audio_internal.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VISUALIZER_ANALYSIS_SIZE 128
#define VISUALIZER_SOURCE_SIZE 2048
#define VISUALIZER_ANALYSIS_INTERVAL_MS 40ULL

/* 约 3 秒 @44.1kHz 的单声道 PCM 历史。足以覆盖 PipeWire 1MB 环的
 * 提前量，同时让工作线程可以按播放时间平滑地向前取样。 */
#define VISUALIZER_PCM_RING_FRAMES 131072

static int g_visualizer_levels[VISUALIZER_BAND_COUNT] = {0};
static int g_visualizer_peaks[VISUALIZER_BAND_COUNT] = {0};
static uint64_t g_visualizer_last_update_ms = 0;

static pthread_mutex_t g_visualizer_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 单声道 PCM 历史环：播放线程写，分析线程读。 */
static int32_t g_pcm_ring[VISUALIZER_PCM_RING_FRAMES];
static uint64_t g_pcm_write_frames = 0;
static uint64_t g_pcm_read_frames = 0;
static int g_pcm_cursor_ready = 0;

static pthread_once_t g_worker_once = PTHREAD_ONCE_INIT;

static uint64_t vis_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ============================================================
 * Analysis worker
 * ============================================================ */

static void analyze_mono_window(const int32_t *mono)
{
    static double window[VISUALIZER_ANALYSIS_SIZE];
    static int window_initialized = 0;

    if (!window_initialized) {
        for (int i = 0; i < VISUALIZER_ANALYSIS_SIZE; i++) {
            window[i] = 0.5 - 0.5 * cos((2.0 * M_PI * (double)i) /
                                        (double)(VISUALIZER_ANALYSIS_SIZE - 1));
        }
        window_initialized = 1;
    }

    double samples[VISUALIZER_ANALYSIS_SIZE];
    for (int i = 0; i < VISUALIZER_ANALYSIS_SIZE; i++) {
        samples[i] = ((double)mono[i] / 2147483648.0) * window[i];
    }

    double magnitudes[VISUALIZER_ANALYSIS_SIZE / 2] = {0.0};
    int useful_bins = VISUALIZER_ANALYSIS_SIZE / 2;

    for (int bin = 1; bin < useful_bins; bin++) {
        double real = 0.0, imag = 0.0;
        double coeff = (2.0 * M_PI * (double)bin) / (double)VISUALIZER_ANALYSIS_SIZE;
        for (int n = 0; n < VISUALIZER_ANALYSIS_SIZE; n++) {
            double angle = coeff * (double)n;
            real += samples[n] * cos(angle);
            imag -= samples[n] * sin(angle);
        }
        double magnitude = sqrt(real * real + imag * imag) /
                           ((double)VISUALIZER_ANALYSIS_SIZE / 2.0);
        double emphasis = 1.0 + ((double)bin / (double)(useful_bins - 1)) * 0.35;
        magnitudes[bin] = magnitude * emphasis;
    }

    pthread_mutex_lock(&g_visualizer_mutex);
    for (int i = 0; i < VISUALIZER_BAND_COUNT; i++) {
        int start_bin = 1 + (i * (useful_bins - 1)) / VISUALIZER_BAND_COUNT;
        int end_bin = 1 + ((i + 1) * (useful_bins - 1)) / VISUALIZER_BAND_COUNT;
        if (end_bin <= start_bin) end_bin = start_bin + 1;
        if (end_bin > useful_bins) end_bin = useful_bins;

        double band_energy = 0.0;
        for (int bin = start_bin; bin < end_bin; bin++) {
            if (magnitudes[bin] > band_energy) band_energy = magnitudes[bin];
        }

        double compressed = log1p(band_energy * 48.0) / log1p(49.0);
        int scaled_level = (int)lround(compressed * 100.0);
        if (scaled_level < 0) scaled_level = 0;
        if (scaled_level > 100) scaled_level = 100;

        int previous = g_visualizer_levels[i];
        if (scaled_level > previous)
            g_visualizer_levels[i] = (previous + scaled_level * 3) / 4;
        else
            g_visualizer_levels[i] = (previous * 3 + scaled_level) / 4;

        if (g_visualizer_levels[i] < 1) g_visualizer_levels[i] = 0;
        if (g_visualizer_levels[i] > g_visualizer_peaks[i])
            g_visualizer_peaks[i] = g_visualizer_levels[i];
        else if (g_visualizer_peaks[i] > 0)
            g_visualizer_peaks[i] -= 1;
    }
    g_visualizer_last_update_ms = vis_now_ms();
    pthread_mutex_unlock(&g_visualizer_mutex);
}

static void *visualizer_worker(void *arg)
{
    (void)arg;
    const struct timespec interval = {
        .tv_sec = (time_t)(VISUALIZER_ANALYSIS_INTERVAL_MS / 1000ULL),
        .tv_nsec = (long)((VISUALIZER_ANALYSIS_INTERVAL_MS % 1000ULL) * 1000000ULL)
    };

    while (1) {
        nanosleep(&interval, NULL);

        /* 暂停时没有新的可听数据；让 UI 侧最后更新时间自然衰减。 */
        if (g_play_state != PLAY_STATE_PLAYING) {
            continue;
        }

        int32_t window[VISUALIZER_ANALYSIS_SIZE];
        int have_window = 0;

        pthread_mutex_lock(&g_visualizer_mutex);
        uint64_t write_frames = g_pcm_write_frames;
        uint64_t sample_rate = (g_output_sample_rate > 0)
            ? (uint64_t)g_output_sample_rate : 44100ULL;

        if (!g_pcm_cursor_ready && write_frames > 0) {
            /* 从后端缓冲之外开始：PipeWire 的 1MB 环保存的是尚未播出的
             * 数据，直接读写指针会把频谱提前数秒。 */
            int buffered = audio_backend_buffered_frames();
            uint64_t lag = buffered > 0 ? (uint64_t)buffered
                : sample_rate * (uint64_t)get_configured_latency_ms() / 1000ULL;
            uint64_t max_lag = VISUALIZER_PCM_RING_FRAMES - VISUALIZER_SOURCE_SIZE;
            if (lag > max_lag) lag = max_lag;
            g_pcm_read_frames = write_frames > lag ? write_frames - lag : 0;
            g_pcm_cursor_ready = 1;
        }

        if (g_pcm_cursor_ready && write_frames >= VISUALIZER_SOURCE_SIZE) {
            uint64_t advance = sample_rate * VISUALIZER_ANALYSIS_INTERVAL_MS / 1000ULL;
            uint64_t read_frames = g_pcm_read_frames + advance;
            if (read_frames > write_frames) read_frames = write_frames;

            /* 环形历史被覆盖时向前跳，避免读到别的曲段。 */
            uint64_t oldest_safe = write_frames > (VISUALIZER_PCM_RING_FRAMES -
                                                    VISUALIZER_SOURCE_SIZE)
                ? write_frames - (VISUALIZER_PCM_RING_FRAMES - VISUALIZER_SOURCE_SIZE)
                : 0;
            if (read_frames < oldest_safe) read_frames = oldest_safe;
            if (read_frames > write_frames) read_frames = write_frames;

            if (read_frames >= VISUALIZER_SOURCE_SIZE) {
                for (int i = 0; i < VISUALIZER_ANALYSIS_SIZE; i++) {
                    uint64_t offset = (uint64_t)i * VISUALIZER_SOURCE_SIZE /
                                      VISUALIZER_ANALYSIS_SIZE;
                    uint64_t idx = read_frames - VISUALIZER_SOURCE_SIZE + offset;
                    window[i] = g_pcm_ring[idx % VISUALIZER_PCM_RING_FRAMES];
                }
                have_window = 1;
            }
            g_pcm_read_frames = read_frames;
        }
        pthread_mutex_unlock(&g_visualizer_mutex);

        if (have_window) {
            analyze_mono_window(window);
        }
    }
    return NULL;
}

static void start_worker(void)
{
    pthread_t tid;
    if (pthread_create(&tid, NULL, visualizer_worker, NULL) == 0) {
        pthread_detach(tid);
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void reset_visualizer_state(void)
{
    pthread_mutex_lock(&g_visualizer_mutex);
    memset(g_visualizer_levels, 0, sizeof(g_visualizer_levels));
    memset(g_visualizer_peaks, 0, sizeof(g_visualizer_peaks));
    g_visualizer_last_update_ms = vis_now_ms();
    memset(g_pcm_ring, 0, sizeof(g_pcm_ring));
    g_pcm_write_frames = 0;
    g_pcm_read_frames = 0;
    g_pcm_cursor_ready = 0;
    pthread_mutex_unlock(&g_visualizer_mutex);
}

void push_visualizer_samples(const int32_t *samples, int frame_count, int channels)
{
    if (!samples || frame_count <= 0 || channels <= 0) return;

    pthread_once(&g_worker_once, start_worker);

    pthread_mutex_lock(&g_visualizer_mutex);
    for (int i = 0; i < frame_count; i++) {
        int64_t mixed = 0;
        for (int ch = 0; ch < channels; ch++) {
            mixed += samples[(size_t)i * (size_t)channels + (size_t)ch];
        }
        int32_t mono = (int32_t)(mixed / channels);
        g_pcm_ring[g_pcm_write_frames % VISUALIZER_PCM_RING_FRAMES] = mono;
        g_pcm_write_frames++;
    }
    pthread_mutex_unlock(&g_visualizer_mutex);
}

void get_visualizer_snapshot(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms)
{
    if (!levels || !peaks || max_levels <= 0) return;
    int copy_count = max_levels < VISUALIZER_BAND_COUNT ? max_levels : VISUALIZER_BAND_COUNT;

    pthread_mutex_lock(&g_visualizer_mutex);
    for (int i = 0; i < copy_count; i++) {
        levels[i] = g_visualizer_levels[i];
        peaks[i]  = g_visualizer_peaks[i];
    }
    if (last_update_ms) *last_update_ms = g_visualizer_last_update_ms;
    pthread_mutex_unlock(&g_visualizer_mutex);
}
