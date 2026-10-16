/**
 * @file backend_queue.c
 * @brief 后端播放队列实现（路径队列 + 执行顺序 + 游标）
 *
 * 设计要点：
 *  - 条目数组用堆分配（`entries` + `capacity`），避免巨型静态结构；
 *  - 数组顺序即**执行顺序**：`bq_apply_mode()` 按播放模式重建顺序并保持当前
 *    条目；`bq_advance()/bq_rewind()` 只动游标（语义与
 *    `audio/play_queue.c` 的 `play_queue_advance/rewind` 一致）；
 *  - 路径合法性由 D-Bus 边界层负责（核心只接受本地路径），本模块不做校验；
 *  - 媒体循环（`Queue.*`）与播放线程（自动续播、预取）都会碰它，故全部共享
 *    状态都在 `g_bq_mutex` 下读写；公开函数互调一律走 `*_locked` 内部版本，
 *    避免同一线程二次加锁。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "queue/backend_queue.h"

#include "audio/play_mode_util.h"
#include "logger/logger.h"
#include "util/json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    BackendQueueEntry *entries;
    int count;
    int capacity;
    int position;                 /* 游标，-1 = 无 */
    unsigned long long revision;
    int shuffle_generation;
    /* Set 契约（见头文件）：整表替换前把原游标条目另存于此，播放线程的下标
     * 因此越界时上层仍能如实上报正在出声的曲目 */
    int pinned_valid;
    BackendQueueEntry pinned;
} BackendQueue;

static BackendQueue g_bq;
static int g_bq_ready = 0;

/* 媒体循环与播放线程共用队列，所有字段都在这把锁下读写 */
static pthread_mutex_t g_bq_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── 基础 ─────────────────────────────────────────────────────────── */

static void bq_init_locked(void)
{
    memset(&g_bq, 0, sizeof(g_bq));
    g_bq.position = -1;
    g_bq.revision = 1;
    g_bq_ready = 1;
}

void bq_init(void)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_init_locked();
    pthread_mutex_unlock(&g_bq_mutex);
}

void bq_shutdown(void)
{
    pthread_mutex_lock(&g_bq_mutex);
    free(g_bq.entries);
    memset(&g_bq, 0, sizeof(g_bq));
    g_bq.position = -1;
    g_bq_ready = 0;
    pthread_mutex_unlock(&g_bq_mutex);
}

/* 惰性初始化：调用方（核心启动路径、单测、queue.txt 恢复）不必记住先调
 * bq_init()，首次触碰队列入口就把状态置成“空队列”（position = -1）。
 * 只在持锁时调用。 */
static void bq_ensure_ready_locked(void)
{
    if (!g_bq_ready) {
        bq_init_locked();
    }
}

int bq_path_is_local(const char *path)
{
    if (!path || path[0] == '\0') {
        return 0;
    }
    const char *scheme = strstr(path, "://");
    if (!scheme) {
        return 1;                       /* 普通文件系统路径 */
    }
    return strncmp(path, "file://", 7) == 0;
}

int bq_count(void)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    int count = g_bq.count;
    pthread_mutex_unlock(&g_bq_mutex);
    return count;
}

int bq_position(void)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    int position = g_bq.position;
    pthread_mutex_unlock(&g_bq_mutex);
    return position;
}

unsigned long long bq_revision(void)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    unsigned long long revision = g_bq.revision;
    pthread_mutex_unlock(&g_bq_mutex);
    return revision;
}

/* 只在持锁时调用：条目内容、顺序或游标的任何变化都要递增修订号，
 * 媒体循环（media/rpc_queue.c 的 rpc_queue_sync）据此补发 QueueChanged。 */
static void bq_bump_revision_locked(void)
{
    bq_ensure_ready_locked();
    g_bq.revision++;
}

void bq_bump_revision(void)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_bump_revision_locked();
    pthread_mutex_unlock(&g_bq_mutex);
}

