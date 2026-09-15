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

/* 启动时调用：读入前端自有的远程配置并初始化下载器 */
void remote_view_init(void);
/* 退出时调用：停掉后台下载线程并释放队列 */
void remote_view_shutdown(void);

/* 设置页内容区渲染 / 输入（settings.c 按选中项分发） */
void remote_view_render(void);
void remote_view_handle_input(int ch);

/* UI 主循环每帧调用：取回后台线程的列目录结果与已下载曲目 */
void remote_view_tick(void);

/* 菜单入口：进入远程设备列表 */
void remote_enter_list_mode(void);

#endif /* UI_REMOTE_VIEW_H */
