/**
 * @file layout.c
 * @brief TUI 主界面运行时布局管理
 *
 * 布局规范由 ui/layout_spec.c 解析为强制平铺的分栏树；本文件负责加载
 * 内置/用户布局、保存本地选中状态、展开分栏树、映射为窗口矩形、建窗、
 * 分配颜色，以及统一的窗格背景/边框/标题绘制。
 *
 * 主区域 = 终端去掉底部菜单提示条后的全部区域。分栏树保证所有窗格严格
 * 铺满该区域，因此这里不对窗格做间距或空白补偿。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "types.h"
#include "config/config.h"
#include "i18n/i18n.h"
#include "logger/logger.h"
#include "ui/layout.h"
#include "ui/layout_state.h"
#include "ui/menu_internal.h"
#include "ui/menus.h"
#include "ui/ui.h"

#include <ncursesw/ncurses.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LAYOUT_STATE_FILE "ui-layout"
#define LAYOUT_USER_FILE  "layouts.json"

#define LAYOUT_BODY_PAIR_BASE   10
#define LAYOUT_BORDER_PAIR_BASE 20
#define LAYOUT_PAIR_LIMIT       26

static LayoutCatalog g_catalog;
static LayoutSpec g_current;
static char g_current_id[LAYOUT_ID_MAX];
static int g_initialized = 0;
static int g_last_adjusted = 0;

static int g_body_pair[LAYOUT_PANE_COUNT];
static int g_border_pair[LAYOUT_PANE_COUNT];

/* ============================================================
 * 路径与文件加载
 * ============================================================ */

static int layout_state_path(char *path, size_t path_size)
{
    const char *dir = app_config_dir(1);
    if (!dir || !dir[0]) {
        return -1;
    }
    int written = snprintf(path, path_size, "%s/%s", dir, LAYOUT_STATE_FILE);
    return (written > 0 && (size_t)written < path_size) ? 0 : -1;
}

static int layout_user_path(char *path, size_t path_size)
{
    const char *dir = app_config_dir(0);
    if (!dir || !dir[0]) {
        return -1;
    }
    int written = snprintf(path, path_size, "%s/%s", dir, LAYOUT_USER_FILE);
    return (written > 0 && (size_t)written < path_size) ? 0 : -1;
}

static int catalog_add_fallback(LayoutCatalog *catalog)
{
    LayoutSpec fallback;
    if (layout_spec_build_fallback(&fallback) != 0) {
        return -1;
    }
    LayoutCatalog one;
    layout_catalog_init(&one);
    one.layouts[0] = fallback;
    one.count = 1;
    snprintf(one.default_id, sizeof(one.default_id), "%s", fallback.id);
    layout_catalog_merge(catalog, &one);
    return 0;
}

static void merge_catalog_file(const char *path, LayoutCatalog *catalog)
{
    if (!path || !path[0] || access(path, R_OK) != 0) {
        return;
    }
    LayoutCatalog parsed;
    char err[192] = "";
    if (layout_spec_parse_file(path, &parsed, err, sizeof(err)) != 0) {
        log_warn("layout", "Ignoring layout file '%s': %s", path, err);
        return;
    }
    layout_catalog_merge(catalog, &parsed);
    g_last_adjusted += parsed.adjusted;
    log_info("layout",
             "Loaded %d layout(s) from '%s' (total %d, adjusted %d, skipped %d)",
             parsed.count, path, catalog->count, parsed.adjusted,
             parsed.skipped);
}

static void load_builtin_catalog(LayoutCatalog *catalog)
{
    static const char *const candidates[] = {
        TER_MUSIC_DATA_DIR "/layouts/layouts.json",
        "/usr/share/ter-music/layouts/layouts.json",
        "data/layouts/layouts.json",
        TER_MUSIC_SOURCE_DIR "/data/layouts/layouts.json",
    };

    char executable_path[MAX_PATH_LEN];
    char executable_relative[MAX_PATH_LEN];
    executable_path[0] = '\0';
    executable_relative[0] = '\0';
    ssize_t link_len = readlink("/proc/self/exe", executable_path,
                                sizeof(executable_path) - 1);
    if (link_len > 0) {
        executable_path[link_len] = '\0';
        char *slash = strrchr(executable_path, '/');
        if (slash) {
            *slash = '\0';
            snprintf(executable_relative, sizeof(executable_relative),
                     "%s/../share/ter-music/layouts/layouts.json",
                     executable_path);
        }
    }

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (catalog->count > 0) {
            break;
        }
        merge_catalog_file(candidates[i], catalog);
    }
    if (catalog->count == 0 && executable_relative[0] != '\0') {
        merge_catalog_file(executable_relative, catalog);
    }
    if (catalog->count == 0) {
        log_warn("layout", "No built-in layout file found; using compiled fallback");
        catalog_add_fallback(catalog);
    }
}

