/**
 * @file config_json.h
 * @brief AppConfig ⇄ JSON 编解码（D-Bus Config 接口）
 *
 * 键名与 config/schema.h 的 XML 元素名一致，分区与 config.xml 相同。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef CONFIG_JSON_H
#define CONFIG_JSON_H

#include <stddef.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 渲染完整配置为 JSON；返回写入字节数，-1 表示参数错误。 */
int config_render_json(const AppConfig *cfg, char *out, size_t out_size);

/* 应用局部 JSON 补丁（键名同 config.xml）。
 * 原子：任何未知分区/未知键/类型错误都会整体失败，g_app_config 不被修改。
 * error_out 收到失败原因（可为 NULL）。返回 0 成功，-1 失败。 */
int config_apply_json(const char *patch_json, char *error_out, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_JSON_H */
