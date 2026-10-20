/**
 * @file actions.c
 * @brief 动作表与覆盖校验实现（见 keymap/actions.h）
 */

#include "keymap/actions.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ACT(id_str, ctx, a, b, c, d) \
    { id_str, "keyaction." id_str, ctx, { a, b, c, d } }

static const KeyActionDef k_actions[] = {
    /* GLOBAL */
    ACT("app.quit",              CTX_GLOBAL, "q", "Q", "f9", NULL),
    ACT("view.main",             CTX_GLOBAL, "f1", NULL, NULL, NULL),
    ACT("view.settings",         CTX_GLOBAL, "f2", NULL, NULL, NULL),
    ACT("view.history",          CTX_GLOBAL, "f3", NULL, NULL, NULL),
    ACT("view.playlist",         CTX_GLOBAL, "f4", NULL, NULL, NULL),
    ACT("view.favorites",        CTX_GLOBAL, "f5", NULL, NULL, NULL),
    ACT("view.info",             CTX_GLOBAL, "f6", NULL, NULL, NULL),
    ACT("view.language",         CTX_GLOBAL, "f7", NULL, NULL, NULL),
    ACT("view.help",             CTX_GLOBAL, "f8", NULL, NULL, NULL),
    ACT("library.toggle",        CTX_GLOBAL, "M", NULL, NULL, NULL),

    /* MAIN */
    ACT("volume.up",             CTX_MAIN, "+", "=", NULL, NULL),
    ACT("volume.down",           CTX_MAIN, "-", "_", NULL, NULL),
    ACT("core.restart",          CTX_MAIN, "R", NULL, NULL, NULL),
    ACT("focus.controls",        CTX_MAIN, "C", NULL, NULL, NULL),
    ACT("focus.list",            CTX_MAIN, "L", NULL, NULL, NULL),
    ACT("lyrics.seek_mode",      CTX_MAIN, "ctrl+l", NULL, NULL, NULL),

    /* MAIN_LIST */
    ACT("list.up",                   CTX_MAIN_LIST, "up", "k", NULL, NULL),
    ACT("list.down",                 CTX_MAIN_LIST, "down", "j", NULL, NULL),
    ACT("list.activate",             CTX_MAIN_LIST, "enter", "space", NULL, NULL),
    ACT("folder.open",               CTX_MAIN_LIST, "o", "O", NULL, NULL),
    ACT("queue.insert_next",         CTX_MAIN_LIST, "i", NULL, NULL, NULL),
    ACT("queue.append",              CTX_MAIN_LIST, "a", NULL, NULL, NULL),
    ACT("folder.append",             CTX_MAIN_LIST, "I", NULL, NULL, NULL),
    ACT("favorite.toggle",           CTX_MAIN_LIST, "f", "F", NULL, NULL),
    ACT("search.open",               CTX_MAIN_LIST, "/", "S", NULL, NULL),
    ACT("queue.remove",              CTX_MAIN_LIST, "d", NULL, NULL, NULL),
    ACT("queue.clear",               CTX_MAIN_LIST, "D", NULL, NULL, NULL),
    ACT("queue.move_down",           CTX_MAIN_LIST, "J", NULL, NULL, NULL),
    ACT("queue.move_up",             CTX_MAIN_LIST, "K", NULL, NULL, NULL),
    ACT("playmode.sequential",       CTX_MAIN_LIST, "1", NULL, NULL, NULL),
    ACT("playmode.single",           CTX_MAIN_LIST, "2", NULL, NULL, NULL),
    ACT("playmode.list",             CTX_MAIN_LIST, "3", NULL, NULL, NULL),
    ACT("playmode.shuffle",          CTX_MAIN_LIST, "4", NULL, NULL, NULL),
    ACT("playmode.folder",           CTX_MAIN_LIST, "5", NULL, NULL, NULL),
    ACT("track.next",                CTX_MAIN_LIST, "n", NULL, NULL, NULL),
    ACT("track.prev",                CTX_MAIN_LIST, "p", NULL, NULL, NULL),
    ACT("history.show",              CTX_MAIN_LIST, "h", NULL, NULL, NULL),
    ACT("playlist.add",              CTX_MAIN_LIST, "A", NULL, NULL, NULL),
    ACT("view.tab_switch",           CTX_MAIN_LIST, "tab", "btab", NULL, NULL),

    /* MAIN_LYRIC */
    ACT("lyrics.prev",               CTX_MAIN_LYRIC, "up", NULL, NULL, NULL),
    ACT("lyrics.next",               CTX_MAIN_LYRIC, "down", NULL, NULL, NULL),
    ACT("lyrics.source_switch",      CTX_MAIN_LYRIC, "tab", "btab", NULL, NULL),

    /* MAIN_SEARCH */
    ACT("search.up",                 CTX_MAIN_SEARCH, "up", NULL, NULL, NULL),
    ACT("search.down",               CTX_MAIN_SEARCH, "down", NULL, NULL, NULL),

    /* MAIN_CONTROL */
    ACT("control.up",                CTX_MAIN_CONTROL, "up", NULL, NULL, NULL),
    ACT("control.down",              CTX_MAIN_CONTROL, "down", NULL, NULL, NULL),
    ACT("control.left",              CTX_MAIN_CONTROL, "left", NULL, NULL, NULL),
    ACT("control.right",             CTX_MAIN_CONTROL, "right", NULL, NULL, NULL),
    ACT("seek.back",                 CTX_MAIN_CONTROL, ",", NULL, NULL, NULL),
    ACT("seek.forward",              CTX_MAIN_CONTROL, ".", NULL, NULL, NULL),
    ACT("control.activate",          CTX_MAIN_CONTROL, "space", NULL, NULL, NULL),

    /* POPUP */
    ACT("popup.up",                  CTX_POPUP, "up", NULL, NULL, NULL),
    ACT("popup.down",                CTX_POPUP, "down", NULL, NULL, NULL),
    ACT("popup.accept",              CTX_POPUP, "space", "enter", NULL, NULL),
    ACT("popup.cancel",              CTX_POPUP, "left", NULL, NULL, NULL),

    /* SELECT MENU */
    ACT("select.up",                 CTX_SELECT_MENU, "up", NULL, NULL, NULL),
    ACT("select.down",               CTX_SELECT_MENU, "down", NULL, NULL, NULL),
    ACT("select.toggle",             CTX_SELECT_MENU, "space", NULL, NULL, NULL),
    ACT("select.accept",             CTX_SELECT_MENU, "enter", NULL, NULL, NULL),
    ACT("select.all",                CTX_SELECT_MENU, "a", "A", NULL, NULL),
    ACT("select.none",               CTX_SELECT_MENU, "n", "N", NULL, NULL),
    ACT("select.cancel",             CTX_SELECT_MENU, "left", NULL, NULL, NULL),

    /* BROWSER */
    ACT("browser.up",                CTX_BROWSER, "up", NULL, NULL, NULL),
    ACT("browser.down",              CTX_BROWSER, "down", NULL, NULL, NULL),
    ACT("browser.activate",          CTX_BROWSER, "enter", "space", NULL, NULL),
    ACT("browser.scan",              CTX_BROWSER, "s", "S", NULL, NULL),
    ACT("browser.rescan",            CTX_BROWSER, "r", "R", NULL, NULL),
    ACT("browser.queue_append",      CTX_BROWSER, "a", "A", NULL, NULL),

    /* SETTINGS */
    ACT("settings.up",               CTX_SETTINGS, "up", NULL, NULL, NULL),
    ACT("settings.down",             CTX_SETTINGS, "down", NULL, NULL, NULL),
    ACT("settings.left",             CTX_SETTINGS, "left", NULL, NULL, NULL),
    ACT("settings.right",            CTX_SETTINGS, "right", NULL, NULL, NULL),
    ACT("settings.tab",              CTX_SETTINGS, "tab", NULL, NULL, NULL),
    ACT("settings.activate",         CTX_SETTINGS, "enter", "space", NULL, NULL),
    ACT("settings.save",             CTX_SETTINGS, "s", "S", NULL, NULL),
    ACT("settings.eq_presets",       CTX_SETTINGS, "p", "P", NULL, NULL),
    ACT("settings.hotkey_reset",     CTX_SETTINGS, "r", NULL, NULL, NULL),
    ACT("settings.hotkey_reset_all", CTX_SETTINGS, "R", NULL, NULL, NULL),

    /* SETTINGS_EQ */
    ACT("eq.band_prev",              CTX_SETTINGS_EQ, "left", NULL, NULL, NULL),
    ACT("eq.band_next",              CTX_SETTINGS_EQ, "right", NULL, NULL, NULL),
    ACT("eq.gain_up",                CTX_SETTINGS_EQ, "up", NULL, NULL, NULL),
    ACT("eq.gain_down",              CTX_SETTINGS_EQ, "down", NULL, NULL, NULL),
    ACT("eq.back",                   CTX_SETTINGS_EQ, "tab", NULL, NULL, NULL),
    ACT("eq.activate",               CTX_SETTINGS_EQ, "enter", "space", NULL, NULL),

    /* HISTORY */
    ACT("history.up",                CTX_HISTORY, "up", NULL, NULL, NULL),
    ACT("history.down",              CTX_HISTORY, "down", NULL, NULL, NULL),
    ACT("history.right",             CTX_HISTORY, "right", "tab", NULL, NULL),
    ACT("history.left",              CTX_HISTORY, "left", NULL, NULL, NULL),
    ACT("history.activate",          CTX_HISTORY, "enter", "space", NULL, NULL),
    ACT("history.queue_append",      CTX_HISTORY, "a", "A", NULL, NULL),
    ACT("history.delete",            CTX_HISTORY, "d", "D", NULL, NULL),
    ACT("history.clear",             CTX_HISTORY, "c", "C", NULL, NULL),

    /* PLAYLIST_VIEW */
    ACT("playlist_view.up",          CTX_PLAYLIST_VIEW, "up", NULL, NULL, NULL),
    ACT("playlist_view.down",        CTX_PLAYLIST_VIEW, "down", NULL, NULL, NULL),
    ACT("playlist_view.right",       CTX_PLAYLIST_VIEW, "right", "tab", NULL, NULL),
    ACT("playlist_view.left",        CTX_PLAYLIST_VIEW, "left", NULL, NULL, NULL),
    ACT("playlist_view.activate",    CTX_PLAYLIST_VIEW, "enter", "space", NULL, NULL),
    ACT("playlist_view.delete",      CTX_PLAYLIST_VIEW, "d", "D", NULL, NULL),
    ACT("playlist_view.rename",      CTX_PLAYLIST_VIEW, "r", "R", NULL, NULL),

    /* FAVORITES */
    ACT("favorites.up",              CTX_FAVORITES, "up", NULL, NULL, NULL),
    ACT("favorites.down",            CTX_FAVORITES, "down", NULL, NULL, NULL),
    ACT("favorites.right",           CTX_FAVORITES, "right", "tab", NULL, NULL),
    ACT("favorites.left",            CTX_FAVORITES, "left", NULL, NULL, NULL),
    ACT("favorites.activate",        CTX_FAVORITES, "enter", "space", NULL, NULL),
    ACT("favorites.remove",          CTX_FAVORITES, "d", "D", NULL, NULL),

    /* INFO */
    ACT("info.up",                   CTX_INFO, "up", NULL, NULL, NULL),
    ACT("info.down",                 CTX_INFO, "down", NULL, NULL, NULL),
    ACT("info.activate",             CTX_INFO, "enter", "space", NULL, NULL),

    /* HELP */
    ACT("help.up",                   CTX_HELP, "up", NULL, NULL, NULL),
    ACT("help.down",                 CTX_HELP, "down", NULL, NULL, NULL),
    ACT("help.left",                 CTX_HELP, "left", NULL, NULL, NULL),
    ACT("help.right",                CTX_HELP, "right", "enter", NULL, NULL),
    ACT("help.search",               CTX_HELP, "/", NULL, NULL, NULL),
    ACT("help.next_match",           CTX_HELP, "n", "N", NULL, NULL),

    /* LANGUAGE */
    ACT("language.up",               CTX_LANGUAGE, "up", NULL, NULL, NULL),
    ACT("language.down",             CTX_LANGUAGE, "down", NULL, NULL, NULL),
    ACT("language.activate",         CTX_LANGUAGE, "enter", "space", NULL, NULL),
    ACT("language.add",              CTX_LANGUAGE, "a", "A", NULL, NULL),
    ACT("language.delete",           CTX_LANGUAGE, "d", "D", NULL, NULL),
    ACT("language.back",             CTX_LANGUAGE, "left", NULL, NULL, NULL),

    /* REMOTE */
    ACT("remote.up",                 CTX_REMOTE, "up", NULL, NULL, NULL),
    ACT("remote.down",               CTX_REMOTE, "down", NULL, NULL, NULL),
    ACT("remote.left",               CTX_REMOTE, "left", NULL, NULL, NULL),
    ACT("remote.right",              CTX_REMOTE, "right", NULL, NULL, NULL),
    ACT("remote.activate",           CTX_REMOTE, "enter", "space", NULL, NULL),
    ACT("remote.adjust_up",          CTX_REMOTE, "+", "=", NULL, NULL),
    ACT("remote.adjust_down",        CTX_REMOTE, "-", "_", NULL, NULL),
    ACT("remote.save",               CTX_REMOTE, "s", "S", NULL, NULL),
};