static int load_full_catalog(LayoutCatalog *catalog)
{
    layout_catalog_init(catalog);
    g_last_adjusted = 0;
    load_builtin_catalog(catalog);

    char user_path[MAX_PATH_LEN];
    if (layout_user_path(user_path, sizeof(user_path)) == 0) {
        merge_catalog_file(user_path, catalog);
    }

    const char *env_path = getenv("TER_MUSIC_LAYOUTS");
    if (env_path && env_path[0]) {
        merge_catalog_file(env_path, catalog);
    }

    if (catalog->count == 0) {
        catalog_add_fallback(catalog);
    }
    if (catalog->default_id[0] == '\0' ||
        !layout_catalog_find(catalog, catalog->default_id)) {
        snprintf(catalog->default_id, sizeof(catalog->default_id), "%s",
                 catalog->layouts[0].id);
    }
    return catalog->count;
}

/* ============================================================
 * 选中状态
 * ============================================================ */

static void select_layout_from_state_or_default(const char *preferred_id)
{
    char path[MAX_PATH_LEN];
    char state_id[LAYOUT_ID_MAX] = "";

    const LayoutSpec *selected = NULL;
    if (layout_state_path(path, sizeof(path)) == 0 &&
        layout_state_read(path, state_id, sizeof(state_id)) == 0) {
        selected = layout_catalog_find(&g_catalog, state_id);
        if (!selected) {
            log_warn("layout", "Saved layout '%s' is no longer available", state_id);
            state_id[0] = '\0';
        }
    }

    if (!selected && preferred_id && preferred_id[0]) {
        selected = layout_catalog_find(&g_catalog, preferred_id);
    }

    if (!selected && state_id[0] == '\0' &&
        !g_app_config.show_lyrics_panel) {
        selected = layout_catalog_find(&g_catalog, "focus");
    }

    if (!selected) {
        selected = layout_catalog_find(&g_catalog, g_catalog.default_id);
    }
    if (!selected) {
        selected = &g_catalog.layouts[0];
    }

    g_current = *selected;
    snprintf(g_current_id, sizeof(g_current_id), "%s", selected->id);
}

/* ============================================================
 * 窗格窗口槽位
 * ============================================================ */

static WINDOW **pane_slot(LayoutPaneKind kind)
{
    switch (kind) {
        case LAYOUT_PANE_PLAYLIST: return &win_playlist;
        case LAYOUT_PANE_INFO:     return &win_info;
        case LAYOUT_PANE_CONTROLS: return &win_controls;
        case LAYOUT_PANE_SPECTRUM: return &win_spectrum;
        case LAYOUT_PANE_LYRICS:   return &win_lyrics;
        case LAYOUT_PANE_VINYL:    return &win_vinyl;
        default:                   return NULL;
    }
}

const char *ui_layout_pane_title(LayoutPaneKind kind)
{
    switch (kind) {
        case LAYOUT_PANE_PLAYLIST: return i18n_get("controls.playlist");
        case LAYOUT_PANE_INFO:     return i18n_get("hint.info");
        case LAYOUT_PANE_CONTROLS: return i18n_get("controls.controls_area");
        case LAYOUT_PANE_SPECTRUM: return i18n_get("controls.spectrum");
        case LAYOUT_PANE_LYRICS:   return i18n_get("controls.lyrics");
        case LAYOUT_PANE_VINYL:    return i18n_get("controls.vinyl");
        default:                   return "";
    }
}

/* ============================================================
 * 颜色
 * ============================================================ */

static int role_pair(LayoutPaneKind kind)
{
    switch (kind) {
        case LAYOUT_PANE_PLAYLIST:
        case LAYOUT_PANE_INFO:
            return COLOR_PAIR_PLAYLIST;
        case LAYOUT_PANE_CONTROLS:
            return COLOR_PAIR_CONTROLS;
        case LAYOUT_PANE_SPECTRUM:
        case LAYOUT_PANE_LYRICS:
        case LAYOUT_PANE_VINYL:
            return COLOR_PAIR_LYRICS;
        default:
            return 0;
    }
}

