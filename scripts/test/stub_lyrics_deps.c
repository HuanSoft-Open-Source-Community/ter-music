/*
 * 歌词解析单测的链接桩：把引擎依赖的外部模块替换成最小实现。
 *
 * 引擎（lyrics/lyrics.c）依赖：
 *   - audio_get_position_seconds()：播放位置（推进的输入），本桩可控；
 *   - i18n_get()：仅用于“纯音乐”占位文本；
 *   - decode_html_entities()：内容层的文本工具；
 *   - 日志：见 stub_logger.c。
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* 引擎默认的播放位置来源是音频层；单测里没有音频栈，故给一个返回 0 的
 * 替代（真正要驱动推进时用 lyrics_set_position_source() 注入）。 */
int audio_get_position_seconds(void)
{
    return 0;
}

const char *i18n_get(const char *key)
{
    return key ? key : "";
}

/* 与 playlist/playlist.c 的实现语义一致的最小 HTML 实体解码 */
void decode_html_entities(char *str);

/* ctype-free 简化版：只处理断言用到的 &amp; &lt; &gt; &quot; &#39; */
void decode_html_entities(char *str)
{
    if (!str) {
        return;
    }
    struct {
        const char *entity;
        const char *replacement;
    } table[] = {
        { "&amp;", "&" },
        { "&lt;", "<" },
        { "&gt;", ">" },
        { "&quot;", "\"" },
        { "&#39;", "'" },
        { "&apos;", "'" },
    };

    for (size_t t = 0; t < sizeof(table) / sizeof(table[0]); t++) {
        size_t entity_len = strlen(table[t].entity);
        char *hit = strstr(str, table[t].entity);
        while (hit) {
            size_t tail = strlen(hit + entity_len);
            size_t replacement_len = strlen(table[t].replacement);
            memcpy(hit, table[t].replacement, replacement_len);
            memmove(hit + replacement_len, hit + entity_len, tail + 1);
            hit = strstr(hit + replacement_len, table[t].entity);
        }
    }
}