#undef ACT

#define ACTION_TABLE_COUNT ((int)(sizeof(k_actions) / sizeof(k_actions[0])))

int actions_count(void)
{
    return ACTION_TABLE_COUNT;
}

int actions_is_bindable(ActionId id)
{
    switch (id) {
        case ACTION_APP_QUIT:
        case ACTION_VIEW_MAIN:
        case ACTION_VIEW_SETTINGS:
        case ACTION_VIEW_HISTORY:
        case ACTION_VIEW_PLAYLIST:
        case ACTION_VIEW_FAVORITES:
        case ACTION_VIEW_INFO:
        case ACTION_VIEW_LANGUAGE:
        case ACTION_VIEW_HELP:
        case ACTION_VIEW_TAB_SWITCH:
            return 0;
        case ACTION_NONE:
        case ACTION_COUNT:
            return 0;
        default:
            return 1;
    }
}

int actions_bindable_count(void)
{
    int count = 0;
    for (int i = ACTION_NONE + 1; i < ACTION_COUNT; i++) {
        if (actions_is_bindable((ActionId)i)) {
            count++;
        }
    }
    return count;
}

int actions_bindable_at(int index, ActionId *out)
{
    if (!out || index < 0) {
        return 0;
    }
    int current = 0;
    for (int i = ACTION_NONE + 1; i < ACTION_COUNT; i++) {
        ActionId id = (ActionId)i;
        if (!actions_is_bindable(id)) {
            continue;
        }
        if (current == index) {
            *out = id;
            return 1;
        }
        current++;
    }
    return 0;
}

