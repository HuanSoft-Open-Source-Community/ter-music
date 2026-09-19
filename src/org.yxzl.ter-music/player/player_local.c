/**
 * @file player_local.c
 * @brief 门面的本地后端：直接调用进程内引擎（迁移期实现）
 *
 * 本文件就是“今天的直调实现”搬进一个文件：界面不再自己摸全局，而是调用
 * 这里的函数；语义与迁移前逐字相同（快照来自 info.c 的采集函数，命令直接
 * 落到 audio/play_queue/library 的既有接口）。
 *
 * M6 删除本文件与 PLAYER_BACKEND_LOCAL——那时前端只剩下 D-Bus 客户端一种
 * 形态。在那之前它是回归基线：`--frontend=local` 必须与迁移前行为一致。
 *
 * 修订号语义：player_local_pump() 重算快照并与上次比较，任何字段变化就递增对应
 * 修订号；界面据此重绘。命令后立即改写本地快照（乐观回显），满足
 * “按键到状态变化 < 50 ms”的验收要求。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "player/player.h"

#include "app/open.h"
#include "audio/audio.h"
#include "audio/audio_internal.h"
#include "audio/visualizer.h"
#include "audio/equalizer.h"
#include "audio/play_queue.h"
#include "audio/progress/progress.h"
#include "config/config.h"
#include "config/config_json.h"
#include "core/core.h"
#include "library/library.h"
#include "logger/logger.h"
#include "playlist/playlist.h"
#include "playlist/playlist_queue.h"
#include "queue/backend_queue.h"
#include "remote/remote.h"
#include "search/search.h"
#include "ui/braille/braille_art.h"
#include "lyrics/lyrics.h"
#include "ui/ui.h"
#include "util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 内部状态 ─────────────────────────────────────────────────── */

static struct {
    int initialized;
    int connected;

    InfoTrack track;
    InfoPlayback playback;
    InfoLyrics lyrics;
    char status_message[CORE_STATUS_MAX];

    uint64_t state_revision;
    uint64_t queue_revision;
    uint64_t lyrics_revision;
    uint64_t config_revision;
    uint64_t cover_revision;

    /* 进度外推锚点：核心位置 + 本地单调时钟 */
    uint64_t anchor_ms;
    int anchor_position;

    /* 封面缓存键 */
    int cover_valid;
    int cover_cols;
    int cover_rows;
    int cover_charset;
    char cover_track_id[96];
    char cover_text[INFO_COVER_TEXT_MAX];

    /* 队列/曲库的变更检测基线 */
    int queue_count;
    int queue_position;

    /* 本地过滤串（与 D-Bus Playlist.SetFilter 同一语义） */
    char filter[256];
} g_local = {0};

static unsigned long long player_local_now_ms(void)
{
    return get_ui_time_ms();
}

/* ── 快照刷新 ─────────────────────────────────────────────────── */

static int track_changed(const InfoTrack *lhs, const InfoTrack *rhs)
{
    return lhs->valid != rhs->valid ||
           lhs->index != rhs->index ||
           lhs->playlist_total != rhs->playlist_total ||
           lhs->queue_position != rhs->queue_position ||
           lhs->queue_count != rhs->queue_count ||
           strcmp(lhs->track_id, rhs->track_id) != 0 ||
           strcmp(lhs->path, rhs->path) != 0;
}

static int playback_changed(const InfoPlayback *lhs, const InfoPlayback *rhs)
{
    return lhs->state != rhs->state ||
           lhs->duration_seconds != rhs->duration_seconds ||
           lhs->volume_percent != rhs->volume_percent ||
           lhs->play_mode != rhs->play_mode ||
           lhs->can_seek != rhs->can_seek ||
           (lhs->speed - rhs->speed > 0.001f) || (rhs->speed - lhs->speed > 0.001f);
}

