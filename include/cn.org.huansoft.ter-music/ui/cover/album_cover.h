/**
 * @file album_cover.h
 * @brief 当前曲目封面：路径、抽取与缓存（声明层）
 *
 * 从旧的 `ui/braille/braille_art.h` 拆出。**实现不在本目录**：这些函数的
 * 定义在 `playlist/playlist.c`（曲库/元数据属前端内容层，封面抽取用 FFmpeg
 * 读内嵌图）。头文件放在 `ui/cover/` 是刻意为之——后端侧
 * （`info/info.c`、`media/rpc_*.c`、`audio/audio.c`、`player/player_local.c`、
 * `cli/daemon.c`）需要"当前曲目的封面"这一**后端职责**的信息，而
 * `scripts/test/check-core-purity.sh` 把后端 include `playlist/` 判为越界、
 * 只豁免本目录；把声明挪到 `playlist/` 会直接踩那条红线。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef ALBUM_COVER_H
#define ALBUM_COVER_H

#include <stddef.h>

/* 当前曲目封面图文件路径与有效性（定义见 playlist/playlist.c）。
 * 512 = MAX_PATH_LEN，与 types.h 一致；此处不 include types.h 以免把
 * 前端类型面拖进只想要封面路径的编译单元。 */
extern char g_current_album_cover_path[512];
extern int g_current_album_cover_valid;

/**
 * @brief 从音频文件抽取内嵌封面到缓存目录（已抽过则复用）。
 * @return 0 成功；-1 无封面或失败
 */
int extract_album_cover(const char *audio_path, char *output_path, size_t output_size);

/** @brief 取当前曲目封面路径。0 成功；-1 无有效封面 */
int get_current_album_cover_path(char *path, size_t path_size);

/** @brief 清空封面抽取缓存（磁盘上的临时封面文件） */
void cleanup_album_cover_cache(void);

/** @brief 换曲后刷新当前曲目封面缓存 */
void update_album_cover_for_track(const char *track_path);

/** @brief 清理 + 复位当前封面路径状态（退出与核心重启路径调用） */
void reset_album_cover_cache(void);

#endif /* ALBUM_COVER_H */