static int bq_reserve_locked(int wanted)
{
    bq_ensure_ready_locked();
    if (wanted <= g_bq.capacity) {
        return 0;
    }
    int next = g_bq.capacity > 0 ? g_bq.capacity : 64;
    while (next < wanted) {
        next *= 2;
    }
    BackendQueueEntry *grown = realloc(g_bq.entries, (size_t)next * sizeof(*grown));
    if (!grown) {
        log_error("bq", "out of memory growing queue to %d entries", next);
        return -1;
    }
    g_bq.entries = grown;
    g_bq.capacity = next;
    return 0;
}

/* 把当前游标条目另存为“在播条目”；游标无效时清除该记录 */
static void bq_pin_current_locked(void)
{
    if (g_bq.position >= 0 && g_bq.position < g_bq.count) {
        g_bq.pinned = g_bq.entries[g_bq.position];
        g_bq.pinned_valid = 1;
    } else {
        g_bq.pinned_valid = 0;
        memset(&g_bq.pinned, 0, sizeof(g_bq.pinned));
    }
}

int bq_pinned_entry(BackendQueueEntry *out)
{
    if (!out) {
        return -1;
    }
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    int have = g_bq.pinned_valid;
    if (have) {
        *out = g_bq.pinned;
    }
    pthread_mutex_unlock(&g_bq_mutex);
    return have ? 0 : -1;
}

static void bq_clear_locked_soft(void)
{
    g_bq.count = 0;
    g_bq.position = -1;
}

int bq_clear(void)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    /* 清空前记住在播条目：正在出声的曲目不因队列表被清空而“消失” */
    bq_pin_current_locked();
    bq_clear_locked_soft();
    bq_bump_revision_locked();
    pthread_mutex_unlock(&g_bq_mutex);
    return 0;
}

static int bq_entry_at_locked(int position, BackendQueueEntry *out)
{
    bq_ensure_ready_locked();
    if (!out || position < 0 || position >= g_bq.count) {
        return -1;
    }
    *out = g_bq.entries[position];
    return 0;
}

int bq_entry_at(int position, BackendQueueEntry *out)
{
    pthread_mutex_lock(&g_bq_mutex);
    int rc = bq_entry_at_locked(position, out);
    pthread_mutex_unlock(&g_bq_mutex);
    return rc;
}

static int bq_current_locked(BackendQueueEntry *out)
{
    bq_ensure_ready_locked();
    if (g_bq.position < 0) {
        return -1;
    }
    return bq_entry_at_locked(g_bq.position, out);
}

int bq_current(BackendQueueEntry *out)
{
    pthread_mutex_lock(&g_bq_mutex);
    int rc = bq_current_locked(out);
    pthread_mutex_unlock(&g_bq_mutex);
    return rc;
}

int bq_path_at(int position, char *out, size_t size)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    int rc = -1;
    if (out && size > 0 && position >= 0 && position < g_bq.count) {
        snprintf(out, size, "%s", g_bq.entries[position].path);
        rc = 0;
    }
    pthread_mutex_unlock(&g_bq_mutex);
    return rc;
}

