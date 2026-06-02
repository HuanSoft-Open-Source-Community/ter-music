/**
 * @file audio_visualizer.c
 * @brief 音频频谱分析 — FFT 可视化数据生成
 *
 * 从 audio.c 拆分，负责对 PCM 数据进行 FFT 分析，生成频谱数据供 UI 调用。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_visualizer_levels[VISUALIZER_BAND_COUNT] = {0};
static int g_visualizer_peaks[VISUALIZER_BAND_COUNT] = {0};
static uint64_t g_visualizer_last_update_ms = 0;
static uint64_t g_visualizer_last_analysis_ms = 0;

#define VISUALIZER_ANALYSIS_SIZE 128
#define VISUALIZER_UPDATE_INTERVAL_MS 40ULL

static pthread_mutex_t g_visualizer_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t vis_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
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
    g_visualizer_last_analysis_ms = 0;
    pthread_mutex_unlock(&g_visualizer_mutex);
}

void push_visualizer_samples(const int32_t *samples, int frame_count, int channels)
{
    if (!samples || frame_count <= 0 || channels <= 0) return;

    uint64_t now_ms = vis_now_ms();
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
        for (int i = 0; i < VISUALIZER_ANALYSIS_SIZE; i++)
            window[i] = 0.5 - 0.5 * cos((2.0 * M_PI * (double)i) / (double)(VISUALIZER_ANALYSIS_SIZE - 1));
        window_initialized = 1;
    }

    double mono[VISUALIZER_ANALYSIS_SIZE];
    for (int i = 0; i < VISUALIZER_ANALYSIS_SIZE; i++) {
        int src_frame = (i * frame_count) / VISUALIZER_ANALYSIS_SIZE;
        if (src_frame >= frame_count) src_frame = frame_count - 1;
        double mixed = 0.0;
        for (int ch = 0; ch < channels; ch++)
            mixed += (double)samples[src_frame * channels + ch];
        mixed /= (double)channels * 2147483648.0;
        mono[i] = mixed * window[i];
    }

    double magnitudes[VISUALIZER_ANALYSIS_SIZE / 2] = {0.0};
    int useful_bins = VISUALIZER_ANALYSIS_SIZE / 2;

    for (int bin = 1; bin < useful_bins; bin++) {
        double real = 0.0, imag = 0.0;
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
    g_visualizer_last_update_ms = now_ms;
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
