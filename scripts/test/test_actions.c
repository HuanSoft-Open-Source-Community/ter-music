/*
 * keymap/actions 单测：动作表完整性、上下文父链、覆盖校验与冲突检测。
 *
 * 运行：scripts/test/run-unit-tests.sh test_actions
 * 依赖（见 test_actions.srcs）：actions.c + util/keyname.c
 */

#include "keymap/actions.h"
#include "util/keyname.h"

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

static void make_override(KeyBindingOverride *ov, const char *action, const char *key)
{
    memset(ov, 0, sizeof(*ov));
    snprintf(ov->action, sizeof(ov->action), "%s", action);
    snprintf(ov->key, sizeof(ov->key), "%s", key);
}

/* 默认表在同一上下文内出现重复键时返回冲突数。 */
static int default_conflicts(void)
{
    int conflicts = 0;
    for (int ctx = 0; ctx < CTX_COUNT; ctx++) {
        for (int a = 1; a < ACTION_COUNT; a++) {
            if (actions_context((ActionId)a) != (KeyContext)ctx) {
                continue;
            }
            char keys_a[4][KEYNAME_MAX_LEN];
            int count_a = actions_default_keys((ActionId)a, keys_a, 4);
            KeyStroke strokes_a[4];
            for (int i = 0; i < count_a; i++) {
                if (keyname_parse(keys_a[i], &strokes_a[i]) != 0) {
                    conflicts++;
                }
            }

            for (int b = a + 1; b < ACTION_COUNT; b++) {
                if (actions_context((ActionId)b) != (KeyContext)ctx) {
                    continue;
                }
                char keys_b[4][KEYNAME_MAX_LEN];
                int count_b = actions_default_keys((ActionId)b, keys_b, 4);
                for (int i = 0; i < count_a; i++) {
                    for (int j = 0; j < count_b; j++) {
                        KeyStroke stroke_b;
                        if (keyname_parse(keys_b[j], &stroke_b) == 0 &&
                            keyname_equal(strokes_a[i], stroke_b)) {
                            conflicts++;
                        }
                    }
                }
            }
        }
    }
    return conflicts;
}

