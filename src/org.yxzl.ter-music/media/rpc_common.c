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
#include "media/session.h"

#include "app/open.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "cli/cli.h"
#include "config/config.h"
#include "info/info.h"
#include "logger/logger.h"
#include "playlist/playlist.h"
#include "remote/remote.h"
#include "ui/braille/braille_art.h"
#include "ui/menus.h"
#include "ui/ui.h"
#include "util/json.h"

#include <math.h>
#include <string.h>
#include <unistd.h>

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

/* ============================================================
 * 共享实现：会话发送/错误/回复、引擎动作、快照采集
 *
 * 同时被 MPRIS 处理器（session.c）与 org.yxzl.ter_music.* 各接口处理器
 * （rpc_*.c）使用，故集中在此；实现与原 session.c 逐字相同。
 * ============================================================ */

const char *rpc_playback_status_name(PlayState state) {
    switch (state) {
        case PLAY_STATE_PLAYING:
            return "Playing";
        case PLAY_STATE_PAUSED:
            return "Paused";
        case PLAY_STATE_STOPPED:
        default:
            return "Stopped";
    }
}

void rpc_send(DBusMessage *message) {
    if (!message) {
        return;
    }

    DBusConnection *connection = rpc_session_connection();
    if (connection) {
        dbus_connection_send(connection, message, NULL);
        dbus_connection_flush(connection);
    }
    dbus_message_unref(message);
}

DBusMessage *rpc_error(DBusMessage *message,
                                        const char *error_name,
                                        const char *text) {
    return dbus_message_new_error(message, error_name, text);
}

int rpc_track_available(void) {
    return g_current_play_index >= 0 && g_current_play_index < playlist_count();
}

void rpc_capture_snapshot(RpcPlaybackSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->current_index = g_current_play_index;
    snapshot->playlist_total = playlist_count();
    snapshot->play_state = g_play_state;
    snapshot->loop_mode = g_play_mode;
    snapshot->volume_percent = get_volume_percent();
    snapshot->position_us = (int64_t)g_current_position * 1000000LL;
    snapshot->length_us = (int64_t)g_total_duration * 1000000LL;
    snapshot->can_seek = rpc_track_available() && g_total_duration > 0;

    if (!rpc_track_available()) {
        return;
    }

    char track_path[MAX_PATH_LEN];
    if (playlist_get_track_path(g_current_play_index, track_path, sizeof(track_path)) != 0) {
        return;
    }

    Track track;
    if (get_track_metadata(g_current_play_index, &track) != 0) {
        return;
    }

    snapshot->valid = 1;
    info_build_track_id(snapshot->track_id, sizeof(snapshot->track_id), track_path);
    snprintf(snapshot->title, sizeof(snapshot->title), "%s", track.title);
    snprintf(snapshot->artist, sizeof(snapshot->artist), "%s", track.artist);
    snprintf(snapshot->album, sizeof(snapshot->album), "%s", track.album);

    char cover_path[MAX_PATH_LEN];
    if (get_current_album_cover_path(cover_path, sizeof(cover_path)) == 0) {
        info_build_file_uri(cover_path, snapshot->art_url, sizeof(snapshot->art_url));
    }
}

int rpc_action_play_selected(void) {
    int playlist_total = playlist_count();

    if (!playlist_is_loaded() || playlist_total <= 0) {
        return 0;
    }

    int target_index = (g_current_play_index >= 0)
        ? g_current_play_index
        : (g_sort_state.active ? g_sort_state.sorted_indices[g_selected_index] : g_selected_index);
    if (target_index >= 0 && target_index < playlist_total) {
        play_audio(target_index);
        return 1;
    }
    return 0;
}

int rpc_action_play(void) {
    if (g_play_state == PLAY_STATE_PAUSED) {
        resume_audio();
        return 1;
    }
    if (g_play_state != PLAY_STATE_PLAYING) {
        return rpc_action_play_selected();
    }
    return 1;
}

int rpc_action_play_pause(void) {
    if (g_play_state == PLAY_STATE_PLAYING) {
        pause_audio();
        return 1;
    }
    if (g_play_state == PLAY_STATE_PAUSED) {
        resume_audio();
        return 1;
    }
    return rpc_action_play_selected();
}

int rpc_action_seek_to_us(int64_t position_us) {
    if (!rpc_track_available()) {
        return 0;
    }
    if (position_us < 0) {
        position_us = 0;
    }
    int64_t length_us = (int64_t)audio_get_duration_seconds() * 1000000LL;
    if (length_us > 0 && position_us > length_us) {
        position_us = length_us;
    }
    seek_audio((double)position_us / 1000000.0);
    return 1;
}

int rpc_action_seek_by_us(int64_t delta_us) {
    int64_t position_us = (int64_t)audio_get_position_seconds() * 1000000LL;
    return rpc_action_seek_to_us(position_us + delta_us);
}

int rpc_action_set_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    set_volume_percent(percent);
    return 1;
}

int rpc_action_set_speed(double rate) {
    if (!(rate >= 0.5 && rate <= 3.0)) {
        return 0;
    }
    g_playback_speed = (float)rate;
    g_app_config.default_playback_speed = g_playback_speed;
    save_config();
    apply_playback_speed_change();
    return 1;
}

int rpc_action_play_index(int index) {
    int total = playlist_count();
    if (index < 0 || index >= total) {
        return 0;
    }
    play_audio(index);
    app_set_selection_for_track(index);
    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
    return 1;
}