static void role_colors(LayoutPaneKind kind, int *fg, int *bg)
{
    switch (kind) {
        case LAYOUT_PANE_PLAYLIST:
        case LAYOUT_PANE_INFO:
            *fg = g_app_config.theme.playlist_fg;
            *bg = g_app_config.theme.playlist_bg;
            break;
        case LAYOUT_PANE_CONTROLS:
            *fg = g_app_config.theme.controls_fg;
            *bg = g_app_config.theme.controls_bg;
            break;
        case LAYOUT_PANE_SPECTRUM:
        case LAYOUT_PANE_LYRICS:
        case LAYOUT_PANE_VINYL:
            *fg = g_app_config.theme.lyrics_fg;
            *bg = g_app_config.theme.lyrics_bg;
            break;
        default:
            *fg = COLOR_WHITE;
            *bg = -1;
            break;
    }
}

static int clamp_color(int value)
{
    int max_color = COLORS - 1;
    if (max_color < 0) {
        max_color = 0;
    }
    if (value < -1) {
        return -1;
    }
    if (value > max_color) {
        return max_color;
    }
    return value;
}

void ui_layout_apply_colors(void)
{
    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        g_body_pair[i] = role_pair((LayoutPaneKind)i);
        g_border_pair[i] = g_body_pair[i];
    }

    if (!has_colors() || COLOR_PAIRS <= LAYOUT_PAIR_LIMIT) {
        return;
    }

    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        LayoutPaneKind kind = (LayoutPaneKind)i;
        const LayoutPaneSpec *pane = layout_spec_pane_config(&g_current, kind);
        if (!pane) {
            continue;
        }
        int has_override = pane->fg != LAYOUT_COLOR_INHERIT ||
                           pane->bg != LAYOUT_COLOR_INHERIT ||
                           pane->border_fg != LAYOUT_COLOR_INHERIT ||
                           pane->border_bg != LAYOUT_COLOR_INHERIT;
        if (!has_override || g_rainbow_mode_enabled) {
            continue;
        }

        int role_fg, role_bg;
        role_colors(kind, &role_fg, &role_bg);
        int fg = pane->fg == LAYOUT_COLOR_INHERIT ? role_fg : clamp_color(pane->fg);
        int bg = pane->bg == LAYOUT_COLOR_INHERIT ? role_bg : clamp_color(pane->bg);

        int body = LAYOUT_BODY_PAIR_BASE + i;
        if (init_pair((short)body, (short)fg, (short)bg) != OK) {
            continue;
        }
        g_body_pair[i] = body;

        if (pane->border_fg == LAYOUT_COLOR_INHERIT &&
            pane->border_bg == LAYOUT_COLOR_INHERIT) {
            g_border_pair[i] = body;
            continue;
        }
        int border_fg = pane->border_fg == LAYOUT_COLOR_INHERIT
                            ? fg : clamp_color(pane->border_fg);
        int border_bg = pane->border_bg == LAYOUT_COLOR_INHERIT
                            ? bg : clamp_color(pane->border_bg);
        int border = LAYOUT_BORDER_PAIR_BASE + i;
        if (init_pair((short)border, (short)border_fg, (short)border_bg) == OK) {
            g_border_pair[i] = border;
        } else {
            g_border_pair[i] = body;
        }
    }
}

int ui_pane_body_attr(LayoutPaneKind kind)
{
    if (kind < 0 || kind >= LAYOUT_PANE_COUNT || !has_colors()) {
        return 0;
    }
    int pair = g_body_pair[kind] > 0 ? g_body_pair[kind] : role_pair(kind);
    return pair > 0 ? COLOR_PAIR(pair) : 0;
}

int ui_pane_border_attr(LayoutPaneKind kind)
{
    if (kind < 0 || kind >= LAYOUT_PANE_COUNT || !has_colors()) {
        return 0;
    }
    int pair = g_border_pair[kind] > 0 ? g_border_pair[kind] : role_pair(kind);
    return pair > 0 ? COLOR_PAIR(pair) : 0;
}

/* ============================================================
 * 统一窗格绘制
 * ============================================================ */

