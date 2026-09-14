/**
 * @file config_xml.h
 * @brief XML config serialization API
 *
 * @author ter-music team
 * @date 2026-06-01
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Extern globals ── */
extern AppConfig g_app_config;

/* SIGHUP hot-reload flag; set by signal handler, checked in event loop */
extern volatile sig_atomic_t g_config_reload_requested;

/* ── 应用目录解析（遵循 XDG，兼容容器/沙箱重定向） ─────────────────
 *
 * 解析顺序（与 XDG Base Directory 规范及 Linyaps 打包契约一致）：
 *   $XDG_CONFIG_HOME/ter-music  ← 回退 $HOME/.config/ter-music
 *   $XDG_DATA_HOME/ter-music    ← 回退 $HOME/.local/share/ter-music
 *   $XDG_CACHE_HOME/ter-music   ← 回退 $HOME/.cache/ter-music
 *
 * Linyaps（如意玲珑）会在容器内把这些变量重定向到宿主机
 * ~/.linglong/<appid>/…（见其 FAQ「应用数据保存到哪里」），因此必须按
 * 环境变量取值，而不能硬编码 ~/.config。
 *
 * @param ensure 非 0 时递归创建目录（含缺失的父目录）
 * @return 静态缓冲中的路径；无法确定时返回 NULL
 */
const char *app_config_dir(int ensure);
const char *app_data_dir(int ensure);
const char *app_cache_dir(int ensure);

/**
 * Validate an XML config file against the expected schema.
 * @param path  Path to config.xml
 * @return 0 on success (schema matches), -1 on error
 */
int config_validate_xml(const char *path);

/**
 * Serialize AppConfig to an XML file.
 * @param path  Output path (e.g. "config.xml")
 * @param cfg   The configuration to serialize
 * @return 0 on success, -1 on error
 */
int config_save_to_xml(const char *path, const AppConfig *cfg);

/**
 * Deserialize AppConfig from an XML file.
 * Overlays parsed values into cfg (caller should memset or init_default first).
 * @param path  Path to config.xml
 * @param cfg   Output structure (will be partly overwritten)
 * @return 0 on success, -1 on error
 */
int config_load_from_xml(const char *path, AppConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
