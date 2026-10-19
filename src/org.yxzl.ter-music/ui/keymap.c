/**
 * @file keymap.c
 * @brief UI 键位运行时实现（见 ui/keymap.h）
 */

#include "ui/keymap.h"

#include <ncursesw/ncurses.h>

#include <stdio.h>
#include <string.h>

#include "config/config.h"
#include "i18n/i18n.h"
#include "logger/logger.h"

static KeyBindingOverride g_runtime[MAX_KEYBINDING_OVERRIDES];
static int g_runtime_count = 0;
static int g_loaded = 0;

static int runtime_find(ActionId id)
{
    const KeyActionDef *def = actions_def(id);
    if (!def) {
        return -1;
    }
    for (int i = 0; i < g_runtime_count; i++) {
        if (strcmp(g_runtime[i].action, def->id) == 0) {
            return i;
        }
    }
    return -1;
}

static int stroke_from_keycode(int ch, KeyStroke *out)
{
    if (!out) {
        return -1;
    }
    out->kind = KEYNAME_UNKNOWN;
    out->code = 0;

    switch (ch) {
        case KEY_UP:    out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_UP;        return 0;
        case KEY_DOWN:  out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_DOWN;      return 0;
        case KEY_LEFT:  out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_LEFT;      return 0;
        case KEY_RIGHT: out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_RIGHT;     return 0;
        case KEY_HOME:  out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_HOME;      return 0;
        case KEY_END:   out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_END;       return 0;
        case KEY_PPAGE: out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_PGUP;      return 0;
        case KEY_NPAGE: out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_PGDN;      return 0;
        case KEY_IC:    out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_INS;       return 0;
        case KEY_DC:    out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_DEL;       return 0;
        case KEY_BACKSPACE: out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_BACKSPACE; return 0;
        case KEY_BTAB:  out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_BTAB;      return 0;
        case KEY_ENTER: out->kind = KEYNAME_SPECIAL; out->code = KEYSPEC_ENTER;     return 0;
        default:
            break;
    }

    for (int n = 1; n <= 12; n++) {
        if (ch == KEY_F(n)) {
            out->kind = KEYNAME_SPECIAL;
            out->code = KEYSPEC_F1 + (n - 1);
            return 0;
        }
    }

    if (ch == 27) {
        out->kind = KEYNAME_SPECIAL;
        out->code = KEYSPEC_ESC;
        return 0;
    }
    if (ch == 8 || ch == 127) {
        out->kind = KEYNAME_SPECIAL;
        out->code = KEYSPEC_BACKSPACE;
        return 0;
    }
    if (ch == 9) {
        out->kind = KEYNAME_SPECIAL;
        out->code = KEYSPEC_TAB;
        return 0;
    }
    if (ch == 10 || ch == 13) {
        out->kind = KEYNAME_SPECIAL;
        out->code = KEYSPEC_ENTER;
        return 0;
    }
    if (ch == ' ') {
        out->kind = KEYNAME_SPECIAL;
        out->code = KEYSPEC_SPACE;
        return 0;
    }
    if (ch >= 0x21 && ch <= 0x7E) {
        out->kind = KEYNAME_CHAR;
        out->code = ch;
        return 0;
    }
    /* 控制字符 1..26：8/9/10/13 已在上面归入特殊键 */
    if (ch >= 1 && ch <= 26) {
        out->kind = KEYNAME_CTRL;
        out->code = 'a' + (ch - 1);
        return 0;
    }

    return -1;
}