const KeyActionDef *actions_def(ActionId id)
{
    int index = (int)id - 1;
    if (index < 0 || index >= ACTION_TABLE_COUNT) {
        return NULL;
    }
    return &k_actions[index];
}

ActionId actions_from_id(const char *id)
{
    if (!id || id[0] == '\0') {
        return ACTION_NONE;
    }
    for (int i = 0; i < ACTION_TABLE_COUNT; i++) {
        if (strcmp(k_actions[i].id, id) == 0) {
            return (ActionId)(i + 1);
        }
    }
    return ACTION_NONE;
}

KeyContext actions_context(ActionId id)
{
    const KeyActionDef *def = actions_def(id);
    return def ? def->context : CTX_COUNT;
}

const char *actions_context_id(KeyContext ctx)
{
    switch (ctx) {
        case CTX_GLOBAL:        return "global";
        case CTX_MAIN:          return "main";
        case CTX_MAIN_LIST:     return "main_list";
        case CTX_MAIN_LYRIC:    return "main_lyric";
        case CTX_MAIN_SEARCH:   return "main_search";
        case CTX_MAIN_CONTROL:  return "main_control";
        case CTX_POPUP:         return "popup";
        case CTX_SELECT_MENU:   return "select_menu";
        case CTX_BROWSER:       return "browser";
        case CTX_SETTINGS:      return "settings";
        case CTX_SETTINGS_EQ:   return "settings_eq";
        case CTX_HISTORY:       return "history";
        case CTX_PLAYLIST_VIEW: return "playlist_view";
        case CTX_FAVORITES:     return "favorites";
        case CTX_INFO:          return "info";
        case CTX_HELP:          return "help";
        case CTX_LANGUAGE:      return "language";
        case CTX_REMOTE:        return "remote";
        case CTX_TEXT_INPUT:    return "text_input";
        case CTX_COUNT:
        default:                return "";
    }
}

