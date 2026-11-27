/**
 * @file stub_i18n.c
 * @brief 单测用 i18n 桩：i18n_get() 原样返回键名
 *
 * karaoke_cache.c / karaoke_worker.c 的错误文案走 i18n key（S3）。
 * 这两个模块可脱离 libxml2/语言包被纯 C 单测直接编译；本桩只满足链接，
 * 单测从不断言错误文案内容（只断言 err 非空/行为），因此返回键名即可。
 * 生产构建链接真实 src/.../i18n/i18n.c，不使用本文件。
 */

#include "i18n/i18n.h"

const char *i18n_get(const char *key)
{
    return (key && key[0]) ? key : "";
}