static int lyrics_changed(const InfoLyrics *lhs, const InfoLyrics *rhs)
{
    return lhs->has_lyrics != rhs->has_lyrics ||
           lhs->has_timestamps != rhs->has_timestamps ||
           lhs->source != rhs->source ||
           lhs->current_index != rhs->current_index ||
           lhs->next_index != rhs->next_index ||
           strcmp(lhs->current_text, rhs->current_text) != 0 ||
           strcmp(lhs->next_text, rhs->next_text) != 0;
}

static void refresh_snapshot(void)
{
    InfoTrack track;
    InfoPlayback playback;
    InfoLyrics lyrics;

    info_track_snapshot(&track);
    info_playback_snapshot(&playback);
    info_lyrics_snapshot(&lyrics);

    if (track_changed(&g_local.track, &track)) {
        g_local.track = track;
        g_local.state_revision++;
        /* 曲目变化时封面缓存失效 */
        g_local.cover_valid = 0;
    }
    if (playback_changed(&g_local.playback, &playback)) {
        g_local.playback = playback;
        g_local.state_revision++;
    }
    if (lyrics_changed(&g_local.lyrics, &lyrics)) {
        g_local.lyrics = lyrics;
        g_local.lyrics_revision++;
    }

    /* 进度锚点：每次刷新都重新锚定，避免外推漂移 */
    g_local.anchor_ms = player_local_now_ms();
    g_local.anchor_position = playback.position_seconds;

    int queue_count = play_queue_count();
    int queue_position = play_queue_position();
    if (queue_count != g_local.queue_count || queue_position != g_local.queue_position) {
        g_local.queue_count = queue_count;
        g_local.queue_position = queue_position;
        g_local.queue_revision++;
    }

    const char *status = core_status_last();
    if (strcmp(status ? status : "", g_local.status_message) != 0) {
        snprintf(g_local.status_message, sizeof(g_local.status_message), "%s",
                 status ? status : "");
        g_local.state_revision++;
    }
}

/* ── 生命周期 ─────────────────────────────────────────────────── */

int player_local_init(PlayerBackend backend, const char *bus_name)
{
    (void)bus_name;
    if (backend != PLAYER_BACKEND_LOCAL) {
        log_error("player", "player_local only implements the local backend");
        return -1;
    }

    memset(&g_local, 0, sizeof(g_local));
    memset(&g_local.track, 0, sizeof(g_local.track));
    g_local.initialized = 1;
    g_local.connected = 1;

    /* 首次快照：把初始值当作“已变化”，界面第一帧就能拿到完整状态 */
    refresh_snapshot();
    g_local.state_revision++;
    g_local.queue_revision++;
    g_local.lyrics_revision++;
    log_info("player", "Local player backend initialized (playlist=%d queue=%d)",
             playlist_count(), play_queue_count());
    return 0;
}

void player_local_shutdown(void)
{
    g_local.initialized = 0;
    g_local.connected = 0;
}

int player_local_pump(void)
{
    if (!g_local.initialized) {
        return -1;
    }

    refresh_snapshot();
    return g_local.connected ? 0 : -1;
}

int player_local_is_connected(void)
{
    return g_local.connected;
}

PlayerBackend player_local_backend(void)
{
    return PLAYER_BACKEND_LOCAL;
}

int player_local_restart_core(void)
{
    return 0;   /* 本地后端就是核心本身 */
}

/* ── 修订号 ───────────────────────────────────────────────────── */

uint64_t player_local_state_revision(void)    { return g_local.state_revision; }
uint64_t player_local_queue_revision(void)    { return g_local.queue_revision; }
uint64_t player_local_lyrics_revision(void)   { return g_local.lyrics_revision; }
uint64_t player_local_config_revision(void)   { return g_local.config_revision; }
uint64_t player_local_cover_revision(void)    { return g_local.cover_revision; }

/* ── 快照读取 ─────────────────────────────────────────────────── */