KeyContext actions_context_from_id(const char *id)
{
    if (!id) {
        return CTX_COUNT;
    }
    for (int ctx = 0; ctx < CTX_COUNT; ctx++) {
        const char *name = actions_context_id((KeyContext)ctx);
        if (name[0] != '\0' && strcmp(name, id) == 0) {
            return (KeyContext)ctx;
        }
    }
    return CTX_COUNT;
}

KeyContext actions_context_parent(KeyContext ctx)
{
    switch (ctx) {
        case CTX_MAIN:
            return CTX_GLOBAL;
        case CTX_MAIN_LIST:
            return CTX_MAIN;
        case CTX_MAIN_LYRIC:
            return CTX_MAIN_LIST;
        case CTX_MAIN_SEARCH:
            return CTX_MAIN_LIST;
        case CTX_MAIN_CONTROL:
            return CTX_MAIN;
        case CTX_POPUP:
        case CTX_BROWSER:
        case CTX_SETTINGS:
        case CTX_SETTINGS_EQ:
        case CTX_HISTORY:
        case CTX_PLAYLIST_VIEW:
        case CTX_FAVORITES:
        case CTX_INFO:
        case CTX_HELP:
        case CTX_LANGUAGE:
        case CTX_REMOTE:
            return CTX_GLOBAL;
        case CTX_GLOBAL:
        case CTX_SELECT_MENU:
        case CTX_TEXT_INPUT:
        case CTX_COUNT:
        default:
            return CTX_COUNT;
    }
}

