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

/* 配置差异 JSON 补丁的推荐缓冲大小：需容纳全部键位覆盖 + 其他分区。 */
#define CONFIG_DIFF_BUFFER_SIZE 65536

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

/* ── 配置归属（原位于 ui/menus.c，属核心职责）─────────────────────────
 * 迁移原因：audio/、cli/、main/ 等非界面模块此前必须包含界面头文件
 * 才能读写配置，形成“核心反向依赖界面”。 */
void ensure_config_dir_exists(void);   /* 解析并缓存配置路径（遵循 XDG） */
const char *get_config_dir(void);      /* 已解析的配置目录，未初始化时返回 NULL */
const char *get_config_file_path(void);/* 已解析的 config.xml 完整路径，未初始化时返回 NULL */
void init_default_config(void);        /* 全量默认值（不读文件） */
void load_config(void);                /* 默认值 + XML 读取（不改运行时全局） */

/**
 * 只读解析 XML 中的 <keybindings><binding action="..." key="..."/></keybindings>。
 *
 * 严格按文件顺序原样拷贝：不校验 action/key、不去重、不做冲突降级，
 * 供 `ter-music keys check` 发现被运行时加载降级掉的非法项。
 * 不触发版本迁移，也不写盘。
 *
 * @param path       config.xml 路径（可为 get_config_file_path()）
 * @param out        输出数组（max 条）
 * @param max        输出容量（通常为 MAX_KEYBINDING_OVERRIDES）
 * @param count_out  实际读取条数（成功时写入）
 * @param err        失败原因（可为 NULL）
 * @param err_size   err 缓冲大小
 * @return 0 成功（含文件不存在/无该段，此时 count_out=0）；
 *         -1 解析失败、条目超过 max 或参数非法
 */
int config_load_raw_keybindings(const char *path, KeyBindingOverride *out, int max,
                                int *count_out, char *err, size_t err_size);

/* 版本迁移：把 config.xml 推进到 CONFIG_CURRENT_VERSION（含 v5→v6 的远程段
 * 移交、v6→v7 与 v7→v8 的新键落盘）。config.xml 归核心独占写，因此只有核心调用它。 */
int config_run_migrations(void);
void save_config(void);                /* 原子写入配置文件 */

/* 注：配置重载标志 g_config_reload_requested 现由 core/core.h 声明
 * （进程级状态，两个主循环共用）。 */

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
