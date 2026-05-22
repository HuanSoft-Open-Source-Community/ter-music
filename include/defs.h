#ifndef DEFS_H
#define DEFS_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "progress.h"
#include "logger.h"
#include <time.h>

#define APP_NAME "ter-music"
#define APP_VERSION "v1.10.0"
#define APP_AUTHORS "@燕戏竹林, @罐子(-.-)"
#define APP_EMAIL "yxzl666xx@outlook.com"
#define APP_REPO "https://github.com/YXZL985/ter-music.git"

typedef enum {
    LOOP_OFF = 0,
    LOOP_SINGLE = 1,
    LOOP_LIST = 2,
    LOOP_RANDOM = 3
} LoopMode;

typedef enum {
    PLAY_STATE_STOPPED = 0,
    PLAY_STATE_PLAYING = 1,
    PLAY_STATE_PAUSED = 2
} PlayState;

typedef enum {
    VIEW_MAIN = 0,
    VIEW_SETTINGS = 1,
    VIEW_HISTORY = 2,
    VIEW_PLAYLIST = 3,
    VIEW_FAVORITES = 4,
    VIEW_INFO = 5
} ViewMode;

typedef enum {
    FOCUS_SIDEBAR = 0,
    FOCUS_CONTENT = 1
} FocusArea;

typedef enum {
    UI_LANG_ZH = 0,
    UI_LANG_EN = 1
} UiLanguage;

#define COLOR_PAIR_BORDER 1
#define COLOR_PAIR_PLAYLIST 2
#define COLOR_PAIR_CONTROLS 3
#define COLOR_PAIR_LYRICS 4
#define COLOR_PAIR_SIDEBAR 5
#define COLOR_PAIR_HIGHLIGHT 6

#define UI_DIRTY_PLAYLIST 0x01
#define UI_DIRTY_CONTROLS 0x02
#define UI_DIRTY_LYRICS 0x04

#define CONTROL_COUNT 8
#define MAX_AUDIO_BUFFER_SIZE (44100 * 2 * sizeof(int32_t))
#define MAX_PATH_LEN 512
#define MAX_TRACKS 1000
#define MAX_META_LEN 256
#define MAX_SEARCH_KEY_LEN (MAX_META_LEN * 8)
#define MAX_SEARCH_RESULTS 1000

#define MAX_HISTORY_COUNT 100
#define MAX_FAVORITES_COUNT 200
#define MAX_DIR_HISTORY_COUNT 50
#define MAX_PLAYLISTS_COUNT 50
#define MAX_PLAYLIST_NAME_LEN 64
#define VISUALIZER_BAND_COUNT 64
#define MAX_CACHE_SIZE 100

#define MAX_ALBUM_COVER_CACHE 10
#define ALBUM_COVER_TEMP_PREFIX "/tmp/ter-music-cover-"
#define MAX_REMOTE_CONNECTIONS 20
#define MAX_REMOTE_NAME_LEN 64

#define AUDIO_BACKEND_AUTO  0
#define AUDIO_BACKEND_PULSE 1
#define AUDIO_BACKEND_ALSA  2

typedef struct {
    char path[MAX_PATH_LEN];
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
} Track;

typedef struct {
    int index;
    int valid;
    time_t last_used;
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
    char title_search[MAX_SEARCH_KEY_LEN];
    char artist_search[MAX_SEARCH_KEY_LEN];
    char album_search[MAX_SEARCH_KEY_LEN];
} CachedTrack;

typedef struct {
    char path[MAX_PATH_LEN];
    time_t open_time;
} DirHistoryEntry;

typedef struct {
    DirHistoryEntry entries[MAX_DIR_HISTORY_COUNT];
    int count;
} DirHistory;

typedef struct {
    char path[MAX_PATH_LEN];
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    time_t play_time;
} HistoryEntry;

typedef struct {
    HistoryEntry entries[MAX_HISTORY_COUNT];
    int count;
} PlayHistory;

typedef struct {
    Track tracks[MAX_FAVORITES_COUNT];
    int count;
} Favorites;

typedef struct {
    char name[MAX_PLAYLIST_NAME_LEN];
    Track tracks[MAX_TRACKS];
    int track_count;
    time_t created_time;
    time_t modified_time;
} UserPlaylist;

typedef struct {
    UserPlaylist playlists[MAX_PLAYLISTS_COUNT];
    int count;
} PlaylistManager;

typedef struct {
    int playlist_fg;
    int playlist_bg;
    int controls_fg;
    int controls_bg;
    int lyrics_fg;
    int lyrics_bg;
    int sidebar_fg;
    int sidebar_bg;
    int highlight_fg;
    int highlight_bg;
    int border_fg;
    int border_bg;
} ColorTheme;

typedef enum {
    REMOTE_PROTOCOL_SMB = 0,
    REMOTE_PROTOCOL_SFTP = 1,
    REMOTE_PROTOCOL_FTP = 2,
    REMOTE_PROTOCOL_WEBDAV = 3,
    REMOTE_PROTOCOL_HTTP = 4
} RemoteProtocol;

typedef struct {
    char name[MAX_REMOTE_NAME_LEN];
    int protocol;               // RemoteProtocol value
    char host[256];
    int port;
    char username[64];
    char password[256];
    char private_key_path[MAX_PATH_LEN];
    char base_path[512];
} RemoteConnectionConfig;