void keymap_reload(void)
{
    g_runtime_count = 0;
    g_loaded = 1;

    int stored = g_app_config.keybinding_override_count;
    if (stored < 0) {
        stored = 0;
    }
    if (stored > MAX_KEYBINDING_OVERRIDES) {
        stored = MAX_KEYBINDING_OVERRIDES;
    }

    for (int i = 0; i < stored; i++) {
        const KeyBindingOverride *item = &g_app_config.keybinding_overrides[i];
        char action[MAX_KEYBINDING_ACTION_LEN];
        char key[MAX_KEYBINDING_KEY_LEN];
        memcpy(action, item->action, sizeof(action));
        action[sizeof(action) - 1] = '\0';
        memcpy(key, item->key, sizeof(key));
        key[sizeof(key) - 1] = '\0';

        ActionId id = actions_from_id(action);
        if (id == ACTION_NONE) {
            log_warn("keymap", "Ignoring override for unknown action '%s'", action);
            continue;
        }
        if (!actions_is_bindable(id)) {
            log_warn("keymap", "Ignoring override for locked action '%s'", action);
            continue;
        }

        KeyStroke stroke;
        if (keyname_parse(key, &stroke) != 0 || !keyname_is_bindable(stroke)) {
            log_warn("keymap", "Ignoring invalid key '%s' for action '%s'",
                     key, action);
            continue;
        }

        int duplicate = 0;
        for (int j = 0; j < g_runtime_count; j++) {
            if (strcmp(g_runtime[j].action, action) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            log_warn("keymap", "Ignoring duplicate override for action '%s'", action);
            continue;
        }

        KeyBindingOverride candidate[MAX_KEYBINDING_OVERRIDES];
        memcpy(candidate, g_runtime, (size_t)g_runtime_count * sizeof(candidate[0]));
        snprintf(candidate[g_runtime_count].action,
                 sizeof(candidate[g_runtime_count].action), "%s", action);
        snprintf(candidate[g_runtime_count].key,
                 sizeof(candidate[g_runtime_count].key), "%s", key);

        char err[256];
        if (actions_validate_overrides(candidate, g_runtime_count + 1,
                                       err, sizeof(err)) != 0) {
            log_warn("keymap", "Ignoring override '%s' -> '%s': %s",
                     action, key, err);
            continue;
        }

        g_runtime[g_runtime_count] = candidate[g_runtime_count];
        g_runtime_count++;
    }
}

void keymap_ensure_loaded(void)
{
    if (!g_loaded) {
        keymap_reload();
    }
}

int keymap_action_keys(ActionId id, char out[][KEYNAME_MAX_LEN], int max)
{
    keymap_ensure_loaded();

    const KeyActionDef *def = actions_def(id);
    if (!def || !out || max <= 0) {
        return 0;
    }

    int index = runtime_find(id);
    if (index >= 0) {
        KeyStroke stroke;
        if (keyname_parse(g_runtime[index].key, &stroke) == 0 &&
            keyname_format(stroke, out[0], KEYNAME_MAX_LEN) == 0) {
            return 1;
        }
    }

    return actions_default_keys(id, out, max);
}

int keymap_action_default_keys(ActionId id, char out[][KEYNAME_MAX_LEN], int max)
{
    return actions_default_keys(id, out, max);
}

int keymap_action_has_override(ActionId id)
{
    keymap_ensure_loaded();
    return runtime_find(id) >= 0;
}

int keymap_set_override(ActionId id, const char *key_name,
                        char *err, size_t err_size)
{
    if (err && err_size) {
        err[0] = '\0';
    }
    keymap_ensure_loaded();

    const KeyActionDef *def = actions_def(id);
    if (!def) {
        if (err && err_size) {
            snprintf(err, err_size, "unknown action");
        }
        return -1;
    }
    if (!actions_is_bindable(id)) {
        if (err && err_size) {
            snprintf(err, err_size, "action '%s' is locked", def->id);
        }
        return -1;
    }
    if (!key_name || key_name[0] == '\0') {
        if (err && err_size) {
            snprintf(err, err_size, "empty key name");
        }
        return -1;
    }

    KeyStroke stroke;
    if (keyname_parse(key_name, &stroke) != 0 || !keyname_is_bindable(stroke)) {
        if (err && err_size) {
            snprintf(err, err_size, "invalid key '%s'", key_name);
        }
        return -1;
    }

    KeyBindingOverride candidate[MAX_KEYBINDING_OVERRIDES];
    int count = g_runtime_count;
    memcpy(candidate, g_runtime, (size_t)count * sizeof(candidate[0]));

    int index = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(candidate[i].action, def->id) == 0) {
            index = i;
            break;
        }
    }

    if (index >= 0) {
        snprintf(candidate[index].key, sizeof(candidate[index].key),
                 "%s", key_name);
    } else {
        if (count >= MAX_KEYBINDING_OVERRIDES) {
            if (err && err_size) {
                snprintf(err, err_size, "too many keybinding overrides");
            }
            return -1;
        }
        index = count++;
        snprintf(candidate[index].action, sizeof(candidate[index].action),
                 "%s", def->id);
        snprintf(candidate[index].key, sizeof(candidate[index].key),
                 "%s", key_name);
    }

    if (actions_validate_overrides(candidate, count, err, err_size) != 0) {
        return -1;
    }

    memcpy(g_app_config.keybinding_overrides, candidate,
           (size_t)count * sizeof(candidate[0]));
    g_app_config.keybinding_override_count = count;
    log_debug("keymap", "override set: %s -> %s (total=%d)",
              def->id, key_name, count);
    keymap_reload();
    return 0;
}

