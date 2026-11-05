/**
 * @file controls.c
 * @brief 控制栏按钮渲染及控制激活
 *
 * 从 ui.c 拆分，负责控制栏按钮的绘制、标签构建和控件激活逻辑。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-06-02
 */

#include "types.h"
#include "player/player.h"
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/keymap.h"
#include "i18n/i18n.h"
#include "ui/menu_internal.h"
#include "audio/audio.h"
#include "audio/play_queue.h"
#include "playlist/playlist.h"
#include "config/config.h"
#include "logger/logger.h"
#include "i18n/i18n.h"
#include "ui/menus.h"
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <string.h>

extern WINDOW *win_controls;

/* ── Control label i18n keys ──
 * 下标必须与 CONTROL_IDX_* 一一对应（编译期断言见文件尾部）。 */
static const char *control_label_keys[] = {
    "controls.label.prev",
    "controls.label.play_pause",
    "controls.label.next",
    "controls.label.stop",
    "controls.label.loop",
    "controls.label.speed",
    "controls.label.volume",
    "controls.label.karaoke"
};

/* 编译期护栏（两条，都是"少一个 case 不会报错"的 silent bug）：
 * 1) 文案表必须覆盖全部按钮格（进度条格没有文案，故为 CONTROL_COUNT-1）；
 * 2) 进度条格必须是最后一个下标，按钮行只渲染它之前的那些格。 */
typedef char control_labels_must_cover_every_button[
    ((sizeof(control_label_keys) / sizeof(control_label_keys[0])) == CONTROL_COUNT - 1)
        ? 1 : -1];
typedef char progress_must_be_the_last_control_index[
    (CONTROL_IDX_PROGRESS == CONTROL_COUNT - 1) ? 1 : -1];

/* ── Speed state (defined in audio.c) ── */

/* 卡拉OK（消人声）档位文案：设置页、CLI 与这里共用同一组 i18n 键。
 * 显示只读配置镜像（与 advanced_play_modes_enabled 同例）：远端模式下
 * 核心改档位后会广播 ConfigChanged，镜像随之刷新；不在每次重绘时发 RPC。 */
static const char *const k_karaoke_level_keys[KARAOKE_MODE_COUNT] = {
    "karaoke.level.off",
    "karaoke.level.soft",
    "karaoke.level.full"
};

static const char *karaoke_level_text(int mode)
{
    if (mode < KARAOKE_MODE_OFF || mode >= KARAOKE_MODE_COUNT) {
        mode = KARAOKE_MODE_OFF;
    }
    return i18n_get(k_karaoke_level_keys[mode]);
}

/* ── Playlist tab mode (defined in playlist_render.c) ── */
extern int g_playlist_tab_mode;

/* ── Popup state ── */
PopupState g_popup = {0};
#define VOLUME_POPUP_STEP 5

/* Forward declarations */
static void popup_dismiss(void);
static void render_controls_popup(void);

/* ============================================================
 * Control label helpers
 * ============================================================ */

static const char *get_control_label(int index)
{
    int count = (int)(sizeof(control_label_keys) / sizeof(control_label_keys[0]));
    if (index < 0 || index >= count) return "";
    return i18n_get(control_label_keys[index]);
}

void build_control_label(int index, char *dest, size_t dest_size)
{
    if (!dest || dest_size == 0) return;
    dest[0] = '\0';
    if (index < 0 || index >= CONTROL_COUNT) return;

    if (index == CONTROL_IDX_LOOP) {
        snprintf(dest, dest_size, "%s:%s", get_control_label(index), player_play_mode_name(0));
        return;
    }
    if (index == CONTROL_IDX_SPEED) {
        snprintf(dest, dest_size, "%s:%.2fx", get_control_label(index), (double)player_speed());
        return;
    }
    if (index == CONTROL_IDX_VOLUME) {
        snprintf(dest, dest_size, "%s:%d%%", get_control_label(index), player_volume_percent());
        return;
    }
    if (index == CONTROL_IDX_KARAOKE) {
        snprintf(dest, dest_size, "%s:%s", get_control_label(index),
                 karaoke_level_text(g_app_config.karaoke_mode));
        return;
    }
    format_display_text(dest, dest_size, get_control_label(index),
                        (int)dest_size - 1, 0);
}

/* ============================================================
 * Control content layout
 * ============================================================ */

/* 控件按钮按渲染宽度换行，整块在窗格内垂直居中；鼠标、进度条和
 * 可视化都使用同一份坐标，不再各自推算行号。 */