void ui_pane_begin(LayoutPaneKind kind, WINDOW *win, const char *title)
{
    if (!win) {
        return;
    }

    int body = ui_pane_body_attr(kind);
    int border = ui_pane_border_attr(kind);

    wbkgd(win, (chtype)body);
    werase(win);

    int h, w;
    getmaxyx(win, h, w);
    if (h >= 2 && w >= 2) {
        wattrset(win, border);
        rounded_box(win);
    }
    wattrset(win, body);

    if (title && title[0] && h >= 1 && w >= 5) {
        int max_width = w - 4;
        if (max_width < 1) {
            max_width = 1;
        }
        char display[128];
        format_display_text(display, sizeof(display), title, max_width, 0);
        mvwprintw(win, 0, 2, "%s", display);
    }
}

void ui_pane_clip_print(WINDOW *win, int row, int col, const char *text)
{
    if (!win || !text) {
        return;
    }
    int h, w;
    getmaxyx(win, h, w);
    if (row < 1 || row >= h - 1 || col < 1 || col >= w - 1) {
        return;
    }
    int max_cols = (w - 1) - col;
    if (max_cols <= 0) {
        return;
    }
    char display[1024];
    /* format_display_text 的裁剪函数按列计，最坏每列 4 字节；限制到
     * 缓冲区安全范围，超宽终端也不会写穿本地缓冲。 */
    int safe_cols = (int)(sizeof(display) / 4) - 4;
    if (max_cols > safe_cols) {
        max_cols = safe_cols;
    }
    format_display_text(display, sizeof(display), text, max_cols, 0);
    mvwprintw(win, row, col, "%s", display);
}

void ui_pane_clip_printf(WINDOW *win, int row, int col,
                         const char *fmt, ...)
{
    if (!win || !fmt) {
        return;
    }
    char text[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    ui_pane_clip_print(win, row, col, text);
}

int ui_pane_is_visible(LayoutPaneKind kind)
{
    WINDOW **slot = pane_slot(kind);
    return slot && *slot;
}

void ui_pane_repaint_border(LayoutPaneKind kind, WINDOW *win)
{
    if (!win) {
        return;
    }

    int h, w;
    getmaxyx(win, h, w);
    if (h < 2 || w < 2) {
        return;
    }

    wattrset(win, ui_pane_border_attr(kind));

    /* 左右竖边：第 0 行是标题，不能整行重画，只补竖边。 */
    for (int y = 1; y < h - 1; y++) {
        mvwaddstr(win, y, 0, "\xe2\x94\x82");          /* │ */
        mvwaddstr(win, y, w - 1, "\xe2\x94\x82");      /* │ */
    }

    /* 底边（含圆角）；顶边与标题保持 ui_pane_begin() 的绘制结果。 */
    mvwaddstr(win, h - 1, 0, "\xe2\x95\xb0");          /* ╰ */
    for (int x = 1; x < w - 1; x++) {
        mvwaddstr(win, h - 1, x, "\xe2\x94\x80");      /* ─ */
    }
    mvwaddstr(win, h - 1, w - 1, "\xe2\x95\xaf");      /* ╯ */
}

/* ============================================================
 * 建窗
 * ============================================================ */

static void clear_pane_windows(void)
{
    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        WINDOW **slot = pane_slot((LayoutPaneKind)i);
        if (slot) {
            *slot = NULL;
        }
    }
}

void destroy_main_windows(void)
{
    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        WINDOW **slot = pane_slot((LayoutPaneKind)i);
        if (slot && *slot) {
            delwin(*slot);
            *slot = NULL;
        }
    }
    clear_pane_windows();
}

static int create_windows(const LayoutGeometry *geometry)
{
    if (!geometry || !geometry->valid) {
        return -1;
    }

    for (int i = 0; i < LAYOUT_PANE_COUNT; i++) {
        const LayoutPaneRect *rect = &geometry->panes[i];
        if (!rect->visible || !rect->has_cells) {
            continue;
        }
        if (rect->w <= 0 || rect->h <= 0) {
            continue;
        }
        WINDOW *win = newwin(rect->h, rect->w, rect->y, rect->x);
        if (!win) {
            log_error("layout", "newwin failed for pane %s (%dx%d at %d,%d)",
                      layout_pane_id_name((LayoutPaneKind)i),
                      rect->w, rect->h, rect->x, rect->y);
            return -1;
        }
        WINDOW **slot = pane_slot((LayoutPaneKind)i);
        if (!slot) {
            delwin(win);
            continue;
        }
        *slot = win;
        ui_pane_begin((LayoutPaneKind)i, win,
                      ui_layout_pane_title((LayoutPaneKind)i));
        wrefresh(win);
    }
    return 0;
}

