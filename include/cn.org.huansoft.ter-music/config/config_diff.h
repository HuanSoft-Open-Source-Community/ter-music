/**
 * @file config_diff.h
 * @brief 把两份 AppConfig 的差异渲染成最小 JSON 补丁
 *
 * 用途：前端（TUI/CLI）不拥有配置文件——`config.xml` 由核心独占写。前端改动
 * 设置后，把"相对上次已知核心配置的差异"经 `Config.Set` 下发，核心落盘并
 * 应用到运行时，再把 `ConfigChanged` 广播回来。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef CONFIG_DIFF_H
#define CONFIG_DIFF_H

#include <stddef.h>

#include "types.h"

/* 渲染 before → after 的差异（只含变化过的键；无变化时为 `{}`）。
 * 分区顺序与 config.xml / Config.GetAll 一致：paths / theme / preferences /
 * equalizer。返回值 = 写入字节数（`{}` 时为 2）；out 始终 NUL 结尾。 */
size_t config_diff_json(const AppConfig *before, const AppConfig *after,
                        char *out, size_t out_size);

#endif /* CONFIG_DIFF_H */
