#ifndef PLAYLIST_H
#define PLAYLIST_H

#include "types.h"
#include "playlist/cue_parser.h"

/* ── Extern globals ── */
extern Playlist g_playlist;
extern int g_selected_index;
extern SortState g_sort_state;
extern PlaylistManager g_playlist_manager;
extern CueSheet g_cue_sheet;  /* parsed CUE data for the current directory */

/* ── Function prototypes ── */
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
int get_track_metadata(int index, Track *out);
void preload_visible_tracks(int start, int end);
void clear_metadata_cache(void);
void recompute_sort_order(void);
void decode_html_entities(char *str);

/* CUE helpers */
int  cue_get_offset(int track_index);
int  cue_get_track_number(int track_index);
void cue_clear_sheet(void);
int  cue_find_next_offset(int current_index);

/* ── 渲染就绪的一页（D-Bus Playlist.GetPage 与前端共用） ─────────────
 * 行内容即界面渲染所需的最小集合：树模式给出缩进/展开状态，平铺模式
 * 只给曲目；过滤生效时按平铺匹配列表返回（与搜索视图一致）。
 * 注意：单页可能上千行（约 1 KB/行），调用方必须堆分配该数组。 */
#define PLAYLIST_PAGE_MAX 1000

typedef struct {
    int row;                        /* 可见行号（0 基） */
    int type;                       /* 0 = 曲目，1 = 目录 */
    int depth;                      /* 树缩进层级 */
    int expanded;                   /* 目录是否展开（目录行） */
    int tree_index;                 /* 树节点下标，-1 = 非树行 */
    int track_index;                /* 曲目物理下标，-1 = 目录行 */
    int is_cue;                     /* CUE 子轨 */
    char name[MAX_META_LEN];        /* 目录名 / 文件名 */
    char title[MAX_META_LEN];
    char artist[MAX_META_LEN];
    char album[MAX_META_LEN];
} PlaylistRow;

/* 可见行总数（filter 为空时即当前视图行数；非空时为匹配曲目数） */
int playlist_page_total(const char *filter);

/* 取一页；返回实际写入行数，-1 = 参数非法。
 * out 必须至少有 count 个 PlaylistRow 的空间，且 count <= PLAYLIST_PAGE_MAX。 */
int playlist_page(int offset, int count, const char *filter,
                  PlaylistRow *out, int cap);

/* ── 异步加载用的构建/安装分离 ─────────────────────────────────────
 * 目录扫描与远程列举都是阻塞 IO，必须在工作线程里做；但换入全局状态
 * （以及随后的 search_clear / 排序 / 队列重建）只能在媒体循环里做。
 * 因此拆成两步：build 只写调用方持有的结构，install 单点换入。 */

typedef void (*PlaylistBuildProgress)(int processed, int total, void *userdata);

/* 工作线程可安全调用（不触碰任何全局状态，append 时先在锁内取快照）。
 * 失败返回 NULL。 */
Playlist *playlist_build_local(const char *path, int append,
                               PlaylistBuildProgress progress, void *userdata);
Playlist *playlist_build_remote(const RemoteConnectionConfig *conn, const char *subpath);

/* 媒体循环内调用：换入播放列表并完成派生状态更新；随后 free(built)。 */
void playlist_install(Playlist *built);

/* Tree browsing */
int  playlist_tree_is_active(void);
int  playlist_visible_count(void);
int  get_visible_node_tree_index(int visible_idx);
int  get_visible_node_type(int visible_idx);
int  get_visible_node_track_index(int visible_idx);
int  get_tree_node_depth(int tree_idx);
int  get_tree_node_expanded(int tree_idx);   /* 目录节点是否展开 */
const char *get_tree_node_name(int tree_idx);
void playlist_toggle_directory_expand(int tree_idx);
int  playlist_reveal_track(int track_idx);

#endif
