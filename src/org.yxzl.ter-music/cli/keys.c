/**
 * @file keys.c
 * @brief `ter-music keys list|check [--json]` 本地键位审计
 *
 * 不要求运行实例、不走 D-Bus：只读配置，重建运行时键位表后输出人读或
 * JSON 结果。`check` 还会用 config_load_raw_keybindings() 读取原始覆盖，
 * 以便发现被运行时加载降级掉的非法项。
 *
 * terminfo 能力检查放在本文件，避免 <term.h> 的 terminfo 小写宏
 *（lines/columns/tab 等）污染 cli.c 的普通标识符。
 */

#include "cli/cli.h"

#include "config/config.h"
#include "i18n/i18n.h"
#include "keymap/actions.h"
#include "types.h"
#include "ui/keymap.h"
#include "util/json.h"
#include "util/keyname.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* term.h 必须放在其余头文件之后：它会定义大量 terminfo 变量名宏。 */
#include <ncursesw/term.h>

#define KEY_AUDIT_MAX_MESSAGES 512
#define KEY_AUDIT_MESSAGE_LEN  320
#define KEY_AUDIT_JSON_CAP     (4u * 1024u * 1024u)

typedef struct {
    char items[KEY_AUDIT_MAX_MESSAGES][KEY_AUDIT_MESSAGE_LEN];
    int count;
} KeyAuditMessages;

typedef struct {
    ActionId child;
    ActionId parent;
    char key[KEYNAME_MAX_LEN];
    int via_override;   /* 1 = 至少一侧键来自用户覆盖（应为 0，否则是错误） */
} KeyShadow;

typedef struct {
    KeyShadow *items;
    int count;
    int capacity;
} KeyShadowList;

static KeyAuditMessages g_key_errors;
static KeyAuditMessages g_key_warnings;

static void msg_add(KeyAuditMessages *list, const char *fmt, ...)
{
    if (!list || list->count >= KEY_AUDIT_MAX_MESSAGES) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(list->items[list->count], KEY_AUDIT_MESSAGE_LEN, fmt, args);
    va_end(args);
    list->count++;
}

