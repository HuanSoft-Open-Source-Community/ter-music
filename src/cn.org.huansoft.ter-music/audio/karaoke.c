/**
 * @file karaoke.c
 * @brief 卡拉OK（消人声）模式 — 频带受限、相似度门控的中置消除
 *
 * 算法（每帧，立体声）：
 *   m  = (L+R)/2            mid：居中内容（人声通常在这里）
 *   d  = (L-R)/2            side：声像两侧的乐器，全程原样保留
 *   lo = LP4(m, f_lo)       低频居中能量（贝斯/底鼓）——保留
 *   hi = HP4(m, f_hi)       高频居中能量（空气感/镲）——保留
 *   band  = m - lo - hi     人声频带内的居中成分
 *   sub1 = LP4(band, f_split)，sub2 = band - sub1（仅用于判定，不直接抵消）
 *   e_m[i], e_d[i]          两声道各自半带的能量包络（一阶跟随 |x|）
 *   g[i] = gate(e_d[i]/e_m[i])   半带内 side 无对应能量→1，纯立体声→0
 *   g  = max(g1, g2)        任一半带出现居中证据，即整带抵消
 *   m' = m - α·g·band       整带一次性抵消，相位相干
 *   L' = m' + d,  R' = m' - d
 *
 * 与"朴素 L-R"的差别：朴素做法等于对全频段 α=1，低音立刻变薄、
 * 声场塌成单点；这里把抵消限制在人声频带，side 完全不动。
 * 与旧版"整带无门控抵消"的差别：整个带内只剩立体声伴奏（前奏、间奏、
 * 铺底 pad、混响尾——side 与 mid 同能量甚至更强）时不再误伤伴奏；
 * 人声一进唱，任一半带立刻出现"mid 有、side 无"的单声道特征，
 * 门即回到 1，恢复满额抵消。
 * 为什么是"取大"而不是逐子带分别抵消：互补分频（LP+残差）在过渡区
 * 存在相位弥散，两个半带的门值若不一致，部分抵消会留下不相干的
 * 正交残差，实测把居中人声从削弱变成增幅；整带共用一个标量门保证
 * 抵消项与 band 严格同相，居中人声的 ≥30dB 抵消量不受门控影响，
 * 而探测分两个半带仍能把伴奏与人声的混叠判定噪声隔开（伴奏单独
 * 出现在另一半带时不再抬高门限）。
 *
 * 分频器用 4 阶 Butterworth（2 级 2 阶直接形式 I 级联，Q=0.5412/1.3066）。
 * 选 4 阶而不是 2 阶，是因为实测（scripts/test/test_karaoke.c）2 阶的
 * 12dB/倍频裙边太缓：1kHz 处只剩 -26dB 抵消量，而 10kHz 的居中高频还会
 * 被裙边牵进抵消带。4 阶在 500Hz–3kHz 给出 >=30dB 抵消量，同时
 * 100Hz 与 10kHz 的居中能量都在 0.1dB 以内。系数写法与
 * audio/equalizer.c 一致（同样按 a0 归一化）。
 *
 * 线程约定：karaoke_set_mode 只发布请求档位（单 int），全部 DSP 状态
 * （applied_mode/target_alpha/alpha/延迟线/包络）只由播放线程在
 * karaoke_process 块首消费与写回——主线程不再直接改这些字段，消掉了
 * 旧版"主线程预置 α、播放线程块尾回写"互相覆盖的竞争。
 *
 * @author ter-music team
 * @date 2026-11-06
 */

#include "audio/karaoke.h"

#include <math.h>
#include <string.h>

/* ================================================================
 * 内部状态
 * ================================================================ */

typedef struct {
    /* 归一化（a0 = 1）后的双二阶系数：
     * y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2] */
    float b0, b1, b2, a1, a2;
    /* 延迟线（直接形式 I 的历史输入/历史输出） */
    float x1, x2, y1, y2;
} Biquad;

/* 4 阶分频器 = 2 个 2 阶 Butterworth 段级联 */
typedef struct {
    Biquad section[2];
} Crossover;

