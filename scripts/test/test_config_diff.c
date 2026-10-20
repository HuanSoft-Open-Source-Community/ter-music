/*
 * config_diff 单测：前端把设置改动算成最小补丁
 *
 * 运行：scripts/test/run-unit-tests.sh test_config_diff
 * 依赖（见 test_config_diff.srcs）：config/config_diff.c + util/json.c
 */

#include "config/config_diff.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

static AppConfig base_config(void)
{
    AppConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.ui_language, sizeof(cfg.ui_language), "zh_CN");
    snprintf(cfg.default_startup_path, sizeof(cfg.default_startup_path), "/music");
    cfg.volume_percent = 100;
    cfg.default_playback_speed = 1.0f;
    cfg.auto_play_on_start = 0;
    cfg.eq_enabled = 0;
    cfg.eq_preamp = 0;
    return cfg;
}

static const char *diff_of(const AppConfig *before, const AppConfig *after, char *buf, size_t size)
{
    memset(buf, 0, size);
    config_diff_json(before, after, buf, size);
    return buf;
}

static int count_substring(const char *haystack, const char *needle)
{
    int count = 0;
    const char *pos = haystack;
    while ((pos = strstr(pos, needle)) != NULL) {
        count++;
        pos++;
    }
    return count;
}

static void set_binding(AppConfig *cfg, const char *action, const char *key)
{
    int index = cfg->keybinding_override_count++;
    snprintf(cfg->keybinding_overrides[index].action,
             sizeof(cfg->keybinding_overrides[index].action), "%s", action);
    snprintf(cfg->keybinding_overrides[index].key,
             sizeof(cfg->keybinding_overrides[index].key), "%s", key);
}