static void shadows_free(KeyShadowList *list)
{
    if (!list) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int shadows_add(KeyShadowList *list, ActionId child, ActionId parent,
                       const char *key, int via_override)
{
    if (!list || !key) {
        return -1;
    }
    if (list->count >= list->capacity) {
        int next = list->capacity > 0 ? list->capacity * 2 : 32;
        KeyShadow *grown = realloc(list->items, (size_t)next * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        list->items = grown;
        list->capacity = next;
    }
    KeyShadow *item = &list->items[list->count++];
    item->child = child;
    item->parent = parent;
    item->via_override = via_override;
    snprintf(item->key, sizeof(item->key), "%s", key);
    return 0;
}

static void keys_prepare(void)
{
    ensure_config_dir_exists();
    init_default_config();
    /* 只读审计：直接读 config.xml，不触发 v1 JSON 迁移或任何写盘路径。 */
    const char *path = get_config_file_path();
    if (path && access(path, R_OK) == 0) {
        config_load_from_xml(path, &g_app_config);
    }
    keymap_reload();
    i18n_init(g_app_config.ui_language);
}

static void keys_print_usage(const char *program)
{
    fprintf(stderr, "用法：%s keys list|check [--json]\n", program ? program : "ter-music");
    fprintf(stderr, "  keys list   列出全部动作的上下文、名称、有效键、默认键与遮蔽关系\n");
    fprintf(stderr, "  keys check  严格审计配置；非法项/冲突/无默认键时退出码非 0\n");
    fprintf(stderr, "  --json      输出机器可读 JSON（便于 CI）\n");
}

static void join_keys(char keys[][KEYNAME_MAX_LEN], int count,
                      char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (count <= 0) {
        snprintf(out, out_size, "(无)");
        return;
    }

    size_t pos = 0;
    out[0] = '\0';
    for (int i = 0; i < count && pos + 1 < out_size; i++) {
        int written = snprintf(out + pos, out_size - pos, "%s%s",
                               i == 0 ? "" : "/", keys[i]);
        if (written < 0) {
            break;
        }
        pos += (size_t)written;
    }
}

static int count_missing_defaults(void)
{
    int missing = 0;
    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        char defaults[4][KEYNAME_MAX_LEN];
        if (actions_default_keys((ActionId)a, defaults, 4) == 0) {
            missing++;
        }
    }
    return missing;
}

/* 用原始覆盖 + 默认键构造审计视图（不含非法项）。
 * 与运行时一样：覆盖项替换该动作的全部默认键。 */
static int effective_keys_from_overrides(ActionId id,
                                         const KeyBindingOverride *overrides,
                                         int count,
                                         char out[][KEYNAME_MAX_LEN], int max)
{
    const KeyActionDef *def = actions_def(id);
    if (!def || !out || max <= 0) {
        return 0;
    }

    for (int i = 0; i < count; i++) {
        if (strcmp(overrides[i].action, def->id) != 0) {
            continue;
        }
        KeyStroke stroke;
        if (keyname_parse(overrides[i].key, &stroke) != 0 ||
            !keyname_is_bindable(stroke)) {
            return 0;
        }
        if (keyname_format(stroke, out[0], KEYNAME_MAX_LEN) != 0) {
            return 0;
        }
        return 1;
    }

    return actions_default_keys(id, out, max);
}

/* 原始覆盖里的同上下文冲突（运行时会把冲突项降级，因此不能只看运行时表）。 */
static int audit_raw_conflicts(const KeyBindingOverride *overrides, int count,
                               KeyAuditMessages *errors)
{
    int conflicts = 0;

    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        ActionId id_a = (ActionId)a;
        const KeyActionDef *def_a = actions_def(id_a);
        if (!def_a || !actions_is_bindable(id_a)) {
            continue;
        }
        char keys_a[4][KEYNAME_MAX_LEN];
        int count_a = effective_keys_from_overrides(id_a, overrides, count,
                                                    keys_a, 4);

        for (int b = a + 1; b < ACTION_COUNT; b++) {
            ActionId id_b = (ActionId)b;
            const KeyActionDef *def_b = actions_def(id_b);
            if (!def_b || !actions_is_bindable(id_b) ||
                def_a->context != def_b->context) {
                continue;
            }
            char keys_b[4][KEYNAME_MAX_LEN];
            int count_b = effective_keys_from_overrides(id_b, overrides, count,
                                                        keys_b, 4);
            for (int i = 0; i < count_a; i++) {
                for (int j = 0; j < count_b; j++) {
                    if (strcmp(keys_a[i], keys_b[j]) != 0) {
                        continue;
                    }
                    conflicts++;
                    msg_add(errors,
                            "冲突：上下文 %s 中 %s 与 %s 都使用 %s",
                            actions_context_id(def_a->context),
                            def_a->id, def_b->id, keys_a[i]);
                }
            }
        }
    }
    return conflicts;
}

