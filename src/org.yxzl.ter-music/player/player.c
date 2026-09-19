/**
 * @file player.c
 * @brief 门面分派层：把界面调用路由到当前后端（本地直调 / D-Bus 客户端）
 *
 * 两个后端各自实现同一组入口（`player_local_*` / `player_remote_*`），本文件
 * 只做两件事：记住 `player_init()` 选定的后端，然后把每个门面调用转发过去。
 * 界面只 include `player/player.h`，不认识后端差异。
 *
 * 为什么需要这一层：迁移期两种后端必须能同时编进同一个二进制（`--frontend=`
 * 切换、A/B 回归），因此不能让两边定义同名符号；分派层让“同名门面 + 两套
 * 实现”共存，且新增门面函数时只需在此处补一行转发。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "player/player.h"

#include "player/player_backend.h"

#include "logger/logger.h"

#include <string.h>

static PlayerBackend g_active_backend = PLAYER_BACKEND_LOCAL;
static int g_initialized = 0;

static int use_remote(void)
{
    return g_active_backend == PLAYER_BACKEND_REMOTE;
}

/* ── 生命周期 ───────────────────────────────────────────────────── */

int player_init(PlayerBackend backend, const char *bus_name)
{
    g_active_backend = backend;
    g_initialized = 1;
    log_info("player", "Player facade using the %s backend",
             backend == PLAYER_BACKEND_REMOTE ? "remote" : "local");
    return backend == PLAYER_BACKEND_REMOTE
        ? player_remote_init(backend, bus_name)
        : player_local_init(backend, bus_name);
}

void player_shutdown(void)
{
    if (!g_initialized) {
        return;
    }
    if (use_remote()) {
        player_remote_shutdown();
    } else {
        player_local_shutdown();
    }
    g_initialized = 0;
}

int player_pump(void)
{
    return use_remote() ? player_remote_pump() : player_local_pump();
}

int player_is_connected(void)
{
    return use_remote() ? player_remote_is_connected() : player_local_is_connected();
}

PlayerBackend player_backend(void)
{
    return g_active_backend;
}

int player_restart_core(void)
{
    return use_remote() ? player_remote_restart_core() : player_local_restart_core();
}

/* ── 修订号 ─────────────────────────────────────────────────────── */

uint64_t player_state_revision(void)
{
    return use_remote() ? player_remote_state_revision() : player_local_state_revision();
}
uint64_t player_queue_revision(void)
{
    return use_remote() ? player_remote_queue_revision() : player_local_queue_revision();
}
uint64_t player_lyrics_revision(void)
{
    return use_remote() ? player_remote_lyrics_revision() : player_local_lyrics_revision();
}
uint64_t player_config_revision(void)
{
    return use_remote() ? player_remote_config_revision() : player_local_config_revision();
}
uint64_t player_cover_revision(void)
{
    return use_remote() ? player_remote_cover_revision() : player_local_cover_revision();
}

/* ── 快照 ───────────────────────────────────────────────────────── */

const InfoTrack *player_track(void)
{
    return use_remote() ? player_remote_track() : player_local_track();
}
const InfoPlayback *player_playback(void)
{
    return use_remote() ? player_remote_playback() : player_local_playback();
}
const InfoLyrics *player_lyrics(void)
{
    return use_remote() ? player_remote_lyrics() : player_local_lyrics();
}
const char *player_status_message(void)
{
    return use_remote() ? player_remote_status_message() : player_local_status_message();
}

int player_track_index(void)
{
    return use_remote() ? player_remote_track_index() : player_local_track_index();
}
int player_track_metadata(int index, Track *out)
{
    return use_remote() ? player_remote_track_metadata(index, out)
                        : player_local_track_metadata(index, out);
}
int player_position_seconds(void)
{
    return use_remote() ? player_remote_position_seconds() : player_local_position_seconds();
}
int player_duration_seconds(void)
{
    return use_remote() ? player_remote_duration_seconds() : player_local_duration_seconds();
}
int player_play_state(void)
{
    return use_remote() ? player_remote_play_state() : player_local_play_state();
}
int player_play_mode(void)
{
    return use_remote() ? player_remote_play_mode() : player_local_play_mode();
}
int player_volume_percent(void)
{
    return use_remote() ? player_remote_volume_percent() : player_local_volume_percent();
}
float player_speed(void)
{
    return use_remote() ? player_remote_speed() : player_local_speed();
}

/* ── transport ──────────────────────────────────────────────────── */

