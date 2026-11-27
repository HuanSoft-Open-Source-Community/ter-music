/**
 * @file karaoke_mode.h
 * @brief 卡拉OK（消人声）档位的取值域与纯模式状态
 *
 * 自 config v11 起，消人声实现改为 "ONNX 模型后台分离 + 缓存 + 播放源切换"
 * （见 audio/karaoke_cache.h、audio/karaoke_worker.h、audio/playback_thread.c）。
 * **旧 DSP（逐频点 STFT 中置提取）已彻底删除**：本头不再包含任何音频处理，
 * 只保留三档取值域与 karaoke_set_mode/get_mode/is_active 三个符号，避免
 * CLI、D-Bus、配置与 UI 断链。
 *
 * 三个符号的语义（与旧实现保持兼容，但不再触发 DSP）：
 *   - karaoke_set_mode(mode)：发布当前档位；越界值一律退回 OFF。实现位于
 *     audio.c，值为进程内单一状态（0/1/2），并在播放中按需切换播放源。
 *   - karaoke_get_mode()：返回当前档位。
 *   - karaoke_is_active()：档位不是 OFF 时为 1（仅表意，不代表正在做任何处理）。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#ifndef KARAOKE_MODE_H
#define KARAOKE_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── 档位（与配置键 karaoke_mode 的取值域一一对应） ──────────────── */

#define KARAOKE_MODE_OFF   0   /* 不启用：播放原声 */
#define KARAOKE_MODE_SOFT  1   /* 较弱人声：播放 0.4×原声 + 0.6×伴奏 缓存 */
#define KARAOKE_MODE_FULL  2   /* 完全消除：播放纯伴奏缓存 */
#define KARAOKE_MODE_COUNT 3

/**
 * 发布档位（越界值一律退回不启用，与配置的钳制语义一致）。
 *
 * 纯模式状态，不做任何音频处理；若播放中且新档位对应不同的播放源
 * （原声 / soft 缓存 / full 缓存）且缓存已就绪，audio.c 会在当前位置
 * 切换播放源（不重跑模型、不改变歌词/进度语义）。
 */
void karaoke_set_mode(int mode);

/** 取当前档位（0/1/2）。 */
int karaoke_get_mode(void);

/** 当前档位是否需要"模型伴奏"（非 OFF 时为 1；仅状态查询，不做处理）。 */
int karaoke_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_MODE_H */