int main(void)
{
    char buf[4096];
    AppConfig before = base_config();

    /* 1. 无变化 → 空补丁 */
    AppConfig same = before;
    CHECK(strcmp(diff_of(&before, &same, buf, sizeof(buf)), "{}") == 0,
          "无变化时输出空补丁");

    /* 2. 单个整数键 */
    AppConfig v = before;
    v.volume_percent = 40;
    diff_of(&before, &v, buf, sizeof(buf));
    CHECK(strstr(buf, "\"preferences\"") != NULL, "整数变化落在 preferences 分区");
    CHECK(strstr(buf, "\"volume_percent\":40") != NULL, "整数键与值正确");
    CHECK(strstr(buf, "\"paths\"") == NULL && strstr(buf, "\"equalizer\"") == NULL,
          "未变化的分区不出现");

    /* 3. 字符串键（paths 分区） */
    AppConfig p = before;
    snprintf(p.default_startup_path, sizeof(p.default_startup_path), "/media/music");
    diff_of(&before, &p, buf, sizeof(buf));
    CHECK(strstr(buf, "\"paths\":{\"default_startup_path\":\"/media/music\"}") != NULL,
          "字符串键落在 paths 分区");

    /* 4. 浮点键 */
    AppConfig f = before;
    f.default_playback_speed = 1.5f;
    diff_of(&before, &f, buf, sizeof(buf));
    CHECK(strstr(buf, "\"default_playback_speed\":1.50") != NULL, "浮点键带两位小数");

    /* 5. 多分区多键 */
    AppConfig m = before;
    m.volume_percent = 55;
    m.auto_play_on_start = 1;
    snprintf(m.default_startup_path, sizeof(m.default_startup_path), "/srv/music");
    diff_of(&before, &m, buf, sizeof(buf));
    CHECK(strstr(buf, "\"paths\"") != NULL && strstr(buf, "\"preferences\"") != NULL,
          "两个分区同时出现");
    CHECK(strstr(buf, "\"volume_percent\":55") != NULL &&
          strstr(buf, "\"auto_play_on_start\":1") != NULL &&
          strstr(buf, "\"default_startup_path\":\"/srv/music\"") != NULL,
          "三个键都在补丁里");

    /* 6. 均衡器（含 bands 数组） */
    AppConfig e = before;
    e.eq_enabled = 1;
    e.eq_band_gains[3] = -6;
    diff_of(&before, &e, buf, sizeof(buf));
    CHECK(strstr(buf, "\"equalizer\":{") != NULL, "均衡器分区出现");
    CHECK(strstr(buf, "\"enabled\":1") != NULL, "eq enabled 出现");
    CHECK(strstr(buf, "\"preamp\"") == NULL, "未变化的 eq preamp 不出现");
    CHECK(strstr(buf, "\"bands\":[") != NULL, "bands 数组出现");

    /* 7. 同义键去重：default_loop_mode / default_play_mode 指向同一字段 */
    AppConfig pm = before;
    pm.default_play_mode = 4;
    diff_of(&before, &pm, buf, sizeof(buf));
    CHECK(count_substring(buf, "default_loop_mode") + count_substring(buf, "default_play_mode") == 1,
          "同一字段的两个键只输出一次");

    /* 8. 输出始终 NUL 结尾且长度合理 */
    size_t written = config_diff_json(&before, &m, buf, sizeof(buf));
    CHECK(written > 2 && written < sizeof(buf) && buf[written] == '\0', "返回值即写入长度");

    /* 9. 新增键位覆盖 */
    AppConfig kb = before;
    set_binding(&kb, "track.prev", "x");
    diff_of(&before, &kb, buf, sizeof(buf));
    CHECK(strstr(buf, "\"keybindings\":{\"track.prev\":\"x\"}") != NULL,
          "新增覆盖输出 action:key");

    /* 10. 数组顺序不同但语义相同 → 无 keybindings 分区 */
    AppConfig kb_a = before;
    set_binding(&kb_a, "track.prev", "x");
    set_binding(&kb_a, "track.next", "]");
    AppConfig kb_b = before;
    set_binding(&kb_b, "track.next", "]");
    set_binding(&kb_b, "track.prev", "x");
    CHECK(strcmp(diff_of(&kb_a, &kb_b, buf, sizeof(buf)), "{}") == 0,
          "覆盖数组顺序不影响差异");

    /* 11. 删除覆盖 → default */
    diff_of(&kb, &before, buf, sizeof(buf));
    CHECK(strstr(buf, "\"keybindings\":{\"track.prev\":\"default\"}") != NULL,
          "删除覆盖输出 default");

    /* 12. 改值 */
    AppConfig kb_c = before;
    set_binding(&kb_c, "track.prev", "ctrl+q");
    diff_of(&kb, &kb_c, buf, sizeof(buf));
    CHECK(strstr(buf, "\"keybindings\":{\"track.prev\":\"ctrl+q\"}") != NULL,
          "覆盖改值输出新键");

    /* 13. 多个 action 按字典序稳定输出 */
    AppConfig kb_many = before;
    set_binding(&kb_many, "track.prev", "x");
    set_binding(&kb_many, "track.next", "]");
    set_binding(&kb_many, "list.up", "w");
    diff_of(&before, &kb_many, buf, sizeof(buf));
    {
        const char *prev = strstr(buf, "\"track.prev\"");
        const char *list = strstr(buf, "\"list.up\"");
        const char *track = strstr(buf, "\"track.next\"");
        CHECK(prev && list && track && list < track && track < prev,
              "keybindings 按 action 字典序输出");
    }

    /* 14. 删除多项与新增混合时都出现 */
    diff_of(&kb_a, &kb_c, buf, sizeof(buf));
    CHECK(strstr(buf, "\"track.prev\":\"ctrl+q\"") != NULL,
          "混合差异包含改值项");
    CHECK(strstr(buf, "\"track.next\":\"default\"") != NULL,
          "混合差异包含删除项");

    /* 15. 最大覆盖量：128 条长 action/key 不得截断 */
    {
        AppConfig many = before;
        many.keybinding_override_count = 128;
        for (int i = 0; i < 128; i++) {
            snprintf(many.keybinding_overrides[i].action,
                     sizeof(many.keybinding_overrides[i].action),
                     "action.%03d.abcdefghijklmnopqrstuvwxyz012345", i);
            snprintf(many.keybinding_overrides[i].key,
                     sizeof(many.keybinding_overrides[i].key),
                     "ctrl+%c", 'a' + (i % 20));
        }
        char big[65536];
        memset(big, 0, sizeof(big));
        size_t n = config_diff_json(&before, &many, big, sizeof(big));
        CHECK(n > 2 && n < sizeof(big) && big[n] == '\0', "最大覆盖量 diff 未截断");
        CHECK(strstr(big, "\"action.127.") != NULL, "最大覆盖量 diff 含最后一条");
        CHECK(big[n - 1] == '}', "最大覆盖量 diff 以 } 收尾");
    }

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS test_config_diff\n");
        return 0;
    }
    printf("FAIL test_config_diff\n");
    return 1;
}
