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
#include "audio/play_queue.h"
#include "audio/play_mode_util.h"
#include "audio/karaoke_cache.h"
#include "audio/karaoke_model.h"
#include "audio/karaoke_worker.h"
#include "audio/visualizer.h"
#include "config/config.h"
#include "core/core.h"
#include "i18n/i18n.h"
#include "logger/logger.h"
#include "media/session.h"
#include "queue/backend_queue.h"
#include "audio/progress/progress.h"
#include "ui/cover/album_cover.h"
#include "lyrics/lyrics.h"
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

/* 本进程是否初始化过音频引擎（core 宿主才会调用 init_audio_device()）。
 * 远端前端只做 D-Bus 客户端：它绝不能在本进程起播放线程/占音频设备。
 * 这个标志守住那条边界，详见 play_audio() 的退出闸门。 */
static int g_audio_engine_initialized = 0;

/* 用户显式指定的后端（初始化时从配置捕获）：运行时回退时优先尝试它 */
int g_requested_backend = AUDIO_BACKEND_AUTO;

/* 音频后端候选顺序：PipeWire > PulseAudio > ALSA。
 * AUTO 选择与运行时回退共用这一顺序（定义在此处供 backend_ops.c 复用）。 */
const int g_backend_candidates[] = {
    AUDIO_BACKEND_PIPEWIRE,
    AUDIO_BACKEND_PULSE,
    AUDIO_BACKEND_ALSA
};

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
/* 当前出声曲目的路径（身份），与队列位置解耦：队列重排后下标会变，
 * 但路径仍是同一首歌；当前条目被删除后下标为 -1，路径仍可用于上报/暂停。 */
char g_current_play_path[MAX_PATH_LEN] = "";
PlayMode g_play_mode = PLAY_MODE_SEQUENTIAL;
pthread_t g_play_thread;
int g_play_thread_running = 0;
pthread_mutex_t g_play_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_play_thread_active = 0;
int g_play_thread_finished = 0;
int g_pending_playback_index = -1;

/* ── Speed ── */
float g_playback_speed = 1.0f;
float g_speed_ratios[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
int g_speed_index = 1;
int g_speed_count = sizeof(g_speed_ratios) / sizeof(g_speed_ratios[0]);

/* ── Seek ── */
int g_seek_request = 0;
int g_seek_position = 0;
int g_initial_seek_position = 0;
int g_cue_offset = 0;

/* 本次 play_audio() 是否由倍速变更触发。可听位置为 0 时 g_initial_seek_position
 * 也是 0，不能再拿它当“是否重启”的判别；否则同曲目已在播放时会被直接跳过。 */
static int g_speed_change_restart_pending = 0;
/* 卡拉OK 切档换播放源也复用"按当前位置重建播放线程"的机制。 */
static int g_source_change_restart_pending = 0;

/* ── 播放中切档的"待切换"状态（t7）──────────────────────────────
 * 旧实现只在 karaoke_set_mode() 里试一次换源：Off→Soft/Full 的瞬间当前段
 * 往往还没分离出来，于是判定"目标源不可用"后就永远停在原声（worker 产出
 * 段之后没有任何重试点，用户报障"中途切档不消音"）。现在把目标登记为
 * 待切换，由 karaoke_tick()（core_tick 每轮调用、内部限频）重试：
 * 当前段就绪即按既有"重启播放线程并在当前位置恢复"机制换源。
 *
 * 成功换源后若 playback_thread 在后续段超时（既有 D3 8s 回退）退回原声，
 * 再在有限次数内重新排队，避免切档后消音永久消失。 */
#define KARAOKE_SWITCH_CHECK_MS            250
#define KARAOKE_SWITCH_TIMEOUT_MS          30000
#define KARAOKE_SWITCH_REACQUIRE_DELAY_MS  1500
#define KARAOKE_SWITCH_REACQUIRE_MAX       3

static int g_karaoke_switch_target = KARAOKE_MODE_OFF;
static char g_karaoke_switch_track[MAX_PATH_LEN];
static unsigned long long g_karaoke_switch_started_ms = 0;
static unsigned long long g_karaoke_switch_last_check_ms = 0;

static char g_karaoke_switched_track[MAX_PATH_LEN];
static int g_karaoke_switched_mode = KARAOKE_MODE_OFF;
static unsigned long long g_karaoke_switched_ms = 0;
static int g_karaoke_switch_reacquire = 0;

/* "已切换到 X"状态行的延后推送：换源重启后 playback_thread 会先推一条
 * "已跳转到 mm:ss"（初始 seek 状态），紧接着推会被它覆盖、用户看不到。
 * 这里记下待推送的提示，等播放源确实切过去且位置推进 ≥1s（seek 状态已出）
 * 或 5s 超时兜底后再推，保证"准备中 → 已切换"在状态行可见。 */
static int g_karaoke_switch_notice_mode = KARAOKE_MODE_OFF;
static unsigned long long g_karaoke_switch_notice_waited_ms = 0;
static int g_karaoke_switch_notice_pos = 0;
static unsigned long long g_karaoke_switch_notice_deadline_ms = 0;

/* R4（captain 裁决：接受有界降级，但必须诚实提示）：re-acquire 达到上限
 * 或等待超时后若实际仍为原声，给一次可读状态（复用
 * karaoke.status.switch_timeout："…继续原声（可再次切档重试）"），不得静默
 * 永久停在原声。按 曲目+档位 去重，用户再次切档/换曲时重置。 */
static char g_karaoke_switch_degraded_track[MAX_PATH_LEN];
static int g_karaoke_switch_degraded_mode = KARAOKE_MODE_OFF;
static int g_karaoke_switch_degraded_notified = 0;

/* ── Progress ── */
int g_current_position = 0;
int g_total_duration = 0;
pthread_mutex_t g_seek_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Default audio device ── */
char g_default_audio_device[128] = "default";

/* ============================================================
 * Helpers
 * ============================================================ */

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
    /* 幂等：daemon 与前端（local/remote）都会调到这里，静音只需生效一次；
     * 早于任何 avformat 使用才是关键（见 main.c 中的调用时机）。 */
    static int done;
    if (done) return;
    done = 1;
    log_info("audio", "Initializing FFmpeg");
    avformat_network_init();
    av_log_set_level(AV_LOG_QUIET);
    av_log_set_callback(ffmpeg_log_callback);
    log_debug("audio", "FFmpeg initialized, log suppressed");
}

void audio_backend_shutdown(void)
{
    karaoke_worker_shutdown();
    /* D6：worker 停止之后再删本进程 /tmp 会话分段目录（退出即删）。 */
    karaoke_cache_session_remove_self();
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
    /* 反映“探测通过”，而不是“库能否加载” */
    if (backend == AUDIO_BACKEND_PULSE)    return (pulse_loaded && pa_connected) ? 1 : 0;
    if (backend == AUDIO_BACKEND_ALSA)     return (alsa_loaded && alsa_ready) ? 1 : 0;
    if (backend == AUDIO_BACKEND_PIPEWIRE) return pipewire_loaded ? 1 : 0;
    return 0;
}

/* ============================================================
 * Audio device initialization
 * ============================================================ */

/* 启动阶段所有候选都失败时的有限重试：间隔约 500ms、总量约 2s，
 * 覆盖“音频服务晚启动”的场景。 */
#define BACKEND_SELECT_ATTEMPTS 5
#define BACKEND_SELECT_RETRY_MS 500

/* 探测一个候选后端，失败时把可读原因写入 *reason_out */
static int probe_backend_candidate(int candidate, const char **reason_out)
{
    if (audio_backend_probe(candidate, reason_out) == 0) return 0;

    log_info("audio", "%s backend unavailable: %s",
             audio_backend_display_name(candidate),
             (reason_out && *reason_out) ? *reason_out : "unknown");
    return -1;
}

/* ALSA 设备探测失败 = 该后端在本机不可用：把原因明确告知用户 */
static void report_alsa_probe_failure(const char *reason)
{
    if (!reason) return;
    char msg[256];
    audio_backend_format_message(msg, sizeof(msg),
                                 i18n_get("audio.backend.alsa_probe_failed"),
                                 NULL, NULL, reason);
    core_status_push(msg);
}

/* 统一选择器：
 *   显式选择 → 只探测该候选；
 *   AUTO      → 依次探测，选中首个可用者。
 * 返回选中的后端；AUDIO_BACKEND_AUTO 表示没有可用后端。 */
