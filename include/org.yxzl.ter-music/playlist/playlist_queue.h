/**
 * @file playlist_queue.h
 * @brief 前端内容列表 → 后端路径队列的装配与下发
 *
 * 前端拥有内容（扫描/元数据/CUE 子轨/歌词来源），后端只执行路径队列；
 * 本模块是两者之间唯一的桥，详见 playlist/playlist_queue.c。
 *
 * 调用时机：**每次内容列表变化**（装载目录、追加目录、加载单文件、清空、
 * 启动恢复）之后。界面自己的列表编辑（插入/删除/上移/下移/清空）走门面
 * `player_queue_*`，不经过本模块。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef PLAYLIST_QUEUE_H
#define PLAYLIST_QUEUE_H

/* 用 g_playlist 的当前内容重建后端队列（分块下发，单块 ≤ BQ_SET_MAX），
 * 并按当前播放模式重建执行顺序、保持当前曲目。
 * @return 实际下发条目数（≥0）；-1 = 失败（内容过大 / 载荷被拒） */
int playlist_queue_sync(void);

/* 分块下发辅助的回调类型。playlist_queue_foreach_chunk 每装配满一块
 * （单块 ≤ BQ_SET_MAX）就回调一次：chunk_json 为 {"entries":[…]} 形态，
 * chunk_index 从 0 计（0 = 首块，调用方据此选择 Set/Append），entries_in_chunk
 * 为本块条目数。回调返回非 0 即中止整表下发。 */
typedef int (*PlaylistQueueChunkCb)(const char *chunk_json, int chunk_index,
                                    int entries_in_chunk, void *user);

/* 按 ≤BQ_SET_MAX 分块遍历 g_playlist 内容并逐块回调，供本地整表同步与远端
 * 整表下发复用同一条装配路径。无路径的行（目录行等）跳过、不进队列。
 * @return 已成功回调下发的条目总数（≥0）；-1 = 出错（含回调失败 / 内存不足）。
 * 内容为空返回 0（不下发任何块，由调用方决定是否清空核心队列）。 */
int playlist_queue_foreach_chunk(PlaylistQueueChunkCb cb, void *user);

/* 把当前内容列表渲染成队列 JSON（{"entries":[...]}}，调用方 free()）。
 * 供“没有后端队列可推”的场景使用：CLI 在自己的进程里装载内容后，
 * 把这个载荷经 D-Bus Queue.Set 交给核心。
 * @return 堆字符串；NULL = 没有可播放条目或内存不足 */
char *playlist_queue_render(void);

/* 单条曲目的队列载荷（{"entries":[…]}}，调用方 free()）。
 * 远端后端在“把某一首加进队列”时用它。 */
char *playlist_queue_render_entry(int index);

/* 只把物理下标 index 的单条曲目下发给后端。
 * insert_after != 0 时插到当前条目之后，否则追加到队尾。
 * @return 写入条目数（0/1）；-1 = 失败 */
int playlist_queue_push_entry(int index, int insert_after);

#endif /* PLAYLIST_QUEUE_H */