void controls_compute_layout(int height, int width, ControlsLayout *out)
{
    if (!out) return;

    memset(out, 0, sizeof(*out));
    out->progress_row = -1;
    out->visualizer_top = -1;
    out->visualizer_bottom = -1;
    for (int i = 0; i < CONTROL_COUNT; i++) {
        out->button_row[i] = -1;
        out->button_col[i] = -1;
        out->button_width[i] = 0;
    }

    if (height < 4 || width < 10) return;

    int available_width = width - 2;
    int max_label_cols = available_width - 4;
    if (max_label_cols < 1) return;

    int button_count = CONTROL_COUNT - 1;
    int button_width[CONTROL_COUNT] = {0};
    int button_row_index[CONTROL_COUNT] = {0};
    int row_width[CONTROL_COUNT + 1] = {0};
    int row_count = 0;

    for (int i = 0; i < button_count; i++) {
        char raw_label[256];
        char display_label[256];
        build_control_label(i, raw_label, sizeof(raw_label));
        format_display_text(display_label, sizeof(display_label),
                            raw_label, max_label_cols, 0);

        int label_width = utf8_str_width(display_label);
        int button_width_value = label_width + 4;
        if (button_width_value > available_width) {
            button_width_value = available_width;
        }

        if (row_count == 0) row_count = 1;
        if (row_width[row_count - 1] > 0 &&
            row_width[row_count - 1] + button_width_value > available_width) {
            row_count++;
        }
        button_width[i] = button_width_value;
        button_row_index[i] = row_count - 1;
        row_width[row_count - 1] += button_width_value;
    }

    int show_progress = height >= 5 && width >= 20 &&
                        player_play_state() != PLAY_STATE_STOPPED &&
                        player_duration_seconds() > 0;

    /* 优先使用状态行上方的完整内容区，避免状态消息盖住按钮；只有
     * 控件确实放不下时，才临时借用状态行。 */
    int block_h = (show_progress ? 1 : 0) + row_count;
    int content_rows = height - 3;
    if (content_rows < 1) content_rows = 1;
    if (block_h > content_rows) {
        content_rows = height - 2;
    }
    if (content_rows < 1) return;

    int visible_button_rows = row_count;
    if (show_progress) {
        int room = content_rows - 1;
        if (visible_button_rows > room) visible_button_rows = room;
    } else if (visible_button_rows > content_rows) {
        visible_button_rows = content_rows;
    }
    if (visible_button_rows < 0) visible_button_rows = 0;

    int visible_block_h = (show_progress ? 1 : 0) + visible_button_rows;
    if (visible_block_h <= 0) return;

    /* 在可用内容区内居中；放不下时贴边，绝不占边框。 */
    int top = 1 + (content_rows - visible_block_h + 1) / 2;
    if (top < 1) top = 1;
    if (top + visible_block_h - 1 > content_rows) {
        top = content_rows - visible_block_h + 1;
    }
    if (top < 1) top = 1;

    int next_row = top;
    if (show_progress) {
        out->progress_row = next_row;
        next_row++;
    }

    int row_cursor[CONTROL_COUNT + 1] = {0};
    for (int i = 0; i < button_count; i++) {
        int row_index = button_row_index[i];
        if (row_index >= visible_button_rows) continue;

        int row = next_row + row_index;
        if (row < 1 || row > height - 2) continue;

        int row_start = 1 + (available_width - row_width[row_index]) / 2;
        if (row_start < 1) row_start = 1;
        int col = row_start + row_cursor[row_index];
        row_cursor[row_index] += button_width[i];

        out->button_row[i] = row;
        out->button_col[i] = col;
        out->button_width[i] = button_width[i];
    }

    int block_bottom = top + visible_block_h - 1;
    int viz_top = block_bottom + 2; /* 中间保留一行分隔线 */
    int viz_bottom = height - 3;
    if (viz_top <= viz_bottom) {
        out->visualizer_top = viz_top;
        out->visualizer_bottom = viz_bottom;
    }
}

/* ============================================================
 * Status line rendering
 * ============================================================ */

static pthread_mutex_t g_controls_status_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_controls_status_message[256] = "";
static time_t g_controls_status_time = 0;
static int g_controls_status_drawn = 0;

void update_controls_status(const char *msg)
{
    if (msg && msg[0]) {
        log_debug("ui", "Controls status: %s", msg);
    }
    pthread_mutex_lock(&g_controls_status_mutex);
    snprintf(g_controls_status_message, sizeof(g_controls_status_message), "%s", msg ? msg : "");
    g_controls_status_time = time(NULL);
    pthread_mutex_unlock(&g_controls_status_mutex);
    request_ui_refresh(UI_DIRTY_CONTROLS);
}