const InfoTrack    *player_local_track(void)    { return &g_local.track; }
const InfoLyrics   *player_local_lyrics(void)   { return &g_local.lyrics; }
int player_local_cover_path(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    return get_current_album_cover_path(out, out_size) == 0 ? 0 : -1;
}

const char *player_local_status_message(void)   { return g_local.status_message; }

const InfoPlayback *player_local_playback(void)
{
    /* 播放中按本地单调时钟外推，界面进度条才平滑（与远程后端同一语义） */
    if (g_local.playback.state == PLAY_STATE_PLAYING &&
        g_local.playback.duration_seconds > 0) {
        unsigned long long now = player_local_now_ms();
        double elapsed = (double)(now - g_local.anchor_ms) / 1000.0;
        if (elapsed > 0.0) {
            int position = g_local.anchor_position +
                           (int)(elapsed * (double)g_local.playback.speed);
            if (position > g_local.playback.duration_seconds) {
                position = g_local.playback.duration_seconds;
            }
            if (position > g_local.playback.position_seconds) {
                g_local.playback.position_seconds = position;
            }
        }
    }
    return &g_local.playback;
}

int player_local_track_index(void)
{
    return (playlist_count() > 0 && g_current_play_index >= 0) ? g_current_play_index : -1;
}

int player_local_track_metadata(int index, Track *out)
{
    if (!out || index < 0 || index >= playlist_count()) {
        return -1;
    }
    return get_track_metadata(index, out);
}

int player_local_position_seconds(void) { return player_local_playback()->position_seconds; }
int player_local_duration_seconds(void) { return player_local_playback()->duration_seconds; }
int player_local_play_state(void)       { return (int)player_local_playback()->state; }
int player_local_play_mode(void)        { return (int)player_local_playback()->play_mode; }
int player_local_volume_percent(void)   { return player_local_playback()->volume_percent; }
float player_local_speed(void)          { return g_playback_speed; }

/* ── transport ────────────────────────────────────────────────── */

void player_local_play(int track_index)
{
    if (track_index >= 0 && track_index < playlist_count()) {
        play_audio(track_index);
        app_set_selection_for_track(track_index);
    } else if (playlist_count() > 0) {
        int index = (g_current_play_index >= 0) ? g_current_play_index : 0;
        play_audio(index);
    }
    refresh_snapshot();
}

void player_local_pause(void)          { pause_audio(); refresh_snapshot(); }
void player_local_resume(void)         { resume_audio(); refresh_snapshot(); }
void player_local_play_pause(void)
{
    if (g_play_state == PLAY_STATE_PLAYING) {
        pause_audio();
    } else if (g_play_state == PLAY_STATE_PAUSED) {
        resume_audio();
    } else {
        player_local_play(-1);
    }
    refresh_snapshot();
}
void player_local_stop(void)           { stop_audio(); refresh_snapshot(); }
void player_local_next(void)           { next_track(); refresh_snapshot(); }
void player_local_prev(void)           { prev_track(); refresh_snapshot(); }

void player_local_seek_seconds(int seconds)
{
    if (g_total_duration <= 0) {
        return;
    }
    if (seconds < 0) seconds = 0;
    if (seconds > g_total_duration) seconds = g_total_duration;
    seek_audio((double)seconds);
    /* 乐观回显：进度条立刻跳到目标位置 */
    g_local.playback.position_seconds = seconds;
    g_local.anchor_position = seconds;
    g_local.anchor_ms = player_local_now_ms();
    g_local.state_revision++;
    refresh_snapshot();
}

void player_local_set_volume(int percent)
{
    set_volume_percent(percent);
    g_local.playback.volume_percent = get_volume_percent();   /* 已钳制 */
    g_local.state_revision++;
    refresh_snapshot();
}

const float *player_local_speed_steps(int *out_count)
{
    if (out_count) {
        *out_count = g_speed_count;
    }
    return g_speed_ratios;
}

