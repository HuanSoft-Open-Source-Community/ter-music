/**
 * @file appearance_terminal.c
 * @brief 按终端记住外观模式：身份键收集 + frontend-local JSON 读写
 *
 * 纯文件函数（load_from/save_to/get/set）不依赖全局状态，可单测；
 * 运行期初始化只调用一次，把身份键与覆盖表缓存起来。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/appearance_terminal.h"

#include "config/appearance.h"
#include "config/config.h"
#include "util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define APPEARANCE_TERMINAL_FILE "appearance-terminals.json"
#define APPEARANCE_TERMINAL_JSON_MAX 32768
#define APPEARANCE_ANCESTOR_MAX 8

static int g_initialized = 0;
static char g_key[APPEARANCE_TERMINAL_KEY_MAX] = "default";
static char g_path[512];
static AppearanceTerminalMap g_map;
static int g_is_deepin = 0;

/* ── /proc 祖先进程收集（仅 Linux；失败即停止，不报错） ───────────── */

static int read_process_comm(pid_t pid, char *out, size_t out_size)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    if (!fgets(out, (int)out_size, file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }
    return out[0] ? 0 : -1;
}

static pid_t read_process_ppid(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    char buffer[512];
    size_t got = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[got] = '\0';

    /* comm 可含空格/括号：从最后一个 ')' 之后解析 "state ppid ..."。 */
    char *close_paren = strrchr(buffer, ')');
    if (!close_paren) {
        return -1;
    }
    char state = 0;
    int ppid = -1;
    if (sscanf(close_paren + 1, " %c %d", &state, &ppid) != 2) {
        return -1;
    }
    (void)state;
    return (pid_t)ppid;
}

static size_t collect_ancestors(char comms[][32], size_t max)
{
    size_t count = 0;
    pid_t pid = getpid();
    for (size_t i = 0; i < max; i++) {
        if (read_process_comm(pid, comms[count], 32) != 0) {
            break;
        }
        count++;
        pid_t parent = read_process_ppid(pid);
        if (parent <= 1 || parent == pid) {
            break;
        }
        pid = parent;
    }
    return count;
}

/* ── 文件级 API（可单测） ─────────────────────────────────────────── */

static int terminals_add(AppearanceTerminalMap *map, const char *key, int mode)
{
    if (!map || !key || !key[0] || mode < APPEARANCE_MODE_AUTO ||
        mode >= APPEARANCE_MODE_COUNT) {
        return -1;
    }
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            map->entries[i].mode = mode;
            return 0;
        }
    }
    if (map->count >= APPEARANCE_TERMINALS_MAX) {
        return -1;
    }
    snprintf(map->entries[map->count].key, sizeof(map->entries[0].key), "%s", key);
    map->entries[map->count].mode = mode;
    map->count++;
    return 0;
}

static void terminals_clear(AppearanceTerminalMap *map)
{
    if (!map) {
        return;
    }
    memset(map, 0, sizeof(*map));
    map->version = 1;
}

