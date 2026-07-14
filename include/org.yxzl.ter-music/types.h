#ifndef TYPES_H
#define TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#include "audio/equalizer.h"

#define APP_NAME "ter-music"
#define APP_VERSION "v2.0.0"
#define APP_AUTHORS "@燕戏竹林, @罐子(-.-)"
#define APP_EMAIL "yxzl666xx@outlook.com"
#define APP_REPO "https://github.com/YXZL985/ter-music.git"

typedef enum {
    PLAY_MODE_SEQUENTIAL             = 0,
    PLAY_MODE_SINGLE_REPEAT          = 1,
    PLAY_MODE_LIST_REPEAT            = 2,
    PLAY_MODE_SHUFFLE_ONCE           = 3,
    PLAY_MODE_SHUFFLE_REPEAT         = 4,
    PLAY_MODE_FOLDER_SEQUENTIAL      = 5,
    PLAY_MODE_FOLDER_REPEAT          = 6,
    PLAY_MODE_FOLDER_SHUFFLE         = 7,
    PLAY_MODE_FOLDER_SHUFFLE_REPEAT  = 8,
    PLAY_MODE_ALBUM_SEQUENTIAL       = 9,
    PLAY_MODE_ALBUM_REPEAT           = 10,
    PLAY_MODE_ALBUM_SHUFFLE          = 11,
    PLAY_MODE_ALBUM_SHUFFLE_REPEAT   = 12,
    PLAY_MODE_ARTIST_SEQUENTIAL      = 13,
    PLAY_MODE_ARTIST_REPEAT          = 14,
    PLAY_MODE_ARTIST_SHUFFLE         = 15,
    PLAY_MODE_ARTIST_SHUFFLE_REPEAT  = 16,
    PLAY_MODE_COUNT = 17
} PlayMode;

typedef enum {
    SORT_DEFAULT = 0,
    SORT_TITLE   = 1,
    SORT_ARTIST  = 2,
    SORT_ALBUM   = 3,
    SORT_FILENAME = 4
} SortMode;

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
    VIEW_INFO = 5,
    VIEW_HELP = 6,
    VIEW_LIBRARY = 7
} ViewMode;

typedef enum {
    FOCUS_SIDEBAR = 0,
    FOCUS_CONTENT = 1,
    FOCUS_CATEGORY = 2
} FocusArea;

typedef enum {
    PLAYLIST_MODE_FILE_BROWSER = 0,
    PLAYLIST_MODE_PLAY_QUEUE   = 1
} PlaylistViewMode;

typedef enum {
    POPUP_NONE = 0,
    POPUP_LOOP_MODE,
    POPUP_SPEED,
    POPUP_VOLUME
} PopupType;

typedef struct {
    int active;
    PopupType type;
    int selected_index;
    int option_count;
    int start_row;
    int start_col;
    int width;
    int height;
    void *popup_win;
} PopupState;

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
#define MAX_PATH_LEN 512
#define MAX_TRACKS 1000
#define MAX_TREE_NODES 5000

typedef struct {
    int indices[MAX_TRACKS];
    int count;
    int current_position;
    int shuffle_generation;
} PlayQueue;

#define MAX_META_LEN 256
#define MAX_SEARCH_KEY_LEN (MAX_META_LEN * 8)

/* Library browsing sub-views */
typedef enum {
    LIBRARY_HOME = 0,
    LIBRARY_ARTISTS,
    LIBRARY_ARTIST_ALBUMS,
    LIBRARY_ALBUMS,
    LIBRARY_GENRES,
    LIBRARY_GENRE_TRACKS,
    LIBRARY_ALBUM_TRACKS,
    LIBRARY_ALL_TRACKS,
    LIBRARY_SEARCH_RESULTS
} LibraryViewMode;

/* Library browsing state */
typedef struct {
    int active;
    LibraryViewMode view;
    int selected_index;
    int scroll_offset;
    int item_count;
    int available;
    char filter_artist[MAX_META_LEN];
    char filter_album[MAX_META_LEN];
    char filter_genre[MAX_META_LEN];
} LibraryState;
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