typedef struct {
    /* 请求档位：karaoke_set_mode（任意线程）唯一写入的单字。
     * 其余字段全部只由播放线程在 karaoke_process 内读写。 */
    int   requested;          /* KARAOKE_MODE_*：最近一次被受理的请求 */
    int   applied_mode;       /* 当前正在应用的档位（播放线程块首消化请求后同步） */
    float target_alpha;       /* 当前应用档位对应的抵消系数 */
    float alpha;              /* 实际使用的系数，向 target_alpha 指数逼近 */
    float ramp_step;          /* 每帧逼近比例，由采样率算出 */
    float gate_step;          /* 能量包络每帧跟随比例，由采样率算出 */
    int   sample_rate;        /* 上一次用于计算系数的采样率 */
    int   dirty;              /* 非 0：需要重算系数 */
    int   primed;             /* 延迟线是否已用当前 mid/side 预热 */
    Crossover lo;             /* mid 低通：护住低频居中能量 */
    Crossover hi;             /* mid 高通：护住高频居中能量 */
    Crossover lo_d;           /* side 低通：与 mid 完全同构，保证比值可比 */
    Crossover hi_d;           /* side 高通：同上 */
    Crossover split;          /* mid 子带低通（KARAOKE_SPLIT_HZ） */
    Crossover split_d;        /* side 子带低通：同上 */
    float env_m[2];           /* mid 两个子带的能量包络 */
    float env_d[2];           /* side 两个子带的能量包络 */
} KaraokeState;

static KaraokeState g_kar = {0};

/* ================================================================
 * 双二阶 / 四阶滤波器
 * ================================================================ */

/* 4 阶 Butterworth 的两个 2 阶段的 Q 值 */
static const float k_butterworth_q[2] = { 0.541196f, 1.306563f };

/* RBJ Cookbook：2 阶 Butterworth（按 Q 分段）低通/高通。
 * fc 被夹到 (0, 0.45·fs]：采样率过低时两个分频点会靠拢，band 自然趋近
 * 于 0，效果退化为"不抵消"，而不是发散或削顶。 */
static void biquad_design(Biquad *f, float fs, float fc, float q, int lowpass)
{
    if (fs <= 0.0f || fc <= 0.0f || q <= 0.0f) {
        /* 无有效参数：直通 */
        f->b0 = 1.0f; f->b1 = 0.0f; f->b2 = 0.0f;
        f->a1 = 0.0f; f->a2 = 0.0f;
        return;
    }
    if (fc > fs * 0.45f) fc = fs * 0.45f;

    const float w0 = (float)(2.0 * M_PI) * fc / fs;
    const float cw = cosf(w0);
    const float sw = sinf(w0);
    const float alpha = sw / (2.0f * q);

    float b0, b1, b2;
    if (lowpass) {
        b0 = (1.0f - cw) * 0.5f;
        b1 = 1.0f - cw;
        b2 = (1.0f - cw) * 0.5f;
    } else {
        b0 = (1.0f + cw) * 0.5f;
        b1 = -(1.0f + cw);
        b2 = (1.0f + cw) * 0.5f;
    }
    const float a0 = 1.0f + alpha;
    const float a1 = -2.0f * cw;
    const float a2 = 1.0f - alpha;

    const float inv_a0 = 1.0f / a0;
    f->b0 = b0 * inv_a0;
    f->b1 = b1 * inv_a0;
    f->b2 = b2 * inv_a0;
    f->a1 = a1 * inv_a0;
    f->a2 = a2 * inv_a0;
}

static void crossover_design(Crossover *x, int sample_rate, float fc, int lowpass)
{
    const float fs = (float)sample_rate;
    for (int i = 0; i < 2; i++)
        biquad_design(&x->section[i], fs, fc, k_butterworth_q[i], lowpass);
}

static inline float biquad_run(Biquad *f, float in)
{
    float out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2
                - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = in;
    f->y2 = f->y1;
    f->y1 = out;
    return out;
}