static int bq_position_of_path_locked(const char *path)
{
    bq_ensure_ready_locked();
    if (!path || path[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < g_bq.count; i++) {
        if (strcmp(g_bq.entries[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

int bq_position_of_path(const char *path)
{
    pthread_mutex_lock(&g_bq_mutex);
    int position = bq_position_of_path_locked(path);
    pthread_mutex_unlock(&g_bq_mutex);
    return position;
}

int bq_set_position(int position)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    int rc = -1;
    if (position >= -1 && position < g_bq.count) {
        g_bq.position = position;
        /* 游标就是“将要出声的条目”：在播记录随之刷新 */
        bq_pin_current_locked();
        bq_bump_revision_locked();
        rc = 0;
    }
    pthread_mutex_unlock(&g_bq_mutex);
    return rc;
}

int bq_play_at(int position)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    int rc = -1;
    if (position >= 0 && position < g_bq.count) {
        g_bq.position = position;
        bq_pin_current_locked();
        bq_bump_revision_locked();
        rc = 0;
    }
    pthread_mutex_unlock(&g_bq_mutex);
    return rc;
}

/* ── JSON 解析（前端下发） ────────────────────────────────────────── */

static void entry_from_json(JsonReader *reader, BackendQueueEntry *entry)
{
    memset(entry, 0, sizeof(*entry));

    JsonValue value;
    if (json_get_path(reader, "path", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, entry->path, sizeof(entry->path));
    }
    if (json_get_path(reader, "title", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, entry->title, sizeof(entry->title));
    }
    if (json_get_path(reader, "artist", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, entry->artist, sizeof(entry->artist));
    }
    if (json_get_path(reader, "album", &value) == 0 && value.type == JSON_VALUE_STRING) {
        json_value_string(&value, entry->album, sizeof(entry->album));
    }
    if (json_get_path(reader, "duration_seconds", &value) == 0) {
        int duration = (int)json_value_int(&value, 0);
        entry->duration_seconds = duration > 0 ? duration : 0;
    }
    if (json_get_path(reader, "cue_offset", &value) == 0) {
        int offset = (int)json_value_int(&value, 0);
        entry->cue_offset = offset > 0 ? offset : 0;
    }
    if (json_get_path(reader, "cue_track_number", &value) == 0) {
        int number = (int)json_value_int(&value, 0);
        entry->cue_track_number = number > 0 ? number : 0;
    }
    if (json_get_path(reader, "is_cue", &value) == 0) {
        entry->is_cue = json_value_bool(&value, 0) ? 1 : 0;
    }
    if (json_get_path(reader, "lyrics_source", &value) == 0) {
        int source = (int)json_value_int(&value, LYRICS_SOURCE_AUTO);
        entry->lyrics_source = source;
    }
}

/* 解析载荷里的条目数组：既接受 {"entries":[...]} 也接受裸数组。
 * 整表解析完再判超限：**绝不**在 BQ_SET_MAX 处早退（旧实现在那里静默截断，
 * 前端把 600 条单发过来时只收下 500 条却报告成功）。
 * @return 条目数（>=0）；BQ_ERR_INVALID = 无法解析；
 *         BQ_ERR_TOO_MANY = 单发条目数超过 BQ_SET_MAX */
static int parse_entries(const char *json, BackendQueueEntry **out, char *err, size_t err_size)
{
    *out = NULL;
    if (err && err_size) err[0] = '\0';
    if (!json || json[0] == '\0') {
        if (err && err_size) snprintf(err, err_size, "empty payload");
        return BQ_ERR_INVALID;
    }

    JsonReader reader;
    JsonValue array;
    json_reader_init(&reader, json, strlen(json));

    if (json_get_path(&reader, "entries", &array) != 0 || array.type != JSON_VALUE_ARRAY) {
        /* 裸数组：容器只需类型与原始范围 */
        JsonValue root = { JSON_VALUE_ARRAY, 0, 0.0, json, strlen(json) };
        json_reader_init(&reader, json, strlen(json));
        if (json_reader_enter(&reader, &root) != 0) {
            if (err && err_size) snprintf(err, err_size, "expected an entry array");
            return BQ_ERR_INVALID;
        }
    } else if (json_reader_enter(&reader, &array) != 0) {
        if (err && err_size) snprintf(err, err_size, "cannot enter entry array");
        return BQ_ERR_INVALID;
    }

    BackendQueueEntry *parsed = calloc(BQ_SET_MAX, sizeof(*parsed));
    if (!parsed) {
        if (err && err_size) snprintf(err, err_size, "out of memory");
        return BQ_ERR_INVALID;
    }

    int stored = 0;     /* 实际写进 parsed 的条目数（<= BQ_SET_MAX） */
    int accepted = 0;   /* 载荷里的合法条目总数（超限判定用它） */
    JsonValue element;
    while (json_array_next(&reader, &element) == 1) {
        if (element.type != JSON_VALUE_OBJECT) {
            continue;
        }
        JsonReader entry_reader = reader;
        if (json_reader_enter(&entry_reader, &element) != 0) {
            continue;
        }
        BackendQueueEntry entry;
        entry_from_json(&entry_reader, &entry);
        if (entry.path[0] == '\0') {
            continue;   /* 没有路径的条目直接丢弃 */
        }
        if (!bq_path_is_local(entry.path)) {
            free(parsed);
            if (err && err_size) snprintf(err, err_size, "non-local path rejected");
            return BQ_ERR_INVALID;
        }
        if (stored < BQ_SET_MAX) {
            parsed[stored++] = entry;
        }
        accepted++;
    }

    if (!json_reader_ok(&reader)) {
        free(parsed);
        if (err && err_size) snprintf(err, err_size, "malformed JSON");
        return BQ_ERR_INVALID;
    }

    if (accepted > BQ_SET_MAX) {
        free(parsed);
        if (err && err_size) {
            snprintf(err, err_size,
                     "payload carries %d entries, over the %d per-call limit",
                     accepted, BQ_SET_MAX);
        }
        return BQ_ERR_TOO_MANY;
    }

    *out = parsed;
    return stored;
}

/* 载荷下发（Set/Append/InsertAfter）都在锁外完成解析：单条载荷可达 256 KB，
 * 解析期间不持锁，避免播放线程的续播读取被拖住。 */
int bq_set_json(const char *json)
{
    char err[128];
    BackendQueueEntry *parsed = NULL;
    int count = parse_entries(json, &parsed, err, sizeof(err));
    if (count < 0) {
        log_warn("bq", "Queue.Set rejected: %s", err);
        return count;
    }

    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    if (bq_reserve_locked(count) != 0) {
        pthread_mutex_unlock(&g_bq_mutex);
        free(parsed);
        return BQ_ERR_INVALID;
    }

    /* Set 契约：整表替换的是**未来曲目**，正在播放的曲目不中断。替换前把原
     * 游标条目记为“在播条目”，新表短于播放线程下标时上层（MPRIS/Info）仍能
     * 用它如实上报正在出声的曲目。游标随后置到新表首。 */
    bq_pin_current_locked();

    if (count > 0) {
        memcpy(g_bq.entries, parsed, (size_t)count * sizeof(*parsed));
    }
    free(parsed);
    g_bq.count = count;
    g_bq.position = count > 0 ? 0 : -1;
    bq_bump_revision_locked();
    pthread_mutex_unlock(&g_bq_mutex);

    log_info("bq", "Queue set to %d entr%s", count, count == 1 ? "y" : "ies");
    return count;
}

int bq_append_json(const char *json)
{
    char err[128];
    BackendQueueEntry *parsed = NULL;
    int count = parse_entries(json, &parsed, err, sizeof(err));
    if (count < 0) {
        log_warn("bq", "Queue.Append rejected: %s", err);
        return count;
    }

    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    if (bq_reserve_locked(g_bq.count + count) != 0) {
        pthread_mutex_unlock(&g_bq_mutex);
        free(parsed);
        return BQ_ERR_INVALID;
    }

    bq_pin_current_locked();
    if (count > 0) {
        memcpy(g_bq.entries + g_bq.count, parsed, (size_t)count * sizeof(*parsed));
        g_bq.count += count;
    }
    free(parsed);
    if (g_bq.position < 0 && g_bq.count > 0) {
        g_bq.position = 0;
        bq_pin_current_locked();
    }
    bq_bump_revision_locked();
    int total = g_bq.count;
    pthread_mutex_unlock(&g_bq_mutex);

    log_info("bq", "Queue appended %d entr%s (total %d)", count, count == 1 ? "y" : "ies", total);
    return count;
}

int bq_insert_after_json(int position, const char *json)
{
    char err[128];
    BackendQueueEntry *parsed = NULL;
    int count = parse_entries(json, &parsed, err, sizeof(err));
    if (count < 0) {
        log_warn("bq", "Queue.InsertAfter rejected: %s", err);
        return count;
    }

    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    if (position < -1 || position >= g_bq.count) {
        pthread_mutex_unlock(&g_bq_mutex);
        free(parsed);
        return BQ_ERR_INVALID;
    }
    if (bq_reserve_locked(g_bq.count + count) != 0) {
        pthread_mutex_unlock(&g_bq_mutex);
        free(parsed);
        return BQ_ERR_INVALID;
    }

    bq_pin_current_locked();
    int at = position + 1;
    memmove(g_bq.entries + at + count, g_bq.entries + at,
            (size_t)(g_bq.count - at) * sizeof(*g_bq.entries));
    if (count > 0) {
        memcpy(g_bq.entries + at, parsed, (size_t)count * sizeof(*parsed));
    }
    free(parsed);
    g_bq.count += count;
    if (g_bq.position >= at) {
        g_bq.position += count;
    }
    if (g_bq.position < 0 && g_bq.count > 0) {
        g_bq.position = 0;
    }
    bq_bump_revision_locked();
    pthread_mutex_unlock(&g_bq_mutex);
    return count;
}

/* ── 编辑 ─────────────────────────────────────────────────────────── */

int bq_remove_at(int position)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    if (position < 0 || position >= g_bq.count) {
        pthread_mutex_unlock(&g_bq_mutex);
        return -1;
    }
    /* 删掉的有可能正是游标条目：先记住在播条目，播放线程的下标越界时上层
     * 仍能用它如实上报正在出声的曲目 */
    bq_pin_current_locked();
    memmove(g_bq.entries + position, g_bq.entries + position + 1,
            (size_t)(g_bq.count - position - 1) * sizeof(*g_bq.entries));
    g_bq.count--;

    if (g_bq.count == 0) {
        g_bq.position = -1;
    } else if (position < g_bq.position) {
        g_bq.position--;
    } else if (position == g_bq.position && g_bq.position >= g_bq.count) {
        g_bq.position = g_bq.count - 1;
    }
    bq_bump_revision_locked();
    pthread_mutex_unlock(&g_bq_mutex);
    return 0;
}

static int bq_swap_locked(int lhs, int rhs)
{
    bq_ensure_ready_locked();
    if (lhs < 0 || rhs < 0 || lhs >= g_bq.count || rhs >= g_bq.count) {
        return -1;
    }
    BackendQueueEntry tmp = g_bq.entries[lhs];
    g_bq.entries[lhs] = g_bq.entries[rhs];
    g_bq.entries[rhs] = tmp;

    if (g_bq.position == lhs) {
        g_bq.position = rhs;
    } else if (g_bq.position == rhs) {
        g_bq.position = lhs;
    }
    bq_bump_revision_locked();
    return 0;
}

static int bq_swap(int lhs, int rhs)
{
    pthread_mutex_lock(&g_bq_mutex);
    int rc = bq_swap_locked(lhs, rhs);
    pthread_mutex_unlock(&g_bq_mutex);
    return rc;
}

int bq_move_up(int position)   { return bq_swap(position, position - 1); }
int bq_move_down(int position) { return bq_swap(position, position + 1); }

/* ── 执行顺序 ─────────────────────────────────────────────────────── */

static void bq_shuffle_rest_locked(void);

static const char *path_dirname(const char *path, char *out, size_t size)
{
    snprintf(out, size, "%s", path ? path : "");
    char *slash = strrchr(out, '/');
    if (slash) {
        *slash = '\0';
    }
    return out;
}

/* 同目录判定必须允许**子目录**：旧实现（audio/play_queue.c 的
 * filter_tracks_by_folder）用 "目录前缀 + '/'" 判定，`/a/01.mp3` 与
 * `/a/sub/02.mp3` 属于同一组。只比较 dirname 会漏掉子目录，语义漂移。 */
static int same_folder(const BackendQueueEntry *lhs, const BackendQueueEntry *rhs)
{
    char dir[MAX_PATH_LEN];
    path_dirname(rhs->path, dir, sizeof(dir));

    size_t len = strlen(dir);
    if (len == 0) {
        return 0;
    }
    return strncmp(lhs->path, dir, len) == 0 && lhs->path[len] == '/';
}

static int same_text(const char *lhs, const char *rhs)
{
    return lhs[0] != '\0' && rhs[0] != '\0' && strcmp(lhs, rhs) == 0;
}

/* 洗牌模式重建：当前条目置首、其余打乱。`bq_shuffle_rest_locked()` 本身不做
 * “当前是否已在首位”的判断，直接调用会在已经正确的队列上白打乱一遍；
 * 这里按路径先定位再决定，与旧实现 `found > 0` 的分支等价。
 * 只在持锁时调用。 */
static void bq_shuffle_keeping_current_locked(const BackendQueueEntry *current)
{
    int position = current ? bq_position_of_path_locked(current->path) : -1;
    if (position > 0) {
        BackendQueueEntry tmp = g_bq.entries[0];
        g_bq.entries[0] = g_bq.entries[position];
        g_bq.entries[position] = tmp;
    }
    g_bq.position = 0;
    bq_shuffle_rest_locked();
}

/* 组模式：把队列重建为"同一组"的条目（当前条目在前），与
 * audio/play_queue.c 的 filter_tracks_by_folder/album/artist 语义一致。
 * 只在持锁时调用。 */
static int bq_apply_group_locked(const BackendQueueEntry *current, PlayMode mode)
{
    BackendQueueEntry *grouped = malloc((size_t)g_bq.count * sizeof(*grouped));
    if (!grouped) {
        return -1;
    }

    int written = 0;
    for (int i = 0; i < g_bq.count; i++) {
        const BackendQueueEntry *entry = &g_bq.entries[i];
        int match = 0;
        if (pm_is_folder_mode(mode)) {
            match = same_folder(entry, current);
        } else if (pm_is_album_mode(mode)) {
            match = same_text(entry->album, current->album);
        } else {
            match = same_text(entry->artist, current->artist);
        }
        if (match) {
            grouped[written++] = *entry;
        }
    }
    if (written == 0) {
        free(grouped);
        return -1;
    }

    memcpy(g_bq.entries, grouped, (size_t)written * sizeof(*grouped));
    free(grouped);
    g_bq.count = written;

    /* 游标回到当前条目（按路径匹配） */
    int position = bq_position_of_path_locked(current->path);
    g_bq.position = position >= 0 ? position : 0;
    return 0;
}

static void bq_shuffle_rest_locked(void)
{
    bq_ensure_ready_locked();
    if (g_bq.count <= 1) {
        return;
    }
    /* 随机重排改变的是执行顺序：先记住在播条目，再打乱游标之后的部分 */
    bq_pin_current_locked();
    int first = g_bq.position > 0 ? g_bq.position : 0;
    for (int i = g_bq.count - 1; i > first; i--) {
        int j = first + (int)(rand() % (i - first + 1));
        BackendQueueEntry tmp = g_bq.entries[i];
        g_bq.entries[i] = g_bq.entries[j];
        g_bq.entries[j] = tmp;
    }
    g_bq.shuffle_generation++;
    bq_bump_revision_locked();
}

void bq_shuffle_rest(void)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_shuffle_rest_locked();
    pthread_mutex_unlock(&g_bq_mutex);
}

void bq_apply_mode(PlayMode mode)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    if (g_bq.count == 0) {
        g_bq.position = -1;
        pthread_mutex_unlock(&g_bq_mutex);
        return;
    }

    /* 模式重建会改执行顺序（也可能收敛成一组）：先记住在播条目 */
    bq_pin_current_locked();

    BackendQueueEntry current;
    int have_current = bq_current_locked(&current) == 0;

    if (have_current && (pm_is_folder_mode(mode) || pm_is_album_mode(mode) || pm_is_artist_mode(mode))) {
        if (bq_apply_group_locked(&current, mode) != 0) {
            log_warn("bq", "mode %d: no entry shares the current group, keeping the full queue", (int)mode);
        }
    }

    if (pm_is_shuffle(mode)) {
        if (have_current) {
            bq_shuffle_keeping_current_locked(&current);
        } else {
            g_bq.position = 0;
            bq_shuffle_rest_locked();
        }
    } else if (have_current) {
        int position = bq_position_of_path_locked(current.path);
        g_bq.position = position >= 0 ? position : 0;
    } else {
        g_bq.position = 0;
    }

    bq_bump_revision_locked();
    pthread_mutex_unlock(&g_bq_mutex);
}

int bq_peek_next(PlayMode mode, int *out_position)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    int result = 0;
    if (g_bq.count == 0) {
        pthread_mutex_unlock(&g_bq_mutex);
        return 0;
    }
    int position;
    if (mode == PLAY_MODE_SINGLE_REPEAT) {
        position = g_bq.position >= 0 ? g_bq.position : 0;
    } else {
        position = g_bq.position + 1;
        if (position >= g_bq.count) {
            if (!pm_repeats(mode)) {
                pthread_mutex_unlock(&g_bq_mutex);
                return 0;
            }
            position = 0;
        }
    }
    if (out_position) {
        *out_position = position;
    }
    result = 1;
    pthread_mutex_unlock(&g_bq_mutex);
    return result;
}

