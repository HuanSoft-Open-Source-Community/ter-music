/**
 * @file karaoke.h
 * @brief 卡拉OK（消人声）模式 — 频带受限中置消除
 *
 * Band-limited centre-channel cancellation: 人声大多录在立体声正中，
 * 因此位于 mid=(L+R)/2 里、且只在人声频带（约 180Hz–6kHz）内做抵消；
 * 低频（贝斯/底鼓）与高频（空气感）的居中能量原样保留，side=(L-R)/2
 * 全程不动，所以声像不会塌陷、低音不会变薄。
 * 处理对象是 S32 交织 PCM，就地修改；不启用或单声道时直接返回。
 *
 * 注意：本模块名 karaoke 与"歌词优先"布局预设 layout.preset.karaoke、
 * 以及歌词逐字高亮、科乐美流光彩蛋无关，只是同名不同物。
 *
 * @author ter-music team
 * @date 2026-11-05
 */

#ifndef KARAOKE_H
#define KARAOKE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 档位（与配置键 karaoke_mode 的取值域一一对应） ──────────────── */

#define KARAOKE_MODE_OFF   0   /* 不启用：完全直通，零额外开销 */
#define KARAOKE_MODE_SOFT  1   /* 保留较弱人声 */
#define KARAOKE_MODE_FULL  2   /* 完全消除人声 */
#define KARAOKE_MODE_COUNT 3

/* ── DSP 常量 ───────────────────────────────────────────────────── */

/* 抵消频带的下/上边界（Hz）。取 180Hz 是为了护住贝斯与底鼓的居中能量，
 * 取 6kHz 是因为人声清晰度之上主要是乐器的空气感，不该一起抹掉。
 * 两侧都用 4 阶 Butterworth（斜率 24dB/倍频），裙边够陡才不会把这两个
 * 频点附近的能量一起拖进抵消带——实测见 scripts/test/test_karaoke.c。 */
#define KARAOKE_LO_HZ       180
#define KARAOKE_HI_HZ       6000

/* 抵消系数 α：频带内 mid 能量乘以 (1-α)。
 * soft 档残留约 -8dB（仍可辨到人声，符合"留较弱人声"的语义），
 * full 档 α=1，频带内居中内容完全抵消。 */
#define KARAOKE_ALPHA_SOFT  0.60f
#define KARAOKE_ALPHA_FULL  1.00f

/* 档位切换时 α 的逼近时间常数（毫秒）。直接阶跃会在播放中"啪"一声，
 * 一阶指数逼近把变化摊到约 30ms 内，听不出台阶也不拖响应。 */
#define KARAOKE_RAMP_TC_MS  30

/* 本算法的工作声道数：必须有左右两个声道才能分离出"居中"与"两侧"。
 * 解码链路已由 swresample 归到 1/2 声道，因此非 2 声道一律直通。 */
#define KARAOKE_STEREO_CHANNELS 2

/* ── 生命周期 ───────────────────────────────────────────────────── */

/** 初始化模块（清零状态、档位为不启用）。启动时调用。 */
void karaoke_init(void);

/**
 * 就地处理一帧块的交织 S32 PCM。不启用或单声道时为无操作。
 * 只能由播放线程调用（内部状态无锁，与 eq_process 同级约定）。
 *
 * @param samples      交织 S32 缓冲（就地修改）
 * @param frame_count  本次处理的帧数
 * @param channels     声道数（仅 2 声道处理，单声道直通）
 * @param sample_rate  采样率（用于分频系数重算）
 */
void karaoke_process(int32_t *samples, int frame_count, int channels, int sample_rate);

/* ── 运行时控制 ─────────────────────────────────────────────────── */

/** 设置档位（越界值一律退回不启用，与配置的钳制语义一致）。 */
void karaoke_set_mode(int mode);

/** 取当前档位（0/1/2）。 */
int karaoke_get_mode(void);

/** 当前档位是否需要处理（不启用时为 0）。供调用方跳过函数调用。 */
int karaoke_is_active(void);

/**
 * 清空滤波器状态：换曲、seek、变速重建 atempo 之后必须调用，
 * 否则上一段音频的延迟线残留会在起播瞬间冒出杂声。
 * 只允许播放线程调用。
 */
void karaoke_reset(void);

/** 档位对应的抵消系数（只读，供测试与调试使用）。 */
float karaoke_alpha_for_mode(int mode);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_H */