void player_play(int track_index)
{
    if (use_remote()) player_remote_play(track_index); else player_local_play(track_index);
}
void player_pause(void)
{
    if (use_remote()) player_remote_pause(); else player_local_pause();
}
void player_resume(void)
{
    if (use_remote()) player_remote_resume(); else player_local_resume();
}
void player_play_pause(void)
{
    if (use_remote()) player_remote_play_pause(); else player_local_play_pause();
}
void player_stop(void)
{
    if (use_remote()) player_remote_stop(); else player_local_stop();
}
void player_next(void)
{
    if (use_remote()) player_remote_next(); else player_local_next();
}
void player_prev(void)
{
    if (use_remote()) player_remote_prev(); else player_local_prev();
}
void player_seek_seconds(int seconds)
{
    if (use_remote()) player_remote_seek_seconds(seconds); else player_local_seek_seconds(seconds);
}
void player_set_volume(int percent)
{
    if (use_remote()) player_remote_set_volume(percent); else player_local_set_volume(percent);
}
void player_adjust_volume(int delta)
{
    if (use_remote()) player_remote_adjust_volume(delta); else player_local_adjust_volume(delta);
}
void player_set_speed(float rate)
{
    if (use_remote()) player_remote_set_speed(rate); else player_local_set_speed(rate);
}
void player_set_play_mode(PlayMode mode)
{
    if (use_remote()) player_remote_set_play_mode(mode); else player_local_set_play_mode(mode);
}
void player_cycle_play_mode(void)
{
    if (use_remote()) player_remote_cycle_play_mode(); else player_local_cycle_play_mode();
}

const char *player_play_mode_name(int use_english)
{
    return use_remote() ? player_remote_play_mode_name(use_english)
                        : player_local_play_mode_name(use_english);
}
const char *player_play_mode_name_of(PlayMode mode, int use_english)
{
    return use_remote() ? player_remote_play_mode_name_of(mode, use_english)
                        : player_local_play_mode_name_of(mode, use_english);
}

const float *player_speed_steps(int *out_count)
{
    return use_remote() ? player_remote_speed_steps(out_count)
                        : player_local_speed_steps(out_count);
}
int player_speed_step_count(void)
{
    return use_remote() ? player_remote_speed_step_count() : player_local_speed_step_count();
}
int player_speed_index(void)
{
    return use_remote() ? player_remote_speed_index() : player_local_speed_index();
}
void player_set_speed_index(int index)
{
    if (use_remote()) player_remote_set_speed_index(index); else player_local_set_speed_index(index);
}

/* ── 均衡器 ─────────────────────────────────────────────────────── */

int player_eq_enabled(void)
{
    return use_remote() ? player_remote_eq_enabled() : player_local_eq_enabled();
}
void player_eq_set_enabled(int enabled)
{
    if (use_remote()) player_remote_eq_set_enabled(enabled); else player_local_eq_set_enabled(enabled);
}
void player_eq_set_band_gain(int band, float gain)
{
    if (use_remote()) player_remote_eq_set_band_gain(band, gain);
    else player_local_eq_set_band_gain(band, gain);
}
float player_eq_get_band_gain(int band)
{
    return use_remote() ? player_remote_eq_get_band_gain(band)
                        : player_local_eq_get_band_gain(band);
}
void player_eq_set_preamp(float preamp)
{
    if (use_remote()) player_remote_eq_set_preamp(preamp); else player_local_eq_set_preamp(preamp);
}
void player_eq_apply_preset(int preset)
{
    if (use_remote()) player_remote_eq_apply_preset(preset);
    else player_local_eq_apply_preset(preset);
}

/* ── 队列 ───────────────────────────────────────────────────────── */