void render_controls_status_line(void)
{
    if (!win_controls) return;

    int h, w;
    getmaxyx(win_controls, h, w);
    if (h <= 2 || w <= 3) return;

    char status_msg[sizeof(g_controls_status_message)];
    time_t status_time = 0;

    pthread_mutex_lock(&g_controls_status_mutex);
    snprintf(status_msg, sizeof(status_msg), "%s", g_controls_status_message);
    status_time = g_controls_status_time;
    pthread_mutex_unlock(&g_controls_status_mutex);

    int active = status_msg[0] != '\0' && (time(NULL) - status_time) < 3;
    if (!active) {
        /* 状态行可能被控件块临时借用；不能直接擦除，否则会擦掉控件。
         * 请求一次完整重绘，由 ui_pane_begin() 清屏后按无状态布局重画。 */
        if (g_controls_status_drawn) {
            g_controls_status_drawn = 0;
            request_ui_refresh(UI_DIRTY_CONTROLS);
        }
        return;
    }

    int status_row = h - 2;
    mvwhline(win_controls, status_row, 1, ' ', w - 2);
    ui_pane_clip_printf(win_controls, status_row, 2, "%s", status_msg);
    g_controls_status_drawn = 1;
}

/* 状态消息到期后主动触发一次整块重绘，避免它一直覆盖借用状态行的按钮。 */
void controls_status_tick(void)
{
    if (!g_controls_status_drawn) return;

    char status_msg[sizeof(g_controls_status_message)];
    time_t status_time = 0;
    pthread_mutex_lock(&g_controls_status_mutex);
    snprintf(status_msg, sizeof(status_msg), "%s", g_controls_status_message);
    status_time = g_controls_status_time;
    pthread_mutex_unlock(&g_controls_status_mutex);

    if (status_msg[0] != '\0' && (time(NULL) - status_time) < 3) return;

    g_controls_status_drawn = 0;
    request_ui_refresh(UI_DIRTY_CONTROLS);
}

/* ============================================================
 * Popup helpers
 * ============================================================ */

#define BASIC_MODE_COUNT 5
#define FOLDER_MODE_COUNT 4
#define ADVANCED_MODE_COUNT 8

static int calculate_available_play_modes(void)
{
    int count = BASIC_MODE_COUNT;
    if (g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER) {
        if (player_track_index() >= 0 || g_selected_index >= 0)
            count += FOLDER_MODE_COUNT;
    }
    if (g_app_config.advanced_play_modes_enabled)
        count += ADVANCED_MODE_COUNT;
    return count;
}

static PlayMode get_available_play_mode_at(int index)
{
    if (index < BASIC_MODE_COUNT)
        return (PlayMode)index;

    int folder_offset = BASIC_MODE_COUNT;
    int advanced_offset = folder_offset + FOLDER_MODE_COUNT;

    if (g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER &&
        (player_track_index() >= 0 || g_selected_index >= 0)) {
        if (index >= folder_offset && index < folder_offset + FOLDER_MODE_COUNT)
            return (PlayMode)(index - folder_offset + PLAY_MODE_FOLDER_SEQUENTIAL);
        advanced_offset = folder_offset + FOLDER_MODE_COUNT;
    } else {
        advanced_offset = folder_offset;
    }

    if (index >= advanced_offset)
        return (PlayMode)(index - advanced_offset + PLAY_MODE_ALBUM_SEQUENTIAL);

    return PLAY_MODE_SEQUENTIAL;
}

static void calculate_popup_dimensions(PopupState *popup, WINDOW *parent)
{
    if (!parent) return;
    int parent_h, parent_w;
    getmaxyx(parent, parent_h, parent_w);
    int popup_h = popup->option_count + 2;
    /* Cap to a reasonable max, no longer constrained by parent height */
    if (popup_h > 20) popup_h = 20;
    int popup_w = 26;
    popup->height = popup_h;
    popup->width = popup_w;
    /* Position relative to parent window's top-left corner on screen */
    int parent_screen_y, parent_screen_x;
    getbegyx(parent, parent_screen_y, parent_screen_x);
    popup->start_row = parent_screen_y + (parent_h - popup_h) / 2;
    popup->start_col = parent_screen_x + (parent_w - popup_w) / 2;
    if (popup->start_row < 1) popup->start_row = 1;
    if (popup->start_col < 1) popup->start_col = 1;
    /* Clamp bottom edge so the popup doesn't overflow the screen */
    int screen_h, screen_w;
    getmaxyx(stdscr, screen_h, screen_w);
    (void)screen_w;
    if (popup->start_row + popup->height > screen_h)
        popup->start_row = screen_h - popup->height;
    if (popup->start_row < 1) popup->start_row = 1;
}