int player_local_speed_step_count(void)
{
    return g_speed_count;
}

int player_local_speed_index(void)
{
    return g_speed_index;
}

void player_local_set_speed_index(int index)
{
    if (index < 0 || index >= g_speed_count) {
        return;
    }
    player_local_set_speed(g_speed_ratios[index]);
}

void player_local_adjust_volume(int delta)
{
    player_local_set_volume(player_local_volume_percent() + delta);
}

const char *player_local_play_mode_name(int use_english)
{
    return play_mode_display_name((PlayMode)player_local_play_mode(), use_english);
}

const char *player_local_play_mode_name_of(PlayMode mode, int use_english)
{
    return play_mode_display_name(mode, use_english);
}

void player_local_cycle_play_mode(void)
{
    cycle_play_mode();
    refresh_snapshot();
}

/* ── 均衡器 ───────────────────────────────────────────────────── */

int   player_local_eq_enabled(void)               { return eq_is_enabled(); }
void  player_local_eq_set_enabled(int enabled)    { eq_set_enabled(enabled); }
void  player_local_eq_set_band_gain(int band, float gain) { eq_set_band_gain(band, gain); }
float player_local_eq_get_band_gain(int band)     { return eq_get_band_gain(band); }
void  player_local_eq_set_preamp(float preamp)    { eq_set_preamp(preamp); }
void  player_local_eq_apply_preset(int preset)    { eq_apply_preset(preset); }

void player_local_set_speed(float rate)
{
    if (!(rate >= 0.5f && rate <= 3.0f)) {
        return;
    }
    g_playback_speed = rate;
    g_app_config.default_playback_speed = rate;
    save_config();
    apply_playback_speed_change();
    g_local.playback.speed = rate;
    g_local.state_revision++;
    g_local.config_revision++;
    refresh_snapshot();
}

void player_local_set_play_mode(PlayMode mode)
{
    if ((int)mode < 0 || (int)mode >= PLAY_MODE_COUNT) {
        return;
    }
    set_play_mode(mode);
    g_local.playback.play_mode = get_play_mode();
    g_local.state_revision++;
    refresh_snapshot();
}

/* ── 队列 ─────────────────────────────────────────────────────── */

/* 内容列表下标解析器：后端队列条目只有路径，界面要的是内容下标 */
static int resolve_content_index(const char *path, void *user)
{
    (void)user;
    return playlist_find_track_index_by_path(path);
}

/* 后端队列变化后重建界面用的队列镜像（队列位置 → 内容下标） */
static void play_local_queue_sync_mirror(void)
{
    play_queue_sync_mirror(resolve_content_index, NULL);
}

/* 队列位置 ↔ 内容列表物理下标：前端的内容列表与后端队列一一对应
 * （装配顺序即物理下标顺序，见 playlist/playlist_queue.c）。 */
int player_local_queue_count(void)      { return play_queue_count(); }
int player_local_queue_position(void)   { return play_queue_position(); }
int player_local_queue_is_active(void)  { return play_queue_is_active(&g_play_queue); }

int player_local_queue_index_at(int position)
{
    BackendQueueEntry entry;
    if (bq_entry_at(position, &entry) != 0) {
        return -1;
    }
    return playlist_find_track_index_by_path(entry.path);
}

int player_local_queue_play(int position)
{
    if (bq_play_at(position) != 0) {
        return -1;
    }
    play_audio(position);
    int track_index = player_local_queue_index_at(position);
    if (track_index >= 0) {
        app_set_selection_for_track(track_index);
    }
    refresh_snapshot();
    return 0;
}

int player_local_queue_append(int track_index)
{
    int rc = playlist_queue_push_entry(track_index, 0);
    refresh_snapshot();
    return rc < 0 ? -1 : 0;
}

int player_local_queue_insert_after(int track_index)
{
    int rc = playlist_queue_push_entry(track_index, 1);
    refresh_snapshot();
    return rc < 0 ? -1 : 0;
}