static int select_audio_backend(int requested)
{
    if (requested != AUDIO_BACKEND_AUTO) {
        log_info("audio", "Trying %s backend", audio_backend_display_name(requested));
        const char *reason = NULL;
        if (probe_backend_candidate(requested, &reason) == 0) {
            log_info("audio", "%s backend selected", audio_backend_display_name(requested));
            return requested;
        }
        if (requested == AUDIO_BACKEND_ALSA) report_alsa_probe_failure(reason);
        log_error("audio", "%s backend selected but unavailable",
                  audio_backend_display_name(requested));
        return AUDIO_BACKEND_AUTO;
    }

    int selected = AUDIO_BACKEND_AUTO;
    const char *alsa_reason = NULL;
    for (int i = 0; i < BACKEND_CANDIDATE_COUNT; i++) {
        int candidate = g_backend_candidates[i];
        log_info("audio", "Trying %s backend", audio_backend_display_name(candidate));
        const char *reason = NULL;
        if (probe_backend_candidate(candidate, &reason) == 0) {
            log_info("audio", "%s backend probe passed", audio_backend_display_name(candidate));
            if (selected == AUDIO_BACKEND_AUTO) selected = candidate;
            if (candidate == AUDIO_BACKEND_ALSA) alsa_reason = NULL;
        } else if (candidate == AUDIO_BACKEND_ALSA) {
            alsa_reason = reason;
        }
    }

    /* 一个后端都选不出来时，ALSA 的失败原因才有诊断价值 */
    if (selected == AUDIO_BACKEND_AUTO) report_alsa_probe_failure(alsa_reason);

    if (selected != AUDIO_BACKEND_AUTO)
        log_info("audio", "%s backend selected", audio_backend_display_name(selected));
    return selected;
}

int audio_engine_initialized(void)
{
    return g_audio_engine_initialized;
}

/* D5：预取队列下一曲（仅 seamless_preload=1；nice 10 的 worker 线程执行，
 * 不抢占当前曲目播放）。换曲/关闭/退出走 karaoke_worker_cancel 取消。 */
static int karaoke_request_job_for_track(const char *track_path);

static void karaoke_maybe_preload_next(const char *ready_track)
{
    if (!ready_track || !ready_track[0])
        return;
    if (!g_app_config.seamless_preload)
        return;
    if (karaoke_get_mode() == KARAOKE_MODE_OFF)
        return;
    /* 只对“当前正在播放曲目”的 READY 做预取，避免预取链无限延伸。 */
    char current[MAX_PATH_LEN];
    if (!audio_current_track_path(current, sizeof(current)) ||
        strcmp(current, ready_track) != 0)
        return;
    int next_pos = -1;
    BackendQueueEntry entry;
    int have = 0;
    pthread_mutex_lock(&g_play_mutex);
    next_pos = play_queue_peek_next(&g_play_queue, g_play_mode);
    if (next_pos >= 0)
        have = (bq_entry_at(next_pos, &entry) == 0);
    pthread_mutex_unlock(&g_play_mutex);
    if (!have || !entry.path[0] || entry.cue_offset > 0)
        return;
    /* R5：请求前二次比对当前曲目，避免“校验→请求”之间用户换曲时，
     * 预取 job 覆盖新曲刚入队的 pending（worker 单槽）。 */
    char current_now[MAX_PATH_LEN];
    if (!audio_current_track_path(current_now, sizeof(current_now)) ||
        strcmp(current_now, ready_track) != 0) {
        log_debug("audio",
                  "karaoke preload aborted: current track changed ('%s' -> '%s')",
                  ready_track, current_now);
        return;
    }
    log_info("audio", "karaoke preload next track (seamless_preload=1): '%s'",
             entry.path);
    karaoke_request_job_for_track(entry.path);
}

/* worker → 状态栏：全部走 data/lang 三语键（不再硬编码双语）。 */
static void karaoke_worker_status_cb(int state, const char *message)
{
    switch (state) {
    case KARAOKE_JOB_RUNNING:
        core_status_push(i18n_get("karaoke.status.running"));
        break;
    case KARAOKE_JOB_READY: {
        core_status_push(i18n_get("karaoke.status.ready"));
        KaraokeWorkerStatus st;
        memset(&st, 0, sizeof(st));
        if (karaoke_worker_status(&st) == 0)
            karaoke_maybe_preload_next(st.track_path);
        break;
    }
    case KARAOKE_JOB_FAILED: {
        char msg[KARAOKE_WORKER_ERR_CAP + 128];
        snprintf(msg, sizeof(msg), i18n_get("karaoke.status.failed_fmt"),
                 (message && message[0]) ? message : "?");
        core_status_push(msg);
        break;
    }
    case KARAOKE_JOB_CANCELLED:
        core_status_push(i18n_get("karaoke.status.cancelled"));
        break;
    default:
        break;
    }
}

void init_audio_device(void)
{
    log_info("audio", "Initializing audio device");
    /* 先置位：即使所有后端探测失败，本进程也已经是 core 宿主，运行时回退
     * 仍允许在播放时重新探测（见 audio_backend_prepare_stream）。 */
    g_audio_engine_initialized = 1;

    /* 调用方（main.c / daemon.c）已把配置里的后端写进 g_active_backend */
    int requested = g_active_backend;
    g_requested_backend = requested;

    if (pulse_load() == 0) { pulse_loaded = 1; log_info("audio", "PulseAudio library loaded"); }
    else { pulse_loaded = 0; log_info("audio", "PulseAudio library not available"); }
    if (alsa_load() == 0) { alsa_loaded = 1; log_info("audio", "ALSA library loaded"); }
    else { alsa_loaded = 0; log_info("audio", "ALSA library not available"); }
    if (pipewire_load() == 0) { pipewire_loaded = 1; log_info("audio", "PipeWire library loaded"); }
    else { pipewire_loaded = 0; log_info("audio", "PipeWire library not available"); }

    /* 统一选择：全部候选都失败时做有限次重试（覆盖“服务晚启动”） */
    int selected = AUDIO_BACKEND_AUTO;
    for (int attempt = 1; attempt <= BACKEND_SELECT_ATTEMPTS; attempt++) {
        selected = select_audio_backend(requested);
        if (selected != AUDIO_BACKEND_AUTO) break;
        if (attempt < BACKEND_SELECT_ATTEMPTS) {
            log_warn("audio", "No audio backend available, retry in %dms (attempt %d/%d)",
                     BACKEND_SELECT_RETRY_MS, attempt, BACKEND_SELECT_ATTEMPTS);
            usleep(BACKEND_SELECT_RETRY_MS * 1000);
        }
    }

    g_active_backend = selected;
    if (selected != AUDIO_BACKEND_AUTO) {
        log_info("audio", "Audio backend in use: %s", audio_backend_display_name(selected));
        /* 后端名提示不再 printf(stdout)：local 模式下它在 initscr()
         * 之后执行、会污染 ncurses 屏幕；由调用方定向展示
         * （daemon 写 daemon.log，TUI 进状态行）。 */
    } else {
        log_error("audio", "No audio backend available");
    }

    /* Initialise equaliser from loaded config */
    init_equalizer_from_config();

    /* 卡拉OK（消人声）同样从已加载的配置初始化 */
    init_karaoke_from_config();
    karaoke_worker_init();
    karaoke_worker_set_status_callback(karaoke_worker_status_cb);
    /* D6：清理同 uid 且 pid 已不存在的 /tmp 会话分段残留（崩溃残留）。 */
    karaoke_cache_session_cleanup_stale();
    {
        KaraokeCacheConfig cfg;
        char root[MAX_PATH_LEN];
        /* 不传 model_path：这里只需要缓存根做残留清理，不必在启动时就
         * 读 30MiB 内置模型做 SHA256（模型校验延迟到首次真正需要分离时）。 */
        if (audio_karaoke_cache_config(&cfg, root, sizeof(root), NULL, 0))
            karaoke_cache_cleanup_stale(&cfg);
    }
}

/* ============================================================
 * Equalizer init (called after config is loaded)
 * ============================================================ */

void init_equalizer_from_config(void)
{
    eq_init();
    eq_set_enabled(g_app_config.eq_enabled);
    eq_set_preamp(g_app_config.eq_preamp);
    eq_set_all_gains(g_app_config.eq_band_gains);
}

