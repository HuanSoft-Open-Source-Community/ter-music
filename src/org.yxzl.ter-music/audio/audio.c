/**
 * @file audio.c
 * @brief 音频框架层 — 全局状态、初始化、播放控制 API
 *
 * 底层功能已拆分到：
 *   - atempo.c         : atempo 倍速滤镜
 *   - audio_visualizer.c : FFT 频谱分析
 *   - playback_thread.c  : PCM 队列、解码循环、play_audio_thread
 *   - backend_ops.c      : 音频后端流操作
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "audio/audio.h"
#include "audio/audio_internal.h"
#include "playlist/playlist.h"
#include "ui/ui.h"
#include "config/config.h"
#include "media/session.h"
#include "ui/menus.h"
#include "audio/progress/progress.h"
#include "ui/lyrics.h"
#include "remote/remote.h"
#include "logger/logger.h"
#include "ui/braille/braille_art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>

#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif

/* ============================================================
 * Function pointer tables (backend dispatching)
 * ============================================================ */

struct pulseaudio_funcs P = {0};
struct alsa_funcs A = {0};
struct pipewire_funcs PW = {0};

/* ============================================================
 * Global state
 * ============================================================ */

int g_active_backend = AUDIO_BACKEND_AUTO;

pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_volume_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_visualizer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  g_play_control_cond = PTHREAD_COND_INITIALIZER;

int g_volume_percent = 100;
int g_pending_volume_sync = 0;
int g_output_sample_rate = 44100;
int g_output_channels = 2;

int g_audio_sample_rate = 0;
int g_audio_bit_rate = 0;
int g_audio_bit_depth = 0;
char g_audio_codec_name[32] = "";

/* ── Playback state ── */
PlayState g_play_state = PLAY_STATE_STOPPED;
int g_current_play_index = -1;
LoopMode g_loop_mode = LOOP_OFF;
pthread_t g_play_thread;
int g_play_thread_running = 0;
pthread_mutex_t g_play_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_play_thread_active = 0;
int g_play_thread_finished = 0;
int g_pending_playback_index = -1;

/* ── Remote cache paths ── */
char g_cached_audio_path[MAX_PATH_LEN] = "";
char g_cached_lyrics_path[MAX_PATH_LEN] = "";

