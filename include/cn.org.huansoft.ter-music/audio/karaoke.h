/**
 * @file karaoke.h
 * @brief 卡拉OK（消人声）模式 — 频带受限、相似度门控的中置消除
 *
 * Band-limited, similarity-gated centre-channel cancellation: 人声大多录在
 * 立体声正中（左右一致），因此位于 mid=(L+R)/2 里、人声频带（约 200Hz–5kHz）
 * 内的居中成分按档位抵消；低频（贝斯/底鼓）与高频（空气感）的居中能量原样
 * 保留，side=(L-R)/2 全程不动。抵消带内再分两个半带做“相似度”探测：只有
 * 两个半带都显示 side 与 mid 同能量（内容是立体声伴奏而非人声）时才整带
 * 放行——纯伴奏的前奏、间奏、铺底不再被误伤；人声一进场，抵消强度与旧版
 * 逐位相同（≥30dB 回归测试不变）。
 * 处理对象是 S32 交织 PCM，就地修改；不启用或单声道时直接返回。
 *
 * 注意：本模块名 karaoke 与"歌词优先"布局预设 layout.preset.karaoke、
 * 以及歌词逐字高亮、科乐美流光彩蛋无关，只是同名不同物。
 *
 * @author ter-music team
 * @date 2026-11-06
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

/* 抵消频带的下/上边界（Hz）。取 200Hz 是为了护住贝斯与底鼓的居中能量，
 * 取 5kHz 是因为人声清晰度之上主要是乐器的空气感与镲片泛音，不该一起
 * 抹掉。两侧都用 4 阶 Butterworth（斜率 24dB/倍频），裙边够陡才不会把
 * 这两个频点附近的能量一起拖进抵消带——实测见 scripts/test/test_karaoke.c。 */
#define KARAOKE_LO_HZ       200
#define KARAOKE_HI_HZ       5000

/* 相似度门控：抵消带内按此频点分两个半带只做“探测”（1kHz 以下偏
 * 节奏组与人声基频域，以上偏质感域），逐半带比较 mid 与 side 的带内
 * 能量包络，取两半带门值的最大者缩放整带抵消——只有两个半带都呈
 * 现 side≈mid 的纯立体声特征（前奏/间奏的铺底伴奏）才整带放行；
 * 人声进场时任一半带出现“mid 有、side 无”即恢复满额抵消，且抵消
 * 项与 band 同相，不留正交残差。分半带探测的意义：伴奏恰好落在另
 * 一半带时，不会把出现人声的半带判定也一起污染掉。 */
#define KARAOKE_SPLIT_HZ    1000
#define KARAOKE_GATE_TC_MS  15    /* 包络跟随时间常数（毫秒） */
#define KARAOKE_GATE_R_LO   0.45f /* side/mid 能量比 ≤ 此值：视为单声道，全力抵消 */
#define KARAOKE_GATE_R_HI   0.80f /* ≥ 此值：立体声内容，不抵消；中间线性锥落 */

/* 抵消系数 α：频带内 mid 能量乘以 (1-α)，门控后再乘门值。
 * soft 档残留约 -8dB（仍可辨到人声，符合"留较弱人声"的语义），
 * full 档 α=1，频带内居中单声道内容完全抵消。 */
#define KARAOKE_ALPHA_SOFT  0.60f
#define KARAOKE_ALPHA_FULL  1.00f

/* 档位切换时 α 的逼近时间常数（毫秒）。直接阶跃会在播放中"啪"一声，
 * 一阶指数逼近把变化摊到约 30ms 内，听不出台阶也不拖响应。
 * 升入启用档从 α=0 起坡；降回不启用档向 α=0 收坡，坡走完才直通。 */
#define KARAOKE_RAMP_TC_MS  30

/* 本算法的工作声道数：必须有左右两个声道才能分离出"居中"与"两侧"。
 * 解码链路已由 swresample 归到 1/2 声道，因此非 2 声道一律直通。 */
#define KARAOKE_STEREO_CHANNELS 2

/* ── 生命周期 ───────────────────────────────────────────────────── */

/** 初始化模块（清零状态、档位为不启用）。启动时调用。 */
void karaoke_init(void);

/**
 * 就地处理一帧块的交织 S32 PCM。不启用（含收坡完毕）或单声道时为无操作。
 * 只能由播放线程调用（内部状态无锁，与 eq_process 同级约定）。
 * 档位切换的请求由 karaoke_set_mode 发布、由本函数在块首消化：进入
 * 启用档时清零重建并从 α=0 起坡，退出到不启用档时向 α=0 收坡。
 * 函数自带直通快速路径，调用方无需（也不应）先查询档位再决定是否调用。
 *
 * @param samples      交织 S32 缓冲（就地修改）
 * @param frame_count  本次处理的帧数
 * @param channels     声道数（仅 2 声道处理，单声道直通）
 * @param sample_rate  采样率（用于分频系数重算）
 */
void karaoke_process(int32_t *samples, int frame_count, int channels, int sample_rate);

/* ── 运行时控制 ─────────────────────────────────────────────────── */

/**
 * 设置档位（越界值一律退回不启用，与配置的钳制语义一致）。
 * 任意线程可调：只发布请求档位（单字写入），下一块 PCM 起由播放线程
 * 平滑应用；返回时新档位即已受理，karaoke_get_mode 立即可见。
 */
void karaoke_set_mode(int mode);

/** 取当前档位（0/1/2）：最近一次被受理的请求档位，不等待 DSP 起坡。 */
int karaoke_get_mode(void);

/** 请求档位是否需要处理（不启用时为 0）。仅供展示/测试查询；
 *  播放路径不要用它做跳过判断——收坡期间仍需本模块输出淡出。 */
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