static void destroy_popup_window(PopupState *popup)
{
    if (popup->popup_win) {
        WINDOW *w = (WINDOW *)popup->popup_win;
        /* 仅释放窗口，不刷新—下层窗口将在后续全量重绘中恢复 */
        delwin(w);
        popup->popup_win = NULL;
    }
}

static void create_popup_window(PopupState *popup)
{
    destroy_popup_window(popup); /* clean up any lingering window */
    WINDOW *w = newwin(popup->height, popup->width, popup->start_row, popup->start_col);
    if (!w) return;
    popup->popup_win = (void *)w;
    log_debug("controls", "popup shown type=%d rect=%d,%d,%d,%d",
              (int)popup->type, popup->start_row, popup->start_col,
              popup->height, popup->width);
}

static void build_popup_option_text(PopupType type, int index,
                                     char *dest, size_t dest_size)
{
    if (!dest || dest_size == 0) return;
    switch (type) {
        case POPUP_LOOP_MODE: {
            PlayMode mode = get_available_play_mode_at(index);
            snprintf(dest, dest_size, "%s", player_play_mode_name_of(mode, 0));
            break;
        }
        case POPUP_SPEED: {
            float speeds[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
            if (index >= 0 && index < 6)
                snprintf(dest, dest_size, "%.2fx", (double)speeds[index]);
            break;
        }
        case POPUP_VOLUME: {
            int vol = index * VOLUME_POPUP_STEP;
            snprintf(dest, dest_size, "%d%%", vol);
            break;
        }
        case POPUP_KARAOKE:
            snprintf(dest, dest_size, "%s", karaoke_level_text(index));
            break;
        default: dest[0] = '\0';
    }
}

static void apply_popup_selection(void)
{
    switch (g_popup.type) {
        case POPUP_LOOP_MODE:
            player_set_play_mode(get_available_play_mode_at(g_popup.selected_index));
            break;
        case POPUP_SPEED: {
            /* 倍速是引擎侧即时项：走门面（本地后端会同时落盘并热应用） */
            player_set_speed_index(g_popup.selected_index);
            char msg[64];
            snprintf(msg, sizeof(msg), "%s: %.2fx",
                     i18n_get("controls.label.speed"), (double)player_speed());
            update_controls_status(msg);
            break;
        }
        case POPUP_VOLUME: {
            int new_vol = g_popup.selected_index * VOLUME_POPUP_STEP;
            player_set_volume(new_vol);
            break;
        }
        case POPUP_KARAOKE: {
            /* 消人声档位：走门面（本地=引擎+落盘，远端=Control.SetKaraoke），
             * 提示文字取门面回写后的镜像，核心没接受就不会谎报档位。 */
            player_set_karaoke_mode(g_popup.selected_index);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s: %s",
                     i18n_get("controls.label.karaoke"),
                     karaoke_level_text(g_app_config.karaoke_mode));
            update_controls_status(msg);
            break;
        }
        default: break;
    }
}

int handle_popup_action(ActionId action)
{
    if (!g_popup.active) return 0;

    switch (action) {
        case ACTION_POPUP_UP:
            if (g_popup.selected_index > 0)
                g_popup.selected_index--;
            render_controls_popup();  /* only redraw popup, not full controls */
            return 1;
        case ACTION_POPUP_DOWN:
            if (g_popup.selected_index < g_popup.option_count - 1)
                g_popup.selected_index++;
            render_controls_popup();  /* only redraw popup, not full controls */
            return 1;
        case ACTION_POPUP_ACCEPT:
            apply_popup_selection();
            popup_dismiss();
            return 1;
        case ACTION_POPUP_CANCEL:
            popup_dismiss();
            return 1;
        default:
            return 0;
    }
}

/* ============================================================
 * Control activation
 * ============================================================ */

