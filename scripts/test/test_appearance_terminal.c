/**
 * @file test_appearance_terminal.c
 * @brief 按终端外观覆盖文件的纯逻辑单测（t16 D2）
 *
 * 覆盖 load/save/get/set 的 round-trip、未知条目保留、非法 mode 丢弃、
 * 损坏 JSON、version>1 只读、原子写；以及键 sanitize。
 */

#include "config/appearance.h"
#include "ui/appearance_terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            printf("  FAIL %s (line %d)\n", (what), __LINE__);                \
        }                                                                     \
    } while (0)

/* 终端模块引用 config 的 app_config_dir；单测只需一个稳定目录。 */
static char g_dir[256];
const char *app_config_dir(int ensure)
{
    (void)ensure;
    return g_dir;
}

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        return -1;
    }
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, file) == len;
    fclose(file);
    return ok ? 0 : -1;
}

static char *read_text(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[got] = '\0';
    return text;
}

static void test_roundtrip_and_merge(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/appearance-terminals.json", g_dir);

    AppearanceTerminalMap map;
    memset(&map, 0, sizeof(map));
    map.version = 1;
    CHECK(appearance_terminals_set(&map, "tp:iTerm.app", APPEARANCE_MODE_LIGHT) == 0,
          "set first entry");
    CHECK(appearance_terminals_set(&map, "anc:deepin-terminal", APPEARANCE_MODE_DARK) == 0,
          "set second entry");
    CHECK(appearance_terminals_set(&map, "tp:iTerm.app", APPEARANCE_MODE_DARK) == 0,
          "update existing entry");
    CHECK(map.count == 2, "update does not grow map");
    CHECK(appearance_terminals_save_to(path, &map) == 0, "save map");

    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    CHECK(access(tmp, F_OK) != 0, "temp file removed after atomic save");

    char *text = read_text(path);
    CHECK(text && strstr(text, "\"version\":1") && strstr(text, "tp:iterm.app") &&
              strstr(text, "anc:deepin-terminal"),
          "saved JSON contains version + keys");
    free(text);

    AppearanceTerminalMap loaded;
    CHECK(appearance_terminals_load_from(path, &loaded) == 0, "load map");
    CHECK(loaded.count == 2, "round-trip entry count");
    CHECK(appearance_terminals_get(&loaded, "tp:iterm.app") == APPEARANCE_MODE_DARK,
          "round-trip updated value");
    CHECK(appearance_terminals_get(&loaded, "anc:deepin-terminal") == APPEARANCE_MODE_DARK,
          "round-trip second value");
    CHECK(appearance_terminals_get(&loaded, "tp:missing") == -1, "unknown => inherit");

    /* 未知终端条目保留（读-改-写不丢别人写的键）。 */
    CHECK(appearance_terminals_set(&loaded, "tp:other", APPEARANCE_MODE_LIGHT) == 0,
          "add third");
    CHECK(appearance_terminals_save_to(path, &loaded) == 0, "save merged");
    AppearanceTerminalMap again;
    CHECK(appearance_terminals_load_from(path, &again) == 0 && again.count == 3,
          "unknown/other entries preserved");

    /* Inherit 删除条目。 */
    CHECK(appearance_terminals_set(&again, "tp:other", -1) == 0, "remove entry");
    CHECK(again.count == 2, "remove decrements count");
    CHECK(appearance_terminals_set(&again, "default", APPEARANCE_MODE_LIGHT) == 0 &&
              again.count == 2,
          "default key is a no-op");
    CHECK(appearance_terminals_set(&again, "tp:x", 7) != 0, "invalid mode rejected");
    CHECK(appearance_terminals_get(&again, "tp:other") == -1, "removed => inherit");

    (void)remove(path);
}