int player_local_queue_remove_at(int position)        { int rc = play_queue_remove_at(&g_play_queue, position); refresh_snapshot(); return rc; }
int player_local_queue_move_up(int position)          { int rc = play_queue_move_up(&g_play_queue, position); refresh_snapshot(); return rc; }
int player_local_queue_move_down(int position)        { int rc = play_queue_move_down(&g_play_queue, position); refresh_snapshot(); return rc; }

int player_local_queue_clear(void)
{
    play_queue_clear(&g_play_queue);
    refresh_snapshot();
    return 0;
}

int player_local_queue_rebuild(void)
{
    play_queue_rebuild(&g_play_queue, g_play_mode, NULL);
    refresh_snapshot();
    return 0;
}

int player_local_queue_shuffle(void)
{
    bq_shuffle_rest();
    play_local_queue_sync_mirror();
    refresh_snapshot();
    return 0;
}

int player_local_queue_push(void)
{
    int written = playlist_queue_sync();
    play_local_queue_sync_mirror();
    refresh_snapshot();
    return written < 0 ? -1 : written;
}

int player_local_queue_find(const char *path)
{
    return bq_position_of_path(path);
}

int player_local_queue_page_count(void)
{
    return bq_count();
}

int player_local_queue_page(int offset, int count, BackendQueueEntry *out, int out_cap)
{
    if (!out || out_cap <= 0) {
        return 0;
    }
    int written = 0;
    for (int i = 0; i < count && written < out_cap; i++) {
        if (bq_entry_at(offset + i, &out[written]) != 0) {
            break;
        }
        written++;
    }
    return written;
}

/* 播放列表 / 曲库 / 收藏 / 历史是**前端自有内容**（playlist/、library/、
 * search/）：界面与这部分门面都不再经播放服务，故此处没有实现。 */

/* ── 配置（前端镜像 + 唯一写入口） ─────────────────────────────── */

int player_local_config_refresh(void)
{
    return 0;   /* 本地后端里 g_app_config 就是权威副本 */
}

int player_local_config_apply_json(const char *patch_json)
{
    char reason[256];
    if (config_apply_json(patch_json, reason, sizeof(reason)) != 0) {
        log_warn("player", "Config patch rejected: %s", reason);
        return -1;
    }
    save_config();
    core_config_apply();
    g_local.config_revision++;
    refresh_snapshot();
    return 0;
}

int player_local_config_set_int(const char *key, int value)
{
    char patch[256];
    snprintf(patch, sizeof(patch), "{\"preferences\":{\"%s\":%d}}", key, value);
    return player_local_config_apply_json(patch);
}

int player_local_config_set_string(const char *key, const char *value)
{
    char patch[MAX_PATH_LEN + 128];
    snprintf(patch, sizeof(patch), "{\"preferences\":{\"%s\":\"%s\"}}",
             key, value ? value : "");
    return player_local_config_apply_json(patch);
}

int player_local_config_set_float(const char *key, float value)
{
    char patch[256];
    snprintf(patch, sizeof(patch), "{\"preferences\":{\"%s\":%.2f}}", key, (double)value);
    return player_local_config_apply_json(patch);
}

int player_local_config_reload(void)
{
    g_config_reload_requested = 1;
    return 0;
}

int player_local_config_reset(void)
{
    init_default_config();
    save_config();
    core_config_apply();
    g_local.config_revision++;
    refresh_snapshot();
    return 0;
}

/* ── 歌词 / 封面 / 可视化 ─────────────────────────────────────── */

