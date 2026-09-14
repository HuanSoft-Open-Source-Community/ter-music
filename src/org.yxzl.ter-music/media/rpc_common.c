/**
 * @file rpc_common.c
 * @brief RPC 接口面的共享实现（分页钳制、方法清单、自省拼装）
 *
 * 本文件不含任何接口专属逻辑：接口处理器分散在 rpc_info.c / rpc_control.c /
 * rpc_lyrics.c / rpc_playlist.c / rpc_queue.c / rpc_library.c / rpc_config.c /
 * rpc_remote.c，共用同一份会话与连接（media/session.c）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "media/rpc.h"

#include "util/json.h"

#include <stdio.h>
#include <string.h>

/* ── 分页钳制 ───────────────────────────────────────────────────── */

int rpc_page_clamp(long long offset, long long count, int *offset_out, int *count_out)
{
    if (offset < 0) {
        offset = 0;
    }

    if (count == 0) {
        count = RPC_PAGE_DEFAULT;
    } else if (count < 0 || count > RPC_PAGE_MAX) {
        return -1;      /* 显式越界：调用方应回 Error.InvalidArgs */
    }

    if (offset > 2147483647LL) {
        offset = 2147483647LL;
    }

    if (offset_out) {
        *offset_out = (int)offset;
    }
    if (count_out) {
        *count_out = (int)count;
    }
    return 0;
}

/* ── 方法清单（握手） ───────────────────────────────────────────────
 *
 * 前端在接入时用 Info.GetInfo 的 core.methods 判断核心是否具备它要用的
 * 方法。表与实际发布的方法必须一致——scripts/test/dbus-rpc-check.sh 会把
 * 这份清单与自省 XML 对照，任何一边漏改都会被测出来。 */

static const char *const k_rpc_methods[] = {
    /* Lyrics */
    "Lyrics.GetLyrics",
    /* Info */
    "Info.GetInfo",
    "Info.GetTrackInfo",
    "Info.GetProgress",
    "Info.GetLyricsLines",
    "Info.GetInstanceInfo",
    "Info.InstanceInfo",
    "Info.GetCoverArt",
    "Info.GetDisplay",
    /* Control */
    "Control.Play",
    "Control.Pause",
    "Control.PlayPause",
    "Control.Stop",
    "Control.Next",
    "Control.Previous",
    "Control.SeekTo",
    "Control.SeekBy",
    "Control.SetVolume",
    "Control.GetVolume",
    "Control.SetSpeed",
    "Control.GetSpeed",
    "Control.SetPlayMode",
    "Control.GetPlayMode",
    "Control.GetPlayModeName",
    "Control.OpenPath",
    "Control.PlayIndex",
    "Control.GetPlaylist",
    "Control.ReloadConfig",
    "Control.Quit",
    NULL
};

int rpc_method_count(void)
{
    int count = 0;
    while (k_rpc_methods[count] != NULL) {
        count++;
    }
    return count;
}

const char *rpc_method_at(int index)
{
    if (index < 0 || index >= rpc_method_count()) {
        return NULL;
    }
    return k_rpc_methods[index];
}

/* 生成 core.methods 数组文本："Info.GetInfo" 里的点号原样保留，
 * 前端只做字符串匹配，不做接口/方法拆解。 */
const char *rpc_methods_json(void)
{
    static char buffer[RPC_PAYLOAD_MAX / 16];
    size_t pos = 0;

    pos = json_append_char(buffer, sizeof(buffer), pos, '[');
    for (int i = 0; k_rpc_methods[i] != NULL; i++) {
        if (i > 0) {
            pos = json_append_char(buffer, sizeof(buffer), pos, ',');
        }
        pos = json_append_escaped(buffer, sizeof(buffer), pos, k_rpc_methods[i]);
    }
    pos = json_append_char(buffer, sizeof(buffer), pos, ']');
    buffer[pos] = '\0';
    return buffer;
}