void activate_current_control(void)
{
    switch (g_current_control_idx) {
        case CONTROL_IDX_PREV:
            player_prev();
            break;
        case CONTROL_IDX_PLAY_PAUSE: {
            PlayState current_state = player_play_state();

            if (current_state == PLAY_STATE_PLAYING) {
                player_pause();
            } else if (current_state == PLAY_STATE_PAUSED) {
                player_resume();
            } else if (current_state == PLAY_STATE_STOPPED) {
                int playlist_total = playlist_count();
                if (playlist_is_loaded() && playlist_total > 0) {
                    int target_index = (player_track_index() >= 0)
                        ? player_track_index()
                        : g_selected_index;
                    if (g_sort_state.active && player_track_index() < 0) {
                        target_index = g_sort_state.sorted_indices[g_selected_index];
                    }
                    /* Tree mode: translate visible index to track index */
                    if (playlist_tree_is_active() && g_playlist_tab_mode == PLAYLIST_MODE_FILE_BROWSER
                        && player_track_index() < 0) {
                        int ti = get_visible_node_track_index(g_selected_index);
                        if (ti >= 0) target_index = ti;
                    }
                    if (target_index >= 0 && target_index < playlist_total) {
                        ui_play_track_index(target_index);
                    }
                }
            }
            break;
        }
        case CONTROL_IDX_NEXT:
            player_next();
            break;
        case CONTROL_IDX_STOP:
            player_stop();
            break;
        case CONTROL_IDX_LOOP:
            if (g_popup.active) {
                popup_dismiss();
            } else {
                g_popup.type = POPUP_LOOP_MODE;
                g_popup.option_count = calculate_available_play_modes();
                /* Find the popup index that maps to the current play mode */
                g_popup.selected_index = 0;
                for (int i = 0; i < g_popup.option_count; i++) {
                    if ((int)get_available_play_mode_at(i) == player_play_mode()) {
                        g_popup.selected_index = i;
                        break;
                    }
                }
                calculate_popup_dimensions(&g_popup, win_controls);
                create_popup_window(&g_popup);
                g_popup.active = (g_popup.popup_win != NULL);
            }
            break;
        case CONTROL_IDX_SPEED:
            if (g_popup.active) {
                popup_dismiss();
            } else {
                g_popup.type = POPUP_SPEED;
                g_popup.selected_index = player_speed_index();
                g_popup.option_count = player_speed_step_count();
                calculate_popup_dimensions(&g_popup, win_controls);
                create_popup_window(&g_popup);
                g_popup.active = (g_popup.popup_win != NULL);
            }
            break;
        case CONTROL_IDX_VOLUME:
            if (g_popup.active) {
                popup_dismiss();
            } else {
                g_popup.type = POPUP_VOLUME;
                g_popup.selected_index = player_volume_percent() / VOLUME_POPUP_STEP;
                g_popup.option_count = 100 / VOLUME_POPUP_STEP + 1;
                calculate_popup_dimensions(&g_popup, win_controls);
                create_popup_window(&g_popup);
                g_popup.active = (g_popup.popup_win != NULL);
            }
            break;
        case CONTROL_IDX_KARAOKE:
            if (g_popup.active) {
                popup_dismiss();
            } else {
                g_popup.type = POPUP_KARAOKE;
                /* 弹层下标就是档位值（0/1/2），无需映射表 */
                g_popup.selected_index = g_app_config.karaoke_mode;
                if (g_popup.selected_index < KARAOKE_MODE_OFF ||
                    g_popup.selected_index >= KARAOKE_MODE_COUNT) {
                    g_popup.selected_index = KARAOKE_MODE_OFF;
                }
                g_popup.option_count = KARAOKE_MODE_COUNT;
                calculate_popup_dimensions(&g_popup, win_controls);
                create_popup_window(&g_popup);
                g_popup.active = (g_popup.popup_win != NULL);
                /* 打开即说明三档各是什么意思（TTL 3 秒的状态行） */
                if (g_popup.active) {
                    update_controls_status(i18n_get("karaoke.hint"));
                }
            }
            break;
    }
}

/* ============================================================
 * Popup rendering
 * ============================================================ */

