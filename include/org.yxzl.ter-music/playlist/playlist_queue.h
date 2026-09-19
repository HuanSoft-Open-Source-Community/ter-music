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

/* 把当前内容列表渲染成队列 JSON（{"entries":[...]}}，调用方 free()）。
 * 供“没有后端队列可推”的场景使用：CLI 在自己的进程里装载内容后，
 * 把这个载荷经 D-Bus Queue.Set 交给核心。
 * @return 堆字符串；NULL = 没有可播放条目或内存不足 */
char *playlist_queue_render(void);

/* 只把物理下标 index 的单条曲目下发给后端。
 * insert_after != 0 时插到当前条目之后，否则追加到队尾。
 * @return 写入条目数（0/1）；-1 = 失败 */
int playlist_queue_push_entry(int index, int insert_after);

#endif /* PLAYLIST_QUEUE_H */
