/**
 * @file remote_view.h
 * @brief 设置页的“远程设备”页（前端功能：服务器管理 + 浏览 + 下载播放）
 *
 * 远程音乐源由**前端**负责：核心只播放本地文件（见
 * scripts/test/check-core-purity.sh）。服务器列表存在前端自有的
 * `<configdir>/remote.xml`，目录浏览与下载由后台线程完成，下载好的本地
 * 缓存路径经 player 门面交给核心。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_REMOTE_VIEW_H
#define UI_REMOTE_VIEW_H

#include "keymap/actions.h"   /* ActionId */

/* 启动时调用：读入前端自有的远程配置并初始化下载器 */
void remote_view_init(void);
/* 退出时调用：停掉后台下载线程并释放队列 */
void remote_view_shutdown(void);

/* 设置页内容区渲染 / 输入（settings.c 按选中项分发） */
void remote_view_render(void);
int remote_view_handle_input(int ch);

/**
 * @brief 按动作处理远程页输入（键盘解析结果与鼠标滚轮共用）。
 *
 * 鼠标不合成按键码：用户可改键，合成按键在改键后会失灵或做错事。
 */
int remote_view_handle_action(ActionId action);

/**
 * @brief 处理落在远程页某一行的鼠标点击。
 *
 * 远程页四种模式的行号推导各不相同（目录模式还有滚动偏移），命中表由
 * remote_view_render() 逐帧记录，因此选中状态只在本模块内解释，设置页
 * 只传屏幕行。
 * @param screen_y 屏幕行
 * @param activate 1 = 选中并执行（左键）；0 = 只选中（右键菜单路径）
 * @return 1 = 命中并处理；0 = 该行不可选（标题/提示/空白）
 */
int remote_view_handle_click(int screen_y, int activate);

/* UI 主循环每帧调用：取回后台线程的列目录结果与已下载曲目 */
void remote_view_tick(void);

/* 菜单入口：进入远程设备列表 */
void remote_enter_list_mode(void);

/* 当前是否处于连接表单编辑模式（右键“保存远程配置”只在此时有效）。 */
int remote_view_form_active(void);

#endif /* UI_REMOTE_VIEW_H */