int appearance_terminals_load_from(const char *path, AppearanceTerminalMap *map)
{
    if (!map) {
        return -1;
    }
    terminals_clear(map);
    if (!path || !path[0]) {
        return 0;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;   /* 不存在 = 全部 Inherit */
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size <= 0 || size > 1024 * 1024) {
        fclose(file);
        if (size <= 0) {
            /* 空文件也按损坏处理：备份后当空表。 */
            char corrupt[600];
            snprintf(corrupt, sizeof(corrupt), "%s.corrupt", path);
            (void)rename(path, corrupt);
        }
        return 0;
    }
    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return -1;
    }
    size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[got] = '\0';

    JsonReader reader;
    json_reader_init(&reader, text, got);
    JsonValue version_value = {0};
    long long version = 1;   /* version 缺失 = 1（手写最小文件也合法） */
    if (json_object_find(&reader, "version", &version_value) == 0 &&
        version_value.type == JSON_VALUE_NUMBER) {
        version = (long long)version_value.number;
    }
    if (!json_reader_ok(&reader)) {
        free(text);
        char corrupt[600];
        snprintf(corrupt, sizeof(corrupt), "%s.corrupt", path);
        (void)rename(path, corrupt);
        return 0;   /* 损坏 → 按空表处理，不崩溃 */
    }

    if (version > 1) {
        map->read_only = 1;   /* 更高版本：本次不应用、不覆盖 */
        map->version = (int)version;
        free(text);
        return 0;
    }
    map->version = 1;

    JsonValue terminals;
    if (json_object_find(&reader, "terminals", &terminals) == 0 &&
        terminals.type == JSON_VALUE_OBJECT) {
        JsonReader items;
        json_reader_init(&items, terminals.start, terminals.length);
        if (json_reader_enter(&items, &terminals) == 0) {
            char raw_key[APPEARANCE_TERMINAL_KEY_MAX];
            JsonValue value;
            while (json_object_next(&items, raw_key, sizeof(raw_key), &value) == 1) {
                if (value.type != JSON_VALUE_OBJECT) {
                    continue;
                }
                JsonReader entry;
                json_reader_init(&entry, value.start, value.length);
                long long mode = json_get_int(&entry, "mode", -1);
                if (mode < APPEARANCE_MODE_AUTO || mode >= APPEARANCE_MODE_COUNT) {
                    continue;   /* 非法条目丢弃 */
                }
                char key[APPEARANCE_TERMINAL_KEY_MAX];
                appearance_terminal_key_sanitize(raw_key, key, sizeof(key));
                if (!key[0] || strcmp(key, "default") == 0) {
                    continue;
                }
                (void)terminals_add(map, key, (int)mode);
            }
        }
    }
    free(text);
    return 0;
}

int appearance_terminals_save_to(const char *path, const AppearanceTerminalMap *map)
{
    if (!path || !path[0] || !map || map->read_only) {
        return -1;
    }

    char *json = malloc(APPEARANCE_TERMINAL_JSON_MAX);
    if (!json) {
        return -1;
    }
    size_t pos = 0;
    pos = json_append_raw(json, APPEARANCE_TERMINAL_JSON_MAX, pos,
                          "{\"version\":1,\"terminals\":{");
    int written = 0;
    for (int i = 0; i < map->count; i++) {
        if (map->entries[i].mode < APPEARANCE_MODE_AUTO ||
            map->entries[i].mode >= APPEARANCE_MODE_COUNT) {
            continue;
        }
        if (written > 0) {
            pos = json_append_char(json, APPEARANCE_TERMINAL_JSON_MAX, pos, ',');
        }
        pos = json_append_escaped(json, APPEARANCE_TERMINAL_JSON_MAX, pos,
                                  map->entries[i].key);
        pos = json_append_raw(json, APPEARANCE_TERMINAL_JSON_MAX, pos, ":{\"mode\":");
        pos = json_append_int(json, APPEARANCE_TERMINAL_JSON_MAX, pos,
                              map->entries[i].mode);
        pos = json_append_char(json, APPEARANCE_TERMINAL_JSON_MAX, pos, '}');
        written++;
    }
    pos = json_append_raw(json, APPEARANCE_TERMINAL_JSON_MAX, pos, "}}\n");
    if (json_writer_overflowed(APPEARANCE_TERMINAL_JSON_MAX, pos)) {
        free(json);
        return -1;
    }

    char tmp_path[600];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld",
                     path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        free(json);
        return -1;
    }
    FILE *file = fopen(tmp_path, "wb");
    if (!file) {
        free(json);
        return -1;
    }
    size_t len = strlen(json);
    int ok = (fwrite(json, 1, len, file) == len);
    if (fclose(file) != 0) {
        ok = 0;
    }
    free(json);
    if (!ok) {
        remove(tmp_path);
        return -1;
    }
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return -1;
    }
    return 0;
}