int keymap_remove_override(ActionId id)
{
    const KeyActionDef *def = actions_def(id);
    if (!def) {
        return -1;
    }
    keymap_ensure_loaded();

    int stored = g_app_config.keybinding_override_count;
    if (stored < 0) {
        stored = 0;
    }
    if (stored > MAX_KEYBINDING_OVERRIDES) {
        stored = MAX_KEYBINDING_OVERRIDES;
    }

    int out = 0;
    for (int i = 0; i < stored; i++) {
        if (strcmp(g_app_config.keybinding_overrides[i].action, def->id) == 0) {
            continue;
        }
        if (out != i) {
            g_app_config.keybinding_overrides[out] =
                g_app_config.keybinding_overrides[i];
        }
        out++;
    }
    g_app_config.keybinding_override_count = out;
    keymap_reload();
    return 0;
}

void keymap_remove_all_bindable_overrides(void)
{
    int stored = g_app_config.keybinding_override_count;
    if (stored < 0) {
        stored = 0;
    }
    if (stored > MAX_KEYBINDING_OVERRIDES) {
        stored = MAX_KEYBINDING_OVERRIDES;
    }

    int out = 0;
    for (int i = 0; i < stored; i++) {
        ActionId id = actions_from_id(g_app_config.keybinding_overrides[i].action);
        if (id != ACTION_NONE && !actions_is_bindable(id)) {
            /* 系统保留动作的旧覆盖保留在文件中，仅由运行时忽略。 */
            if (out != i) {
                g_app_config.keybinding_overrides[out] =
                    g_app_config.keybinding_overrides[i];
            }
            out++;
        }
    }
    if (out < MAX_KEYBINDING_OVERRIDES) {
        memset(g_app_config.keybinding_overrides + out, 0,
               (size_t)(MAX_KEYBINDING_OVERRIDES - out) *
               sizeof(g_app_config.keybinding_overrides[0]));
    }
    g_app_config.keybinding_override_count = out;
    keymap_reload();
}

ActionId keymap_resolve(KeyContext ctx, int ch)
{
    keymap_ensure_loaded();

    KeyStroke stroke;
    if (stroke_from_keycode(ch, &stroke) != 0) {
        return ACTION_NONE;
    }
    if (!keyname_is_bindable(stroke)) {
        return ACTION_NONE;
    }

    for (KeyContext current = ctx; current != CTX_COUNT;
         current = actions_context_parent(current)) {
        if (current == CTX_TEXT_INPUT) {
            return ACTION_NONE;
        }

        for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
            ActionId id = (ActionId)a;
            if (actions_context(id) != current) {
                continue;
            }

            char keys[4][KEYNAME_MAX_LEN];
            int count = keymap_action_keys(id, keys, 4);
            for (int i = 0; i < count; i++) {
                KeyStroke candidate;
                if (keyname_parse(keys[i], &candidate) == 0 &&
                    keyname_equal(candidate, stroke)) {
                    return id;
                }
            }
        }
    }

    return ACTION_NONE;
}

int keymap_keycode_to_name(int ch, char *out, size_t out_size)
{
    KeyStroke stroke;
    if (stroke_from_keycode(ch, &stroke) != 0) {
        return -1;
    }
    return keyname_format(stroke, out, out_size);
}

int keymap_keycode_to_stroke(int ch, KeyStroke *out)
{
    return stroke_from_keycode(ch, out);
}

int keymap_is_reserved_keycode(int ch)
{
    return (ch == 27 || ch == 3) ? 1 : 0;
}

const char *keymap_action_label(ActionId id)
{
    const KeyActionDef *def = actions_def(id);
    if (!def) {
        return "";
    }
    const char *translated = i18n_get(def->i18n_key);
    if (!translated || strcmp(translated, def->i18n_key) == 0) {
        return def->id;
    }
    return translated;
}