int player_queue_count(void)
{
    return use_remote() ? player_remote_queue_count() : player_local_queue_count();
}
int player_queue_position(void)
{
    return use_remote() ? player_remote_queue_position() : player_local_queue_position();
}
int player_queue_index_at(int position)
{
    return use_remote() ? player_remote_queue_index_at(position)
                        : player_local_queue_index_at(position);
}
int player_queue_play(int position)
{
    return use_remote() ? player_remote_queue_play(position) : player_local_queue_play(position);
}
int player_queue_append(int track_index)
{
    return use_remote() ? player_remote_queue_append(track_index)
                        : player_local_queue_append(track_index);
}
int player_queue_insert_after(int track_index)
{
    return use_remote() ? player_remote_queue_insert_after(track_index)
                        : player_local_queue_insert_after(track_index);
}
int player_queue_remove_at(int position)
{
    return use_remote() ? player_remote_queue_remove_at(position)
                        : player_local_queue_remove_at(position);
}
int player_queue_move_up(int position)
{
    return use_remote() ? player_remote_queue_move_up(position)
                        : player_local_queue_move_up(position);
}
int player_queue_move_down(int position)
{
    return use_remote() ? player_remote_queue_move_down(position)
                        : player_local_queue_move_down(position);
}
int player_queue_clear(void)
{
    return use_remote() ? player_remote_queue_clear() : player_local_queue_clear();
}
int player_queue_rebuild(void)
{
    return use_remote() ? player_remote_queue_rebuild() : player_local_queue_rebuild();
}
int player_queue_shuffle(void)
{
    return use_remote() ? player_remote_queue_shuffle() : player_local_queue_shuffle();
}
int player_queue_is_active(void)
{
    return use_remote() ? player_remote_queue_is_active() : player_local_queue_is_active();
}
int player_queue_push(void)
{
    return use_remote() ? player_remote_queue_push() : player_local_queue_push();
}
int player_queue_find(const char *path)
{
    return use_remote() ? player_remote_queue_find(path) : player_local_queue_find(path);
}
int player_queue_page_count(void)
{
    return use_remote() ? player_remote_queue_page_count() : player_local_queue_page_count();
}
int player_queue_page(int offset, int count, BackendQueueEntry *out, int out_cap)
{
    return use_remote() ? player_remote_queue_page(offset, count, out, out_cap)
                        : player_local_queue_page(offset, count, out, out_cap);
}

/* ── 歌词 / 封面 / 可视化 ───────────────────────────────────────── */

int player_lyrics_document(int offset, int count, PlayerLyricsDoc *out)
{
    return use_remote() ? player_remote_lyrics_document(offset, count, out)
                        : player_local_lyrics_document(offset, count, out);
}
int player_lyrics_reload_source(int source)
{
    return use_remote() ? player_remote_lyrics_reload_source(source)
                        : player_local_lyrics_reload_source(source);
}
int player_lyrics_highlight(int *out_current, int *out_next, int *out_has_timestamps)
{
    return use_remote() ? player_remote_lyrics_highlight(out_current, out_next, out_has_timestamps)
                        : player_local_lyrics_highlight(out_current, out_next, out_has_timestamps);
}
int player_lyrics_highlight_count(void)
{
    return use_remote() ? player_remote_lyrics_highlight_count()
                        : player_local_lyrics_highlight_count();
}
int player_lyrics_source(void)
{
    return use_remote() ? player_remote_lyrics_source() : player_local_lyrics_source();
}
int player_lyrics_total(void)
{
    return use_remote() ? player_remote_lyrics_total() : player_local_lyrics_total();
}
int player_lyrics_line_at(int index, LyricLine *out)
{
    return use_remote() ? player_remote_lyrics_line_at(index, out)
                        : player_local_lyrics_line_at(index, out);
}

int player_cover_rows(int cols, int rows, int charset, char *out, size_t out_size)
{
    return use_remote() ? player_remote_cover_rows(cols, rows, charset, out, out_size)
                        : player_local_cover_rows(cols, rows, charset, out, out_size);
}
int player_cover_path(char *out, size_t out_size)
{
    return use_remote() ? player_remote_cover_path(out, out_size)
                        : player_local_cover_path(out, out_size);
}
void player_visualizer(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms)
{
    if (use_remote()) player_remote_visualizer(levels, peaks, max_levels, last_update_ms);
    else player_local_visualizer(levels, peaks, max_levels, last_update_ms);
}
void player_set_visualizer_active(int active)
{
    if (use_remote()) player_remote_set_visualizer_active(active);
    else player_local_set_visualizer_active(active);
}

/* ── 配置 ───────────────────────────────────────────────────────── */

int player_config_refresh(void)
{
    return use_remote() ? player_remote_config_refresh() : player_local_config_refresh();
}
int player_config_apply_json(const char *patch_json)
{
    return use_remote() ? player_remote_config_apply_json(patch_json)
                        : player_local_config_apply_json(patch_json);
}
int player_config_set_int(const char *key, int value)
{
    return use_remote() ? player_remote_config_set_int(key, value)
                        : player_local_config_set_int(key, value);
}
int player_config_set_string(const char *key, const char *value)
{
    return use_remote() ? player_remote_config_set_string(key, value)
                        : player_local_config_set_string(key, value);
}
int player_config_set_float(const char *key, float value)
{
    return use_remote() ? player_remote_config_set_float(key, value)
                        : player_local_config_set_float(key, value);
}
int player_config_reload(void)
{
    return use_remote() ? player_remote_config_reload() : player_local_config_reload();
}
int player_config_reset(void)
{
    return use_remote() ? player_remote_config_reset() : player_local_config_reset();
}