int appearance_terminals_get(const AppearanceTerminalMap *map, const char *key)
{
    if (!map || map->read_only || !key || !key[0]) {
        return -1;
    }
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            return map->entries[i].mode;
        }
    }
    return -1;
}

int appearance_terminals_set(AppearanceTerminalMap *map, const char *key, int mode)
{
    if (!map || map->read_only) {
        return -1;
    }
    char sanitized[APPEARANCE_TERMINAL_KEY_MAX];
    appearance_terminal_key_sanitize(key, sanitized, sizeof(sanitized));
    if (!sanitized[0] || strcmp(sanitized, "default") == 0) {
        return 0;   /* default 不写条目 */
    }

    int index = -1;
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, sanitized) == 0) {
            index = i;
            break;
        }
    }

    if (mode < 0) {
        if (index >= 0) {
            for (int i = index; i + 1 < map->count; i++) {
                map->entries[i] = map->entries[i + 1];
            }
            map->count--;
        }
        return 0;
    }
    if (mode >= APPEARANCE_MODE_COUNT) {
        return -1;
    }
    return terminals_add(map, sanitized, mode);
}

/* ── 运行期 API ───────────────────────────────────────────────────── */

const char *appearance_terminal_key(void)
{
    return g_key;
}

const char *appearance_terminal_file_path(void)
{
    return g_path[0] ? g_path : NULL;
}

int appearance_terminal_is_deepin(void)
{
    return g_is_deepin;
}

void appearance_terminal_init(void)
{
    if (g_initialized) {
        return;
    }
    g_initialized = 1;

    char comms[APPEARANCE_ANCESTOR_MAX][32];
    size_t count = collect_ancestors(comms, APPEARANCE_ANCESTOR_MAX);
    const char *ancestors[APPEARANCE_ANCESTOR_MAX];
    for (size_t i = 0; i < count; i++) {
        ancestors[i] = comms[i];
    }

    AppearanceTerminalEnv env;
    memset(&env, 0, sizeof(env));
    env.term_program = getenv("TERM_PROGRAM");
    env.term = getenv("TERM");
    env.colorterm = getenv("COLORTERM");
    env.tmux = getenv("TMUX");
    env.sty = getenv("STY");
    env.ssh_tty = getenv("SSH_TTY");
    env.ssh_connection = getenv("SSH_CONNECTION");

    (void)appearance_terminal_key_build(&env, ancestors, count,
                                        g_key, sizeof(g_key));
    g_is_deepin = appearance_terminal_is_deepin_env(&env, ancestors, count);

    const char *dir = app_config_dir(1);
    if (dir && dir[0]) {
        snprintf(g_path, sizeof(g_path), "%s/%s", dir,
                 APPEARANCE_TERMINAL_FILE);
        (void)appearance_terminals_load_from(g_path, &g_map);
    } else {
        g_path[0] = '\0';
        terminals_clear(&g_map);
    }
}

int appearance_terminal_override_get(void)
{
    if (!g_initialized) {
        return -1;
    }
    return appearance_terminals_get(&g_map, g_key);
}

int appearance_terminal_set_override(int mode)
{
    if (!g_initialized) {
        return -1;
    }
    if (mode < -1 || mode >= APPEARANCE_MODE_COUNT) {
        return -1;
    }
    AppearanceTerminalMap next = g_map;

    /* Inherit = 删除本终端条目；写入前先算好，失败不改内存。 */
    if (appearance_terminals_set(&next, g_key, mode < 0 ? -1 : mode) != 0) {
        return -1;
    }
    if (!g_path[0]) {
        return -1;
    }
    if (appearance_terminals_save_to(g_path, &next) != 0) {
        return -1;
    }
    g_map = next;
    return 0;
}

/* 供单元测试重置运行期缓存（非公开 API，测试可 declare extern）。 */
void appearance_terminal_test_reset(void)
{
    g_initialized = 0;
    g_is_deepin = 0;
    snprintf(g_key, sizeof(g_key), "default");
    g_path[0] = '\0';
    terminals_clear(&g_map);
}