typedef struct {
    char default_startup_path[MAX_PATH_LEN];
    char last_opened_path[MAX_PATH_LEN];
    char last_played_folder_path[MAX_PATH_LEN];
    char last_played_track_path[MAX_PATH_LEN];
    ColorTheme theme;
    int auto_play_on_start;
    int remember_last_path;
    int clear_history_on_startup;
    int resume_last_playback;
    int last_played_position;
    int ui_language;
    int volume_percent;
    int audio_latency_ms;
    int show_lyrics_panel;
    int default_loop_mode;
    float default_playback_speed;
    int show_album_cover;
    int lyrics_alignment;  // 0=居左(Left), 1=居中(Center), 2=居右(Right)
    int audio_backend;     // 0=Auto, 1=PulseAudio, 2=ALSA
    int config_version;
    RemoteConnectionConfig remote_connections[MAX_REMOTE_CONNECTIONS];
    int remote_connection_count;
} AppConfig;

typedef struct {
    char tracks[MAX_TRACKS][MAX_PATH_LEN];
    int count;
    char folder_path[MAX_PATH_LEN];
    int is_loaded;
    int has_multiple_sources;
    CachedTrack cache[MAX_CACHE_SIZE];
    int cache_count;
} Playlist;

typedef struct {
    int result_indices[MAX_SEARCH_RESULTS];
    int result_count;
    int active;
    int selected_index;
    int result_offset;
} SearchState;

extern Playlist g_playlist;
extern int g_selected_index;
extern int g_control_focus;
extern int g_current_control_idx;
extern int g_control_count;
extern PlayState g_play_state;
extern int g_current_play_index;
extern LoopMode g_loop_mode;
extern pthread_t g_play_thread;
extern int g_play_thread_running;
extern char g_default_audio_device[128];

extern ViewMode g_current_view;
extern int g_menu_selected_idx;
extern PlayHistory g_play_history;
extern Favorites g_favorites;
extern DirHistory g_dir_history;
extern PlaylistManager g_playlist_manager;
extern AppConfig g_app_config;

extern int g_content_selected_idx;
extern FocusArea g_focus_area;

extern int g_current_position;
extern int g_total_duration;
extern int g_initial_seek_position;
extern pthread_mutex_t g_seek_mutex;

extern int g_lyric_cursor_mode;
extern int g_lyric_cursor_index;

extern int g_active_backend;

extern SearchState g_search_state;
extern float g_playback_speed;

void toggle_playback_speed(void);
void init_ffmpeg();
void init_audio_device();
int audio_backend_is_available(int backend);
int load_playlist(const char *folder_path);
int append_playlist(const char *folder_path);
int load_single_file(const char *file_path);
int load_remote_playlist(const RemoteConnectionConfig *conn, const char *subpath);
void reset_playlist_state(void);
void playlist_lock(void);
void playlist_unlock(void);
int playlist_count(void);
int playlist_is_loaded(void);
int playlist_has_multiple_sources(void);
void playlist_copy_folder_path(char *dest, size_t dest_size);
int playlist_get_track_path(int index, char *dest, size_t dest_size);
int playlist_find_track_index_by_path(const char *track_path);
int track_matches_query(int index, const char *query);
void play_audio(int index);
void pause_audio();
void resume_audio();
void stop_audio();
void wait_for_playback_thread_shutdown(void);
void prev_track();
void next_track();
void toggle_loop_mode();
const char *get_loop_mode_str();
void cleanup();
void seek_audio(double position);
int get_and_clear_initial_seek_position(void);
int get_volume_percent(void);
void set_volume_percent(int volume);
void adjust_volume(int delta);
void persist_playback_session_state(void);

int utf8_str_truncate(char *dest, const char *src, int max_cols);
int utf8_str_width(const char *src);
int utf8_str_substring(char *dest, const char *src, int start_col, int max_cols);
int utf8_str_pad(char *dest, size_t dest_size, const char *src, int width);
void decode_html_entities(char *str);
int use_ascii_fallback_ui(void);
int use_english_ui(void);

void update_progress_bar();
void update_controls_status(const char *msg);
void request_ui_refresh(int dirty_mask);
void process_pending_ui_refresh(void);
void reap_finished_playback_thread(void);
void process_pending_playback_action(void);
void reset_visualizer_state(void);
void push_visualizer_samples(const int32_t *samples, int frame_count, int channels);
void get_visualizer_snapshot(int *levels, int *peaks, int max_levels, uint64_t *last_update_ms);

void apply_color_theme(void);

extern int g_rainbow_mode_enabled;
extern ColorTheme g_saved_theme;
extern int g_debug_enabled;
extern int g_konami_input_pos;
extern uint64_t g_konami_last_time;
extern int g_rainbow_color_offset;
void check_konami_input(int ch);
void toggle_rainbow_mode(void);
void update_rainbow_colors(void);

int get_track_metadata(int index, Track *out);
void preload_visible_tracks(int start, int end);
void clear_metadata_cache(void);

int extract_album_cover(const char *audio_path, char *output_path, size_t output_size);
int get_current_album_cover_path(char *path, size_t path_size);
void cleanup_album_cover_cache(void);
void update_album_cover_for_track(const char *track_path);
void reset_album_cover_cache(void);

extern char g_current_album_cover_path[MAX_PATH_LEN];
extern int g_current_album_cover_valid;

extern char g_braille_art_buffer[];
extern int g_album_cover_size;

extern int g_audio_sample_rate;          // Hz, e.g. 44100
extern int g_audio_bit_rate;             // bps, e.g. 320000
extern int g_audio_bit_depth;            // bits, e.g. 16 or 24 (0 = unknown)
extern char g_audio_codec_name[32];      // e.g. "flac", "mp3"

#endif
