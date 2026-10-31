/**
 * @file menu_views.h
 * @brief 菜单视图模块头文件
 * 
 * 提供底部菜单栏功能，支持 F1-F6 功能键切换不同界面
 * 包括：设置、历史记录、播放列表、收藏夹、信息、退出
 * 
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 * @date 2026-03-22
 */

#ifndef MENU_VIEWS_H
#define MENU_VIEWS_H

#include "types.h"
#include "keymap/actions.h"

/* 底部提示条单项：动作、`显示键:名称` 文本与终端显示宽度。 */
typedef struct {
    ActionId action;
    char label[96];
    int width;
} MenuHintItem;

/* 按当前有效键位构建固定 9 项提示；返回写入条数。 */
int build_menu_hint_items(MenuHintItem *items, int max);

/* 执行全局动作（退出、视图切换、曲库开关）；已处理返回 1。 */
int execute_global_action(ActionId action);

void init_menu_views(void);
void render_menu_frame(const char *title);
void render_menu_sidebar(int selected_idx, const char **items, int item_count);
void handle_menu_input(int ch);
void switch_to_view(ViewMode view);
void exit_current_view(void);

void render_settings_content(void);
void render_history_content(void);
void render_favorites_content(void);
void render_info_content(void);
void render_help_content(void);
void render_language_content(void);
void rerender_active_view(void);
void render_playlist_manager_content(void);

void handle_function_keys(int fkey);

void add_history_entry(Track *track);

int add_to_favorites(Track *track);
int remove_from_favorites(int index);

void add_dir_history_entry(const char *path);
void clear_dir_history(void);

/* 配置读写（load/save/defaults/路径）已迁至 config/config.h；
 * 配置重载流程属核心职责（core_tick），界面只提供配色/刷新回调。 */

int create_user_playlist(const char *name);
int delete_user_playlist(int index);
int add_track_to_playlist(int playlist_idx, Track *track);
int remove_track_from_playlist(int playlist_idx, int track_idx);
int rename_user_playlist(int index, const char *new_name);

/* ensure_config_dir_exists() / get_config_dir() 已迁至 config/config.h */

/* JSON parser helpers — used by config_migration.c for v1→v2 config upgrade only */
char* extract_json_string(const char *json, const char *key, char *output, size_t output_size);
long  extract_json_int(const char *json, const char *key);
double extract_json_float(const char *json, const char *key);
/* 启动分叉（架构反转）：
 *   frontend_init_config(readonly_mirror) —— 配置镜像 + 主题：两种模式都要做。
 *       readonly_mirror=1（远端前端）只 load_config_readonly()，不迁移、不应用
 *       运行时字段、不设脏、不落盘；=0（TUI-local 即核心）走 core_config_apply()。
 *   content_init() —— 曲库/收藏/历史/歌单/迁移：两种模式都要做，因为内容始终
 *       由前端进程持有（remote 前端也需要读写自己的内容库）。
 * init_all_persistent_data() 是两者的组合。 */
void frontend_init_config(int readonly_mirror);
void content_init(void);
void init_all_persistent_data(int readonly_mirror);

void save_temp_playlist(void);
void cleanup_temp_playlist(void);
int load_temp_playlist(void);

void show_status_message(const char *msg);

void render_menu_hint_bar(void);

#endif