int bq_advance(PlayMode mode, int *out_position)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    if (g_bq.count == 0) {
        pthread_mutex_unlock(&g_bq_mutex);
        return 0;
    }

    if (mode == PLAY_MODE_SINGLE_REPEAT) {
        if (out_position) {
            *out_position = g_bq.position >= 0 ? g_bq.position : 0;
        }
        pthread_mutex_unlock(&g_bq_mutex);
        return 1;   /* 同一位置重播 */
    }

    if (pm_is_shuffle(mode) && pm_repeats(mode) && g_bq.position + 1 >= g_bq.count) {
        bq_shuffle_rest_locked();
        g_bq.position = 1 < g_bq.count ? 1 : 0;
        /* 游标指向即将出声的条目：在播记录随之刷新 */
        bq_pin_current_locked();
        bq_bump_revision_locked();
        if (out_position) {
            *out_position = g_bq.position;
        }
        pthread_mutex_unlock(&g_bq_mutex);
        return 1;
    }

    int position = g_bq.position + 1;
    if (position >= g_bq.count) {
        if (!pm_repeats(mode)) {
            position = g_bq.count - 1;
            g_bq.position = position;
            bq_pin_current_locked();
            bq_bump_revision_locked();
            if (out_position) {
                *out_position = position;
            }
            pthread_mutex_unlock(&g_bq_mutex);
            return 0;   /* 到头，停 */
        }
        position = 0;
    }
    g_bq.position = position;
    bq_pin_current_locked();
    bq_bump_revision_locked();
    if (out_position) {
        *out_position = position;
    }
    pthread_mutex_unlock(&g_bq_mutex);
    return 1;
}