/* ============================================================
 * Karaoke (vocal removal) mode state
 *
 * 旧 DSP（逐频点 STFT 中置提取）已删除；现在只剩三档模式状态。
 * 真正的音频差异由播放源体现：off=原声、soft=soft.flac、full=inst.flac
 * （见 audio/karaoke_cache.h / karaoke_worker.h）。本模块的 karaoke_set_mode
 * 只发布档位；播放中的源切换由 apply_karaoke_mode_change() 按缓存就绪情况
 * 决定（见下方），不在这里做任何音频处理。
 * ============================================================ */

static int g_karaoke_mode = KARAOKE_MODE_OFF;
int g_karaoke_playback_source = KARAOKE_SOURCE_ORIGINAL;

/* 前置声明：实现在文件后部（依赖播放状态与缓存/worker 模块）。 */
static void apply_karaoke_mode_change(int can_prepare);
static int karaoke_prepare_current_track(const char *track_path, int cue_offset);
static int karaoke_desired_source(void);
static int karaoke_current_cue_offset(void);

void karaoke_set_mode(int mode)
{
    if (mode < KARAOKE_MODE_OFF || mode >= KARAOKE_MODE_COUNT)
        mode = KARAOKE_MODE_OFF;
    int old = g_karaoke_mode;
    g_karaoke_mode = mode;
    if (old != mode) {
        /* 用户显式切档：重置"掉回原声后恢复"计数（这是新的切档意图），
         * 并让上一档尚未推送的"已切换到 X"提示过期（避免快速互切时把
         * 旧档的提示补发出来）。 */
        g_karaoke_switch_reacquire = 0;
        g_karaoke_switch_notice_mode = KARAOKE_MODE_OFF;
        g_karaoke_switch_degraded_notified = 0;
        g_karaoke_switch_degraded_mode = KARAOKE_MODE_OFF;
        g_karaoke_switch_degraded_track[0] = '\0';
        if (mode == KARAOKE_MODE_OFF) {
            g_karaoke_switched_mode = KARAOKE_MODE_OFF;
            g_karaoke_switched_track[0] = '\0';
        }
        /* config v12 起没有"功能开关"：档位本身就是开关。Off→非 Off 时
         * 立刻为当前曲目请求后台分离（否则起播时 mode=off 没请求，
         * 用户中途开档就永远等不到缓存）；请求由 worker 去重，重复无害。
         * can_prepare 决定"目标段未就绪时是否登记待切换"：模型不可用/
         * CUE 子曲目等本次不可能换源的情况不登记，避免无意义等待。
         * CUE 子曲目必须带真实 cue_offset 判定（t10 R1：不能用固定 0），
         * 与播放线程"CUE 不用 v2 分段源"的语义保持一致。 */
        int can_prepare = 0;
        if (mode != KARAOKE_MODE_OFF && g_audio_engine_initialized) {
            char track[MAX_PATH_LEN];
            int cue_offset = karaoke_current_cue_offset();
            if (audio_current_track_path(track, sizeof(track)))
                can_prepare = karaoke_prepare_current_track(track, cue_offset);
        }
        apply_karaoke_mode_change(can_prepare);
    }
}

int karaoke_get_mode(void)
{
    return g_karaoke_mode;
}

int karaoke_is_active(void)
{
    return g_karaoke_mode != KARAOKE_MODE_OFF;
}

void init_karaoke_from_config(void)
{
    karaoke_set_mode(g_app_config.karaoke_mode);
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

void audio_set_initial_seek_position(int seconds)
{
    g_initial_seek_position = seconds > 0 ? seconds : 0;
}

int audio_get_position_seconds(void)
{
    return (int)(audio_get_position_ms() / 1000);
}

int64_t audio_get_position_ms(void)
{
    int64_t position = progress_tracker_get_position_ms();
    if (position <= 0 && g_current_position > 0) {
        /* 跟踪器尚未就绪（刚切曲/刚恢复）时沿用播放线程写下的整秒位置 */
        position = (int64_t)g_current_position * 1000;
    }

    /* progress_tracker 统计的是“已写入后端”的样本：PipeWire 的 1MB 环形
     * 缓冲会先装下约 3 秒音频再由 process 回调消费，直接返回会把进度、
     * 断点恢复位置都报得比实际听到的提前一个环（q 退出后再启动会像凭空
     * 多走了一段）。这里扣除后端尚未消费的帧，让对外位置等于可听位置。 */
    if (g_play_state != PLAY_STATE_STOPPED && g_output_sample_rate > 0) {
        int buffered_frames = audio_backend_buffered_frames();
        if (buffered_frames > 0) {
            position -= ((int64_t)buffered_frames * 1000) / g_output_sample_rate;
        }
    }
    return position > 0 ? position : 0;
}

int audio_get_duration_seconds(void)
{
    return g_total_duration > 0 ? g_total_duration : 0;
}

int audio_current_track_path(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    pthread_mutex_lock(&g_play_mutex);
    snprintf(out, out_size, "%s", g_current_play_path);
    pthread_mutex_unlock(&g_play_mutex);
    return out[0] ? 1 : 0;
}

void audio_sync_queue_position(void)
{
    pthread_mutex_lock(&g_play_mutex);
    if (g_current_play_path[0] != '\0') {
        int position = bq_position_of_path(g_current_play_path);
        if (position >= 0) {
            g_current_play_index = position;
            /* 非前缀 Queue.Set 可能把游标置 0，但正在出声的路径仍在新表中；
             * 让游标跟着当前曲目，避免 Queue.Get 与 Info/MPRIS 各说一套。 */
            if (bq_position() != position) {
                bq_set_position(position);
            }
        } else {
            g_current_play_index = -1;
        }
    }
    pthread_mutex_unlock(&g_play_mutex);
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
        /* 音量是连续小步变化的字段：只置脏，落盘由 core_tick() 去抖合并 */
        core_config_mark_dirty();
        char msg[64];
        snprintf(msg, sizeof(msg), i18n_get("status.volume_fmt"), clamped);
        core_status_push(msg);
        core_notify_state_changed();
        signal_playback_thread();
    }
}

void adjust_volume(int delta) {
    set_volume_percent(get_volume_percent() + delta);
}

/* ============================================================
 * Play mode / speed
 * ============================================================ */

const char *get_play_mode_str(void)
{
    return play_mode_display_name(g_play_mode, 0);
}

PlayMode get_play_mode(void)
{
    return g_play_mode;
}

void set_play_mode(PlayMode mode)
{
    if (mode < 0 || mode >= PLAY_MODE_COUNT) return;
    g_play_mode = mode;
    log_info("audio", "Play mode changed: %d (%s)", g_play_mode, get_play_mode_str());
    /* 对已下发的路径队列重建执行顺序；后端不认识内容列表，故锚点用
     * 当前条目的路径。 */
    if (bq_count() > 0) {
        BackendQueueEntry current;
        char anchor_path[MAX_PATH_LEN];
        const char *anchor = audio_current_track_path(anchor_path, sizeof(anchor_path))
                                 ? anchor_path
                                 : ((bq_current(&current) == 0) ? current.path : NULL);
        play_queue_rebuild(&g_play_queue, g_play_mode, anchor);
    }
    audio_sync_queue_position();
    g_app_config.default_play_mode = g_play_mode;
    save_config();
    core_notify_state_changed();
}

void cycle_play_mode(void)
{
    int next = (g_play_mode + 1) % 5;
    g_play_mode = (PlayMode)next;
    log_info("audio", "Play mode changed: %d (%s)", g_play_mode, get_play_mode_str());
    if (bq_count() > 0) {
        BackendQueueEntry current;
        char anchor_path[MAX_PATH_LEN];
        const char *anchor = audio_current_track_path(anchor_path, sizeof(anchor_path))
                                 ? anchor_path
                                 : ((bq_current(&current) == 0) ? current.path : NULL);
        play_queue_rebuild(&g_play_queue, g_play_mode, anchor);
    }
    audio_sync_queue_position();
    g_app_config.default_play_mode = g_play_mode;
    save_config();
    core_notify_state_changed();
}

void toggle_playback_speed(void)
{
    float old_speed = g_playback_speed;
    g_speed_index = (g_speed_index + 1) % g_speed_count;
    g_playback_speed = g_speed_ratios[g_speed_index];
    log_info("audio", "Playback speed changed: %.2fx -> %.2fx", (double)old_speed, (double)g_playback_speed);
    g_app_config.default_playback_speed = g_playback_speed;
    /* 与音量同路：配置镜像已更新，落盘交给 core_tick() 去抖 */
    core_config_mark_dirty();
    char msg[64];
    snprintf(msg, sizeof(msg), "%s: %.2fx", i18n_get("controls.label.speed"), (double)g_playback_speed);
    core_status_push(msg);
    core_notify_state_changed();
    apply_playback_speed_change();
}

