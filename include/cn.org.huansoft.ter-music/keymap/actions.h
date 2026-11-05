/**
 * @file actions.h
 * @brief 全部可绑定动作、所属上下文与默认键位（单一来源）
 *
 * 不依赖 ncurses：动作表可被配置层、审计与单元测试复用。
 * 字符串 ID 写入配置文件与审计输出，发布后不得更名。
 */

#ifndef KEYMAP_ACTIONS_H
#define KEYMAP_ACTIONS_H

#include <stddef.h>

#include "types.h"
#include "util/keyname.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACTION_NONE = 0,

    /* GLOBAL */
    ACTION_APP_QUIT,
    ACTION_VIEW_MAIN,
    ACTION_VIEW_SETTINGS,
    ACTION_VIEW_HISTORY,
    ACTION_VIEW_PLAYLIST,
    ACTION_VIEW_FAVORITES,
    ACTION_VIEW_INFO,
    ACTION_VIEW_LANGUAGE,
    ACTION_VIEW_HELP,
    ACTION_LIBRARY_TOGGLE,

    /* MAIN */
    ACTION_VOLUME_UP,
    ACTION_VOLUME_DOWN,
    ACTION_CORE_RESTART,
    ACTION_FOCUS_CONTROLS,
    ACTION_FOCUS_LIST,
    ACTION_LYRICS_SEEK_MODE,

    /* MAIN_LIST */
    ACTION_LIST_UP,
    ACTION_LIST_DOWN,
    ACTION_LIST_ACTIVATE,
    ACTION_FOLDER_OPEN,
    ACTION_QUEUE_INSERT_NEXT,
    ACTION_QUEUE_APPEND,
    ACTION_FOLDER_APPEND,
    ACTION_FAVORITE_TOGGLE,
    ACTION_SEARCH_OPEN,
    ACTION_QUEUE_REMOVE,
    ACTION_QUEUE_CLEAR,
    ACTION_QUEUE_MOVE_DOWN,
    ACTION_QUEUE_MOVE_UP,
    ACTION_PLAYMODE_SEQUENTIAL,
    ACTION_PLAYMODE_SINGLE,
    ACTION_PLAYMODE_LIST,
    ACTION_PLAYMODE_SHUFFLE,
    ACTION_PLAYMODE_FOLDER,
    ACTION_TRACK_NEXT,
    ACTION_TRACK_PREV,
    ACTION_HISTORY_SHOW,
    ACTION_PLAYLIST_ADD,
    ACTION_VIEW_TAB_SWITCH,

    /* MAIN_LYRIC */
    ACTION_LYRICS_PREV,
    ACTION_LYRICS_NEXT,
    ACTION_LYRICS_SOURCE_SWITCH,

    /* MAIN_SEARCH */
    ACTION_SEARCH_UP,
    ACTION_SEARCH_DOWN,

    /* MAIN_CONTROL */
    ACTION_CONTROL_UP,
    ACTION_CONTROL_DOWN,
    ACTION_CONTROL_LEFT,
    ACTION_CONTROL_RIGHT,
    ACTION_SEEK_BACK,
    ACTION_SEEK_FORWARD,
    ACTION_CONTROL_ACTIVATE,

    /* POPUP */
    ACTION_POPUP_UP,
    ACTION_POPUP_DOWN,
    ACTION_POPUP_ACCEPT,
    ACTION_POPUP_CANCEL,

    /* SELECT MENU（设置内的模态选择菜单） */
    ACTION_SELECT_UP,
    ACTION_SELECT_DOWN,
    ACTION_SELECT_TOGGLE,
    ACTION_SELECT_ACCEPT,
    ACTION_SELECT_ALL,
    ACTION_SELECT_NONE,
    ACTION_SELECT_CANCEL,

    /* BROWSER */
    ACTION_BROWSER_UP,
    ACTION_BROWSER_DOWN,
    ACTION_BROWSER_ACTIVATE,
    ACTION_BROWSER_SCAN,
    ACTION_BROWSER_RESCAN,
    ACTION_BROWSER_QUEUE_APPEND,

    /* SETTINGS */
    ACTION_SETTINGS_UP,
    ACTION_SETTINGS_DOWN,
    ACTION_SETTINGS_LEFT,
    ACTION_SETTINGS_RIGHT,
    ACTION_SETTINGS_TAB,
    ACTION_SETTINGS_ACTIVATE,
    ACTION_SETTINGS_SAVE,
    ACTION_SETTINGS_EQ_PRESETS,
    ACTION_SETTINGS_HOTKEY_RESET,
    ACTION_SETTINGS_HOTKEY_RESET_ALL,

    /* SETTINGS_EQ */
    ACTION_EQ_BAND_PREV,
    ACTION_EQ_BAND_NEXT,
    ACTION_EQ_GAIN_UP,
    ACTION_EQ_GAIN_DOWN,
    ACTION_EQ_BACK,
    ACTION_EQ_ACTIVATE,

    /* HISTORY */
    ACTION_HISTORY_UP,
    ACTION_HISTORY_DOWN,
    ACTION_HISTORY_RIGHT,
    ACTION_HISTORY_LEFT,
    ACTION_HISTORY_ACTIVATE,
    ACTION_HISTORY_QUEUE_APPEND,
    ACTION_HISTORY_DELETE,
    ACTION_HISTORY_CLEAR,

    /* PLAYLIST_VIEW */
    ACTION_PLAYLIST_VIEW_UP,
    ACTION_PLAYLIST_VIEW_DOWN,
    ACTION_PLAYLIST_VIEW_RIGHT,
    ACTION_PLAYLIST_VIEW_LEFT,
    ACTION_PLAYLIST_VIEW_ACTIVATE,
    ACTION_PLAYLIST_VIEW_DELETE,
    ACTION_PLAYLIST_VIEW_RENAME,

    /* FAVORITES */
    ACTION_FAVORITES_UP,
    ACTION_FAVORITES_DOWN,
    ACTION_FAVORITES_RIGHT,
    ACTION_FAVORITES_LEFT,
    ACTION_FAVORITES_ACTIVATE,
    ACTION_FAVORITES_REMOVE,

    /* INFO */
    ACTION_INFO_UP,
    ACTION_INFO_DOWN,
    ACTION_INFO_ACTIVATE,

    /* HELP */
    ACTION_HELP_UP,
    ACTION_HELP_DOWN,
    ACTION_HELP_LEFT,
    ACTION_HELP_RIGHT,
    ACTION_HELP_SEARCH,
    ACTION_HELP_NEXT_MATCH,

    /* LANGUAGE */
    ACTION_LANGUAGE_UP,
    ACTION_LANGUAGE_DOWN,
    ACTION_LANGUAGE_ACTIVATE,
    ACTION_LANGUAGE_ADD,
    ACTION_LANGUAGE_DELETE,
    ACTION_LANGUAGE_BACK,

    /* REMOTE */
    ACTION_REMOTE_UP,
    ACTION_REMOTE_DOWN,
    ACTION_REMOTE_LEFT,
    ACTION_REMOTE_RIGHT,
    ACTION_REMOTE_ACTIVATE,
    ACTION_REMOTE_ADJUST_UP,
    ACTION_REMOTE_ADJUST_DOWN,
    ACTION_REMOTE_SAVE,

    ACTION_COUNT
} ActionId;