static void render_controls_popup(void)
{
    if (!g_popup.active) return;
    WINDOW *popup_win = (WINDOW *)g_popup.popup_win;
    if (!popup_win) return;

    int popup_h = g_popup.height;
    int popup_w = g_popup.width;
    int visible_options = popup_h - 2;

    /* Scroll offset: keep selected_index visible */
    int scroll_offset = 0;
    if (g_popup.selected_index >= visible_options)
        scroll_offset = g_popup.selected_index - visible_options + 1;

    /* Step 1 — clear window (default background, no color pair) */
    werase(popup_win);

    /* Step 2 — draw border and title with HIGHLIGHT color pair */
    wattrset(popup_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    rounded_box(popup_win);

    const char *title = "";
    switch (g_popup.type) {
        case POPUP_LOOP_MODE: title = i18n_get("popup.play_mode"); break;
        case POPUP_SPEED:     title = i18n_get("popup.speed"); break;
        case POPUP_VOLUME:    title = i18n_get("popup.volume"); break;
        case POPUP_KARAOKE:   title = i18n_get("popup.karaoke"); break;
        default: break;
    }
    if (title[0])
        mvwprintw(popup_win, 0, 2, "%s", title);

    /* Step 3 — fill all option rows with HIGHLIGHT background */
    for (int i = 0; i < visible_options; i++)
        mvwprintw(popup_win, i + 1, 1, " %-*s ", popup_w - 4, "");

    /* Step 4 — draw option text with HIGHLIGHT color pair (same as the
       fill background), selected item gets A_REVERSE for contrast. */
    for (int i = 0; i < visible_options && (scroll_offset + i) < g_popup.option_count; i++) {
        int opt_idx = scroll_offset + i;
        char option_text[48];
        build_popup_option_text(g_popup.type, opt_idx, option_text, sizeof(option_text));

        if (opt_idx == g_popup.selected_index)
            wattrset(popup_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_REVERSE);
        else
            wattrset(popup_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
        mvwprintw(popup_win, i + 1, 1, " %-*s ", popup_w - 4, option_text);
    }

    /* Step 5 — scroll indicator arrows */
    wattrset(popup_win, COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
    if (scroll_offset > 0)
        mvwaddch(popup_win, 1, popup_w < 3 ? 1 : popup_w - 3, '^');
    if (scroll_offset + visible_options < g_popup.option_count)
        mvwaddch(popup_win, popup_h - 2 < 1 ? 1 : popup_h - 2,
                 popup_w < 3 ? 1 : popup_w - 3, 'v');

    /* Step 6 — cleanup and flush */
    wattrset(popup_win, A_NORMAL);
    ui_refresh_window(popup_win);
}

/* ============================================================
 * Popup dismiss — force full refresh of all UI elements
 * ============================================================ */

static void popup_dismiss(void)
{
    if (!g_popup.active) return;
    destroy_popup_window(&g_popup);
    g_popup.active = 0;

    /* Step 1 — overwrite popup content in curscr's gap areas (areas
     * between sub-windows) by copying stdscr's clean background to
     * curscr.  wrefresh(popup_win) called wnoutrefresh(popup_win)
     * which wrote into curscr; the gap rows are never covered by the
     * sub-window wnoutrefresh calls below, so we must scrub them here
     * before doupdate() sees them. */
    redrawwin(stdscr);
    wnoutrefresh(stdscr);

    /* Step 2 — force full re-render of playlist and controls so
     * their sub-window content overwrites the remaining areas. */
    if (win_playlist) redrawwin(win_playlist);
    if (win_controls) redrawwin(win_controls);
    render_playlist_content();
    render_controls();
    render_menu_hint_bar();
}

/* 布局重建时丢弃残留弹窗：只销毁窗口，不触发旧窗口重绘。 */
void controls_reset_popup(void)
{
    destroy_popup_window(&g_popup);
    g_popup.active = 0;
}

/* 流光彩蛋整屏合成用：弹层当前是否可见，以及它的窗口。 */
WINDOW *controls_popup_window(void)
{
    return g_popup.active ? (WINDOW *)g_popup.popup_win : NULL;
}

/* ============================================================
 * Popup mouse handling
 * ============================================================ */

int popup_handle_mouse_click(int screen_y, int screen_x)
{
    if (!g_popup.active || !g_popup.popup_win)
        return -1;

    int py = g_popup.start_row;
    int px = g_popup.start_col;
    int ph = g_popup.height;
    int pw = g_popup.width;
    int visible_options = ph - 2;

    /* Scroll offset applied to the popup */
    int scroll_offset = 0;
    if (g_popup.selected_index >= visible_options)
        scroll_offset = g_popup.selected_index - visible_options + 1;

    if (screen_y >= py && screen_y < py + ph &&
        screen_x >= px && screen_x < px + pw) {
        /* Click inside popup — select option */
        int option_row = screen_y - py - 1;
        int opt_idx = scroll_offset + option_row;

        if (option_row >= 0 && option_row < visible_options &&
            opt_idx >= 0 && opt_idx < g_popup.option_count) {
            g_popup.selected_index = opt_idx;
            apply_popup_selection();
        }

        popup_dismiss();
        return 1;  /* fully handled */
    }

    /* Click outside popup — dismiss.
     * 返回 0 表示"没有选中任何项"，但**事件本身已由弹层消费**：调用方
     * （ui/mouse.c 的全局分发）不得再把这次点击交给弹层背后的窗格，
     * 否则一次点击会同时"收起弹层"和"播放某一行"。 */
    popup_dismiss();
    return 0;  /* dismissed */
}

/* ── 弹层展开状态与几何（供全局鼠标分发判归属） ─────────────────────
 * 用 PopupState 的 start_row/start_col/height/width：弹层是画在 stdscr 上的
 * 独立窗口，几何就是它自己的屏幕矩形，无需 getbegyx。 */

int controls_popup_expanded(void)
{
    return (g_popup.active && g_popup.popup_win) ? 1 : 0;
}

/* 右键上下文菜单的竞争规则：弹层展开时右键=取消弹层并吞掉。 */
int controls_popup_cancel(void)
{
    if (!controls_popup_expanded()) {
        return 0;
    }
    popup_dismiss();
    return 1;
}

int controls_popup_rect(UIRect *out)
{
    if (!out || !controls_popup_expanded()) return 0;
    out->top = g_popup.start_row;
    out->left = g_popup.start_col;
    out->rows = g_popup.height;
    out->cols = g_popup.width;
    return 1;
}

/* 弹层展开时，滚轮一格 = 上下移动一个候选项。
 * 边界钳制与只重绘弹层都由 handle_popup_action 负责（键盘共用同一条路径）。 */
int controls_popup_handle_wheel(int delta)
{
    return handle_popup_action(delta < 0 ? ACTION_POPUP_UP : ACTION_POPUP_DOWN);
}

/* ── 控制区窗格上的滚轮：按指针所在的那一行决定做什么 ───────────────
 * 窗格几何与按钮命中由调用方（ui/mouse.c，用 controls_compute_layout 与
 * get_control_index_from_window_point）算好后传进来，这里只决定语义：
 *   进度条行 → 快进/快退 UI_SEEK_STEP_SECONDS 秒
 *   音量按钮 → 音量 ±UI_VOLUME_STEP_PERCENT
 *   倍速按钮 → 档位循环（到头回绕，与音量/播放模式弹层的循环感一致）
 *   其余按钮（上一首/播放/下一首/停止/播放模式/卡拉OK）→ 不做事，但事件已归此处 */
int controls_handle_wheel_control(int control_index, int delta)
{
    switch (control_index) {
        case CONTROL_IDX_PROGRESS:
            seek_relative_seconds(delta * UI_SEEK_STEP_SECONDS);
            render_controls();
            return 1;

        case CONTROL_IDX_VOLUME:
            player_adjust_volume(delta * UI_VOLUME_STEP_PERCENT);
            render_controls();
            return 1;

        case CONTROL_IDX_SPEED: {
            int count = player_speed_step_count();
            if (count <= 0) return 1;
            int idx = player_speed_index();
            int next = ((idx + delta) % count + count) % count;
            player_set_speed_index(next);
            char msg[64];
            snprintf(msg, sizeof(msg), "%s: %.2fx",
                     i18n_get("controls.label.speed"), (double)player_speed());
            update_controls_status(msg);
            render_controls();
            return 1;
        }

        default:
            /* 其它按钮的滚轮语义未被定义：不臆造动作（例如滚轮切播放模式
             * 会让人误触），但事件已归控制区，不再穿透。 */
            return 1;
    }
}

/* ============================================================
 * Controls rendering
 * ============================================================ */

void render_controls(void)
{
    if (!win_controls) return;

    int h, w;
    getmaxyx(win_controls, h, w);
    if (h < 1 || w < 1) return;

    const char *focus_hint = g_control_focus
        ? i18n_get("controls.ctrl_focus")
        : i18n_get("controls.list_focus");
    const char *lyric_hint = g_lyric_cursor_mode
        ? i18n_get("controls.ctrl_l_exit_seek")
        : i18n_get("controls.ctrl_l_lyric_seek");
    char controls_header[160];
    snprintf(controls_header, sizeof(controls_header), "%s %s %s %s %s %s",
             i18n_get("controls.controls_area"),
             i18n_get("controls.space_run"),
             i18n_get("controls.c_ctrl"),
             i18n_get("controls.l_list"),
             i18n_get("controls.tab_view"),
             focus_hint);
    char header_line[1024];
    snprintf(header_line, sizeof(header_line), " %s %s", controls_header, lyric_hint);

    ui_pane_begin(LAYOUT_PANE_CONTROLS, win_controls, header_line);
    if (h < 4 || w < 10) {
        ui_refresh_window(win_controls);
        return;
    }

    ControlsLayout layout;
    controls_compute_layout(h, w, &layout);

    // Progress bar
    int duration = player_duration_seconds();
    if (layout.progress_row >= 0 && duration > 0) {
        int progress_row = layout.progress_row;
        int current_pos = player_position_seconds();
        if (current_pos < 0) current_pos = 0;
        if (current_pos > duration) current_pos = duration;

        int progress_percent = (current_pos * 100) / duration;
        if (progress_percent > 100) progress_percent = 100;

        int current_min = current_pos / 60;
        int current_sec = current_pos % 60;
        int total_min = duration / 60;
        int total_sec = duration % 60;
        current_min %= 100;
        total_min %= 100;

        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%02d:%02d / %02d:%02d",
                 current_min, current_sec, total_min, total_sec);
        ui_pane_clip_printf(win_controls, progress_row, 2, "%s", time_str);

        int time_width = 13;
        int percent_width = 4;
        int padding = 6;
        int progress_bar_width = w - time_width - percent_width - padding - 4;
        if (progress_bar_width >= 1) {
            int progress_start_col = 2 + time_width + 1;

            ui_pane_clip_printf(win_controls, progress_row, progress_start_col, "[");

            int filled_width = (progress_bar_width * progress_percent) / 100;
            if (filled_width > progress_bar_width) filled_width = progress_bar_width;

            for (int i = 0; i < progress_bar_width && (progress_start_col + 1 + i) < w - 2; i++) {
                char c = '-';
                if (i < filled_width) c = '=';
                else if (i == filled_width && progress_percent < 100) c = '>';
                mvwaddch(win_controls, progress_row, progress_start_col + 1 + i, c);
            }

            ui_pane_clip_printf(win_controls, progress_row, progress_start_col + 1 + progress_bar_width, "]");
            ui_pane_clip_printf(win_controls, progress_row, progress_start_col + 2 + progress_bar_width, "%d%%", progress_percent);
        }

        mvwaddstr(win_controls, progress_row, 0, "\xe2\x94\x82");
        mvwaddstr(win_controls, progress_row, w - 1, "\xe2\x94\x82");
    }

    // Control buttons
    int max_label_cols = w - 6;
    for (int i = 0; i < CONTROL_COUNT - 1; i++) {
        if (layout.button_row[i] < 0 || layout.button_width[i] <= 0) {
            continue;
        }

        char raw_label[256];
        char display_label[256];
        build_control_label(i, raw_label, sizeof(raw_label));
        if (max_label_cols < 1) break;
        format_display_text(display_label, sizeof(display_label),
                            raw_label, max_label_cols, 0);

        int current_row = layout.button_row[i];
        int current_col = layout.button_col[i];
        int button_width = layout.button_width[i];
        if (current_row < 1 || current_row > h - 2 ||
            current_col < 1 || current_col + button_width > w - 1) {
            continue;
        }

        if (i == g_current_control_idx && g_control_focus == 1) {
            wattron(win_controls, A_REVERSE | A_BOLD);
            mvwprintw(win_controls, current_row, current_col, " [%s] ", display_label);
            wattroff(win_controls, A_REVERSE | A_BOLD);
        } else {
            mvwprintw(win_controls, current_row, current_col, " [%s] ", display_label);
        }
    }

    render_controls_status_line();
    /* 旧版没有独立频谱窗格，频谱条画在控制窗格下部；布局系统引入独立
     * spectrum 窗格后，再画就是同一屏两份频谱。故仅在布局没有独立
     * 频谱窗格时（focus/karaoke 等）保留旧的封面+频谱行为。 */
    if (!ui_pane_is_visible(LAYOUT_PANE_SPECTRUM)) {
        render_visualizer_with_album_cover();
    }
    /* 封面/可视化之后补边框，防止字形溢出盖住边框。 */
    ui_pane_repaint_border(LAYOUT_PANE_CONTROLS, win_controls);

    /* Refresh controls base window first, then overlay popup on top */
    wnoutrefresh(win_controls);
    if (g_popup.active && g_popup.popup_win) {
        render_controls_popup();
    }
    doupdate();
}
