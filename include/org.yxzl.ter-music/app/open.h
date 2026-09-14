/**
 * @file open.h
 * @brief 路径打开与播放会话恢复的共用原语
 *
 * 原实现位于 main/main.c（TUI 专用），抽取后由 TUI 启动、
 * CLI 后台 daemon 启动以及 D-Bus Control.OpenPath 三处共用，
 * 避免同一套 “路径 → 播放列表” 逻辑出现三份实现。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef APP_OPEN_H
#define APP_OPEN_H

#include "types.h"

typedef enum {
    APP_OPEN_OK = 0,
    APP_OPEN_ERR_EMPTY,          /* 路径为空 */
    APP_OPEN_ERR_INVALID_PATH,   /* stat 失败，或既非普通文件也非目录 */
    APP_OPEN_ERR_FILE_LOAD,      /* 普通文件：所在目录加载失败或文件不在列表中 */
    APP_OPEN_ERR_NO_AUDIO        /* 目录：无可播放文件 */
} AppOpenResult;

/**
 * 打开一个本地路径（目录或音频文件）并装载播放列表。
 * 路径支持 `~` 前缀展开；普通文件会加载其所在目录并定位到该文件。
 *
 * @param path              输入路径
 * @param final_path        输出实际加载的目录/路径，可为 NULL
 * @param final_path_size   final_path 容量
 * @param track_index_out   输出目标曲目的物理索引（目录时为 0），可为 NULL
 * @param is_single_file_out 输出是否为“打开单个文件”场景，可为 NULL
 */
AppOpenResult app_open_path(const char *path,
                            char *final_path,
                            size_t final_path_size,
                            int *track_index_out,
                            int *is_single_file_out);

/**
 * 无显式路径时的启动链（daemon / 非 TUI 场景）：
 *   1) 恢复 temp playlist（必要时把树根切回 last_opened_path）
 *   2) default_startup_path
 *   3) remember_last_path 记录的 last_opened_path
 * 成功装载后载入播放队列，并在 honor_autoplay 为真时按
 * resume_last_playback / auto_play_on_start 决定是否开始播放。
 *
 * @return 装载后的曲目数量（0 表示没有任何可恢复内容）
 */
int app_restore_session(int honor_autoplay,
                        char *final_path,
                        size_t final_path_size);

/**
 * 恢复上次播放的曲目与位置（读取 g_app_config 中持久化的会话）。
 * @return 1 已开始播放，0 未恢复
 */
int app_resume_saved_playback(void);

/* 清除持久化的播放会话（resume_last_playback / 位置 / 曲目路径）并保存配置 */
void app_clear_saved_session(void);

/* 把曲目物理索引映射为当前列表显示所用的行索引并写入 g_selected_index */
void app_set_selection_for_track(int physical_idx);

/* 目录内是否存在（直接子文件的）音频文件 */
int app_dir_has_audio_files(const char *path);

#endif /* APP_OPEN_H */