/**
 * @brief 重启当前播放以使倍速变更生效 — 被 toggle_playback_speed()
 *        和控件窗格 POPUP_SPEED 共用
 */
void apply_playback_speed_change(void)
{
    if (g_play_state == PLAY_STATE_PLAYING || g_play_state == PLAY_STATE_PAUSED) {
        pthread_mutex_lock(&g_play_mutex);
        int was_paused = (g_play_state == PLAY_STATE_PAUSED);
        int current_idx = g_current_play_index;
        /* 与 persist_playback_session_state() 同一口径：保存/恢复的是用户
         * 实际听到的位置，而不是 PipeWire 环里提前写入的几秒。 */
        int current_pos = audio_get_position_seconds();
        char current_path[MAX_PATH_LEN];
        snprintf(current_path, sizeof(current_path), "%s", g_current_play_path);
        if (current_idx < 0 && current_path[0] != '\0') {
            current_idx = bq_position_of_path(current_path);
        }
        if (current_idx < 0) {
            pthread_mutex_unlock(&g_play_mutex);
            /* 当前条目已从队列移除：无法按队列位置重启，但倍速仍会应用到
             * 下一首。不要用 -1 调 play_audio() 把正在播放的声音打断。 */
            log_warn("audio", "Speed change: current track is no longer in the queue");
            return;
        }

        /* 可听位置可能为 0（刚起播、缓冲还没被消费），此时 g_initial_seek_position
         * 与“没有倍速重启”无法区分，因此额外记录本次重启意图。 */
        g_initial_seek_position = current_pos;
        g_speed_change_restart_pending = 1;
        g_play_thread_running = 0;
        pthread_mutex_unlock(&g_play_mutex);

        signal_playback_thread();
        /* 倍速重启必须先等旧线程完全退出（含后端流清理）再起新线程。
         * 只 reap 一次时旧线程可能仍 active，play_audio() 会走 pending 异步
         * 切换；新旧 PipeWire 流在 20ms 窗口内竞争，可导致新线程刚起就退出。 */
        wait_for_playback_thread_shutdown();
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
    char track_path[MAX_PATH_LEN] = "", track_folder_path[MAX_PATH_LEN];

    /* 退出路径：先去抖落盘里攒着的音量/倍速，再写本次会话的恢复字段 */
    core_config_flush_dirty();

    reap_finished_playback_thread();
    pthread_mutex_lock(&g_play_mutex);
    if (g_play_thread_running &&
        (g_play_state == PLAY_STATE_PLAYING || g_play_state == PLAY_STATE_PAUSED)) {
        /* 保存可听位置，而不是已写入环形缓冲的提前量（见 audio_get_position_seconds）。 */
        play_position = audio_get_position_seconds();
        play_index = g_current_play_index;
        snprintf(track_path, sizeof(track_path), "%s", g_current_play_path);
        should_resume = (track_path[0] != '\0' || play_index >= 0);
    }
    pthread_mutex_unlock(&g_play_mutex);

    /* 兼容极端时序：路径尚未写入但旧下标仍指向队列条目时，按旧下标补齐。 */
    if (should_resume && track_path[0] == '\0') {
        if (play_index >= 0 && bq_path_at(play_index, track_path, sizeof(track_path)) == 0) {
            /* track_path ready */
        } else {
            track_path[0] = '\0';
        }
    }

    if (!should_resume || track_path[0] == '\0') {
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

/* 核心换曲时的会话恢复字段记录（Task 4.2）。
 *
 * 这是"由核心持久化"的换曲钩子：play_audio() 只在核心宿主进程里运行
 * （本地模式 TUI 即核心；daemon 与远程前端都经 RPC/播放线程走到这里），
 * 远程 TUI 前端从不调用 play_audio，因此前端进程不会触达本函数、不会写 config。
 *
 * 记录"刚切到的当前曲目"路径与它的起始位置：
 *   - 普通换曲：起始位置为 0（g_initial_seek_position 已被上一曲线程消费清零）；
 *   - 断点恢复 / 倍速重启：起始位置为 g_initial_seek_position（本次要跳到的进度）。
 * 退出路径（persist_playback_session_state）仍会写入精确的实时位置，二者互补。
 *
 * 字段语义与 persist_playback_session_state 保持一致：resume_last_playback /
 * last_played_position / last_played_track_path / last_played_folder_path。 */
static void persist_playback_session_on_switch(int index)
{
    char track_path[MAX_PATH_LEN], track_folder_path[MAX_PATH_LEN];

    if (index < 0 ||
        bq_path_at(index, track_path, sizeof(track_path)) != 0 ||
        track_path[0] == '\0') {
        return;
    }

    int start_position = g_initial_seek_position > 0 ? g_initial_seek_position : 0;

    g_app_config.resume_last_playback = 1;
    g_app_config.last_played_position = start_position;
    snprintf(g_app_config.last_played_track_path,
             sizeof(g_app_config.last_played_track_path), "%s", track_path);
    extract_parent_directory(track_path, track_folder_path, sizeof(track_folder_path));
    snprintf(g_app_config.last_played_folder_path,
             sizeof(g_app_config.last_played_folder_path), "%s", track_folder_path);
    save_config();
}

/* ============================================================
 * Play audio (public API)
 * ============================================================ */

/* 取走“本次起播来自倍速变更”的意图。只在队列条目有效、确实要进入起播流程时
 * 调用；返回 1 时 false 分支也不能再因“同曲目已在播放”而跳过线程重建。 */
static int take_speed_change_restart(void)
{
    int pending;
    pthread_mutex_lock(&g_play_mutex);
    pending = g_speed_change_restart_pending;
    g_speed_change_restart_pending = 0;
    pthread_mutex_unlock(&g_play_mutex);
    return pending;
}

static int take_source_change_restart(void)
{
    int pending;
    pthread_mutex_lock(&g_play_mutex);
    pending = g_source_change_restart_pending;
    g_source_change_restart_pending = 0;
    pthread_mutex_unlock(&g_play_mutex);
    return pending;
}

void play_audio(int index)
{
    /* 退出闸门：一旦进程被要求退出（Control.Quit / SIGTERM / 总线丢失），
     * 主循环可能在退出前继续派发同一批 D-Bus 消息。此时若还允许起播，
     * 就会在“正在收尾的旧核心”里再拉起一个播放线程，与新前端随后接管的
     * 核心叠在一起。退出一旦开始，播放线程只减不增。 */
    if (g_should_exit) {
        log_warn("audio", "play_audio(%d) ignored: process is shutting down", index);
        return;
    }

    /* 进程边界：只有 core 宿主（local 模式的 TUI，或 daemon）初始化过音频
     * 引擎，才允许创建播放线程。远端前端只经 D-Bus 下发命令；如果它在这里
     * 意外走到本地起播，就会与核心各播一路，形成叠播（重音）。
     * 注意：此闸门必须在 bq_play_at()/持久化之前，拒绝时不留任何副作用。 */
    if (!g_audio_engine_initialized) {
        log_error("audio",
                  "play_audio(%d) refused: this process is a remote front end "
                  "without an audio engine", index);
        return;
    }

    BackendQueueEntry entry;
    if (bq_entry_at(index, &entry) != 0) {
        log_warn("audio", "play_audio(%d): queue position out of range", index);
        return;
    }
    char track_path[MAX_PATH_LEN];
    snprintf(track_path, sizeof(track_path), "%s", entry.path);
    log_info("audio", "play_audio(index=%d) track='%s'", index, track_path);

    /* 换到另一首曲目时取消旧的分离 job（同曲目重启如倍速/切档不取消）。 */
    {
        char worker_track[MAX_PATH_LEN];
        if (karaoke_worker_current_track(worker_track, sizeof(worker_track)) &&
            strcmp(worker_track, track_path) != 0)
            karaoke_worker_cancel();
    }

    /* 倍速重启可能在可听位置 0 触发，g_initial_seek_position 此时也是 0；
     * 必须靠显式意图区分“普通同曲目起播（可跳过）”与“倍速重建线程”。 */
    int force_speed_restart = take_speed_change_restart();
    int force_source_restart = take_source_change_restart();
    int force_restart = force_speed_restart || force_source_restart;

    /* 队列由前端下发：位置 index 必须与后端游标一致，但前端也可能直接
     * 指定某个位置（Queue.PlayAt），因此这里以调用方给的位置为准。 */
    if (bq_position() != index) {
        bq_play_at(index);
    }

    reap_finished_playback_thread();
    pthread_mutex_lock(&g_play_mutex);

    int is_speed_change_restart = force_restart || (g_initial_seek_position > 0);
    if (!is_speed_change_restart && g_play_state == PLAY_STATE_PLAYING && g_current_play_index == index) {
        log_debug("audio", "Skipping play: same index=%d already playing", index);
        snprintf(g_current_play_path, sizeof(g_current_play_path), "%s", track_path);
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

        /* 暂停中切歌：所有后端都要恢复流，否则写线程会一直停在“写不进”上，
         * 切歌无法完成（ALSA/PipeWire 的写线程不会被 Pulse 的 corked 状态唤醒）。 */
        audio_backend_resume_stream();
        signal_playback_thread();

        if (was_paused) {
            for (int i = 0; i < 50; i++) {
                reap_finished_playback_thread();
                pthread_mutex_lock(&g_play_mutex);
                if (!g_play_thread_active) {
                    g_pending_playback_index = -1;
                    g_current_play_index = index;
                    snprintf(g_current_play_path, sizeof(g_current_play_path), "%s", track_path);
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
        core_notify_state_changed();
        return;
    }

    g_pending_playback_index = -1;
    g_current_play_index = index;
    snprintf(g_current_play_path, sizeof(g_current_play_path), "%s", track_path);
    g_play_thread_active = 1;
    g_play_thread_running = 1;
    g_play_thread_finished = 0;
    g_play_state = PLAY_STATE_STOPPED;
    pthread_mutex_unlock(&g_play_mutex);

start_playback:
    {
    /* Set CUE offset for the playback thread */
    g_cue_offset = entry.cue_offset;

    /* R1：先把“正在播放”推到状态行，再请求 worker 分离（请求仍早于
     * pthread_create，保留 D1 的“线程能看到 job”语义）。这样 worker 的
     * RUNNING/FAILED（带具体原因）才是状态行的最后一条，不被“正在播放”覆盖。 */
    {
        char msg[MAX_PATH_LEN + MAX_META_LEN * 2 + 64];
        snprintf(msg, sizeof(msg), "%s%s - %s",
                 i18n_get("audio.status.playing"),
                 entry.title[0] ? entry.title : entry.path,
                 entry.artist);
        core_status_push(msg);
        log_info("audio", "Now playing: '%s' - '%s' (idx=%d)",
                 entry.title, entry.artist, index);
    }

    /* 模型消人声：起播前请求后台准备（v2 分段）。线程会等首段就绪再起播
     * （D1），因此请求必须早于 pthread_create，避免线程先看到"无 job"而回退。 */
    (void)karaoke_prepare_current_track(track_path, entry.cue_offset);

    int *index_ptr = malloc(sizeof(int));
    if (!index_ptr) {
        pthread_mutex_lock(&g_play_mutex);
        g_play_thread_active = 0;
        g_current_play_index = -1;
        g_current_play_path[0] = '\0';
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
        g_current_play_path[0] = '\0';
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    pthread_mutex_lock(&g_play_mutex);
    g_play_state = PLAY_STATE_PLAYING;
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();

    /* 歌词来源随队列条目下发（前端扫描时已知）；条目未带时才回落自动。
     * 歌词与字符封面都是“当前曲目信息”，归后端。 */
    int lyrics_source = (entry.lyrics_source >= LYRICS_SOURCE_AUTO &&
                         entry.lyrics_source <= LYRICS_SOURCE_EXTERNAL)
        ? entry.lyrics_source : LYRICS_SOURCE_AUTO;
    load_lyrics(track_path, lyrics_source);
    update_album_cover_for_track(track_path);

    /* 换曲持久化改由核心负责（Task 4.2）：记录当前曲目路径与起始位置。
     * 远程 TUI 前端不会走到这里（其播放经 D-Bus 交给核心），故前端不写 config。 */
    persist_playback_session_on_switch(index);
    core_notify_state_changed();

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
    /* 不检查队列下标：当前条目可能已被 Queue.RemoveAt/Clear 从队列中移除，
     * 但播放线程仍在出声；此时仍应允许暂停。当前曲目的身份是
     * g_current_play_path，而不是队列位置。 */
    g_play_state = PLAY_STATE_PAUSED;
    audio_backend_pause_stream();
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();
    progress_tracker_on_pause();
    core_notify_state_changed();
}

void resume_audio(void)
{
    log_debug("audio", "resume_audio() called");
    pthread_mutex_lock(&g_play_mutex);
    if (g_play_state != PLAY_STATE_PAUSED || !g_play_thread_running) {
        pthread_mutex_unlock(&g_play_mutex);
        return;
    }
    g_play_state = PLAY_STATE_PLAYING;
    audio_backend_resume_stream();
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();
    progress_tracker_on_resume();
    core_notify_state_changed();
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
    g_current_play_path[0] = '\0';
    g_audio_sample_rate = 0;
    g_audio_bit_rate = 0;
    g_audio_bit_depth = 0;
    g_audio_codec_name[0] = '\0';
    pthread_mutex_unlock(&g_play_mutex);
    signal_playback_thread();

    g_current_position = 0;
    progress_tracker_on_stop();
    clear_lyrics();
    reset_visualizer_state();
    core_notify_state_changed();
}

/* ============================================================
 * Next / Prev
 * ============================================================ */

void next_track(void)
{
    log_debug("audio", "next_track() called, mode=%d, current_idx=%d", g_play_mode, g_current_play_index);
    if (bq_count() == 0) return;

    int next_position = play_queue_peek_next(&g_play_queue, g_play_mode);
    if (next_position < 0) {
        stop_audio();
        return;
    }
    bq_play_at(next_position);
    play_audio(next_position);
}

void prev_track(void)
{
    log_debug("audio", "prev_track() called, mode=%d, current_idx=%d", g_play_mode, g_current_play_index);
    if (bq_count() == 0) return;

    int prev_position = play_queue_peek_prev(&g_play_queue, g_play_mode);
    if (prev_position < 0) return;
    bq_play_at(prev_position);
    play_audio(prev_position);
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
        core_notify_state_changed();
        return;
    }

    pthread_mutex_lock(&g_seek_mutex);
    g_seek_position = int_position;
    g_current_position = int_position;
    g_seek_request = 1;
    pthread_mutex_unlock(&g_seek_mutex);
    signal_playback_thread();

    core_notify_state_changed();
    media_session_notify_seek((uint64_t)g_current_position * 1000ULL);
}

int get_and_clear_initial_seek_position(void)
{
    int pos = g_initial_seek_position;
    g_initial_seek_position = 0;
    return pos;
}

/* ============================================================
 * Karaoke mode change → playback source switch
 *
 * 播放源由 playback_thread 在起播时按当前档位与缓存命中情况决定
 * （off=原声，soft/full=对应缓存）。运行中切档时，如果目标源已经就绪
 * 且与当前源不同，就在当前位置重启播放线程（与倍速变更同一机制，
 * 不重跑模型、保持进度/歌词/播放状态）；缓存未就绪则维持原声继续。
 * ============================================================ */

/* 内置模型不可用的用户可见提示：每进程只推一次（模型随包分发，缺失/损坏
 * 都是打包问题；重复起播不应反复刷状态行）。错误文本由
 * karaoke_model_bundled_path() 给出，明确区分"缺失"与"校验失败"。 */
static void karaoke_report_bundled_model_error(const char *err)
{
    static int reported = 0;
    static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
    int first = 0;
    pthread_mutex_lock(&report_lock);
    if (!reported) {
        reported = 1;
        first = 1;
    }
    pthread_mutex_unlock(&report_lock);
    if (!first)
        return;
    char msg[KARAOKE_MODEL_ERR_CAP + 160];
    snprintf(msg, sizeof(msg), i18n_get("karaoke.status.model_unavailable_fmt"),
             (err && err[0]) ? err : i18n_get("general.unknown"));
    log_error("audio", "karaoke bundled model unavailable: %s",
              (err && err[0]) ? err : "?");
    core_status_push(msg);
}

/* 卡拉OK缓存配置。config v12 起模型固定为内置的 uvr-mdxnet-3-9662：
 * 运行时定位 `<TER_MUSIC_DATA_DIR>/models/…`（其次旧用户 XDG 目录）并校验
 * 大小 + SHA256，**不复制到 XDG**。model_path 非空时才做模型定位/校验
 * （启动清理缓存根只需要 root，不必为此读 30MiB 文件）。
 * 返回 0 表示"没有可用的模型/缓存根"，调用方回退原声（不是错误）。 */
int audio_karaoke_cache_config(KaraokeCacheConfig *cfg, char *root_buf,
                               size_t root_cap, char *model_path, size_t model_cap)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!cfg || !root_buf || root_cap == 0)
        return 0;
    const KaraokeModelSpec *spec = karaoke_model_by_id(KARAOKE_MODEL_BUNDLED_ID);
    if (!spec)
        return 0;

    pthread_mutex_lock(&lock);
    const char *cache_dir = app_cache_dir(1);
    int ok = 0;
    if (cache_dir && cache_dir[0]) {
        int n = snprintf(root_buf, root_cap, "%s/karaoke", cache_dir);
        if (n > 0 && (size_t)n < root_cap) {
            if (model_path && model_cap) {
                char model_err[KARAOKE_MODEL_ERR_CAP];
                model_err[0] = '\0';
                /* 旧用户兼容路径 = $XDG_DATA_HOME/ter-music（只读，不创建）。 */
                ok = karaoke_model_bundled_path(app_data_dir(0), model_path,
                                                model_cap, model_err,
                                                sizeof(model_err)) == 0;
                if (!ok) {
                    pthread_mutex_unlock(&lock);
                    karaoke_report_bundled_model_error(model_err);
                    return 0;
                }
            } else {
                ok = 1;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    if (!ok)
        return 0;
    memset(cfg, 0, sizeof(*cfg));
    cfg->root = root_buf;
    cfg->model_id = spec->id;
    cfg->model_sha256 = spec->sha256;
    cfg->overlap = KARAOKE_CACHE_OVERLAP;
    cfg->soft_inst_mix = KARAOKE_CACHE_SOFT_INST_MIX;
    cfg->limit_mb = KARAOKE_CACHE_LIMIT_MB;
    return 1;
}

/* 请求 worker 为某曲目准备伴奏（v1 成对缓存只读优先；否则产出 v2 分段）。
 * 不阻塞调用线程；重复请求由 worker 去重。
 * 返回 1 = 已有可用缓存或 job 已提交（后续只需等段就绪）；0 = 无法准备
 * （模型/缓存不可用、参数非法）——调用方据此决定是否登记"待切换"。 */
static int karaoke_request_job_for_track(const char *track_path)
{
    if (!track_path || !track_path[0])
        return 0;
    KaraokeCacheConfig cfg;
    char root[MAX_PATH_LEN];
    char model_path[MAX_PATH_LEN];
    if (!audio_karaoke_cache_config(&cfg, root, sizeof(root), model_path,
                                    sizeof(model_path)))
        return 0;
    /* v1 整曲缓存优先（只读兼容）：命中则不产出 /tmp 分段。 */
    if (karaoke_cache_complete(&cfg, track_path))
        return 1;
    /* v2 会话分段已完整（同一次运行内重播）：无需再跑模型。 */
    KaraokeCacheConfig seg_cfg = cfg;
    char session_root[KARAOKE_CACHE_PATH_CAP];
    if (karaoke_cache_session_root(session_root, sizeof(session_root)) == 0) {
        seg_cfg.root = session_root;
        seg_cfg.limit_mb = 0;
        if (karaoke_cache_segment_complete(&seg_cfg, track_path))
            return 1;
    }
    KaraokeWorkerJob job;
    memset(&job, 0, sizeof(job));
    snprintf(job.track_path, sizeof(job.track_path), "%s", track_path);
    snprintf(job.model_id, sizeof(job.model_id), "%s", cfg.model_id);
    snprintf(job.model_path, sizeof(job.model_path), "%s", model_path);
    snprintf(job.model_sha256, sizeof(job.model_sha256), "%s", cfg.model_sha256);
    snprintf(job.cache_root, sizeof(job.cache_root), "%s", cfg.root);
    job.format = KARAOKE_JOB_FORMAT_SEGMENTS_V2;   /* 生产只产出 v2 分段 */
    job.overlap = cfg.overlap;
    job.soft_inst_mix = cfg.soft_inst_mix;
    if (karaoke_worker_request(&job) != 0) {
        log_debug("audio", "karaoke worker request failed for '%s'", track_path);
        return 0;
    }
    return 1;
}

/* 当前曲目起播前准备（CUE 子曲目不用 v2 分段源）。
 *
 * config v12 起没有 karaoke_feature 开关：档位即开关。Off 档不请求分离
 * （不为从不使用消音的用户跑模型/占 CPU）；用户随时可从控制栏或设置页
 * 切成 soft/full，karaoke_set_mode() 会为当前曲目补发一次请求。
 * 返回 1 = 已有可用缓存或 job 已提交；0 = 本次不能准备（Off/CUE/模型不可用）。 */
static int karaoke_prepare_current_track(const char *track_path, int cue_offset)
{
    if (!track_path || !track_path[0])
        return 0;
    if (karaoke_get_mode() == KARAOKE_MODE_OFF) {
        log_debug("audio", "karaoke separation skipped: mode is off ('%s')",
                  track_path);
        return 0;
    }
    if (cue_offset > 0) {
        log_debug("audio", "karaoke segments skipped for CUE sub-track: '%s'",
                  track_path);
        return 0;
    }
    return karaoke_request_job_for_track(track_path);
}

/* 当前播放条目的 CUE 子曲目偏移（秒）；0 = 普通曲目 / 无法解析。
 *
 * 用队列条目（g_current_play_index + bq_entry_at）判定，而不是"记住"某个
 * 全局：换曲/切子曲目后队列项才是唯一事实来源。与 playback_thread 对
 * "CUE 子曲目不用 v2 分段源"的语义保持一致（t10 R1）。 */
static int karaoke_current_cue_offset(void)
{
    int cue = 0;
    BackendQueueEntry entry;
    pthread_mutex_lock(&g_play_mutex);
    int index = g_current_play_index;
    if (index >= 0 && bq_entry_at(index, &entry) == 0)
        cue = entry.cue_offset;
    pthread_mutex_unlock(&g_play_mutex);
    return cue > 0 ? cue : 0;
}

/* 目标源此刻是否真的可用：v1 整曲缓存命中，或当前所在段的 v2 分段已就绪。
 * 不可用时由 karaoke_tick() 在待切换状态里重试（而不是永远停在原声）。
 * CUE 子曲目只认 v1 整曲缓存（不产 v2 分段），因此不做段判定。 */
static int karaoke_desired_source(void)
{
    int mode = karaoke_get_mode();
    if (mode == KARAOKE_MODE_OFF)
        return KARAOKE_SOURCE_ORIGINAL;
    KaraokeCacheConfig cfg;
    char root[MAX_PATH_LEN];
    char model_path[MAX_PATH_LEN];
    if (!audio_karaoke_cache_config(&cfg, root, sizeof(root), model_path,
                                    sizeof(model_path)))
        return KARAOKE_SOURCE_ORIGINAL;
    char track[MAX_PATH_LEN];
    if (!audio_current_track_path(track, sizeof(track)))
        return KARAOKE_SOURCE_ORIGINAL;
    char resolved[MAX_PATH_LEN];
    if (karaoke_cache_resolve(&cfg, track, mode, resolved, sizeof(resolved),
                              NULL, 0) == 1)
        return mode;
    /* CUE 子曲目不用 v2 分段源（见 playback_thread 的 CUE 跳过语义）：
     * 没有 v1 整曲缓存时目标源不可能就绪，不做"当前 15s 段就绪"判定。 */
    if (karaoke_current_cue_offset() > 0)
        return KARAOKE_SOURCE_ORIGINAL;
    char session_root[KARAOKE_CACHE_PATH_CAP];
    if (karaoke_cache_session_root(session_root, sizeof(session_root)) != 0)
        return KARAOKE_SOURCE_ORIGINAL;
    KaraokeCacheConfig seg_cfg = cfg;
    seg_cfg.root = session_root;
    seg_cfg.limit_mb = 0;
    int seg = audio_get_position_seconds() / KARAOKE_SEGMENT_SECONDS;
    if (karaoke_cache_segment_ready_for_mode(&seg_cfg, track, seg, mode))
        return mode;
    return KARAOKE_SOURCE_ORIGINAL;
}

/* ── 播放中切档：待切换状态机（t7）──────────────────────────────── */

static unsigned long long karaoke_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)(ts.tv_nsec / 1000000ULL);
}

static int karaoke_playback_active(void)   /* 含暂停：暂停中切档同样要换源 */
{
    pthread_mutex_lock(&g_play_mutex);
    int active = (g_play_state == PLAY_STATE_PLAYING ||
                  g_play_state == PLAY_STATE_PAUSED);
    pthread_mutex_unlock(&g_play_mutex);
    return active;
}

static int karaoke_playback_thread_alive(void)
{
    pthread_mutex_lock(&g_play_mutex);
    int alive = g_play_thread_running;
    pthread_mutex_unlock(&g_play_mutex);
    return alive;
}

static int karaoke_playback_source_now(void)
{
    pthread_mutex_lock(&g_play_mutex);
    int source = g_karaoke_playback_source;
    pthread_mutex_unlock(&g_play_mutex);
    return source;
}

static void karaoke_clear_switch_pending(const char *why)
{
    if (g_karaoke_switch_target == KARAOKE_MODE_OFF) {
        g_karaoke_switch_track[0] = '\0';
        return;
    }
    log_debug("audio", "karaoke switch pending cleared (%s): target=%d track='%s'",
              why ? why : "?", g_karaoke_switch_target, g_karaoke_switch_track);
    g_karaoke_switch_target = KARAOKE_MODE_OFF;
    g_karaoke_switch_track[0] = '\0';
    g_karaoke_switch_started_ms = 0;
    g_karaoke_switch_last_check_ms = 0;
}

/* 登记"目标档位待换源"。当前段还没分离出来时不是失败——worker 正在跑，
 * 由 karaoke_tick() 就绪后完成切换；先给一条"准备中"状态。 */
static void karaoke_schedule_source_switch(int mode, const char *track,
                                           const char *why)
{
    if (mode == KARAOKE_MODE_OFF || !track || !track[0])
        return;
    if (g_karaoke_switch_target == mode &&
        strcmp(g_karaoke_switch_track, track) == 0)
        return;   /* 已在等同一目标：不重复刷状态 */
    g_karaoke_switch_target = mode;
    snprintf(g_karaoke_switch_track, sizeof(g_karaoke_switch_track), "%s", track);
    g_karaoke_switch_started_ms = karaoke_mono_ms();
    g_karaoke_switch_last_check_ms = 0;
    int seg = audio_get_position_seconds() / KARAOKE_SEGMENT_SECONDS;
    /* 晚位置启用 / 等待期间 seek：让 worker 优先产出当前段（D4 hint 语义；
     * job 尚未跑到时该调用无副作用，karaoke_tick() 每 250ms 会再提示）。 */
    (void)karaoke_worker_hint_segment(track, seg);
    log_info("audio",
             "karaoke source switch pending (%s): mode=%d seg=%d track='%s'",
             why ? why : "?", mode, seg, track);
    core_status_push(i18n_get("karaoke.status.switch_pending"));
}

/* 目标源已确定：重启播放线程并在当前位置恢复（既有换源机制，不从 0 重播）。
 * 返回 1 = 已发起重启；0 = 当前不可（未播放/无当前条目/已是目标源）。 */
static int karaoke_restart_for_source(int desired)
{
    pthread_mutex_lock(&g_play_mutex);
    int playing = (g_play_state == PLAY_STATE_PLAYING ||
                   g_play_state == PLAY_STATE_PAUSED);
    int current = g_karaoke_playback_source;
    if (!playing || desired == current) {
        pthread_mutex_unlock(&g_play_mutex);
        return 0;
    }
    int was_paused = (g_play_state == PLAY_STATE_PAUSED);
    int current_idx = g_current_play_index;
    int current_pos = audio_get_position_seconds();
    if (current_idx < 0 && g_current_play_path[0] != '\0')
        current_idx = bq_position_of_path(g_current_play_path);
    if (current_idx < 0) {
        pthread_mutex_unlock(&g_play_mutex);
        return 0;
    }
    g_initial_seek_position = current_pos;
    g_source_change_restart_pending = 1;
    g_play_thread_running = 0;
    pthread_mutex_unlock(&g_play_mutex);

    signal_playback_thread();
    wait_for_playback_thread_shutdown();
    log_info("audio", "Karaoke source switch: %d -> %d at %ds (idx=%d)",
             current, desired, current_pos, current_idx);
    play_audio(current_idx);
    if (was_paused) {
        usleep(50000);
        pause_audio();
    }
    return 1;
}

/* can_prepare：本次切档是否已成功把分离请求交给 worker（或缓存已完整）。
 * 0 = 模型不可用 / CUE 子曲目等"本次不可能换源"，不登记待切换。 */
static void apply_karaoke_mode_change(int can_prepare)
{
    if (!g_audio_engine_initialized)
        return;

    int mode = karaoke_get_mode();
    pthread_mutex_lock(&g_play_mutex);
    int playing = (g_play_state == PLAY_STATE_PLAYING ||
                   g_play_state == PLAY_STATE_PAUSED);
    int thread_running = g_play_thread_running;
    pthread_mutex_unlock(&g_play_mutex);

    if (mode == KARAOKE_MODE_OFF) {
        karaoke_clear_switch_pending("mode off");
        g_karaoke_switch_notice_mode = KARAOKE_MODE_OFF;
        if (playing)
            (void)karaoke_restart_for_source(KARAOKE_SOURCE_ORIGINAL);
        return;
    }

    char track[MAX_PATH_LEN];
    int have_track = audio_current_track_path(track, sizeof(track)) ? 1 : 0;
    if (karaoke_desired_source() == mode) {
        karaoke_clear_switch_pending("target ready");
        if (playing) {
            (void)karaoke_restart_for_source(mode);
        } else if (thread_running) {
            /* 播放线程正在起播（还没进入 PLAYING）：它可能已经按旧档位
             * 选了源，不能把这次切档丢掉；登记待切换，进 PLAYING 后由
             * karaoke_tick() 校正（已是目标源时 restart 为 no-op）。 */
            karaoke_schedule_source_switch(mode, track,
                                           "mode change during playback start");
        }
        return;
    }
    if (!can_prepare || !have_track) {
        karaoke_clear_switch_pending("cannot schedule");
        log_debug("audio",
                  "karaoke switch not scheduled: mode=%d prepare=%d track=%d playing=%d",
                  mode, can_prepare, have_track, playing);
        return;
    }
    if (!playing && !thread_running) {
        /* 完全没在播放：下次起播会按当前档位选源，无需登记。 */
        karaoke_clear_switch_pending("no playback");
        return;
    }
    karaoke_schedule_source_switch(mode, track, "mode change");
}

/* R4：有界降级时的诚实提示。只在"实际仍为原声"时推一次可读状态
 * （复用 karaoke.status.switch_timeout：继续原声、可再次切档重试），
 * 按 曲目+档位 去重；用户再次切档/换曲时由调用方重置。
 * 绝不在这里推"已切换"，避免实际未切换却谎报。 */
static void karaoke_notify_switch_degraded(const char *track, int mode,
                                           const char *why)
{
    char current[MAX_PATH_LEN];
    const char *key_track = (track && track[0]) ? track : NULL;
    if (!key_track) {
        if (!audio_current_track_path(current, sizeof(current)))
            return;
        key_track = current;
    }
    if (mode == KARAOKE_MODE_OFF)
        return;
    if (g_karaoke_switch_degraded_notified &&
        g_karaoke_switch_degraded_mode == mode &&
        strcmp(g_karaoke_switch_degraded_track, key_track) == 0)
        return;
    int src = karaoke_playback_source_now();
    if (src != KARAOKE_SOURCE_ORIGINAL)
        return;   /* 已经不在原声：无需降级提示 */
    g_karaoke_switch_degraded_notified = 1;
    g_karaoke_switch_degraded_mode = mode;
    snprintf(g_karaoke_switch_degraded_track,
             sizeof(g_karaoke_switch_degraded_track), "%s", key_track);
    core_status_push(i18n_get("karaoke.status.switch_timeout"));
    log_warn("audio",
             "karaoke switch degraded (%s): mode=%d track='%s'; staying original, "
             "switch level again to retry", why ? why : "?", mode, key_track);
}

/* 播放中切档的待切换检查：由 core_tick() 每轮调用，内部限频 250ms
 * （不做每轮 manifest 读取）。目标段就绪 → 换源并提示"已切换"；
 * worker FAILED/CANCELLED → 停止等待（失败原因已由状态回调给出）；
 * 超时 → 保持原声并给出可读提示（可再次切档重试）。 */
void karaoke_tick(void)
{
    if (!g_audio_engine_initialized)
        return;
    unsigned long long now = karaoke_mono_ms();

    /* 延后的"已切换到 X"提示（见 globals 注释）：等 playback_thread 的初始
     * seek 状态先出，再推，避免被"已跳转到 mm:ss"覆盖。 */
    if (g_karaoke_switch_notice_mode != KARAOKE_MODE_OFF) {
        int notice_mode = g_karaoke_switch_notice_mode;
        int src = karaoke_playback_source_now();
        int pos = audio_get_position_seconds();
        if (src == notice_mode &&
            (pos >= g_karaoke_switch_notice_pos + 1 ||
             now >= g_karaoke_switch_notice_deadline_ms)) {
            char msg[256];
            snprintf(msg, sizeof(msg), i18n_get("karaoke.status.switch_done_fmt"),
                     i18n_get(notice_mode == KARAOKE_MODE_SOFT
                                  ? "karaoke.level.soft"
                                  : "karaoke.level.full"),
                     (int)(g_karaoke_switch_notice_waited_ms / 1000));
            core_status_push(msg);
            log_info("audio",
                     "karaoke source switch status pushed: mode=%d src=%d pos=%ds",
                     notice_mode, src, pos);
            g_karaoke_switch_notice_mode = KARAOKE_MODE_OFF;
        } else if (now >= g_karaoke_switch_notice_deadline_ms) {
            /* 5s 内实际播放源仍不是目标源：**绝不谎报"已切换到"**。
             * 若已退回原声，给可读降级提示（可再次切档重试）。 */
            g_karaoke_switch_notice_mode = KARAOKE_MODE_OFF;
            log_warn("audio",
                     "karaoke source switch notice dropped (not switched): "
                     "mode=%d src=%d pos=%ds", notice_mode, src, pos);
            karaoke_notify_switch_degraded(g_karaoke_switched_track, notice_mode,
                                           "switch notice deadline");
        }
    }

    if (g_karaoke_switch_target == KARAOKE_MODE_OFF) {
        /* 无待切换：若曾成功换源、又被 playback_thread 的 D3 边界回退
         * 打回原声，在有限次数内重新排队，避免切档后消音永久消失。 */
        if (g_karaoke_switched_mode == KARAOKE_MODE_OFF)
            return;
        if (g_karaoke_switch_reacquire >= KARAOKE_SWITCH_REACQUIRE_MAX) {
            /* R4：已到有界降级上限；仍为原声时给一次可读提示（不静默）。 */
            if (karaoke_playback_source_now() == KARAOKE_SOURCE_ORIGINAL)
                karaoke_notify_switch_degraded(g_karaoke_switched_track,
                                               g_karaoke_switched_mode,
                                               "re-acquire limit reached");
            return;
        }
        if (now - g_karaoke_switched_ms < KARAOKE_SWITCH_REACQUIRE_DELAY_MS)
            return;
        if (g_karaoke_switch_last_check_ms != 0 &&
            now - g_karaoke_switch_last_check_ms < KARAOKE_SWITCH_CHECK_MS)
            return;
        g_karaoke_switch_last_check_ms = now;

        int mode = karaoke_get_mode();
        char track[MAX_PATH_LEN];
        if (mode != g_karaoke_switched_mode ||
            !audio_current_track_path(track, sizeof(track)) ||
            strcmp(track, g_karaoke_switched_track) != 0) {
            g_karaoke_switched_mode = KARAOKE_MODE_OFF;
            g_karaoke_switched_track[0] = '\0';
            g_karaoke_switch_degraded_notified = 0;
            g_karaoke_switch_degraded_mode = KARAOKE_MODE_OFF;
            g_karaoke_switch_degraded_track[0] = '\0';
            return;
        }
        /* CUE 子曲目不用 v2 分段源：不可能通过 re-acquire 恢复，明确不排队
         * （t10 R1；v1 整曲缓存的立即换源在 apply_karaoke_mode_change 里）。 */
        if (karaoke_current_cue_offset() > 0) {
            g_karaoke_switched_mode = KARAOKE_MODE_OFF;
            g_karaoke_switched_track[0] = '\0';
            return;
        }
        if (!karaoke_playback_active() ||
            karaoke_playback_source_now() != KARAOKE_SOURCE_ORIGINAL)
            return;
        g_karaoke_switch_reacquire++;
        log_info("audio",
                 "karaoke source re-acquire %d/%d after fallback: mode=%d track='%s'",
                 g_karaoke_switch_reacquire, KARAOKE_SWITCH_REACQUIRE_MAX,
                 mode, track);
        karaoke_schedule_source_switch(mode, track, "re-acquire after fallback");
        return;
    }

    if (g_karaoke_switch_last_check_ms != 0 &&
        now - g_karaoke_switch_last_check_ms < KARAOKE_SWITCH_CHECK_MS)
        return;
    g_karaoke_switch_last_check_ms = now;

    int mode = karaoke_get_mode();
    char track[MAX_PATH_LEN];
    if (mode != g_karaoke_switch_target ||
        !audio_current_track_path(track, sizeof(track)) ||
        strcmp(track, g_karaoke_switch_track) != 0) {
        karaoke_clear_switch_pending("mode or track changed");
        return;
    }
    if (!karaoke_playback_thread_alive()) {
        karaoke_clear_switch_pending("no playback thread");
        return;
    }
    if (!karaoke_playback_active()) {
        /* 播放线程还在首播等待首段（D1）等过渡态：保持待切换，进入
         * PLAYING/PAUSED 后的下一轮再换源（不清 pending，避免漏切）。 */
        return;
    }
    /* 分离失败/取消：继续等下去没有意义（失败原因已由状态回调推送）。 */
    KaraokeWorkerStatus st;
    if (karaoke_worker_status(&st) == 0 &&
        strcmp(st.track_path, track) == 0 &&
        (st.state == KARAOKE_JOB_FAILED || st.state == KARAOKE_JOB_CANCELLED)) {
        karaoke_clear_switch_pending("worker failed/cancelled");
        return;
    }
    /* 跟随播放位置做 seek 优先（晚位置启用 / 等待期间 seek 都覆盖）。 */
    int seg = audio_get_position_seconds() / KARAOKE_SEGMENT_SECONDS;
    (void)karaoke_worker_hint_segment(track, seg);

    if (karaoke_desired_source() == mode) {
        unsigned long long waited = now - g_karaoke_switch_started_ms;
        karaoke_clear_switch_pending("target ready");
        if (!karaoke_restart_for_source(mode))
            return;   /* 播放线程恰好已退出/已是目标源：无副作用 */
        snprintf(g_karaoke_switched_track, sizeof(g_karaoke_switched_track),
                 "%s", track);
        g_karaoke_switched_mode = mode;
        g_karaoke_switched_ms = karaoke_mono_ms();
        /* "已切换到"提示延后到 seek 状态之后再推（见 globals 注释）。 */
        g_karaoke_switch_notice_mode = mode;
        g_karaoke_switch_notice_waited_ms = waited;
        g_karaoke_switch_notice_pos = audio_get_position_seconds();
        g_karaoke_switch_notice_deadline_ms = g_karaoke_switched_ms + 5000;
        log_info("audio",
                 "karaoke source switch done: mode=%d waited=%llums seg=%d track='%s'",
                 mode, waited, seg, track);
        return;
    }

    if (now - g_karaoke_switch_started_ms >= KARAOKE_SWITCH_TIMEOUT_MS) {
        unsigned long long waited = now - g_karaoke_switch_started_ms;
        karaoke_clear_switch_pending("timeout");
        core_status_push(i18n_get("karaoke.status.switch_timeout"));
        /* 已给过可读提示：记下（曲目+档位），避免后续 re-acquire 上限重复推。 */
        g_karaoke_switch_degraded_notified = 1;
        g_karaoke_switch_degraded_mode = mode;
        snprintf(g_karaoke_switch_degraded_track,
                 sizeof(g_karaoke_switch_degraded_track), "%s", track);
        log_warn("audio",
                 "karaoke source switch timeout after %llums: mode=%d seg=%d track='%s'",
                 waited, mode, seg, track);
    }
}
