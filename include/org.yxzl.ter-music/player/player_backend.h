/**
 * @file player_backend.h
 * @brief 两端后端（local/remote）的内部原型声明（仅 player.c 使用）
 *
 * 门面（player/player.h）是界面看到的唯一入口；两端后端实现同一组符号，
 * 分别以 player_local_ / player_remote_ 为前缀，由 player.c 按当前后端转发。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef PLAYER_BACKEND_H
#define PLAYER_BACKEND_H

#include "player/player.h"

struct PlayerBackendOps;

PlayerBackend player_local_backend(void);
PlayerBackend player_remote_backend(void);
const InfoLyrics   *player_local_lyrics(void);
const InfoLyrics   *player_remote_lyrics(void);
const InfoPlayback *player_local_playback(void);
const InfoPlayback *player_remote_playback(void);
const InfoTrack    *player_local_track(void);
const InfoTrack    *player_remote_track(void);
const char         *player_local_status_message(void);
const char         *player_remote_status_message(void);
const char *player_local_play_mode_name(int use_english);
const char *player_local_play_mode_name_of(PlayMode mode, int use_english);
const char *player_remote_play_mode_name(int use_english);
const char *player_remote_play_mode_name_of(PlayMode mode, int use_english);
const float *player_local_speed_steps(int *out_count);
const float *player_remote_speed_steps(int *out_count);
float player_local_eq_get_band_gain(int band);
float player_local_speed(void);
float player_remote_eq_get_band_gain(int band);
float player_remote_speed(void);
int   player_local_eq_enabled(void);
int   player_remote_eq_enabled(void);
int player_local_config_apply_json(const char *patch_json);
int player_local_config_refresh(void);
int player_local_config_reload(void);
int player_local_config_reset(void);
int player_local_config_set_float(const char *key, float value);
int player_local_config_set_int(const char *key, int value);
int player_local_config_set_string(const char *key, const char *value);
int player_local_cover_path(char *out, size_t out_size);
int player_local_cover_rows(int cols, int rows, int charset, char *out, size_t out_size);
int player_local_duration_seconds(void);
int player_local_is_connected(void);
int player_local_lyrics_document(int offset, int count, PlayerLyricsDoc *out);
int player_local_lyrics_highlight(int *out_current, int *out_next, int *out_has_timestamps);
int player_local_lyrics_highlight_count(void);
int player_local_lyrics_line_at(int index, LyricLine *out);
int player_local_lyrics_reload_source(int source);
int player_local_lyrics_source(void);
int player_local_lyrics_total(void);
int player_local_play_mode(void);
int player_local_play_state(void);
int player_local_position_seconds(void);
int player_local_pump(void);
int player_local_queue_append(int track_index);
int player_local_queue_clear(void);
int player_local_queue_count(void);
int player_local_queue_find(const char *path);
int player_local_queue_index_at(int position);
int player_local_queue_insert_after(int track_index);
int player_local_queue_is_active(void);
int player_local_queue_move_down(int position);
int player_local_queue_move_up(int position);
int player_local_queue_page(int offset, int count, BackendQueueEntry *out, int out_cap);
int player_local_queue_page_count(void);
int player_local_queue_play(int position);
int player_local_queue_position(void);
int player_local_queue_push(void);
int player_local_queue_rebuild(void);
int player_local_queue_remove_at(int position);
int player_local_queue_shuffle(void);
int player_local_restart_core(void);
int player_local_speed_index(void);
int player_local_speed_step_count(void);
int player_local_track_index(void);
int player_local_track_metadata(int index, Track *out);
int player_local_volume_percent(void);
int player_remote_config_apply_json(const char *patch_json);
int player_remote_config_refresh(void);
int player_remote_config_reload(void);
int player_remote_config_reset(void);
int player_remote_config_set_float(const char *key, float value);
int player_remote_config_set_int(const char *key, int value);
int player_remote_config_set_string(const char *key, const char *value);
int player_remote_cover_path(char *out, size_t out_size);
int player_remote_cover_rows(int cols, int rows, int charset, char *out, size_t out_size);
int player_remote_duration_seconds(void);
int player_remote_is_connected(void);
int player_remote_lyrics_document(int offset, int count, PlayerLyricsDoc *out);
int player_remote_lyrics_highlight(int *out_current, int *out_next, int *out_has_timestamps);
int player_remote_lyrics_highlight_count(void);
int player_remote_lyrics_line_at(int index, LyricLine *out);
int player_remote_lyrics_reload_source(int source);
int player_remote_lyrics_source(void);
int player_remote_lyrics_total(void);
int player_remote_play_mode(void);
int player_remote_play_state(void);
int player_remote_position_seconds(void);
int player_remote_pump(void);
int player_remote_queue_append(int track_index);
int player_remote_queue_clear(void);
int player_remote_queue_count(void);
int player_remote_queue_find(const char *path);
int player_remote_queue_index_at(int position);
int player_remote_queue_insert_after(int track_index);
int player_remote_queue_is_active(void);
int player_remote_queue_move_down(int position);
int player_remote_queue_move_up(int position);
int player_remote_queue_page(int offset, int count, BackendQueueEntry *out, int out_cap);
int player_remote_queue_page_count(void);
int player_remote_queue_play(int position);
int player_remote_queue_position(void);
int player_remote_queue_push(void);
int player_remote_queue_rebuild(void);
int player_remote_queue_remove_at(int position);
int player_remote_queue_shuffle(void);
int player_remote_restart_core(void);
int player_remote_speed_index(void);
int player_remote_speed_step_count(void);
int player_remote_track_index(void);
int player_remote_track_metadata(int index, Track *out);
int player_remote_volume_percent(void);
uint64_t player_local_config_revision(void);
uint64_t player_local_cover_revision(void);
uint64_t player_local_lyrics_revision(void);
uint64_t player_local_queue_revision(void);
uint64_t player_local_state_revision(void);
uint64_t player_remote_config_revision(void);
uint64_t player_remote_cover_revision(void);
uint64_t player_remote_lyrics_revision(void);
uint64_t player_remote_queue_revision(void);
uint64_t player_remote_state_revision(void);
void  player_local_eq_apply_preset(int preset);
void  player_local_eq_set_band_gain(int band, float gain);
void  player_local_eq_set_enabled(int enabled);
void  player_local_eq_set_preamp(float preamp);
void  player_remote_eq_apply_preset(int preset);
void  player_remote_eq_set_band_gain(int band, float gain);
void  player_remote_eq_set_enabled(int enabled);
void  player_remote_eq_set_preamp(float preamp);
void player_local_adjust_volume(int delta);
void player_local_cycle_play_mode(void);
void player_local_next(void);
void player_local_pause(void);
void player_local_play(int track_index);
void player_local_play_pause(void);
void player_local_prev(void);
void player_local_resume(void);
void player_local_seek_seconds(int seconds);
void player_local_set_play_mode(PlayMode mode);
void player_local_set_speed(float rate);
void player_local_set_speed_index(int index);
void player_local_set_visualizer_active(int active);
void player_local_set_volume(int percent);
void player_local_stop(void);
void player_local_visualizer(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms);
void player_remote_adjust_volume(int delta);
void player_remote_cycle_play_mode(void);
void player_remote_next(void);
void player_remote_pause(void);
void player_remote_play(int track_index);
void player_remote_play_pause(void);
void player_remote_prev(void);
void player_remote_resume(void);
void player_remote_seek_seconds(int seconds);
void player_remote_set_play_mode(PlayMode mode);
void player_remote_set_speed(float rate);
void player_remote_set_speed_index(int index);
void player_remote_set_visualizer_active(int active);
void player_remote_set_volume(int percent);
void player_remote_stop(void);
void player_remote_visualizer(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms);

#endif /* PLAYER_BACKEND_H */