static inline float crossover_run(Crossover *x, float in)
{
    return biquad_run(&x->section[1], biquad_run(&x->section[0], in));
}

/* 用当前信号值预热延迟线：低通的稳态输出等于输入（直流增益 1），
 * 高通的稳态输出为 0（隔直流）。不预热的话，起播第一帧会从全零历史
 * 里"冲"出一个瞬态，表现为轻微的一声闷响。 */
static void crossover_prime(Crossover *x, float input, float steady)
{
    for (int i = 0; i < 2; i++) {
        x->section[i].x1 = input;
        x->section[i].x2 = input;
        x->section[i].y1 = steady;
        x->section[i].y2 = steady;
    }
}

static void crossover_clear(Crossover *x)
{
    for (int i = 0; i < 2; i++) {
        x->section[i].x1 = x->section[i].x2 = 0.0f;
        x->section[i].y1 = x->section[i].y2 = 0.0f;
    }
}

static void karaoke_clear_state(void)
{
    crossover_clear(&g_kar.lo);
    crossover_clear(&g_kar.hi);
    crossover_clear(&g_kar.lo_d);
    crossover_clear(&g_kar.hi_d);
    crossover_clear(&g_kar.split);
    crossover_clear(&g_kar.split_d);
    g_kar.env_m[0] = g_kar.env_m[1] = 0.0f;
    g_kar.env_d[0] = g_kar.env_d[1] = 0.0f;
    g_kar.primed = 0;
}

static void karaoke_recalc(void)
{
    crossover_design(&g_kar.lo,     g_kar.sample_rate, (float)KARAOKE_LO_HZ,    1);
    crossover_design(&g_kar.hi,     g_kar.sample_rate, (float)KARAOKE_HI_HZ,    0);
    crossover_design(&g_kar.lo_d,   g_kar.sample_rate, (float)KARAOKE_LO_HZ,    1);
    crossover_design(&g_kar.hi_d,   g_kar.sample_rate, (float)KARAOKE_HI_HZ,    0);
    crossover_design(&g_kar.split,  g_kar.sample_rate, (float)KARAOKE_SPLIT_HZ, 1);
    crossover_design(&g_kar.split_d,g_kar.sample_rate, (float)KARAOKE_SPLIT_HZ, 1);

    /* 时间常数换算成"每帧逼近比例"：
     * α[n+1] = α[n] + (target - α[n]) · step，step = 1 - e^(-1/(fs·τ)) */
    if (g_kar.sample_rate > 0) {
        const float fs = (float)g_kar.sample_rate;
        const float tau = (float)KARAOKE_RAMP_TC_MS / 1000.0f;
        g_kar.ramp_step = 1.0f - expf(-1.0f / (fs * tau));
        if (g_kar.ramp_step <= 0.0f || g_kar.ramp_step > 1.0f) {
            g_kar.ramp_step = 1.0f;   /* 退化保护：直接跳到目标 */
        }
        const float gtau = (float)KARAOKE_GATE_TC_MS / 1000.0f;
        g_kar.gate_step = 1.0f - expf(-1.0f / (fs * gtau));
        if (g_kar.gate_step <= 0.0f || g_kar.gate_step > 1.0f) {
            g_kar.gate_step = 1.0f;
        }
    } else {
        g_kar.ramp_step = 1.0f;
        g_kar.gate_step = 1.0f;
    }
}

/* 相似度门：比较同一子带 side 与 mid 的能量包络。
 * 比值小（side 没有对应成分）→ 内容真居中，全力抵消；
 * 比值大（左右都有）→ 立体声内容，放行不抵消；中间线性锥落。
 * 用 +1e-7 防零除：双包络皆 0 的静音段比值记 0，门全开也无害
 * （抵消的对象本身就是 0）。 */
static inline float karaoke_gate(float env_mid, float env_side)
{
    const float r = env_side / (env_mid + 1e-7f);
    if (r <= KARAOKE_GATE_R_LO) return 1.0f;
    if (r >= KARAOKE_GATE_R_HI) return 0.0f;
    return (KARAOKE_GATE_R_HI - r) / (KARAOKE_GATE_R_HI - KARAOKE_GATE_R_LO);
}