static void reset_focus_for_layout(void)
{
    if (!ui_pane_is_visible(LAYOUT_PANE_LYRICS)) {
        g_lyric_cursor_mode = 0;
        g_lyric_cursor_index = -1;
    }
    if (!ui_pane_is_visible(LAYOUT_PANE_CONTROLS)) {
        g_control_focus = 0;
    }
}

/* ============================================================
 * 加载与选择 API
 * ============================================================ */

int ui_layout_init(void)
{
    if (g_initialized) {
        return 0;
    }
    load_full_catalog(&g_catalog);
    select_layout_from_state_or_default(NULL);
    g_initialized = 1;
    log_info("layout", "Selected layout '%s' (%d available)", g_current_id,
             g_catalog.count);
    return 0;
}

int ui_layout_last_adjusted(void)
{
    return g_last_adjusted;
}

int ui_layout_reload(void)
{
    LayoutCatalog fresh;
    if (load_full_catalog(&fresh) <= 0) {
        return -1;
    }
    g_catalog = fresh;

    char previous[LAYOUT_ID_MAX];
    snprintf(previous, sizeof(previous), "%s", g_current_id);
    select_layout_from_state_or_default(previous);
    g_initialized = 1;
    log_info("layout", "Reloaded %d layout(s); selected '%s'",
             g_catalog.count, g_current_id);
    return g_catalog.count;
}

int ui_layout_count(void)
{
    if (!g_initialized) {
        ui_layout_init();
    }
    return g_catalog.count;
}

const char *ui_layout_id_at(int index)
{
    if (!g_initialized) {
        ui_layout_init();
    }
    if (index < 0 || index >= g_catalog.count) {
        return NULL;
    }
    return g_catalog.layouts[index].id;
}

const char *ui_layout_name_at(int index)
{
    if (!g_initialized) {
        ui_layout_init();
    }
    if (index < 0 || index >= g_catalog.count) {
        return "";
    }
    const LayoutSpec *layout = &g_catalog.layouts[index];
    if (layout->name_key[0]) {
        const char *translated = i18n_get(layout->name_key);
        if (translated && strcmp(translated, layout->name_key) != 0) {
            return translated;
        }
    }
    if (layout->name[0]) {
        return layout->name;
    }
    return layout->id;
}

int ui_layout_current_index(void)
{
    if (!g_initialized) {
        ui_layout_init();
    }
    for (int i = 0; i < g_catalog.count; i++) {
        if (strcmp(g_catalog.layouts[i].id, g_current_id) == 0) {
            return i;
        }
    }
    return 0;
}

const char *ui_layout_current_id(void)
{
    if (!g_initialized) {
        ui_layout_init();
    }
    return g_current_id;
}

const char *ui_layout_current_name(void)
{
    int index = ui_layout_current_index();
    return ui_layout_name_at(index);
}

int ui_layout_current_spec(LayoutSpec *out)
{
    if (!out || !g_initialized) {
        return -1;
    }
    *out = g_current;
    return 0;
}

int ui_layout_select(const char *id)
{
    if (!g_initialized) {
        ui_layout_init();
    }
    const LayoutSpec *layout = layout_catalog_find(&g_catalog, id);
    if (!layout) {
        return -1;
    }
    g_current = *layout;
    snprintf(g_current_id, sizeof(g_current_id), "%s", layout->id);

    char path[MAX_PATH_LEN];
    if (layout_state_path(path, sizeof(path)) != 0) {
        return -1;
    }
    return layout_state_write(path, g_current_id);
}

/* ============================================================
 * 用户布局文件（布局编辑器保存/恢复内置预设）
 * ============================================================ */

int layout_user_file_path(char *path, size_t path_size)
{
    return layout_user_path(path, path_size);
}

