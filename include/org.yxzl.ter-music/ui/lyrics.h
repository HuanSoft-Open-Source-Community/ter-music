/**
 * @file lyrics.h
 * @brief 歌词面板渲染接口（前端）
 *
 * 歌词数据与推进归后端（`lyrics/lyrics.h`：加载、解析、来源偏好、
 * `lyrics_tick()` 由 `core_tick()` 每轮调用）；本头文件只声明**界面渲染**
 * 入口。界面读后端状态用 `lyrics/lyrics.h` 的只读接口，回放传输走
 * `player_*` 门面。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_LYRICS_H
#define UI_LYRICS_H

/* 把歌词面板画到 win_lyrics（无歌词时画频谱动画） */
void render_lyrics(void);

/* 按后端当前高亮行重绘（纯文本歌词的首帧也由它触发） */
void update_lyrics_display(void);

#endif /* UI_LYRICS_H */