/* S32 写回：只有越出 [-1,1] 的样本才走 tanh 软限幅（分频叠加时可能
 * 略微超出满幅），正常样本保持原值，不做无谓的整形失真。 */
static inline int32_t karaoke_store(float x)
{
    if (x > 1.0f || x < -1.0f) {
        x = tanhf(x);
    }
    double v = (double)x * 2147483647.0;
    if (v > 2147483647.0) v = 2147483647.0;
    if (v < -2147483647.0) v = -2147483647.0;
    return (int32_t)(v + (v >= 0.0 ? 0.5 : -0.5));
}

/* ================================================================
 * 公有接口
 * ================================================================ */

void karaoke_init(void)
{
    memset(&g_kar, 0, sizeof(g_kar));
    g_kar.requested = KARAOKE_MODE_OFF;
    g_kar.applied_mode = KARAOKE_MODE_OFF;
    g_kar.ramp_step = 1.0f;
    g_kar.gate_step = 1.0f;
}

float karaoke_alpha_for_mode(int mode)
{
    switch (mode) {
        case KARAOKE_MODE_SOFT: return KARAOKE_ALPHA_SOFT;
        case KARAOKE_MODE_FULL: return KARAOKE_ALPHA_FULL;
        case KARAOKE_MODE_OFF:
        default:                return 0.0f;
    }
}

void karaoke_set_mode(int mode)
{
    if (mode < KARAOKE_MODE_OFF || mode >= KARAOKE_MODE_COUNT) {
        mode = KARAOKE_MODE_OFF;   /* 越界退回不启用，与配置钳制语义一致 */
    }
    /* 只发布请求：目标档位、起坡/收坡、滤波器重建全部由播放线程在
     * karaoke_process 块首完成。本函数不再碰 DSP 字段，避免与播放线程
     * 的块尾回写互相覆盖（旧版竞争 bug 的根源）。单 int 写入，与 EQ
     * 模块的跨线程约定同级。 */
    g_kar.requested = mode;
}

int karaoke_get_mode(void)
{
    return g_kar.requested;
}

int karaoke_is_active(void)
{
    return g_kar.requested != KARAOKE_MODE_OFF;
}

void karaoke_reset(void)
{
    karaoke_clear_state();
    /* 重新起播（换曲/seek/变速重建 atempo）时从头逼近，
     * 避免带着上一段的 α 与残留状态出声。 */
    g_kar.alpha = 0.0f;
}