int bq_rewind(PlayMode mode, int *out_position)
{
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    if (g_bq.count == 0) {
        pthread_mutex_unlock(&g_bq_mutex);
        return 0;
    }

    if (mode == PLAY_MODE_SINGLE_REPEAT) {
        if (out_position) {
            *out_position = g_bq.position >= 0 ? g_bq.position : 0;
        }
        pthread_mutex_unlock(&g_bq_mutex);
        return 1;
    }

    int position = g_bq.position - 1;
    if (position < 0) {
        position = pm_repeats(mode) ? g_bq.count - 1 : 0;
    }
    g_bq.position = position;
    bq_pin_current_locked();
    bq_bump_revision_locked();
    if (out_position) {
        *out_position = position;
    }
    pthread_mutex_unlock(&g_bq_mutex);
    return 1;
}

/* ── 序列化 ───────────────────────────────────────────────────────── */

size_t bq_render_entry_json(const BackendQueueEntry *entry, char *out, size_t out_size)
{
    if (!entry || !out || out_size == 0) {
        return 0;
    }
    size_t pos = 0;
    pos = json_append_char(out, out_size, pos, '{');
    pos = json_append_key(out, out_size, pos, "path");
    pos = json_append_escaped(out, out_size, pos, entry->path);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "title");
    pos = json_append_escaped(out, out_size, pos, entry->title);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "artist");
    pos = json_append_escaped(out, out_size, pos, entry->artist);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "album");
    pos = json_append_escaped(out, out_size, pos, entry->album);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "duration_seconds");
    pos = json_append_int(out, out_size, pos, entry->duration_seconds);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cue_offset");
    pos = json_append_int(out, out_size, pos, entry->cue_offset);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "cue_track_number");
    pos = json_append_int(out, out_size, pos, entry->cue_track_number);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "is_cue");
    pos = json_append_bool(out, out_size, pos, entry->is_cue);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "lyrics_source");
    pos = json_append_int(out, out_size, pos, entry->lyrics_source);
    pos = json_append_char(out, out_size, pos, '}');
    return pos;
}