/* ── Speed ── */
float g_playback_speed = 1.0f;
static float g_speed_ratios[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
static int g_speed_index = 1;
static int g_speed_count = sizeof(g_speed_ratios) / sizeof(g_speed_ratios[0]);

/* ── Seek ── */
int g_seek_request = 0;
int g_seek_position = 0;
int g_initial_seek_position = 0;

/* ── Progress ── */
int g_current_position = 0;
int g_total_duration = 0;
pthread_mutex_t g_seek_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Default audio device ── */
char g_default_audio_device[128] = "default";

/* ============================================================
 * Helpers
 * ============================================================ */

const char *audio_text(const char *utf8, const char *ascii) {
    return use_english_ui() ? ascii : utf8;
}

static int clamp_volume_percent(int volume) {
    if (volume < 0) return 0;
    if (volume > 100) return 100;
    return volume;
}

int get_configured_latency_ms(void) {
    int latency_ms = g_app_config.audio_latency_ms;
    if (latency_ms < 20) latency_ms = 20;
    if (latency_ms > 250) latency_ms = 250;
    return latency_ms;
}

int get_pcm_prefill_target_ms(int sample_rate) {
    int prefill_ms = get_configured_latency_ms() * 3;
    if (prefill_ms < PCM_QUEUE_MIN_PREFILL_MS) prefill_ms = PCM_QUEUE_MIN_PREFILL_MS;
    if (sample_rate >= 88200 && prefill_ms < 240) prefill_ms = 240;
    if (sample_rate >= 176400 && prefill_ms < 320) prefill_ms = 320;
    if (prefill_ms > PCM_QUEUE_MAX_PREFILL_MS) prefill_ms = PCM_QUEUE_MAX_PREFILL_MS;
    return prefill_ms;
}

void apply_volume_to_samples(int32_t *samples, int sample_count)
{
    if (g_active_backend == AUDIO_BACKEND_PULSE ||
        g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        (void)samples; (void)sample_count; return;
    }
    if (!samples || sample_count <= 0) return;
    int volume = get_volume_percent();
    if (volume >= 100) return;
    if (volume <= 0) { memset(samples, 0, (size_t)sample_count * sizeof(int32_t)); return; }
    for (int i = 0; i < sample_count; i++)
        samples[i] = (int32_t)(((int64_t)samples[i] * volume) / 100);
}

static void signal_playback_thread(void) {
    pthread_cond_broadcast(&g_play_control_cond);
}

/* ============================================================
 * FFmpeg log callback & helpers
 * ============================================================ */

static void ffmpeg_log_callback(void *ptr, int level, const char *fmt, va_list vl) {
    (void)ptr; (void)level; (void)fmt; (void)vl;
}

void init_ffmpeg(void)
{
    log_info("audio", "Initializing FFmpeg");
    avformat_network_init();
    av_log_set_level(AV_LOG_QUIET);
    av_log_set_callback(ffmpeg_log_callback);
    log_debug("audio", "FFmpeg initialized, log suppressed");
}

void audio_backend_shutdown(void)
{
    if (pulse_loaded && pa_s) { P.stream_disconnect(pa_s); P.stream_unref(pa_s); pa_s = NULL; }
    if (pulse_loaded && pa_ml) {
        if (pa_ctx) { P.context_disconnect(pa_ctx); P.context_unref(pa_ctx); pa_ctx = NULL; }
        P.mainloop_free(pa_ml); pa_ml = NULL;
    }
    pa_connected = 0;
    if (alsa_loaded && alsa_pcm) { A.pcm_drop(alsa_pcm); A.pcm_close(alsa_pcm); alsa_pcm = NULL; }
    alsa_ready = 0;
    if (pipewire_loaded) { pw_cleanup_stream(); pipewire_unload(); pipewire_loaded = 0; }
}

int audio_backend_is_available(int backend) {
    if (backend == AUDIO_BACKEND_PULSE)    return pulse_loaded;
    if (backend == AUDIO_BACKEND_ALSA)     return alsa_loaded;
    if (backend == AUDIO_BACKEND_PIPEWIRE) return pipewire_loaded;
    return 0;
}

/* ============================================================
 * Audio device initialization
 * ============================================================ */

void init_audio_device(void)
{
    log_info("audio", "Initializing audio device");

    if (pulse_load() == 0) { pulse_loaded = 1; log_info("audio", "PulseAudio library loaded"); }
    else { pulse_loaded = 0; log_info("audio", "PulseAudio library not available"); }
    if (alsa_load() == 0) { alsa_loaded = 1; log_info("audio", "ALSA library loaded"); }
    else { alsa_loaded = 0; log_info("audio", "ALSA library not available"); }
    if (pipewire_load() == 0) { pipewire_loaded = 1; log_info("audio", "PipeWire library loaded"); }
    else { pipewire_loaded = 0; log_info("audio", "PipeWire library not available"); }

    /* PipeWire > PulseAudio > ALSA */
    if (g_active_backend == AUDIO_BACKEND_AUTO || g_active_backend == AUDIO_BACKEND_PIPEWIRE) {
        if (pipewire_loaded) {
            log_info("audio", "Trying PipeWire backend");
            if (pw_probe_backend() == 0) {
                g_active_backend = AUDIO_BACKEND_PIPEWIRE;
                log_info("audio", "PipeWire backend selected");
                printf("%s\n", audio_text("当前使用 PipeWire 音频后端", "Using PipeWire backend"));
                return;
            }
            pipewire_unload(); pipewire_loaded = 0;
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
            if (!pa_ml) { log_warn("audio", "Failed to create PulseAudio mainloop"); goto pulse_failed; }
            pa_ctx = P.context_new(P.mainloop_get_api(pa_ml), APP_NAME);
            if (!pa_ctx) { log_warn("audio", "Failed to create PulseAudio context"); P.mainloop_free(pa_ml); pa_ml = NULL; goto pulse_failed; }
            P.context_connect(pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
            while (P.context_get_state(pa_ctx) != PA_CONTEXT_READY) {
                if (P.context_get_state(pa_ctx) == PA_CONTEXT_FAILED || P.context_get_state(pa_ctx) == PA_CONTEXT_TERMINATED) {
                    log_warn("audio", "Failed to connect to PulseAudio server");
                    P.context_unref(pa_ctx); pa_ctx = NULL; P.mainloop_free(pa_ml); pa_ml = NULL; goto pulse_failed;
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
            if (g_active_backend == AUDIO_BACKEND_PULSE) { log_error("audio", "PulseAudio selected but unavailable"); return; }
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

/* ============================================================
 * Volume management
 * ============================================================ */

int get_volume_percent(void)
{
    int volume = 100;
    pthread_mutex_lock(&g_volume_mutex);
    volume = g_volume_percent;
    pthread_mutex_unlock(&g_volume_mutex);
    return volume;
}

void set_volume_percent(int volume)
{
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
        snprintf(msg, sizeof(msg), use_english_ui() ? "Volume: %d%%" : "音量：%d%%", clamped);
        update_controls_status(msg);
        request_ui_refresh(UI_DIRTY_CONTROLS);
        signal_playback_thread();
    }
}

void adjust_volume(int delta) {
    set_volume_percent(get_volume_percent() + delta);
}

/* ============================================================
 * Loop mode / speed
 * ============================================================ */

const char *get_loop_mode_str(void)
{
    switch(g_loop_mode) {
        case LOOP_OFF:    return audio_text("关闭", "Off");
        case LOOP_SINGLE: return audio_text("单曲", "Single");
        case LOOP_LIST:   return audio_text("列表", "List");
        case LOOP_RANDOM: return audio_text("随机", "Random");
        default:          return audio_text("关闭", "Off");
    }
}

void toggle_loop_mode(void)
{
    g_loop_mode = (g_loop_mode + 1) % 4;
    log_info("audio", "Loop mode changed: %d (%s)", g_loop_mode, get_loop_mode_str());
    render_controls();
}

void toggle_playback_speed(void)
{
    float old_speed = g_playback_speed;
    g_speed_index = (g_speed_index + 1) % g_speed_count;
    g_playback_speed = g_speed_ratios[g_speed_index];
    log_info("audio", "Playback speed changed: %.2fx -> %.2fx", (double)old_speed, (double)g_playback_speed);
    g_app_config.default_playback_speed = g_playback_speed;
    save_config();
    char msg[64];
    snprintf(msg, sizeof(msg), "%s: %.2fx", use_english_ui() ? "Speed" : "倍速", (double)g_playback_speed);
    update_controls_status(msg);
    render_controls();

    if (g_play_state == PLAY_STATE_PLAYING || g_play_state == PLAY_STATE_PAUSED) {
        pthread_mutex_lock(&g_play_mutex);
        int was_paused = (g_play_state == PLAY_STATE_PAUSED);
        int current_idx = g_current_play_index;
        int current_pos = g_current_position;
        pthread_mutex_unlock(&g_play_mutex);

        g_initial_seek_position = current_pos;
        pthread_mutex_lock(&g_play_mutex);
        g_play_thread_running = 0;
        pthread_mutex_unlock(&g_play_mutex);
        signal_playback_thread();
        reap_finished_playback_thread();
        play_audio(current_idx);
        if (was_paused) { usleep(50000); pause_audio(); }
    }
}

/* ============================================================
 * Thread management
 * ============================================================ */

void reap_finished_playback_thread(void)
{
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

void process_pending_playback_action(void)
{
    int pending_index = -1;
    pthread_mutex_lock(&g_play_mutex);
    if (g_pending_playback_index >= 0 && !g_play_thread_active) {
        pending_index = g_pending_playback_index;
        g_pending_playback_index = -1;
    }
    pthread_mutex_unlock(&g_play_mutex);
    if (pending_index >= 0) play_audio(pending_index);
}

void cleanup_playback_cache(void)
{
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

void wait_for_playback_thread_shutdown(void)
{
    int timeout_ms = 2000;
    int waited = 0;
    while (waited < timeout_ms) {
        reap_finished_playback_thread();
        pthread_mutex_lock(&g_play_mutex);
        int is_running = g_play_thread_running;
        int is_active  = g_play_thread_active;
        int is_finished = g_play_thread_finished;
        pthread_mutex_unlock(&g_play_mutex);
        if (!is_running && !is_active && !is_finished) break;
        signal_playback_thread();
        usleep(10000);
        waited += 10;
    }
    if (waited >= timeout_ms)
        log_warn("audio", "Playback thread did not stop within %dms, forcing shutdown", timeout_ms);
}

/* ============================================================
 * Session persistence
 * ============================================================ */

static void extract_parent_directory(const char *path, char *dest, size_t dest_size)
{
    if (!dest || dest_size == 0) { dest[0] = '\0'; return; }
    dest[0] = '\0';
    if (!path || path[0] == '\0') return;
    const char *slash = strrchr(path, '/');
    if (!slash) return;
    size_t length = (size_t)(slash - path);
    if (length >= dest_size) length = dest_size - 1;
    memcpy(dest, path, length);
    dest[length] = '\0';
}

void persist_playback_session_state(void)
{
    int should_resume = 0;
    int play_index = -1, play_position = 0;
    char track_path[MAX_PATH_LEN], track_folder_path[MAX_PATH_LEN];

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

    if (play_position < 0) play_position = 0;
    if (g_total_duration > 0 && play_position >= g_total_duration)
        play_position = g_total_duration > 1 ? g_total_duration - 1 : 0;

    g_app_config.resume_last_playback = 1;
    g_app_config.last_played_position = play_position;
    snprintf(g_app_config.last_played_track_path, sizeof(g_app_config.last_played_track_path), "%s", track_path);
    extract_parent_directory(track_path, track_folder_path, sizeof(track_folder_path));
    snprintf(g_app_config.last_played_folder_path, sizeof(g_app_config.last_played_folder_path), "%s", track_folder_path);
    save_config();
}

/* ============================================================
 * Remote download progress callback
 * ============================================================ */

static void remote_progress_refresh(void) { refresh(); }

/* ============================================================
 * Visual position helper (for sorted display)
 * ============================================================ */

static int visual_position_of(int track_index)
{
    if (!g_sort_state.active) return track_index;
    for (int i = 0; i < g_playlist.count; i++) {
        if (g_sort_state.sorted_indices[i] == track_index) return i;
    }
    return track_index;
}

/* ============================================================
 * Play audio (public API)
 * ============================================================ */

void play_audio(int index)
{
    char track_path[MAX_PATH_LEN];
    if (playlist_get_track_path(index, track_path, sizeof(track_path)) != 0) {
        log_warn("audio", "play_audio(%d): invalid track index", index);
        return;
    }
    log_info("audio", "play_audio(index=%d) track='%s'", index, track_path);

    reap_finished_playback_thread();
    pthread_mutex_lock(&g_play_mutex);

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

        if (g_active_backend == AUDIO_BACKEND_PULSE) audio_backend_resume_stream();
        signal_playback_thread();

        if (was_paused) {
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

start_playback:
    {
    char local_audio_path[MAX_PATH_LEN] = "";
    char local_lyrics_path[MAX_PATH_LEN] = "";

    if (remote_is_remote_path(track_path)) {
        cleanup_playback_cache();
        remote_set_progress_hook(remote_progress_refresh);

        const char *src_ext = strrchr(track_path, '.');
        char ext_buf[16] = "";
        if (src_ext) {
            size_t ext_len = strlen(src_ext);
            if (ext_len < sizeof(ext_buf)) strncpy(ext_buf, src_ext, ext_len);
        }

        char tmp_audio[] = "/tmp/ter-music-cache-XXXXXX";
        int audio_fd = -1;
        if (ext_buf[0]) {
            char pattern[MAX_PATH_LEN];
            snprintf(pattern, sizeof(pattern), "/tmp/ter-music-cache-XXXXXX%s", ext_buf);
            audio_fd = mkstemps(pattern, (int)strlen(ext_buf));
            if (audio_fd >= 0) { close(audio_fd); strncpy(tmp_audio, pattern, sizeof(tmp_audio) - 1); }
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

    load_lyrics(local_lyrics_path[0] ? local_lyrics_path : track_path);
    render_lyrics();
    update_album_cover_for_track(local_audio_path[0] ? local_audio_path : track_path);

    Track track;
    get_track_metadata(index, &track);
    char msg[64];
    snprintf(msg, sizeof(msg), "%s%s - %s",
             audio_text("正在播放：", "Playing: "), track.title, track.artist);
    update_controls_status(msg);
    add_history_entry(&track);
    log_info("audio", "Now playing: '%s' - '%s' (idx=%d)", track.title, track.artist, index);
    render_playlist_content();
    request_ui_refresh(UI_DIRTY_CONTROLS);
    }
}

/* ============================================================
 * Pause / Resume / Stop
 * ============================================================ */

void pause_audio(void)
{
    log_debug("audio", "pause_audio() called");
    pthread_mutex_lock(&g_play_mutex);
    if (g_play_state != PLAY_STATE_PLAYING || !g_play_thread_running) {
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    if (g_current_play_index < 0 || g_current_play_index >= playlist_count()) {
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    g_play_state = PLAY_STATE_PAUSED;
    audio_backend_pause_stream();
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();
    progress_tracker_on_pause();
    render_playlist_content();
}

void resume_audio(void)
{
    log_debug("audio", "resume_audio() called");
    pthread_mutex_lock(&g_play_mutex);
    if (g_play_state != PLAY_STATE_PAUSED || !g_play_thread_running) {
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    if (g_current_play_index < 0 || g_current_play_index >= playlist_count()) {
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    g_play_state = PLAY_STATE_PLAYING;
    audio_backend_resume_stream();
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();
    progress_tracker_on_resume();
    render_playlist_content();
    update_progress_bar();
    update_lyrics_display();
}

void stop_audio(void)
{
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

/* ============================================================
 * Next / Prev
 * ============================================================ */

void next_track(void)
{
    log_debug("audio", "next_track() called, loop=%d, current_idx=%d", g_loop_mode, g_current_play_index);
    int playlist_total = playlist_count();
    if (playlist_total == 0) return;

    int next_index;
    if (g_loop_mode == LOOP_RANDOM) {
        next_index = rand() % playlist_total;
    } else if (g_sort_state.active) {
        int visual_pos = (g_current_play_index >= 0) ? visual_position_of(g_current_play_index) : g_selected_index;
        int next_visual = (visual_pos + 1) % playlist_total;
        next_index = g_sort_state.sorted_indices[next_visual];
        g_selected_index = next_visual;
    } else {
        next_index = (g_current_play_index >= 0) ? g_current_play_index + 1 : g_selected_index + 1;
        if (next_index >= playlist_total) next_index = 0;
    }
    play_audio(next_index);
}

void prev_track(void)
{
    log_debug("audio", "prev_track() called, loop=%d, current_idx=%d", g_loop_mode, g_current_play_index);
    int playlist_total = playlist_count();
    if (playlist_total == 0) return;

    int prev_index;
    if (g_loop_mode == LOOP_RANDOM) {
        prev_index = rand() % playlist_total;
    } else if (g_sort_state.active) {
        int visual_pos = (g_current_play_index >= 0) ? visual_position_of(g_current_play_index) : g_selected_index;
        int prev_visual = (visual_pos - 1 + playlist_total) % playlist_total;
        prev_index = g_sort_state.sorted_indices[prev_visual];
        g_selected_index = prev_visual;
    } else {
        prev_index = (g_current_play_index >= 0) ? g_current_play_index - 1 : g_selected_index - 1;
        if (prev_index < 0) prev_index = playlist_total - 1;
    }
    play_audio(prev_index);
}

/* ============================================================
 * Seek
 * ============================================================ */

void seek_audio(double position)
{
    log_debug("audio", "seek_audio(pos=%.1f) called, total=%d", position, g_total_duration);
    reap_finished_playback_thread();

    if (position < 0) position = 0;
    if (g_total_duration > 0 && position > g_total_duration) position = g_total_duration;

    int int_position = (int)position;

    if (g_play_state == PLAY_STATE_STOPPED || !g_play_thread_running) {
        pthread_mutex_lock(&g_play_mutex);
        g_current_position = int_position;
        pthread_mutex_unlock(&g_play_mutex);
        if (progress_tracker_is_ready()) progress_tracker_seek(int_position);
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

int get_and_clear_initial_seek_position(void)
{
    int pos = g_initial_seek_position;
    g_initial_seek_position = 0;
    return pos;
}