void karaoke_process(int32_t *samples, int frame_count, int channels, int sample_rate)
{
    if (!samples || frame_count <= 0) return;

    /* ── 块首消化档位请求（播放线程内唯一的 DSP 状态变更入口） ── */
    const int requested = g_kar.requested;
    if (requested != g_kar.applied_mode) {
        g_kar.applied_mode = requested;
        g_kar.target_alpha = karaoke_alpha_for_mode(requested);
        if (requested != KARAOKE_MODE_OFF) {
            /* 进入启用档：延迟线与包络全部重建，α 从 0 起坡。
             * 否则整块人声会在一瞬间被抹掉，听感是一次台阶。 */
            g_kar.alpha = 0.0f;
            karaoke_clear_state();
        }
        /* 退出到不启用档：只把目标归零，让 α 沿同一条曲线收坡淡出，
         * 坡走完（下面直通检查命中）才真正旁路，不留硬切。 */
    }
    /* 直通快速路径：稳态不启用、或收坡已走完 —— 一次浮点比较即返回。 */
    if (g_kar.alpha == 0.0f && g_kar.target_alpha == 0.0f) return;
    /* 单声道里"居中"就是全部信号，无从分离人声；多于 2 声道时本模块
     * 不做臆测的下混——两者都直通。解码链路已由 swresample 归 1/2 声道。 */
    if (channels != KARAOKE_STEREO_CHANNELS) return;
    if (sample_rate <= 0) return;

    if (g_kar.sample_rate != sample_rate) {
        g_kar.sample_rate = sample_rate;
        g_kar.dirty = 1;
    }
    if (g_kar.dirty) {
        g_kar.dirty = 0;
        karaoke_recalc();
        karaoke_clear_state();
        g_kar.alpha = 0.0f;   /* 系数变了，旧的逼近值没有意义 */
    }

    float alpha = g_kar.alpha;
    const float target = g_kar.target_alpha;
    const float step = g_kar.ramp_step;
    const float gstep = g_kar.gate_step;
    int primed = g_kar.primed;
    float env_m[2] = { g_kar.env_m[0], g_kar.env_m[1] };
    float env_d[2] = { g_kar.env_d[0], g_kar.env_d[1] };

    for (int f = 0; f < frame_count; f++) {
        int32_t *pair = samples + (size_t)f * KARAOKE_STEREO_CHANNELS;
        const float in_l = (float)pair[0] / 2147483647.0f;
        const float in_r = (float)pair[1] / 2147483647.0f;

        float m = 0.5f * (in_l + in_r);
        const float d = 0.5f * (in_l - in_r);

        if (!primed) {
            crossover_prime(&g_kar.lo, m, m);        /* 低通：稳态输出 = 输入 */
            crossover_prime(&g_kar.hi, m, 0.0f);     /* 高通：稳态输出 = 0 */
            crossover_prime(&g_kar.lo_d, d, d);
            crossover_prime(&g_kar.hi_d, d, 0.0f);
            crossover_prime(&g_kar.split, 0.0f, 0.0f);    /* 子带分频喂 band，起步为 0 */
            crossover_prime(&g_kar.split_d, 0.0f, 0.0f);
            primed = 1;
        }

        /* α 平滑逼近目标档位（升档起坡、降档收坡共用这条曲线） */
        alpha += (target - alpha) * step;
        if (fabsf(target - alpha) < 1e-6f) {
            alpha = target;
        }

        const float lo = crossover_run(&g_kar.lo, m);
        const float hi = crossover_run(&g_kar.hi, m);

        /* band = m - lo - hi：人声频带内的居中成分 */
        const float band = m - lo - hi;
        const float sub1 = crossover_run(&g_kar.split, band);   /* 带内低频段 */
        const float sub2 = band - sub1;                          /* 带内高频段 */

        const float lo_d = crossover_run(&g_kar.lo_d, d);
        const float hi_d = crossover_run(&g_kar.hi_d, d);
        const float band_d = d - lo_d - hi_d;
        const float sub1_d = crossover_run(&g_kar.split_d, band_d);
        const float sub2_d = band_d - sub1_d;

        /* 能量包络（一阶跟随 |x|，时间常数 KARAOKE_GATE_TC_MS） */
        env_m[0] += (fabsf(sub1)   - env_m[0]) * gstep;
        env_m[1] += (fabsf(sub2)   - env_m[1]) * gstep;
        env_d[0] += (fabsf(sub1_d) - env_d[0]) * gstep;
        env_d[1] += (fabsf(sub2_d) - env_d[1]) * gstep;

        /* 双子带取大门控：两个半带都呈纯立体声特征才整带放行；
         * 抵消项始终是完整的 band，居中人声的抵消量与门控严格解耦，
         * 相位相干无残差（分带各自缩放曾实测把人声削成增幅，不可用）。 */
        float g1 = karaoke_gate(env_m[0], env_d[0]);
        float g2 = karaoke_gate(env_m[1], env_d[1]);
        const float gate = (g2 > g1) ? g2 : g1;
        m = m - alpha * gate * band;

        pair[0] = karaoke_store(m + d);
        pair[1] = karaoke_store(m - d);
    }

    g_kar.alpha = alpha;
    g_kar.primed = primed;
    g_kar.env_m[0] = env_m[0];
    g_kar.env_m[1] = env_m[1];
    g_kar.env_d[0] = env_d[0];
    g_kar.env_d[1] = env_d[1];
}