int layout_user_save_layout(const LayoutSpec *spec, char *err, size_t err_size)
{
    if (err && err_size) {
        err[0] = '\0';
    }
    if (!spec) {
        snprintf(err, err_size, "no layout to save");
        return -1;
    }

    char path[MAX_PATH_LEN];
    if (layout_user_path(path, sizeof(path)) != 0) {
        snprintf(err, err_size, "cannot resolve user layout path");
        return -1;
    }

    LayoutCatalog user;
    layout_catalog_init(&user);
    if (access(path, R_OK) == 0) {
        char parse_err[192] = "";
        if (layout_spec_parse_file(path, &user, parse_err,
                                   sizeof(parse_err)) != 0) {
            /* 损坏的用户文件按空目录处理，本次保存重建整个文件 */
            log_warn("layout", "User layout file unusable, rebuilding: %s",
                     parse_err);
            layout_catalog_init(&user);
        }
    }

    if (layout_user_catalog_upsert(&user, spec) < 0) {
        snprintf(err, err_size, "layout catalog is full (%d)",
                 LAYOUT_MAX_CATALOG);
        return -1;
    }
    if (user.default_id[0] == '\0') {
        snprintf(user.default_id, sizeof(user.default_id), "%s", spec->id);
    }
    if (layout_spec_write_file(path, &user, err, err_size) != 0) {
        return -1;
    }
    log_info("layout", "Saved layout '%s' to '%s'", spec->id, path);
    return 0;
}

int layout_user_remove_layout(const char *id, char *err, size_t err_size)
{
    if (err && err_size) {
        err[0] = '\0';
    }
    if (!id || id[0] == '\0') {
        snprintf(err, err_size, "no layout id to remove");
        return -1;
    }

    char path[MAX_PATH_LEN];
    if (layout_user_path(path, sizeof(path)) != 0) {
        snprintf(err, err_size, "cannot resolve user layout path");
        return -1;
    }
    if (access(path, R_OK) != 0) {
        return 0;   /* 没有用户文件：内置预设本就未被覆盖 */
    }

    LayoutCatalog user;
    layout_catalog_init(&user);
    char parse_err[192] = "";
    if (layout_spec_parse_file(path, &user, parse_err,
                               sizeof(parse_err)) != 0) {
        snprintf(err, err_size, "user layout file unusable: %s", parse_err);
        return -1;
    }

    int count = layout_user_catalog_remove(&user, id);
    if (count < 0) {
        snprintf(err, err_size, "invalid arguments");
        return -1;
    }
    if (count == 0) {
        if (remove(path) != 0) {
            snprintf(err, err_size, "cannot remove '%s'", path);
            return -1;
        }
        log_info("layout", "User layout file removed (empty): '%s'", path);
        return 0;
    }
    if (layout_spec_write_file(path, &user, err, err_size) != 0) {
        return -1;
    }
    log_info("layout", "Removed layout '%s' from '%s'", id, path);
    return 0;
}

/* ============================================================
 * 主布局创建
 * ============================================================ */

void create_layout(void)
{
    static int view_initialized = 0;
    if (!view_initialized) {
        g_current_view = VIEW_MAIN;
        g_menu_selected_idx = 0;
        view_initialized = 1;
    }

    if (!g_initialized) {
        ui_layout_init();
    }

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    LayoutResolved resolved;
    char layout_err[192] = "";
    if (layout_spec_resolve(&g_current, &resolved) != 0 ||
        layout_spec_validate(&g_current, layout_err, sizeof(layout_err)) != 0) {
        LayoutSpec fallback;
        if (layout_spec_build_fallback(&fallback) == 0 &&
            layout_spec_resolve(&fallback, &resolved) == 0) {
            log_warn("layout",
                     "Layout '%s' failed validation (%s); using classic fallback",
                     g_current_id, layout_err[0] ? layout_err : "resolve failed");
        } else {
            log_error("layout", "No usable layout; cannot create main area");
            refresh();
            render_menu_hint_bar();
            return;
        }
    }

    LayoutGeometry geometry;
    layout_geometry_compute(&resolved, rows, cols, &geometry);

    destroy_main_windows();
    controls_reset_popup();
    ui_layout_apply_colors();

    if (!geometry.valid) {
        refresh();
        render_menu_hint_bar();
        return;
    }

    if (create_windows(&geometry) != 0) {
        destroy_main_windows();
        layout_geometry_rescue(rows, cols, &geometry);
        if (create_windows(&geometry) != 0) {
            destroy_main_windows();
            refresh();
            render_menu_hint_bar();
            return;
        }
        log_warn("layout", "Fell back to full-screen playlist rescue layout");
    }

    reset_focus_for_layout();

    refresh();
    render_menu_hint_bar();
}