int actions_default_keys(ActionId id, char out[][KEYNAME_MAX_LEN], int max)
{
    const KeyActionDef *def = actions_def(id);
    if (!def || !out || max <= 0) {
        return 0;
    }
    int written = 0;
    for (int i = 0; i < 4 && written < max; i++) {
        if (!def->defaults[i]) {
            break;
        }
        snprintf(out[written], KEYNAME_MAX_LEN, "%s", def->defaults[i]);
        written++;
    }
    return written;
}

int actions_override_index(const KeyBindingOverride *ov, int count,
                           const char *action, const char *key)
{
    if (!ov || !action || !key || count <= 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(ov[i].action, action) == 0 && strcmp(ov[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static void set_error(char *err, size_t err_size, const char *fmt, ...)
{
    if (!err || err_size == 0) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(err, err_size, fmt, args);
    va_end(args);
}

static int find_override(const KeyBindingOverride *ov, int count, ActionId id)
{
    const KeyActionDef *def = actions_def(id);
    if (!def) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(ov[i].action, def->id) == 0) {
            return i;
        }
    }
    return -1;
}

/* 动作的最终键集合：有覆盖则只用覆盖键，否则用全部默认键。
 * 系统保留动作不参与冲突计算：它们的覆盖在运行时被忽略。 */
static int effective_strokes(ActionId id, const KeyBindingOverride *ov, int count,
                             KeyStroke out[4])
{
    const KeyActionDef *def = actions_def(id);
    if (!def || !actions_is_bindable(id)) {
        return 0;
    }

    int index = find_override(ov, count, id);
    if (index >= 0) {
        KeyStroke stroke;
        if (keyname_parse(ov[index].key, &stroke) != 0 ||
            !keyname_is_bindable(stroke)) {
            return 0;
        }
        out[0] = stroke;
        return 1;
    }

    int written = 0;
    for (int i = 0; i < 4; i++) {
        if (!def->defaults[i]) {
            break;
        }
        KeyStroke stroke;
        if (keyname_parse(def->defaults[i], &stroke) != 0) {
            continue;
        }
        out[written++] = stroke;
    }
    return written;
}

/* ancestor 是否为 ctx 的严格祖先（沿父链上溯；CTX_TEXT_INPUT 不参与）。
 * 用于判断"子上下文动作会先于父上下文动作解析"这一遮蔽关系。 */
static int context_is_ancestor(KeyContext ancestor, KeyContext ctx)
{
    for (KeyContext c = actions_context_parent(ctx);
         c != CTX_COUNT;
         c = actions_context_parent(c)) {
        if (c == CTX_TEXT_INPUT) {
            continue;
        }
        if (c == ancestor) {
            return 1;
        }
    }
    return 0;
}

/* 遮蔽检查用的有效键：与运行时 keymap 解析一致——锁定动作（app.quit 等）
 * 的覆盖被忽略、其默认键在所有界面始终生效，故返回默认键；普通动作
 * 同 effective_strokes（有覆盖用覆盖键，否则用默认键）。 */
static int shadow_effective_strokes(ActionId id, const KeyBindingOverride *ov,
                                    int count, KeyStroke out[4])
{
    const KeyActionDef *def = actions_def(id);
    if (!def) {
        return 0;
    }
    if (actions_is_bindable(id)) {
        return effective_strokes(id, ov, count, out);
    }
    int written = 0;
    for (int i = 0; i < 4; i++) {
        if (!def->defaults[i]) {
            break;
        }
        KeyStroke stroke;
        if (keyname_parse(def->defaults[i], &stroke) == 0) {
            out[written++] = stroke;
        }
    }
    return written;
}

int actions_validate_overrides(const KeyBindingOverride *ov, int count,
                               char *err, size_t err_size)
{
    if (err && err_size) {
        err[0] = '\0';
    }
    if (count < 0 || (count > 0 && !ov)) {
        set_error(err, err_size, "invalid override table");
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (count > MAX_KEYBINDING_OVERRIDES) {
        set_error(err, err_size, "too many keybinding overrides (%d > %d)",
                  count, MAX_KEYBINDING_OVERRIDES);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (ov[i].action[0] == '\0') {
            set_error(err, err_size, "empty action id in override %d", i);
            return -1;
        }
        ActionId id = actions_from_id(ov[i].action);
        if (id == ACTION_NONE) {
            set_error(err, err_size, "unknown action '%s'", ov[i].action);
            return -1;
        }
        KeyStroke stroke;
        if (keyname_parse(ov[i].key, &stroke) != 0 ||
            !keyname_is_bindable(stroke)) {
            set_error(err, err_size, "invalid key '%s' for action '%s'",
                      ov[i].key, ov[i].action);
            return -1;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(ov[j].action, ov[i].action) == 0) {
                set_error(err, err_size, "duplicate override for action '%s'",
                          ov[i].action);
                return -1;
            }
        }
    }

    /* 同一上下文（仅自身表，不含父表）内不允许一个键映射到两个动作 */
    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        const KeyActionDef *def_a = actions_def((ActionId)a);
        KeyStroke keys_a[4];
        int count_a = effective_strokes((ActionId)a, ov, count, keys_a);

        for (int b = a + 1; b < ACTION_COUNT; b++) {
            const KeyActionDef *def_b = actions_def((ActionId)b);
            if (def_a->context != def_b->context) {
                continue;
            }
            KeyStroke keys_b[4];
            int count_b = effective_strokes((ActionId)b, ov, count, keys_b);
            for (int i = 0; i < count_a; i++) {
                for (int j = 0; j < count_b; j++) {
                    if (!keyname_equal(keys_a[i], keys_b[j])) {
                        continue;
                    }
                    char key_name[KEYNAME_MAX_LEN];
                    if (keyname_format(keys_a[i], key_name, sizeof(key_name)) != 0) {
                        snprintf(key_name, sizeof(key_name), "?");
                    }
                    set_error(err, err_size,
                              "conflict in context '%s': '%s' and '%s' both use '%s'",
                              actions_context_id(def_a->context),
                              def_a->id, def_b->id, key_name);
                    return -1;
                }
            }
        }
    }

    /* 父子上下文遮蔽：子上下文动作先解析，父上下文动作的键一旦被子上下文
     * 占用（覆盖键或子层覆盖抢走父层默认键），父动作在子界面下永远不生效。
     * 至少一侧是覆盖才报错——默认键固有遮蔽（lyrics/search 等靠上下文条件
     * 分发）属既有设计，不在此拦。 */
    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        const KeyActionDef *def_parent = actions_def((ActionId)a);
        /* 锁定动作的覆盖在运行时被忽略，不算"涉及覆盖"；但其默认键始终
         * 生效，子层覆盖抢走即遮蔽（如 remote.save→q 抢走 app.quit 的 q）。 */
        int parent_has_override = actions_is_bindable((ActionId)a) &&
                                  find_override(ov, count, (ActionId)a) >= 0;
        KeyStroke keys_parent[4];
        int count_parent = shadow_effective_strokes((ActionId)a, ov, count,
                                                    keys_parent);

        for (int b = ACTION_NONE + 1; b < ACTION_COUNT; b++) {
            if (a == b) {
                continue;
            }
            const KeyActionDef *def_child = actions_def((ActionId)b);
            if (!context_is_ancestor(def_parent->context, def_child->context)) {
                continue;
            }
            int child_has_override = actions_is_bindable((ActionId)b) &&
                                     find_override(ov, count, (ActionId)b) >= 0;
            if (!parent_has_override && !child_has_override) {
                continue;
            }
            KeyStroke keys_child[4];
            int count_child = shadow_effective_strokes((ActionId)b, ov, count,
                                                       keys_child);
            for (int i = 0; i < count_parent; i++) {
                for (int j = 0; j < count_child; j++) {
                    if (!keyname_equal(keys_parent[i], keys_child[j])) {
                        continue;
                    }
                    char key_name[KEYNAME_MAX_LEN];
                    if (keyname_format(keys_parent[i], key_name,
                                       sizeof(key_name)) != 0) {
                        snprintf(key_name, sizeof(key_name), "?");
                    }
                    set_error(err, err_size,
                              "shadowing conflict: '%s' in child context '%s' "
                              "takes key '%s' from '%s', which would never fire there",
                              def_child->id,
                              actions_context_id(def_child->context),
                              key_name, def_parent->id);
                    return -1;
                }
            }
        }
    }

    return 0;
}