size_t bq_render_get(char *out, size_t out_size, int offset, int count)
{
    if (!out || out_size == 0) {
        return 0;
    }
    /* 整页渲染期间持锁：播放线程可能同时在推进游标 */
    pthread_mutex_lock(&g_bq_mutex);
    bq_ensure_ready_locked();
    if (offset < 0) offset = 0;
    if (count < 0) count = 0;
    int end = offset + count;
    if (end > g_bq.count) end = g_bq.count;

    size_t pos = 0;
    pos = json_append_char(out, out_size, pos, '{');
    pos = json_append_key(out, out_size, pos, "revision");
    /* 修订号是 64 位；以文本形式交给写入器（json_append_int 只收 long long） */
    {
        char revision_text[32];
        snprintf(revision_text, sizeof(revision_text), "%llu", g_bq.revision);
        pos = json_append_number(out, out_size, pos, revision_text);
    }
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "count");
    pos = json_append_int(out, out_size, pos, g_bq.count);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "current_position");
    if (g_bq.position < 0) {
        pos = json_append_raw(out, out_size, pos, "null");
    } else {
        pos = json_append_int(out, out_size, pos, g_bq.position);
    }
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "offset");
    pos = json_append_int(out, out_size, pos, offset);
    pos = json_append_raw(out, out_size, pos, ",");
    pos = json_append_key(out, out_size, pos, "rows");
    pos = json_append_char(out, out_size, pos, '[');
    for (int i = offset; i < end; i++) {
        if (i > offset) {
            pos = json_append_char(out, out_size, pos, ',');
        }
        pos = json_append_char(out, out_size, pos, '{');
        pos = json_append_key(out, out_size, pos, "position");
        pos = json_append_int(out, out_size, pos, i);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "path");
        pos = json_append_escaped(out, out_size, pos, g_bq.entries[i].path);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "title");
        pos = json_append_escaped(out, out_size, pos, g_bq.entries[i].title);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "artist");
        pos = json_append_escaped(out, out_size, pos, g_bq.entries[i].artist);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "album");
        pos = json_append_escaped(out, out_size, pos, g_bq.entries[i].album);
        pos = json_append_raw(out, out_size, pos, ",");
        pos = json_append_key(out, out_size, pos, "is_cue");
        pos = json_append_bool(out, out_size, pos, g_bq.entries[i].is_cue);
        pos = json_append_char(out, out_size, pos, '}');
    }
    pos = json_append_char(out, out_size, pos, ']');
    pos = json_append_char(out, out_size, pos, '}');
    size_t written = pos;
    pthread_mutex_unlock(&g_bq_mutex);
    return written;
}
