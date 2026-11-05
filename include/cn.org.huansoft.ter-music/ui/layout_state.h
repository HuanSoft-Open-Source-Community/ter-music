/**
 * @file layout_state.h
 * @brief 前端本地布局选中状态（单行 id 文件）
 *
 * 布局选择属于前端本地偏好，不写核心 config.xml，也不经过 D-Bus。
 * 写入采用临时文件 + rename，避免中途崩溃留下半截状态。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_LAYOUT_STATE_H
#define UI_LAYOUT_STATE_H

#include <stddef.h>

/**
 * @brief 读取状态文件中的布局 id。
 * @return 0 成功；-1 文件不存在/读失败/内容为空。
 */
int layout_state_read(const char *path, char *id, size_t id_size);

/**
 * @brief 原子写入布局 id（追加换行）。
 * @return 0 成功；-1 失败。
 */
int layout_state_write(const char *path, const char *id);

#endif /* UI_LAYOUT_STATE_H */