typedef enum {
    CTX_GLOBAL = 0,
    CTX_MAIN,
    CTX_MAIN_LIST,
    CTX_MAIN_LYRIC,
    CTX_MAIN_SEARCH,
    CTX_MAIN_CONTROL,
    CTX_POPUP,
    CTX_SELECT_MENU,
    CTX_BROWSER,
    CTX_SETTINGS,
    CTX_SETTINGS_EQ,
    CTX_HISTORY,
    CTX_PLAYLIST_VIEW,
    CTX_FAVORITES,
    CTX_INFO,
    CTX_HELP,
    CTX_LANGUAGE,
    CTX_REMOTE,
    CTX_TEXT_INPUT,
    CTX_COUNT
} KeyContext;

typedef struct {
    const char *id;
    const char *i18n_key;
    KeyContext context;
    const char *defaults[4];
} KeyActionDef;

/** 可绑定动作总数（不含 ACTION_NONE 哨兵）。 */
int actions_count(void);

/**
 * 动作是否允许用户改绑。退出键（app.quit）、视图/选项卡切换键
 * （view.main…view.help）与文件/队列切换键（view.tab_switch）为系统保留，
 * 返回 0；其余合法动作返回 1。
 */
int actions_is_bindable(ActionId id);

/** 可绑定动作数量（actions_count() 减去系统保留动作）。 */
int actions_bindable_count(void);

/**
 * 按动作表顺序取第 index 个可绑定动作（0 基）。
 * 成功写 *out 并返回 1；index 越界返回 0。
 */
int actions_bindable_at(int index, ActionId *out);

/** 取动作定义；id 非法时返回 NULL。 */
const KeyActionDef *actions_def(ActionId id);

/** 字符串 ID → 动作；未知返回 ACTION_NONE。 */
ActionId actions_from_id(const char *id);

KeyContext actions_context(ActionId id);

const char *actions_context_id(KeyContext ctx);

/** 字符串 ID → 上下文；未知返回 CTX_COUNT。 */
KeyContext actions_context_from_id(const char *id);

/** 父上下文；无父（GLOBAL / TEXT_INPUT）返回 CTX_COUNT。 */
KeyContext actions_context_parent(KeyContext ctx);

/** 默认键（规范键名），最多写入 max 条；返回条数。 */
int actions_default_keys(ActionId id, char out[][KEYNAME_MAX_LEN], int max);

/**
 * 校验覆盖表：未知动作、非法/不可绑定键、重复 action、同上下文冲突、
 * 跨上下文遮蔽（子上下文有效键抢走父上下文动作的键，或父上下文覆盖键被
 * 子上下文有效键占用；至少一侧是覆盖才判错，默认键固有遮蔽不拦）均失败。
 * count == 0 返回 0。失败返回 -1 并在 err 写具体原因（err 可为 NULL）。
 */
int actions_validate_overrides(const KeyBindingOverride *ov, int count,
                               char *err, size_t err_size);

/** 查找 action + key 完全匹配的覆盖项下标；未找到返回 -1。 */
int actions_override_index(const KeyBindingOverride *ov, int count,
                           const char *action, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* KEYMAP_ACTIONS_H */