static void test_corrupt_and_versions(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/corrupt.json", g_dir);
    AppearanceTerminalMap map;

    CHECK(write_text(path, "{not json") == 0, "write corrupt json");
    CHECK(appearance_terminals_load_from(path, &map) == 0, "corrupt json does not fail");
    CHECK(map.count == 0 && map.version == 1, "corrupt => empty v1");
    char corrupt_copy[600];
    snprintf(corrupt_copy, sizeof(corrupt_copy), "%s.corrupt", path);
    CHECK(access(corrupt_copy, F_OK) == 0, "corrupt file backed up");
    (void)remove(corrupt_copy);

    /* 非法 mode 条目丢弃，合法条目保留。 */
    CHECK(write_text(path,
                     "{\"version\":1,\"terminals\":{"
                     "\"tp:good\":{\"mode\":1},"
                     "\"tp:bad\":{\"mode\":9},"
                     "\"tp:str\":{\"mode\":\"light\"},"
                     "\"tp:empty\":{}"
                     "}}") == 0,
          "write mixed entries");
    CHECK(appearance_terminals_load_from(path, &map) == 0, "load mixed entries");
    CHECK(map.count == 1, "only valid mode entry kept");
    CHECK(appearance_terminals_get(&map, "tp:good") == APPEARANCE_MODE_LIGHT,
          "valid entry parsed");

    /* 未知顶层字段/未知条目字段忽略。 */
    CHECK(write_text(path,
                     "{\"version\":1,\"future\":true,\"terminals\":{"
                     "\"tp:a\":{\"mode\":2,\"extra\":\"x\"},\"raw\":{\"mode\":0}"
                     "}}") == 0,
          "write unknown fields");
    CHECK(appearance_terminals_load_from(path, &map) == 0 && map.count == 2,
          "unknown fields ignored");

    /* version > 1：只读，不应用覆盖，不允许写回。 */
    CHECK(write_text(path,
                     "{\"version\":2,\"terminals\":{\"tp:a\":{\"mode\":1}}}") == 0,
          "write version 2");
    CHECK(appearance_terminals_load_from(path, &map) == 0, "version 2 load ok");
    CHECK(map.read_only == 1, "version 2 read-only");
    CHECK(appearance_terminals_get(&map, "tp:a") == -1,
          "version 2 overrides not applied");
    CHECK(appearance_terminals_save_to(path, &map) != 0, "version 2 save refused");

    /* F2：合法 JSON 但无 version 键 → 按 version=1 加载且可写。 */
    snprintf(path, sizeof(path), "%s/no-version.json", g_dir);
    CHECK(write_text(path,
                     "{\"terminals\":{\"tp:noversion\":{\"mode\":1}}}") == 0,
          "write json without version");
    CHECK(appearance_terminals_load_from(path, &map) == 0, "no-version load ok");
    CHECK(map.count == 1 && map.read_only == 0 && map.version == 1,
          "missing version defaults to 1 and stays writable");
    CHECK(appearance_terminals_get(&map, "tp:noversion") == APPEARANCE_MODE_LIGHT,
          "no-version entry applied");
    CHECK(appearance_terminals_set(&map, "tp:added", APPEARANCE_MODE_DARK) == 0 &&
              appearance_terminals_save_to(path, &map) == 0,
          "no-version file remains writable");
    (void)remove(path);

    /* F2：合法 JSON 但根类型不是对象 → 空表、不崩溃。 */
    snprintf(path, sizeof(path), "%s/root-array.json", g_dir);
    CHECK(write_text(path, "[1,2,3]") == 0, "write root array json");
    CHECK(appearance_terminals_load_from(path, &map) == 0,
          "root array does not crash");
    CHECK(map.count == 0 && map.read_only == 0, "root array => empty map");
    snprintf(path, sizeof(path), "%s/root-string.json", g_dir);
    CHECK(write_text(path, "\"just text\"") == 0, "write root string json");
    CHECK(appearance_terminals_load_from(path, &map) == 0,
          "root string does not crash");
    CHECK(map.count == 0, "root string => empty map");
    char corrupt[600];
    snprintf(corrupt, sizeof(corrupt), "%s.corrupt", path);
    (void)remove(corrupt);
    snprintf(corrupt, sizeof(corrupt), "%s/root-array.json.corrupt", g_dir);
    (void)remove(corrupt);
    (void)remove(path);

    /* 不存在的文件 = 空表，不是错误。 */
    snprintf(path, sizeof(path), "%s/does-not-exist.json", g_dir);
    CHECK(appearance_terminals_load_from(path, &map) == 0 && map.count == 0,
          "missing file => empty map");
    (void)remove(path);
}

static void test_key_sanitize(void)
{
    char key[64];
    appearance_terminal_key_sanitize("TP:iTerm.App / X", key, sizeof(key));
    CHECK(strcmp(key, "tp:iterm.app___x") == 0, "sanitize lowercase + replace");

    appearance_terminal_key_sanitize("", key, sizeof(key));
    CHECK(key[0] == '\0', "sanitize empty");

    char tiny[5];
    appearance_terminal_key_sanitize("abcdefgh", tiny, sizeof(tiny));
    CHECK(strlen(tiny) == 4, "sanitize truncates safely");
}

int main(void)
{
    char template[] = "/tmp/tm-terminal-test-XXXXXX";
    char *dir = mkdtemp(template);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", dir);

    printf("test_appearance_terminal: file round-trip\n");
    test_roundtrip_and_merge();
    printf("test_appearance_terminal: corrupt/version handling\n");
    test_corrupt_and_versions();
    printf("test_appearance_terminal: key sanitize\n");
    test_key_sanitize();

    (void)rmdir(g_dir);
    if (g_failures) {
        printf("FAIL test_appearance_terminal: %d/%d checks failed\n",
               g_failures, g_checks);
        return 1;
    }
    printf("PASS test_appearance_terminal (%d checks)\n", g_checks);
    return 0;
}