int main(void)
{
    /* ── 数量与表完整性 ── */
    CHECK(actions_count() == 128, "动作总数为 128");
    CHECK(actions_bindable_count() == 118, "可绑定动作总数为 118");
    CHECK(actions_is_bindable(ACTION_APP_QUIT) == 0, "app.quit 锁定");
    CHECK(actions_is_bindable(ACTION_VIEW_MAIN) == 0, "view.main 锁定");
    CHECK(actions_is_bindable(ACTION_VIEW_HELP) == 0, "view.help 锁定");
    CHECK(actions_is_bindable(ACTION_VIEW_TAB_SWITCH) == 0, "view.tab_switch 锁定");
    CHECK(actions_is_bindable(ACTION_TRACK_NEXT) == 1, "track.next 可绑定");
    CHECK(actions_is_bindable(ACTION_VOLUME_UP) == 1, "volume.up 可绑定");
    {
        ActionId first = ACTION_NONE;
        ActionId second = ACTION_NONE;
        CHECK(actions_bindable_at(0, &first) == 1 &&
              first == ACTION_LIBRARY_TOGGLE,
              "首个可绑定动作是 library.toggle");
        CHECK(actions_bindable_at(1, &second) == 1 &&
              second == ACTION_VOLUME_UP,
              "第二个可绑定动作是 volume.up");
        CHECK(actions_bindable_at(-1, &first) == 0, "负索引无效");
        CHECK(actions_bindable_at(118, &first) == 0, "可绑定动作越界无效");
    }
    CHECK(actions_def(ACTION_NONE) == NULL, "ACTION_NONE 无定义");
    CHECK(actions_def(ACTION_COUNT) == NULL, "ACTION_COUNT 无定义");

    int ids_unique = 1;
    int i18n_ok = 1;
    int defaults_ok = 1;
    for (int a = 1; a < ACTION_COUNT; a++) {
        const KeyActionDef *def = actions_def((ActionId)a);
        if (!def) {
            defaults_ok = 0;
            continue;
        }
        if (actions_from_id(def->id) != (ActionId)a) {
            ids_unique = 0;
        }
        char expected[128];
        snprintf(expected, sizeof(expected), "keyaction.%s", def->id);
        if (strcmp(def->i18n_key, expected) != 0) {
            i18n_ok = 0;
        }

        char keys[4][KEYNAME_MAX_LEN];
        int count = actions_default_keys((ActionId)a, keys, 4);
        if (count < 1) {
            defaults_ok = 0;
        }
        for (int i = 0; i < count; i++) {
            KeyStroke stroke;
            if (keyname_parse(keys[i], &stroke) != 0 ||
                !keyname_is_bindable(stroke)) {
                defaults_ok = 0;
            }
        }
    }
    CHECK(ids_unique, "动作字符串 ID 唯一且可反查");
    CHECK(i18n_ok, "i18n_key 统一为 keyaction.<id>");
    CHECK(defaults_ok, "每个动作至少一个可解析、可绑定的默认键");

    /* 字符串 ID 与动作映射 */
    CHECK(actions_from_id("app.quit") == ACTION_APP_QUIT, "actions_from_id app.quit");
    CHECK(actions_from_id("list.up") == ACTION_LIST_UP, "actions_from_id list.up");
    CHECK(actions_from_id("track.next") == ACTION_TRACK_NEXT, "actions_from_id track.next");
    CHECK(actions_from_id("no.such.action") == ACTION_NONE, "未知 action 返回 ACTION_NONE");
    CHECK(actions_from_id(NULL) == ACTION_NONE, "NULL action 返回 ACTION_NONE");

    /* ── 上下文与父链 ── */
    CHECK(strcmp(actions_context_id(CTX_GLOBAL), "global") == 0, "global id");
    CHECK(strcmp(actions_context_id(CTX_MAIN_LIST), "main_list") == 0, "main_list id");
    CHECK(strcmp(actions_context_id(CTX_SETTINGS_EQ), "settings_eq") == 0, "settings_eq id");
    CHECK(strcmp(actions_context_id(CTX_TEXT_INPUT), "text_input") == 0, "text_input id");
    CHECK(actions_context_from_id("remote") == CTX_REMOTE, "context_from_id remote");
    CHECK(actions_context_from_id("nope") == CTX_COUNT, "context_from_id 未知返回 CTX_COUNT");

    CHECK(actions_context_parent(CTX_MAIN) == CTX_GLOBAL, "main -> global");
    CHECK(actions_context_parent(CTX_MAIN_LIST) == CTX_MAIN, "main_list -> main");
    CHECK(actions_context_parent(CTX_MAIN_LYRIC) == CTX_MAIN_LIST, "main_lyric -> main_list");
    CHECK(actions_context_parent(CTX_MAIN_CONTROL) == CTX_MAIN, "main_control -> main");
    CHECK(actions_context_parent(CTX_POPUP) == CTX_GLOBAL, "popup -> global");
    CHECK(actions_context_parent(CTX_SETTINGS_EQ) == CTX_GLOBAL, "settings_eq -> global");
    CHECK(actions_context_parent(CTX_GLOBAL) == CTX_COUNT, "global 无父");
    CHECK(actions_context_parent(CTX_TEXT_INPUT) == CTX_COUNT, "text_input 无父");

    /* 父链：main_lyric → main_list → main → global */
    CHECK(actions_context_parent(actions_context_parent(
              actions_context_parent(CTX_MAIN_LYRIC))) == CTX_GLOBAL,
          "main_lyric 链到 global");

    /* ── 默认表自身无冲突 ── */
    CHECK(default_conflicts() == 0, "默认键位在同一上下文内无冲突");

    /* ── 覆盖校验 ── */
    CHECK(actions_validate_overrides(NULL, 0, NULL, 0) == 0, "空覆盖表合法");

    KeyBindingOverride ov;
    make_override(&ov, "track.next", "]");
    CHECK(actions_validate_overrides(&ov, 1, NULL, 0) == 0, "合法覆盖");

    make_override(&ov, "unknown.action", "x");
    CHECK(actions_validate_overrides(&ov, 1, NULL, 0) == -1, "未知 action 拒绝");

    make_override(&ov, "list.up", "esc");
    CHECK(actions_validate_overrides(&ov, 1, NULL, 0) == -1, "esc 作为绑定值拒绝");

    make_override(&ov, "list.up", "ctrl+c");
    CHECK(actions_validate_overrides(&ov, 1, NULL, 0) == -1, "ctrl+c 拒绝");

    make_override(&ov, "list.up", "");
    CHECK(actions_validate_overrides(&ov, 1, NULL, 0) == -1, "空键名拒绝");

    /* 同上下文冲突：folder.open 改绑 n，与 track.next 默认 n 冲突 */
    make_override(&ov, "folder.open", "n");
    {
        char err[256];
        CHECK(actions_validate_overrides(&ov, 1, err, sizeof(err)) == -1,
              "同上下文冲突拒绝");
        CHECK(strstr(err, "folder.open") != NULL && strstr(err, "track.next") != NULL &&
              strstr(err, "n") != NULL, "冲突错误包含 action 与键名");
    }

    /* 不同上下文允许同键：control.left 用 q 不受 global 的 app.quit q 影响 */
    make_override(&ov, "control.left", "q");
    CHECK(actions_validate_overrides(&ov, 1, NULL, 0) == 0, "不同上下文允许同键");

    /* 系统保留动作的覆盖仍可被配置加载接受（运行时忽略） */
    make_override(&ov, "app.quit", "x");
    CHECK(actions_validate_overrides(&ov, 1, NULL, 0) == 0,
          "锁定动作覆盖可加载但运行时忽略");

    /* 锁定动作不参与冲突：view.settings=q 不会与 app.quit 默认 q 冲突 */
    make_override(&ov, "view.settings", "q");
    CHECK(actions_validate_overrides(&ov, 1, NULL, 0) == 0,
          "锁定动作不参与冲突计算");

    /* 覆盖后别名失效：track.next 改绑 x 后，folder.open 可以使用 n */
    {
        KeyBindingOverride pair[2];
        make_override(&pair[0], "track.next", "x");
        make_override(&pair[1], "folder.open", "n");
        CHECK(actions_validate_overrides(pair, 2, NULL, 0) == 0,
              "覆盖后动作的默认别名失效");
    }

    /* 重复 action 拒绝 */
    {
        KeyBindingOverride pair[2];
        make_override(&pair[0], "list.up", "w");
        make_override(&pair[1], "list.up", "e");
        CHECK(actions_validate_overrides(pair, 2, NULL, 0) == -1,
              "重复 action 拒绝");
    }

    /* 覆盖（单键）本身不再与旧默认键冲突 */
    {
        KeyBindingOverride pair[2];
        make_override(&pair[0], "track.next", "]");
        make_override(&pair[1], "folder.open", "n");
        CHECK(actions_validate_overrides(pair, 2, NULL, 0) == 0,
              "改绑后旧默认键释放");
    }

    /* ── override_index ── */
    {
        KeyBindingOverride pair[2];
        make_override(&pair[0], "app.quit", "x");
        make_override(&pair[1], "list.up", "w");
        CHECK(actions_override_index(pair, 2, "list.up", "w") == 1, "override_index 命中");
        CHECK(actions_override_index(pair, 2, "list.up", "q") == -1, "override_index 键不符");
        CHECK(actions_override_index(pair, 2, "view.help", "f8") == -1, "override_index 未命中");
    }

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS test_actions\n");
        return 0;
    }
    printf("FAIL test_actions\n");
    return 1;
}