static int collect_shadows(KeyShadowList *out)
{
    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        ActionId id_child = (ActionId)a;
        const KeyActionDef *def_child = actions_def(id_child);
        if (!def_child) {
            continue;
        }

        char child_keys[4][KEYNAME_MAX_LEN];
        int child_count = keymap_action_keys(id_child, child_keys, 4);
        if (child_count <= 0) {
            continue;
        }

        for (KeyContext parent = actions_context_parent(def_child->context);
             parent != CTX_COUNT; parent = actions_context_parent(parent)) {
            if (parent == CTX_TEXT_INPUT) {
                continue;
            }

            for (int b = ACTION_NONE + 1; b < ACTION_COUNT; b++) {
                ActionId id_parent = (ActionId)b;
                const KeyActionDef *def_parent = actions_def(id_parent);
                if (!def_parent || def_parent->context != parent) {
                    continue;
                }

                char parent_keys[4][KEYNAME_MAX_LEN];
                int parent_count = keymap_action_keys(id_parent, parent_keys, 4);
                for (int i = 0; i < child_count; i++) {
                    for (int j = 0; j < parent_count; j++) {
                        if (strcmp(child_keys[i], parent_keys[j]) != 0) {
                            continue;
                        }
                        int via_override =
                            keymap_action_has_override(id_child) ||
                            keymap_action_has_override(id_parent);
                        if (shadows_add(out, id_child, id_parent,
                                        child_keys[i], via_override) != 0) {
                            return -1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

static void audit_terminfo(KeyAuditMessages *warnings)
{
    int present[KEYSPEC_F12 + 1];
    memset(present, 0, sizeof(present));

    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        char keys[4][KEYNAME_MAX_LEN];
        int count = keymap_action_keys((ActionId)a, keys, 4);
        for (int i = 0; i < count; i++) {
            KeyStroke stroke;
            if (keyname_parse(keys[i], &stroke) == 0 &&
                stroke.kind == KEYNAME_SPECIAL &&
                stroke.code >= KEYSPEC_UP && stroke.code <= KEYSPEC_F12) {
                present[stroke.code] = 1;
            }
        }
    }

    const char *term = getenv("TERM");
    if (!term || term[0] == '\0') {
        msg_add(warnings, "TERM 未设置，跳过 terminfo 能力检查");
        return;
    }

    int errret = 0;
    if (setupterm(NULL, STDOUT_FILENO, &errret) != 0) {
        msg_add(warnings, "setupterm 失败（TERM=%s），跳过 terminfo 能力检查", term);
        return;
    }

    static const struct {
        int spec;
        const char *cap;
        const char *name;
    } caps[] = {
        {KEYSPEC_UP,        "kcuu1", "up"},
        {KEYSPEC_DOWN,      "kcud1", "down"},
        {KEYSPEC_LEFT,      "kcub1", "left"},
        {KEYSPEC_RIGHT,     "kcuf1", "right"},
        {KEYSPEC_HOME,      "khome", "home"},
        {KEYSPEC_END,       "kend",  "end"},
        {KEYSPEC_PGUP,      "kpp",   "pgup"},
        {KEYSPEC_PGDN,      "knp",   "pgdn"},
        {KEYSPEC_INS,       "kich1", "ins"},
        {KEYSPEC_DEL,       "kdch1", "del"},
        {KEYSPEC_BACKSPACE, "kbs",   "backspace"},
        {KEYSPEC_BTAB,      "kcbt",  "btab"},
    };

    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        if (!present[caps[i].spec]) {
            continue;
        }
        char *cap = tigetstr(caps[i].cap);
        if (cap == NULL || cap == (char *)-1) {
            msg_add(warnings, "terminfo 缺少能力 %s（%s，TERM=%s）",
                    caps[i].cap, caps[i].name, term);
        }
    }

    for (int n = 1; n <= 12; n++) {
        int spec = KEYSPEC_F1 + (n - 1);
        if (!present[spec]) {
            continue;
        }
        char cap_name[8];
        snprintf(cap_name, sizeof(cap_name), "kf%d", n);
        char *cap = tigetstr(cap_name);
        if (cap == NULL || cap == (char *)-1) {
            msg_add(warnings, "terminfo 缺少能力 %s（f%d，TERM=%s）",
                    cap_name, n, term);
        }
    }

    del_curterm(cur_term);
}

/* ── keys list ──────────────────────────────────────────────────── */

static int keys_list_json(const KeyShadowList *shadows)
{
    char *buffer = malloc(KEY_AUDIT_JSON_CAP);
    if (!buffer) {
        fprintf(stderr, "错误：内存不足，无法生成 JSON。\n");
        return CLI_EXIT_USAGE;
    }

    size_t pos = 0;
    size_t cap = KEY_AUDIT_JSON_CAP;
    pos = json_append_raw(buffer, cap, pos, "{\"actions\":[");
    int first = 1;

    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        ActionId id = (ActionId)a;
        const KeyActionDef *def = actions_def(id);
        if (!def) {
            continue;
        }

        char keys[4][KEYNAME_MAX_LEN];
        char defaults[4][KEYNAME_MAX_LEN];
        int key_count = keymap_action_keys(id, keys, 4);
        int default_count = keymap_action_default_keys(id, defaults, 4);

        if (!first) {
            pos = json_append_char(buffer, cap, pos, ',');
        }
        first = 0;

        pos = json_append_raw(buffer, cap, pos, "{\"id\":");
        pos = json_append_escaped(buffer, cap, pos, def->id);
        pos = json_append_raw(buffer, cap, pos, ",\"context\":");
        pos = json_append_escaped(buffer, cap, pos,
                                  actions_context_id(def->context));
        pos = json_append_raw(buffer, cap, pos, ",\"label\":");
        pos = json_append_escaped(buffer, cap, pos, keymap_action_label(id));

        pos = json_append_raw(buffer, cap, pos, ",\"keys\":[");
        for (int i = 0; i < key_count; i++) {
            if (i > 0) {
                pos = json_append_char(buffer, cap, pos, ',');
            }
            pos = json_append_escaped(buffer, cap, pos, keys[i]);
        }
        pos = json_append_raw(buffer, cap, pos, "],\"defaults\":[");
        for (int i = 0; i < default_count; i++) {
            if (i > 0) {
                pos = json_append_char(buffer, cap, pos, ',');
            }
            pos = json_append_escaped(buffer, cap, pos, defaults[i]);
        }
        pos = json_append_raw(buffer, cap, pos, "],\"overridden\":");
        pos = json_append_bool(buffer, cap, pos, keymap_action_has_override(id));
        pos = json_append_raw(buffer, cap, pos, ",\"bindable\":");
        pos = json_append_bool(buffer, cap, pos, actions_is_bindable(id));
        pos = json_append_char(buffer, cap, pos, '}');
    }

    pos = json_append_raw(buffer, cap, pos, "],\"shadows\":[");
    int first_shadow = 1;
    for (int i = 0; i < shadows->count; i++) {
        const KeyShadow *shadow = &shadows->items[i];
        const KeyActionDef *child = actions_def(shadow->child);
        const KeyActionDef *parent = actions_def(shadow->parent);
        if (!child || !parent) {
            continue;
        }
        if (!first_shadow) {
            pos = json_append_char(buffer, cap, pos, ',');
        }
        first_shadow = 0;
        pos = json_append_raw(buffer, cap, pos, "{\"child\":");
        pos = json_append_escaped(buffer, cap, pos, child->id);
        pos = json_append_raw(buffer, cap, pos, ",\"parent\":");
        pos = json_append_escaped(buffer, cap, pos, parent->id);
        pos = json_append_raw(buffer, cap, pos, ",\"key\":");
        pos = json_append_escaped(buffer, cap, pos, shadow->key);
        pos = json_append_raw(buffer, cap, pos, ",\"via_override\":");
        pos = json_append_bool(buffer, cap, pos, shadow->via_override);
        pos = json_append_char(buffer, cap, pos, '}');
    }
    pos = json_append_raw(buffer, cap, pos, "]}");

    if (json_writer_overflowed(cap, pos)) {
        free(buffer);
        fprintf(stderr, "错误：键位审计 JSON 超出缓冲上限。\n");
        return CLI_EXIT_USAGE;
    }

    printf("%s\n", buffer);
    free(buffer);
    return CLI_EXIT_OK;
}

static int keys_list_human(const KeyShadowList *shadows)
{
    for (int ctx = 0; ctx < CTX_COUNT; ctx++) {
        int has_action = 0;
        for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
            if (actions_context((ActionId)a) == (KeyContext)ctx) {
                has_action = 1;
                break;
            }
        }
        if (!has_action) {
            continue;
        }

        printf("[%s]\n", actions_context_id((KeyContext)ctx));
        for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
            ActionId id = (ActionId)a;
            const KeyActionDef *def = actions_def(id);
            if (!def || def->context != (KeyContext)ctx) {
                continue;
            }

            char keys[4][KEYNAME_MAX_LEN];
            char defaults[4][KEYNAME_MAX_LEN];
            char keys_text[4 * KEYNAME_MAX_LEN] = "";
            char defaults_text[4 * KEYNAME_MAX_LEN] = "";
            int key_count = keymap_action_keys(id, keys, 4);
            int default_count = keymap_action_default_keys(id, defaults, 4);
            join_keys(keys, key_count, keys_text, sizeof(keys_text));
            join_keys(defaults, default_count, defaults_text,
                      sizeof(defaults_text));

            int overridden = keymap_action_has_override(id);
            printf("  %-30s %-24s %s", def->id, keymap_action_label(id),
                   keys_text);
            if (!actions_is_bindable(id)) {
                printf("  [锁定]  [默认: %s]", defaults_text);
            } else if (overridden) {
                printf("  [*覆盖]  [默认: %s]", defaults_text);
            }
            printf("\n");
        }
        printf("\n");
    }

    printf("遮蔽关系（子上下文动作 -> 父上下文动作）：\n");
    if (shadows->count == 0) {
        printf("  无\n");
    } else {
        for (int i = 0; i < shadows->count; i++) {
            const KeyShadow *shadow = &shadows->items[i];
            const KeyActionDef *child = actions_def(shadow->child);
            const KeyActionDef *parent = actions_def(shadow->parent);
            if (!child || !parent) {
                continue;
            }
            printf("  %s 遮蔽 %s（%s）%s\n", child->id, parent->id, shadow->key,
                   shadow->via_override ? "［覆盖引入-应修复］" : "");
        }
    }
    return CLI_EXIT_OK;
}

static int keys_cmd_list(int want_json)
{
    KeyShadowList shadows = {0};
    if (collect_shadows(&shadows) != 0) {
        shadows_free(&shadows);
        fprintf(stderr, "错误：内存不足，无法收集遮蔽关系。\n");
        return CLI_EXIT_USAGE;
    }

    int rc = want_json ? keys_list_json(&shadows)
                       : keys_list_human(&shadows);
    shadows_free(&shadows);
    return rc;
}

/* ── keys check ─────────────────────────────────────────────────── */

static int keys_check_json(int ok, int actions, int overrides,
                           int locked_overrides, int invalid,
                           int conflicts, int missing_defaults, int shadows,
                           int override_shadows,
                           const KeyAuditMessages *errors,
                           const KeyAuditMessages *warnings)
{
    char *buffer = malloc(KEY_AUDIT_JSON_CAP);
    if (!buffer) {
        fprintf(stderr, "错误：内存不足，无法生成 JSON。\n");
        return CLI_EXIT_USAGE;
    }

    size_t pos = 0;
    size_t cap = KEY_AUDIT_JSON_CAP;
    pos = json_append_raw(buffer, cap, pos, "{\"ok\":");
    pos = json_append_bool(buffer, cap, pos, ok);
    pos = json_append_raw(buffer, cap, pos, ",\"actions\":");
    pos = json_append_int(buffer, cap, pos, actions);
    pos = json_append_raw(buffer, cap, pos, ",\"overrides\":");
    pos = json_append_int(buffer, cap, pos, overrides);
    pos = json_append_raw(buffer, cap, pos, ",\"locked_overrides\":");
    pos = json_append_int(buffer, cap, pos, locked_overrides);
    pos = json_append_raw(buffer, cap, pos, ",\"invalid\":");
    pos = json_append_int(buffer, cap, pos, invalid);
    pos = json_append_raw(buffer, cap, pos, ",\"conflicts\":");
    pos = json_append_int(buffer, cap, pos, conflicts);
    pos = json_append_raw(buffer, cap, pos, ",\"missing_defaults\":");
    pos = json_append_int(buffer, cap, pos, missing_defaults);
    pos = json_append_raw(buffer, cap, pos, ",\"shadows\":");
    pos = json_append_int(buffer, cap, pos, shadows);
    pos = json_append_raw(buffer, cap, pos, ",\"override_shadows\":");
    pos = json_append_int(buffer, cap, pos, override_shadows);

    pos = json_append_raw(buffer, cap, pos, ",\"errors\":[");
    for (int i = 0; i < errors->count; i++) {
        if (i > 0) {
            pos = json_append_char(buffer, cap, pos, ',');
        }
        pos = json_append_escaped(buffer, cap, pos, errors->items[i]);
    }
    pos = json_append_raw(buffer, cap, pos, "],\"warnings\":[");
    for (int i = 0; i < warnings->count; i++) {
        if (i > 0) {
            pos = json_append_char(buffer, cap, pos, ',');
        }
        pos = json_append_escaped(buffer, cap, pos, warnings->items[i]);
    }
    pos = json_append_raw(buffer, cap, pos, "]}");

    if (json_writer_overflowed(cap, pos)) {
        free(buffer);
        fprintf(stderr, "错误：键位审计 JSON 超出缓冲上限。\n");
        return CLI_EXIT_USAGE;
    }

    printf("%s\n", buffer);
    free(buffer);
    return CLI_EXIT_OK;
}

static int keys_check_human(int ok, int actions, int overrides,
                            int locked_overrides, int invalid,
                            int conflicts, int missing_defaults, int shadows,
                            int override_shadows,
                            const char *config_path,
                            const KeyAuditMessages *errors,
                            const KeyAuditMessages *warnings)
{
    printf("键位审计\n");
    printf("  配置文件：%s\n", config_path ? config_path : "(未初始化)");
    printf("  动作总数：%d\n", actions);
    printf("  覆盖数（原始条目）：%d\n", overrides);
    printf("  锁定动作覆盖（已忽略）：%d\n", locked_overrides);
    printf("  非法项：%d\n", invalid);
    printf("  冲突：%d\n", conflicts);
    printf("  无默认键动作：%d\n", missing_defaults);
    printf("  遮蔽数：%d（覆盖引入：%d，须为 0）\n", shadows,
           override_shadows);

    if (warnings->count > 0) {
        printf("警告：\n");
        for (int i = 0; i < warnings->count; i++) {
            printf("  - %s\n", warnings->items[i]);
        }
    }
    if (errors->count > 0) {
        printf("错误：\n");
        for (int i = 0; i < errors->count; i++) {
            printf("  - %s\n", errors->items[i]);
        }
    }

    printf("结论：%s\n", ok ? "通过" : "失败");
    return CLI_EXIT_OK;
}

static int keys_cmd_check(int want_json)
{
    memset(&g_key_errors, 0, sizeof(g_key_errors));
    memset(&g_key_warnings, 0, sizeof(g_key_warnings));

    int invalid = 0;
    int failed = 0;

    const char *config_path = get_config_file_path();
    KeyBindingOverride raw[MAX_KEYBINDING_OVERRIDES];
    memset(raw, 0, sizeof(raw));
    int raw_count = 0;

    if (config_path) {
        char raw_err[KEY_AUDIT_MESSAGE_LEN] = "";
        if (config_load_raw_keybindings(config_path, raw,
                                        MAX_KEYBINDING_OVERRIDES, &raw_count,
                                        raw_err, sizeof(raw_err)) != 0) {
            msg_add(&g_key_errors, "读取原始覆盖失败：%s",
                    raw_err[0] ? raw_err : "未知错误");
            failed = 1;
            raw_count = 0;
        }
    }

    /* 逐项找出非法 action/key 与重复 action；合法项进入审计视图。 */
    KeyBindingOverride valid_raw[MAX_KEYBINDING_OVERRIDES];
    memset(valid_raw, 0, sizeof(valid_raw));
    int valid_count = 0;
    int locked_overrides = 0;

    for (int i = 0; i < raw_count; i++) {
        ActionId id = actions_from_id(raw[i].action);
        if (id == ACTION_NONE) {
            invalid++;
            msg_add(&g_key_errors, "未知动作：action='%s' key='%s'",
                    raw[i].action, raw[i].key);
            continue;
        }

        KeyStroke stroke;
        if (keyname_parse(raw[i].key, &stroke) != 0 ||
            !keyname_is_bindable(stroke)) {
            invalid++;
            msg_add(&g_key_errors, "非法键：action='%s' key='%s'",
                    raw[i].action, raw[i].key);
            continue;
        }

        /* 系统保留动作：运行时忽略其覆盖；仅告警，不参与冲突审计，
         * 也不改变 keys check 的退出码（文件条目保留）。 */
        if (!actions_is_bindable(id)) {
            if (strcmp(raw[i].key, "default") != 0) {
                locked_overrides++;
                msg_add(&g_key_warnings,
                        "动作 '%s' 已锁定，覆盖被忽略（文件条目保留）",
                        raw[i].action);
            }
            continue;
        }

        int duplicate = 0;
        for (int j = 0; j < valid_count; j++) {
            if (strcmp(valid_raw[j].action, raw[i].action) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            invalid++;
            msg_add(&g_key_errors, "重复动作：action='%s'", raw[i].action);
            continue;
        }

        valid_raw[valid_count++] = raw[i];
    }

    /* 要求：用 actions_validate_overrides 对原始覆盖与运行时覆盖都校验一遍。 */
    char validate_err[KEY_AUDIT_MESSAGE_LEN] = "";
    if (actions_validate_overrides(raw, raw_count, validate_err,
                                   sizeof(validate_err)) != 0) {
        msg_add(&g_key_errors, "原始覆盖校验失败：%s", validate_err);
        failed = 1;
    }

    int runtime_count = g_app_config.keybinding_override_count;
    if (runtime_count < 0) {
        runtime_count = 0;
    }
    if (runtime_count > MAX_KEYBINDING_OVERRIDES) {
        runtime_count = MAX_KEYBINDING_OVERRIDES;
    }
    if (actions_validate_overrides(g_app_config.keybinding_overrides,
                                   runtime_count, validate_err,
                                   sizeof(validate_err)) != 0) {
        msg_add(&g_key_errors, "运行时覆盖校验失败：%s", validate_err);
        failed = 1;
    }

    int conflicts = audit_raw_conflicts(valid_raw, valid_count, &g_key_errors);
    int missing_defaults = count_missing_defaults();

    KeyShadowList shadows = {0};
    if (collect_shadows(&shadows) != 0) {
        msg_add(&g_key_errors, "内存不足，无法收集遮蔽关系");
        failed = 1;
    }

    audit_terminfo(&g_key_warnings);

    int override_shadows = 0;
    for (int i = 0; i < shadows.count; i++) {
        if (shadows.items[i].via_override) {
            override_shadows++;
        }
    }

    int ok = !failed && invalid == 0 && conflicts == 0 &&
             missing_defaults == 0 && override_shadows == 0;

    int rc;
    if (want_json) {
        rc = keys_check_json(ok, actions_count(), raw_count, locked_overrides,
                             invalid, conflicts, missing_defaults, shadows.count,
                             override_shadows,
                             &g_key_errors, &g_key_warnings);
    } else {
        rc = keys_check_human(ok, actions_count(), raw_count, locked_overrides,
                              invalid, conflicts, missing_defaults,
                              shadows.count, override_shadows, config_path,
                              &g_key_errors, &g_key_warnings);
    }

    shadows_free(&shadows);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    return ok ? CLI_EXIT_OK : CLI_EXIT_USAGE;
}

int cli_cmd_keys(int argc, char **argv)
{
    if (argc < 3) {
        keys_print_usage(argv[0]);
        return CLI_EXIT_USAGE;
    }

    const char *action = argv[2];
    int want_json = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            want_json = 1;
        } else {
            fprintf(stderr, "错误：未知的 keys 选项 '%s'。\n", argv[i]);
            keys_print_usage(argv[0]);
            return CLI_EXIT_USAGE;
        }
    }

    if (strcmp(action, "list") == 0) {
        keys_prepare();
        return keys_cmd_list(want_json);
    }
    if (strcmp(action, "check") == 0) {
        keys_prepare();
        return keys_cmd_check(want_json);
    }

    fprintf(stderr, "错误：未知 keys 子命令 '%s'。\n", action);
    keys_print_usage(argv[0]);
    return CLI_EXIT_USAGE;
}