/* 打开本地路径 / 远程 URL（MPRIS OpenUri 与 Control.OpenPath 共用） */
int rpc_action_open_path(const char *path, int autoplay) {
    if (!path || path[0] == '\0') {
        return 0;
    }

    char local_path[MAX_PATH_LEN];
    if (info_uri_to_path(path, local_path, sizeof(local_path)) != 0) {
        return 0;
    }
    if (local_path[0] == '\0') {
        return 0;
    }

    if (remote_is_remote_path(local_path)) {
        RemoteConnectionConfig connection;
        if (remote_parse_url(local_path, &connection) != 0) {
            return 0;
        }
        if (load_remote_playlist(&connection, connection.base_path) <= 0) {
            return 0;
        }
        g_selected_index = 0;
    } else if (app_open_path(local_path, NULL, 0, NULL, NULL) != APP_OPEN_OK) {
        return 0;
    }

    /* 新播放列表：清空旧队列，交由 play_audio() 按当前模式重建 */
    play_queue_clear(&g_play_queue);

    if (autoplay && playlist_count() > 0) {
        int index = (g_current_play_index >= 0 && g_current_play_index < playlist_count())
            ? g_current_play_index : 0;
        play_audio(index);
        app_set_selection_for_track(index);
    }

    request_ui_refresh(UI_DIRTY_PLAYLIST | UI_DIRTY_CONTROLS | UI_DIRTY_LYRICS);
    return 1;
}

InfoInstance rpc_instance_info(void) {
    InfoInstance instance;
    memset(&instance, 0, sizeof(instance));
    instance.is_daemon = g_daemon_mode;
    instance.pid = (int)getpid();
    instance.version = APP_VERSION;
    instance.bus_name = rpc_session_bus_name();
    instance.has_primary_name = rpc_session_has_primary_name();
    return instance;
}

DBusMessage *rpc_reply_bool(DBusMessage *message, int ok) {
    DBusMessage *reply = dbus_message_new_method_return(message);
    if (!reply) {
        return NULL;
    }
    dbus_bool_t value = ok ? TRUE : FALSE;
    dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &value, DBUS_TYPE_INVALID);
    return reply;
}

DBusMessage *rpc_reply_int(DBusMessage *message, int value) {
    DBusMessage *reply = dbus_message_new_method_return(message);
    if (!reply) {
        return NULL;
    }
    dbus_int32_t out = (dbus_int32_t)value;
    dbus_message_append_args(reply, DBUS_TYPE_INT32, &out, DBUS_TYPE_INVALID);
    return reply;
}

DBusMessage *rpc_reply_string(DBusMessage *message, const char *value) {
    DBusMessage *reply = dbus_message_new_method_return(message);
    if (!reply) {
        return NULL;
    }
    const char *safe = value ? value : "";
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &safe, DBUS_TYPE_INVALID);
    return reply;
}

/* ── core 对象（Info.GetInfo 的 "core" 字段） ───────────────────────
 * 前端接入时用它判断核心是否兼容：api_version 不满足即拒绝接入。 */

const char *rpc_core_json(void)
{
    static char buffer[4096];
    size_t pos = 0;

    pos = json_append_char(buffer, sizeof(buffer), pos, '{');
    pos = json_append_key(buffer, sizeof(buffer), pos, "api_version");
    pos = json_append_int(buffer, sizeof(buffer), pos, TER_MUSIC_API_VERSION);
    pos = json_append_raw(buffer, sizeof(buffer), pos, ",");
    pos = json_append_key(buffer, sizeof(buffer), pos, "payload_max");
    pos = json_append_int(buffer, sizeof(buffer), pos, RPC_PAYLOAD_MAX);
    pos = json_append_raw(buffer, sizeof(buffer), pos, ",");
    pos = json_append_key(buffer, sizeof(buffer), pos, "page_default");
    pos = json_append_int(buffer, sizeof(buffer), pos, RPC_PAGE_DEFAULT);
    pos = json_append_raw(buffer, sizeof(buffer), pos, ",");
    pos = json_append_key(buffer, sizeof(buffer), pos, "page_max");
    pos = json_append_int(buffer, sizeof(buffer), pos, RPC_PAGE_MAX);
    pos = json_append_raw(buffer, sizeof(buffer), pos, ",");
    pos = json_append_key(buffer, sizeof(buffer), pos, "methods");
    pos = json_append_raw(buffer, sizeof(buffer), pos, rpc_methods_json());
    pos = json_append_char(buffer, sizeof(buffer), pos, '}');
    buffer[pos] = '\0';

    return buffer;
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
    "Info.InstanceInfo",
    "Info.GetCoverArt",
    "Info.GetDisplay",
    "Info.GetVisualizer",
    "Info.GetStatus",
    /* Control */
    "Control.Attach",
    "Control.Ping",
    "Control.Detach",
    "Control.FrontendInfo",
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
    /* Playlist */
    "Playlist.Load",
    "Playlist.Append",
    "Playlist.Clear",
    "Playlist.Sort",
    "Playlist.SetFilter",
    "Playlist.Search",
    "Playlist.GetTree",
    "Playlist.GetPage",
    "Playlist.ToggleExpand",
    "Playlist.RevealIndex",
    "Playlist.Status",
    /* Queue */
    "Queue.Get",
    "Queue.Append",
    "Queue.InsertAfter",
    "Queue.RemoveAt",
    "Queue.MoveUp",
    "Queue.MoveDown",
    "Queue.Clear",
    "Queue.Rebuild",
    "Queue.Shuffle",
    "Queue.PlayAt",
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