int player_local_lyrics_document(int offset, int count, PlayerLyricsDoc *out)
{
    if (!out || offset < 0 || count <= 0) {
        return -1;
    }
    if (count > PLAYER_LYRIC_PAGE_MAX) {
        count = PLAYER_LYRIC_PAGE_MAX;
    }

    memset(out, 0, sizeof(*out));
    out->offset = offset;

    char track_id[96] = "";
    if (g_current_play_index >= 0 && g_current_play_index < playlist_count()) {
        char track_path[MAX_PATH_LEN];
        if (playlist_get_track_path(g_current_play_index, track_path,
                                    sizeof(track_path)) == 0) {
            info_build_track_id(track_id, sizeof(track_id), track_path);
        }
    }
    snprintf(out->track_id, sizeof(out->track_id), "%s", track_id);

    pthread_mutex_lock(&g_lyrics.lock);
    out->total = g_lyrics.count;
    out->has_lyrics = g_lyrics.has_lyrics;
    out->has_timestamps = g_lyrics.has_timestamps;
    out->source = g_lyrics.source;
    out->current_index = (g_lyrics.has_lyrics && g_lyrics.current_index >= 0)
        ? g_lyrics.current_index : -1;

    int written = 0;
    for (int i = offset; i < g_lyrics.count && written < count; i++) {
        out->lines[written++] = g_lyrics.lines[i];
    }
    pthread_mutex_unlock(&g_lyrics.lock);

    out->count = written;
    out->revision = g_local.lyrics_revision;
    return written;
}

int player_local_lyrics_reload_source(int source)
{
    lyrics_switch_source(source);
    refresh_snapshot();
    return 0;
}

int player_local_lyrics_highlight(int *out_current, int *out_next, int *out_has_timestamps)
{
    return lyrics_highlight(out_current, out_next, out_has_timestamps, NULL);
}

int player_local_lyrics_highlight_count(void)
{
    return lyrics_highlight_count();
}

int player_local_lyrics_source(void)
{
    return lyrics_source();
}

int player_local_lyrics_total(void)
{
    int total = 0;
    pthread_mutex_lock(&g_lyrics.lock);
    total = g_lyrics.count;
    pthread_mutex_unlock(&g_lyrics.lock);
    return total;
}

int player_local_lyrics_line_at(int index, LyricLine *out)
{
    if (!out || index < 0) {
        return -1;
    }
    int rc = -1;
    pthread_mutex_lock(&g_lyrics.lock);
    if (index < g_lyrics.count) {
        *out = g_lyrics.lines[index];
        rc = 0;
    }
    pthread_mutex_unlock(&g_lyrics.lock);
    return rc;
}

int player_local_cover_rows(int cols, int rows, int charset, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    InfoTrack *track = &g_local.track;
    if (!track->valid) {
        return 0;
    }

    if (g_local.cover_valid &&
        g_local.cover_cols == cols &&
        g_local.cover_rows == rows &&
        g_local.cover_charset == charset &&
        strcmp(g_local.cover_track_id, track->track_id) == 0) {
        snprintf(out, out_size, "%s", g_local.cover_text);
        return out[0] ? 1 : 0;
    }

    int have = info_cover_text(cols, rows, charset, g_local.cover_text,
                               sizeof(g_local.cover_text));
    g_local.cover_valid = 1;
    g_local.cover_cols = cols;
    g_local.cover_rows = rows;
    g_local.cover_charset = charset;
    snprintf(g_local.cover_track_id, sizeof(g_local.cover_track_id), "%s",
             track->track_id);
    g_local.cover_revision++;

    if (have) {
        snprintf(out, out_size, "%s", g_local.cover_text);
    }
    return have;
}

void player_local_visualizer(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms)
{
    get_visualizer_snapshot(levels, peaks, max_levels, last_update_ms);
}

void player_local_set_visualizer_active(int active)
{
    (void)active;   /* 本地后端始终采样；远程后端用它决定是否订阅帧信号 */
}

/* 远程服务器不在门面里：远程音乐源是前端功能（remote/remote_store.c 与
 * remote/remote_cache.c），核心不认识远程，故门面不暴露任何远程接口。 */

