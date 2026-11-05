/**
 * @file karaoke.c
 * @brief 卡拉OK（消人声）模式 — 频带受限中置消除
 *
 * 算法（每帧，立体声）：
 *   m = (L+R)/2          mid：居中内容（人声通常在这里）
 *   d = (L-R)/2          side：声像两侧的乐器，全程原样保留
 *   lo = LP4(m, f_lo)    低频居中能量（贝斯/底鼓）——保留
 *   hi = HP4(m, f_hi)    高频居中能量（空气感/镲）——保留
 *   band = m - lo - hi   人声频带内的居中成分
 *   m' = m - α·band      只在这一段里抵消，α 由档位决定
 *   L' = m' + d,  R' = m' - d
 *
 * 与"朴素 L-R"的差别：朴素做法等于对全频段 α=1，低音立刻变薄、
 * 声场塌成单点；这里把抵消限制在人声频带，side 完全不动。
 *
 * 分频器用 4 阶 Butterworth（2 级 2 阶直接形式 I 级联，Q=0.5412/1.3066）。
 * 选 4 阶而不是 2 阶，是因为实测（scripts/test/test_karaoke.c）2 阶的
 * 12dB/倍频裙边太缓：1kHz 处只剩 -26dB 抵消量，而 10kHz 的居中高频还会
 * 被裙边牵进抵消带。4 阶在 500Hz–3kHz 给出 >=30dB 抵消量，同时
 * 100Hz 与 10kHz 的居中能量都在 0.1dB 以内。系数写法与
 * audio/equalizer.c 一致（同样按 a0 归一化）。
 *
 * @author ter-music team
 * @date 2026-11-05
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
    int   mode;             /* KARAOKE_MODE_*：主线程写、播放线程读（单 int，与 EQ 同级约定） */
    float target_alpha;     /* 当前档位对应的抵消系数 */
    float alpha;            /* 实际使用的系数，向 target_alpha 指数逼近 */
    float ramp_step;        /* 每帧逼近比例，由采样率算出 */
    int   sample_rate;      /* 上一次用于计算系数的采样率 */
    int   dirty;            /* 非 0：需要重算系数 */
    int   primed;           /* 延迟线是否已用当前 mid 预热 */
    Crossover lo;           /* 低通：只作用于 mid（单声道流），无需按声道存状态 */
    Crossover hi;           /* 高通：同上 */
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
    g_kar.primed = 0;
}

static void karaoke_recalc(void)
{
    crossover_design(&g_kar.lo, g_kar.sample_rate, (float)KARAOKE_LO_HZ, 1);
    crossover_design(&g_kar.hi, g_kar.sample_rate, (float)KARAOKE_HI_HZ, 0);

    /* 时间常数换算成"每帧逼近比例"：
     * α[n+1] = α[n] + (target - α[n]) · step，step = 1 - e^(-1/(fs·τ)) */
    if (g_kar.sample_rate > 0) {
        const float tau = (float)KARAOKE_RAMP_TC_MS / 1000.0f;
        g_kar.ramp_step = 1.0f - expf(-1.0f / ((float)g_kar.sample_rate * tau));
        if (g_kar.ramp_step <= 0.0f || g_kar.ramp_step > 1.0f) {
            g_kar.ramp_step = 1.0f;   /* 退化保护：直接跳到目标 */
        }
    } else {
        g_kar.ramp_step = 1.0f;
    }
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
    g_kar.mode = KARAOKE_MODE_OFF;
    g_kar.ramp_step = 1.0f;
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
    if (g_kar.mode == mode) {
        return;
    }

    const int was_active = (g_kar.mode != KARAOKE_MODE_OFF);
    g_kar.mode = mode;
    g_kar.target_alpha = karaoke_alpha_for_mode(mode);

    if (!was_active && mode != KARAOKE_MODE_OFF) {
        /* 从直通进入处理：延迟线重新预热，α 从 0 爬升，
         * 否则会在一瞬间把人声整块抹掉，听感是一次台阶。 */
        g_kar.alpha = 0.0f;
        g_kar.primed = 0;
    }
}

int karaoke_get_mode(void)
{
    return g_kar.mode;
}

int karaoke_is_active(void)
{
    return g_kar.mode != KARAOKE_MODE_OFF;
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
    if (g_kar.mode == KARAOKE_MODE_OFF) return;
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
    int primed = g_kar.primed;

    for (int f = 0; f < frame_count; f++) {
        int32_t *pair = samples + (size_t)f * KARAOKE_STEREO_CHANNELS;
        const float in_l = (float)pair[0] / 2147483647.0f;
        const float in_r = (float)pair[1] / 2147483647.0f;

        float m = 0.5f * (in_l + in_r);
        const float d = 0.5f * (in_l - in_r);

        if (!primed) {
            crossover_prime(&g_kar.lo, m, m);      /* 低通：稳态输出 = 输入 */
            crossover_prime(&g_kar.hi, m, 0.0f);   /* 高通：稳态输出 = 0 */
            primed = 1;
        }

        /* α 平滑逼近目标档位 */
        alpha += (target - alpha) * step;
        if (fabsf(target - alpha) < 1e-6f) {
            alpha = target;
        }

        const float lo = crossover_run(&g_kar.lo, m);
        const float hi = crossover_run(&g_kar.hi, m);

        /* band = m - lo - hi：人声频带内的居中成分，按 α 抵消 */
        m = m - alpha * (m - lo - hi);

        pair[0] = karaoke_store(m + d);
        pair[1] = karaoke_store(m - d);
    }

    g_kar.alpha = alpha;
    g_kar.primed = primed;
}