#define AUDIO_BACKEND_AUTO      0
#define AUDIO_BACKEND_PULSE     1
#define AUDIO_BACKEND_ALSA      2
#define AUDIO_BACKEND_PIPEWIRE  3

#define CONFIG_CURRENT_VERSION 4

typedef struct {
    char path[MAX_PATH_LEN];
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
    int cue_offset;          /* seconds offset within the file (0 = not a CUE sub-track) */
    int cue_track_number;    /* CUE track number (0 = not a CUE sub-track) */
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
    int cue_offset;
    int cue_track_number;
} CachedTrack;

/* ── Tree browsing types ── */
typedef enum {
    TREE_NODE_FILE      = 0,
    TREE_NODE_DIRECTORY = 1
} TreeNodeType;

typedef struct {
    TreeNodeType type;
    char name[MAX_META_LEN];          /* display name (file/dir basename) */
    char full_path[MAX_PATH_LEN];     /* absolute path */
    int depth;                        /* indentation level (0 = root) */
    int expanded;                     /* 1 = showing children, 0 = collapsed (directories only) */
    int parent_index;                 /* index into tree_nodes[], -1 for root */
    int track_index;                  /* index into Playlist.tracks[]; -1 for directories */
} TreeNode;

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
    int db_id;                       /* SQLite rowid (0 = not persisted) */
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
    int default_play_mode;
    int advanced_play_modes_enabled;
    float default_playback_speed;
    int show_album_cover;
    int seamless_preload;    // 0=off, 1=pre-decode next track at end of current
    int lyrics_alignment;  // 0=居左(Left), 1=居中(Center), 2=居右(Right)
    int audio_backend;     // 0=Auto, 1=PulseAudio, 2=ALSA, 3=PipeWire
    int sort_mode;         // SortMode value, 0=default (no sort)
    int cue_encoding;      // CUE_ENCODING_* constant (playlist/encoding.h), 0=auto
    int config_version;
    RemoteConnectionConfig remote_connections[MAX_REMOTE_CONNECTIONS];
    int remote_connection_count;
    int eq_enabled;              /* 0/1 — equaliser master switch */
    int eq_preamp;               /* pre-amp gain in dB, -12..12 */
    int eq_band_gains[EQ_BAND_COUNT]; /* per-band gain in dB, -12..12 */
} AppConfig;

typedef struct {
    char tracks[MAX_TRACKS][MAX_PATH_LEN];
    int count;
    char folder_path[MAX_PATH_LEN];
    int is_loaded;
    int has_multiple_sources;
    CachedTrack cache[MAX_CACHE_SIZE];
    int cache_count;
    int cue_offsets[MAX_TRACKS];        /* per-track CUE offset in seconds (0 if not CUE) */
    int cue_track_numbers[MAX_TRACKS];  /* per-track CUE track number (0 if not CUE) */

    /* ── Tree browsing fields ── */
    int tree_mode;                           /* 1 = tree browsing enabled */
    TreeNode tree_nodes[MAX_TREE_NODES];     /* all tree nodes (files + dirs) */
    int tree_node_count;                     /* how many tree_nodes are valid */
    int visible_indices[MAX_TREE_NODES];     /* map visible line → tree_nodes index */
    int visible_count;                       /* how many visible nodes */
} Playlist;

typedef struct {
    int result_indices[MAX_SEARCH_RESULTS];
    int result_count;
    int active;
    int selected_index;
    int result_offset;

    // 异步搜索字段（由 g_search_mutex 保护）
    volatile int in_progress;       // 搜索线程正在运行
    volatile int cancel;            // 设置为 1 以取消搜索
    int progress;                   // 迄今为止已检查的轨道数
    pthread_t thread;               // 搜索线程句柄
    char query[MAX_META_LEN];       // 当前搜索查询
} SearchState;

typedef struct {
    int sorted_indices[MAX_TRACKS];
    int active;
} SortState;

#endif
