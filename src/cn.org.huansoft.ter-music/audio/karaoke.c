/**
 * @file karaoke.c
 * @brief 卡拉OK（消人声）模式 — 逐频点 STFT 中置提取门控
 *
 * 算法（每帧 STFT，立体声）：
 *   m  = (L+R)/2            mid：居中内容（人声通常在这里）
 *   d  = (L-R)/2            side：声像两侧的乐器，全程原样保留
 *   对 m、d 分别做 Nw 点 Hann 分析窗 + Nf 点 FFT（Nf ≥ Nw，尾部补零）
 *   对 200Hz–5kHz 内的每个频点 k：
 *       r[k] = |S[k]| / (|M[k]| + ε)       该频点的 side/mid 幅度比
 *       tgt[k] = gate(r[k])                 r 小 → 1（居中的相干内容，抵消）
 *                                           r 大 → 0（宽立体声内容，保留）
 *       g[k] 非对称跟随：tgt 下降按 80ms 慢放，tgt 回升按 6ms 快开
 *       m'[k] = m[k] · (1 - α · g[k])      逐频点实数缩放，相位不动
 *   逆 FFT + 重叠相加（按 Σw² 归一，COLA 严格重建）；带外频点缩放系数恒为 1
 *   L' = m' + d,  R' = m' - d
 *
 * 为什么逐频点而不是整带一个标量门（旧版做法，已实测确认是漏音根因）：
 * 旧版把 200Hz–5kHz 整带当成一个整体，用两个半带的 side/mid 包络比取大
 * 得到一个标量门乘到整带抵消项上。旁侧瞬态（鼓/镲/宽立体声乐器）一出现，
 * 两个半带的比值同时抬升，整带抵消在几毫秒内被放行，居中人声随原声漏出
 * （实测 full 档漏出 592.7ms/14 次、最长 43ms，漏出瞬间人声回到 0.00dB）。
 * 逐频点门控把判断下沉到频点：旁侧瞬态的能量摊在几百个频点上，人声所在
 * 的少数频点 side/mid 比仍然很低，抵消不会被旁侧瞬态打断；真正宽立体声的
 * 频点（前奏/间奏/铺底）side/mid ≈ 1，门值为 0，mid 原样保留——门控的
 * 原始目的（保护纯立体声伴奏）没有回退。
 *
 * 时间常数为什么是"慢放快开"：门值下降意味着释放抵消（人声会漏出来），
 * 必须慢（80ms）；门值回升意味着恢复抵消，必须快（6ms），人声一进唱
 * 立刻恢复满额抵消。旧版 15ms 对称跟随 + 整带生效，任何旁侧冲击都会在
 * 几毫秒内放掉整带抵消，这正是要修的问题。
 *
 * 频带边界用频点选择实现：只有中心频率 ∈ [200, 5000] Hz 的频点参与缩放，
 * 带外频点系数恒为 1。边界附近因此存在一条由窗主瓣宽度决定的**过渡带**
 * （44.1kHz 下约 ±230Hz），单个居中正弦的实测衰减见 karaoke.h 的注释：
 * 100Hz −0.25dB、200Hz −4.5dB、4.8kHz −68.7dB、5.0kHz −11.3dB、
 * 5.2kHz −0.015dB、10kHz 0.000dB。带外是"实测意义上的保留"，不是逐位不变。
 *
 * 附加延迟 D = Nw-1 个样本（重建样本 p 要等覆盖它的最后一个分析窗到齐，
 * 该窗右边缘最远到 p+Nw-1）。按采样率选窗见 karaoke_pick_config：
 * 44.1kHz 下 Nw=384/Nf=512/hop=96 → D=383 样本 = 8.69ms；48kHz 7.98ms；
 * 22.05kHz 用 Nw=192 → 8.66ms；96kHz 用 Nw=768 → 7.98ms，全部 ≤10ms。
 *
 * 流式时间轴（按样本精确，D = delay，详见 karaoke.h 的同名契约）：
 *   OFF/旁路      out[n] = in[n]（逐位）
 *   n < D         out[n] = in[n]（预热段 raw；这段内容随后会被处理流重放）
 *   D ≤ n < 2D    out[n] = (1-w)·in[n] + w·processed[n-D]（接缝交叉淡化）
 *   n ≥ 2D        out[n] = processed[n-D]（内容整体延迟 D 个样本）
 * 降档时 α 收坡到 1e-5 以下（约 11.5τ ≈ 345ms）后的下一个 process 块首，再做 D 个
 * 样本的 processed→raw 交叉淡化，随后逐位直通（块长 1/1024/4096 实测：判定点
 * 344/347/371ms，逐位直通起点 353/356/380ms）。因此"前 D 个样本被听两次"
 * 与"降档切换晚约 11.5τ（345ms）"都是本设计的事实。
 *
 * 档位切换中仅 OFF→启用会重置流并重新预热；启用态下 reset/seek/采样率切换
 * 也会重置流并重新预热（OFF 态下 reset/采样率切换不预热，直接旁路）。
 *
 * 线程约定：karaoke_set_mode 只发布请求档位（单 int），全部 DSP 状态
 * （applied_mode/alpha/分析窗历史/重叠相加累加器/门值）只由播放线程在
 * karaoke_process 块首消费与写回——主线程不再直接改这些字段。
 *
 * 分配：全部缓冲在 karaoke_init 时按 KARAOKE_STFT_FFT_MAX 一次性分配，
 * 热路径无 malloc/free；分配失败时安全降级为直通（process 直接返回）
 * 并在 stderr 打一次告警。
 *
 * @author ter-music team
 * @date 2026-11-06
 */

#include "audio/karaoke.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================
 * 内部状态
 * ================================================================ */

/* 一次 process 调用内部按此上限切片：保证重叠相加环不会被单块撑爆，
 * 同时让块长不影响输出（内部按绝对样本序号推进，与块边界无关）。 */
#define KARAOKE_STFT_CHUNK 1024

typedef struct {
    /* 请求档位：karaoke_set_mode（任意线程）唯一写入的单字。
     * 其余字段全部只由播放线程在 karaoke_process 内读写。 */
    int   requested;          /* KARAOKE_MODE_*：最近一次被受理的请求 */
    int   applied_mode;       /* 当前正在应用的档位（播放线程块首消化请求后同步） */
    float target_alpha;       /* 当前应用档位对应的抵消系数 */
    float alpha;              /* 实际使用的系数，向 target_alpha 指数逼近 */
    float ramp_step;          /* 每帧逼近比例，由采样率/跳距算出 */
    float rise_step;          /* 频点门值回升（快开）比例 */
    float fall_step;          /* 频点门值下降（慢放）比例 */
    int   sample_rate;        /* 上一次用于计算配置的采样率 */
    int   dirty;              /* 非 0：需要按新采样率重算配置与表 */
    int   nf;                 /* FFT 点数 */
    int   nw;                 /* 分析窗长（≤ nf） */
    int   hop;                /* 帧跳距 */
    int   delay;              /* 附加算法延迟（样本）= nw-1 */
    long  pos;                /* 本次激活以来已处理的输入样本数（绝对序号） */
    long  next_center;        /* 下一个分析帧中心（绝对序号） */
    int   gate_seeded;        /* 本次激活后第一个分析帧把门值直接置为目标值 */
    int   primed;             /* 已用当前输入预热过分析窗（调试/状态记录用） */
    int   seam_left;          /* 升档接缝 raw→processed 交叉淡化剩余样本 */
    int   fade_left;          /* 降档旁路 processed→raw 交叉淡化剩余样本 */
    int   bypassed;           /* 1=已旁路（不碰任何 DSP 状态的逐位直通） */
} KaraokeState;

static KaraokeState g_kar = {0};

/* 缓冲单独存放：karaoke_init 会 memset g_kar，不能让指针一起被清掉 */
typedef struct {
    float *win;        /* nw：Hann 分析/合成窗 */
    float *hist_m;     /* nw：最近 nw 个输入 mid（环形，按绝对序号取模） */
    float *hist_s;     /* nw：最近 nw 个输入 side（环形） */
    float *ola;        /* 2*FFT_MAX：重叠相加累加器（环形） */
    float *norm;       /* 2*FFT_MAX：Σw² 归一化累加器（环形） */
    float *gate;       /* FFT_MAX/2+2：逐频点门值状态 */
    double *cos_t;     /* FFT_MAX/2：旋转因子 cos(2πi/N) */
    double *sin_t;     /* FFT_MAX/2：旋转因子 sin(2πi/N) */
    int   *rev;        /* FFT_MAX：位反转表 */
    double *re_m, *im_m;   /* FFT_MAX：mid 频谱 */
    double *re_s, *im_s;   /* FFT_MAX：side 频谱 */
} KaraokeBuffers;

static KaraokeBuffers g_buf;
static int g_buf_state = 0;   /* 0=未分配 1=已分配 -1=分配失败 */

static void karaoke_buffers_free(void)
{
    free(g_buf.win);   free(g_buf.hist_m); free(g_buf.hist_s);
    free(g_buf.ola);   free(g_buf.norm);   free(g_buf.gate);
    free(g_buf.cos_t); free(g_buf.sin_t);  free(g_buf.rev);
    free(g_buf.re_m);  free(g_buf.im_m);
    free(g_buf.re_s);  free(g_buf.im_s);
    memset(&g_buf, 0, sizeof(g_buf));
}

/* 全部缓冲按最大 FFT 一次性分配；失败时安全降级（process 直接返回）。 */
static int karaoke_buffers_alloc(void)
{
    if (g_buf_state != 0) return g_buf_state > 0;
    const size_t nfmax = KARAOKE_STFT_FFT_MAX;
    const size_t ola = 2u * nfmax;

    g_buf.win    = calloc(nfmax, sizeof(float));
    g_buf.hist_m = calloc(nfmax, sizeof(float));
    g_buf.hist_s = calloc(nfmax, sizeof(float));
    g_buf.ola    = calloc(ola, sizeof(float));
    g_buf.norm   = calloc(ola, sizeof(float));
    g_buf.gate   = calloc(nfmax / 2 + 2, sizeof(float));
    g_buf.cos_t  = calloc(nfmax / 2, sizeof(double));
    g_buf.sin_t  = calloc(nfmax / 2, sizeof(double));
    g_buf.rev    = calloc(nfmax, sizeof(int));
    g_buf.re_m   = calloc(nfmax, sizeof(double));
    g_buf.im_m   = calloc(nfmax, sizeof(double));
    g_buf.re_s   = calloc(nfmax, sizeof(double));
    g_buf.im_s   = calloc(nfmax, sizeof(double));

    if (!g_buf.win || !g_buf.hist_m || !g_buf.hist_s || !g_buf.ola ||
        !g_buf.norm || !g_buf.gate || !g_buf.cos_t || !g_buf.sin_t ||
        !g_buf.rev || !g_buf.re_m || !g_buf.im_m || !g_buf.re_s || !g_buf.im_s) {
        karaoke_buffers_free();
        g_buf_state = -1;
        fprintf(stderr, "karaoke: STFT 缓冲分配失败，消人声降级为直通\n");
        return 0;
    }
    g_buf_state = 1;
    return 1;
}

/* ================================================================
 * 采样率 → 窗长 / FFT 点数 / 跳距
 * ================================================================ */

/* 窗长在时间上保持约 8–10ms，附加延迟 = nw-1 样本，因此在所有采样率下
 * 都 ≤ 10ms（44.1k: 383 样本 = 8.69ms；48k: 7.98ms；22.05k: 8.66ms；
 * 96k: 7.98ms）。FFT 点数取 ≥ nw 的最小 2 的幂（尾部补零），频点间隔
 * 约 86–125Hz。跳距取 nw/4（75% 重叠），门值每 2.2–2.4ms 更新一次。 */
static void karaoke_pick_config(int sample_rate, int *nf, int *nw, int *hop)
{
    if (sample_rate >= 80000) {
        *nf = 1024; *nw = 768;
    } else if (sample_rate >= 40000) {
        *nf = 512;  *nw = 384;
    } else if (sample_rate >= 20000) {
        *nf = 256;  *nw = 192;
    } else if (sample_rate >= 10000) {
        *nf = 128;  *nw = 96;
    } else {
        *nf = 64;   *nw = 48;
    }
    *hop = *nw / 4;
}

/* ================================================================
 * FFT（radix-2，就地复数）
 * ================================================================ */

static void karaoke_fft(double *re, double *im, int inverse)
{
    const int n = g_kar.nf;
    for (int i = 0; i < n; i++) {
        const int j = g_buf.rev[i];
        if (j > i) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2;
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                const double wr = g_buf.cos_t[k * step];
                const double wi = inverse ? g_buf.sin_t[k * step]
                                          : -g_buf.sin_t[k * step];
                const int a = i + k, b = a + half;
                const double xr = re[b] * wr - im[b] * wi;
                const double xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
    if (inverse) {
        const double inv = 1.0 / (double)n;
        for (int i = 0; i < n; i++) { re[i] *= inv; im[i] *= inv; }
    }
}

/* ================================================================
 * 逐频点门控 / 重建
 * ================================================================ */

/* 频点相似度门：r = |S|/|M| 小 → 该频点内容真居中，抵消权重 1；
 * r 大 → 左右都有，该频点 mid 保留，权重 0；中间线性锥落。 */
static inline float karaoke_bin_gate(float r)
{
    if (r <= KARAOKE_GATE_R_LO) return 1.0f;
    if (r >= KARAOKE_GATE_R_HI) return 0.0f;
    return (KARAOKE_GATE_R_HI - r) / (KARAOKE_GATE_R_HI - KARAOKE_GATE_R_LO);
}

/* S32 写回：只有越出 [-1,1] 的样本才走 tanh 软限幅；NaN/Inf 直接归零，
 * 避免 (int32_t)(NaN·2^31) 的未定义行为。 */
static inline int32_t karaoke_store(float x)
{
    if (!isfinite(x)) x = 0.0f;
    if (x > 1.0f || x < -1.0f) {
        x = tanhf(x);
    }
    double v = (double)x * 2147483647.0;
    if (v > 2147483647.0) v = 2147483647.0;
    if (v < -2147483647.0) v = -2147483647.0;
    return (int32_t)(v + (v >= 0.0 ? 0.5 : -0.5));
}

/* 处理一个分析帧：中心在绝对样本序号 center，窗覆盖
 * [center-nw/2, center+nw/2-1]（早于 0 的部分按 0 处理——这是起播时
 * "信号从 0 开始"的自然假设，且这些样本本来就在预热段里原样输出）。 */
static void karaoke_frame(long center)
{
    const int nf = g_kar.nf;
    const int nw = g_kar.nw;
    const int half = nw / 2;
    const float *win = g_buf.win;

    memset(g_buf.re_m, 0, (size_t)nf * sizeof(double));
    memset(g_buf.im_m, 0, (size_t)nf * sizeof(double));
    memset(g_buf.re_s, 0, (size_t)nf * sizeof(double));
    memset(g_buf.im_s, 0, (size_t)nf * sizeof(double));

    for (int j = 0; j < nw; j++) {
        const long t = center - half + j;
        if (t < 0) continue;
        const int hi = (int)(t % nw);
        g_buf.re_m[j] = (double)win[j] * (double)g_buf.hist_m[hi];
        g_buf.re_s[j] = (double)win[j] * (double)g_buf.hist_s[hi];
    }

    karaoke_fft(g_buf.re_m, g_buf.im_m, 0);
    karaoke_fft(g_buf.re_s, g_buf.im_s, 0);

    /* α 逼近：预热段（center < delay）保持 0，处理流一开始从 0 起坡，
     * 避免预热段与处理流衔接处出现电平台阶。 */
    if (center >= (long)g_kar.delay) {
        g_kar.alpha += (g_kar.target_alpha - g_kar.alpha) * g_kar.ramp_step;
        if (fabsf(g_kar.target_alpha - g_kar.alpha) < 1e-5f)
            g_kar.alpha = g_kar.target_alpha;
    }

    const double bin_hz = (double)g_kar.sample_rate / (double)nf;
    const float alpha = g_kar.alpha;
    const int seeded = g_kar.gate_seeded;

    for (int b = 1; b < nf / 2; b++) {
        const double f = (double)b * bin_hz;
        if (f < (double)KARAOKE_LO_HZ || f > (double)KARAOKE_HI_HZ) continue;

        const double pm = g_buf.re_m[b] * g_buf.re_m[b] + g_buf.im_m[b] * g_buf.im_m[b];
        const double ps = g_buf.re_s[b] * g_buf.re_s[b] + g_buf.im_s[b] * g_buf.im_s[b];
        if (!isfinite(pm) || !isfinite(ps)) continue;

        const double r = sqrt(ps / (pm + 1e-20));
        const float tgt = karaoke_bin_gate((float)r);

        /* 非对称跟随：目标下降（判为宽内容）按 fall_step 慢放，
         * 目标回升（判为居中）按 rise_step 快开。 */
        float state = g_buf.gate[b];
        if (!seeded) {
            state = tgt;                    /* 激活首帧：直接对齐，避免起播凹坑 */
        } else if (tgt > state) {
            state += (tgt - state) * g_kar.rise_step;
        } else {
            state += (tgt - state) * g_kar.fall_step;
        }
        if (!isfinite(state)) state = tgt;
        if (state < 1e-20f) state = 0.0f;   /* denormal 防护 */
        g_buf.gate[b] = state;

        const double scale = 1.0 - (double)alpha * (double)state;
        g_buf.re_m[b] *= scale;
        g_buf.im_m[b] *= scale;
        g_buf.re_m[nf - b] *= scale;
        g_buf.im_m[nf - b] *= scale;
    }
    g_kar.gate_seeded = 1;

    karaoke_fft(g_buf.re_m, g_buf.im_m, 1);

    /* 重叠相加：分析窗与合成窗都乘一次（Hann²），归一化用实际累加的
     * Σw²，因此任意窗长/跳距都能严格重建（COLA 归一）。 */
    const int ola_size = 2 * KARAOKE_STFT_FFT_MAX;
    for (int j = 0; j < nw; j++) {
        const long t = center - half + j;
        if (t < 0) continue;
        const int idx = (int)(t % ola_size);
        g_buf.ola[idx] += (float)(g_buf.re_m[j] * (double)win[j]);
        g_buf.norm[idx] += win[j] * win[j];
    }
}

/* ================================================================
 * 配置 / 状态
 * ================================================================ */

static void karaoke_rebuild_tables(void)
{
    const int nf = g_kar.nf;
    const int nw = g_kar.nw;

    memset(g_buf.win, 0, (size_t)KARAOKE_STFT_FFT_MAX * sizeof(float));
    for (int i = 0; i < nw; i++)
        g_buf.win[i] = 0.5f - 0.5f * (float)cos(2.0 * M_PI * (double)i / (double)nw);

    for (int i = 0; i < nf / 2; i++) {
        g_buf.cos_t[i] = cos(2.0 * M_PI * (double)i / (double)nf);
        g_buf.sin_t[i] = sin(2.0 * M_PI * (double)i / (double)nf);
    }

    int bits = 0;
    while ((1 << bits) < nf) bits++;
    for (int i = 0; i < nf; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        g_buf.rev[i] = r;
    }
}

static void karaoke_clear_stream(void)
{
    if (g_buf_state <= 0) return;
    const int nf = g_kar.nf;
    memset(g_buf.hist_m, 0, (size_t)KARAOKE_STFT_FFT_MAX * sizeof(float));
    memset(g_buf.hist_s, 0, (size_t)KARAOKE_STFT_FFT_MAX * sizeof(float));
    memset(g_buf.ola, 0, (size_t)(2 * KARAOKE_STFT_FFT_MAX) * sizeof(float));
    memset(g_buf.norm, 0, (size_t)(2 * KARAOKE_STFT_FFT_MAX) * sizeof(float));
    memset(g_buf.gate, 0, (size_t)(nf / 2 + 2) * sizeof(float));
    g_kar.pos = 0;
    g_kar.next_center = 0;
    g_kar.gate_seeded = 0;
    g_kar.primed = 0;
}

static void karaoke_recalc(void)
{
    const int fs = g_kar.sample_rate;
    int nf = KARAOKE_STFT_FFT_44100, nw = KARAOKE_STFT_WIN_44100, hop = KARAOKE_STFT_HOP_44100;

    if (fs > 0) {
        karaoke_pick_config(fs, &nf, &nw, &hop);
    }
    g_kar.nf = nf;
    g_kar.nw = nw;
    g_kar.hop = hop > 0 ? hop : 1;
    g_kar.delay = nw - 1;

    karaoke_rebuild_tables();

    if (fs > 0) {
        const double fsf = (double)fs;
        const double hopd = (double)g_kar.hop;
        g_kar.ramp_step = (float)(1.0 - exp(-hopd / (fsf * ((double)KARAOKE_RAMP_TC_MS / 1000.0))));
        g_kar.rise_step = (float)(1.0 - exp(-hopd / (fsf * ((double)KARAOKE_GATE_RISE_TC_MS / 1000.0))));
        g_kar.fall_step = (float)(1.0 - exp(-hopd / (fsf * ((double)KARAOKE_GATE_FALL_TC_MS / 1000.0))));
    } else {
        g_kar.ramp_step = 1.0f;
        g_kar.rise_step = 1.0f;
        g_kar.fall_step = 1.0f;
    }
    if (!(g_kar.ramp_step > 0.0f && g_kar.ramp_step <= 1.0f)) g_kar.ramp_step = 1.0f;
    if (!(g_kar.rise_step > 0.0f && g_kar.rise_step <= 1.0f)) g_kar.rise_step = 1.0f;
    if (!(g_kar.fall_step > 0.0f && g_kar.fall_step <= 1.0f)) g_kar.fall_step = 1.0f;
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
    g_kar.rise_step = 1.0f;
    g_kar.fall_step = 1.0f;
    g_kar.dirty = 1;
    g_kar.bypassed = 1;          /* 未启用：进程内走逐位直通快速路径 */
    karaoke_buffers_alloc();
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
    /* 只发布请求：目标档位、起坡/收坡、窗长/FFT 表重建全部由播放线程在
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
    karaoke_clear_stream();
    /* 重新起播（换曲/seek/变速重建 atempo）时从头逼近，
     * 避免带着上一段的 α 与残留状态出声。 */
    g_kar.alpha = 0.0f;
    /* 重新预热 = 时间轴重新开始：若仍在启用档，前 D 个样本会再次被重放，
     * 并在接缝处做交叉淡化（见 karaoke.h 的流式时间轴契约）。 */
    if (g_kar.target_alpha > 0.0f) {
        g_kar.seam_left = g_kar.delay;
        g_kar.fade_left = 0;
        g_kar.bypassed = 0;
    } else {
        g_kar.seam_left = 0;
        g_kar.fade_left = 0;
        g_kar.bypassed = 1;
    }
}

static void karaoke_process_chunk(int32_t *samples, int frame_count)
{
    const int nw = g_kar.nw;
    const int delay = g_kar.delay;
    const int ola_size = 2 * KARAOKE_STFT_FFT_MAX;

    for (int i = 0; i < frame_count; i++) {
        int32_t *pair = samples + (size_t)i * KARAOKE_STEREO_CHANNELS;
        const float in_l = (float)pair[0] / 2147483647.0f;
        const float in_r = (float)pair[1] / 2147483647.0f;

        if (g_kar.bypassed) {
            /* 收坡交叉淡化已完成：不再触碰任何 DSP 状态，样本原样留在
             * 缓冲里（逐位直通）。pos 继续推进只为时间轴记录。 */
            g_kar.pos++;
            continue;
        }

        const float m = 0.5f * (in_l + in_r);
        const float d = 0.5f * (in_l - in_r);
        const long p = g_kar.pos;

        g_buf.hist_m[p % nw] = m;
        g_buf.hist_s[p % nw] = d;
        if (!g_kar.primed) g_kar.primed = 1;

        /* 帧推进：某个分析窗的右边缘到达当前样本时处理它。 */
        while (g_kar.next_center + (long)(nw / 2) - 1 <= p) {
            karaoke_frame(g_kar.next_center);
            g_kar.next_center += g_kar.hop;
        }

        const long q = p - (long)delay;
        float out_l, out_r;
        if (q < 0) {
            /* 预热段：当前输入原样输出（不产生静音缺口）。这段内容随后
             * 会作为 processed[n-D] 再出现一次，接缝处用下面的交叉淡化
             * 把"重放"摊成平滑过渡（见 karaoke.h 的流式时间轴契约）。 */
            out_l = in_l;
            out_r = in_r;
        } else {
            const int idx = (int)(q % ola_size);
            const float nrm = g_buf.norm[idx];
            float mm = (nrm > 1e-20f) ? g_buf.ola[idx] / nrm : 0.0f;
            if (!isfinite(mm)) mm = 0.0f;
            if (fabsf(mm) < 1e-30f) mm = 0.0f;   /* denormal 防护 */
            const float sd = g_buf.hist_s[q % nw];
            out_l = mm + sd;
            out_r = mm - sd;
            g_buf.ola[idx] = 0.0f;
            g_buf.norm[idx] = 0.0f;

            if (g_kar.seam_left > 0 && delay > 1) {
                /* 升档接缝：out = (1-w)·raw(p) + w·processed(p-D)，
                 * w 从 1/(D+1) 线性升到 D/(D+1)，首尾都连续。 */
                const int done = delay - g_kar.seam_left;
                const float w = (float)(done + 1) / (float)(delay + 1);
                out_l = (1.0f - w) * in_l + w * out_l;
                out_r = (1.0f - w) * in_r + w * out_r;
                g_kar.seam_left--;
            }
        }

        if (g_kar.fade_left > 0 && delay > 1) {
            /* 降档旁路交叉淡化：out = (1-w)·processed(p-D) + w·raw(p)，
             * 淡化完成后 out 与 raw 的差只剩 1/(D+1)，随后逐位直通。 */
            const int done = delay - g_kar.fade_left;
            const float w = (float)(done + 1) / (float)(delay + 1);
            out_l = (1.0f - w) * out_l + w * in_l;
            out_r = (1.0f - w) * out_r + w * in_r;
            g_kar.fade_left--;
            if (g_kar.fade_left == 0) g_kar.bypassed = 1;
        }

        pair[0] = karaoke_store(out_l);
        pair[1] = karaoke_store(out_r);
        g_kar.pos = p + 1;
    }
}

void karaoke_process(int32_t *samples, int frame_count, int channels, int sample_rate)
{
    if (!samples || frame_count <= 0) return;

    /* ── 块首消化档位请求（播放线程内唯一的 DSP 状态变更入口） ── */
    const int requested = g_kar.requested;
    if (requested != g_kar.applied_mode) {
        const int prev_applied = g_kar.applied_mode;
        g_kar.applied_mode = requested;
        g_kar.target_alpha = karaoke_alpha_for_mode(requested);
        if (requested != KARAOKE_MODE_OFF && prev_applied == KARAOKE_MODE_OFF) {
            /* OFF → 启用：分析窗历史、重叠相加累加器、门值全部重建，
             * α 从 0 起坡（否则整块人声会在一瞬间被抹掉，听感是一次台阶）。
             * 时间轴从样本 0 重新开始：前 D 个样本是 raw 预热段，随后 D
             * 个样本做 raw→processed 接缝交叉淡化（见头文件契约）。 */
            g_kar.alpha = 0.0f;
            karaoke_clear_stream();
            g_kar.seam_left = g_kar.delay;
            g_kar.fade_left = 0;
            g_kar.bypassed = 0;
        } else if (requested != KARAOKE_MODE_OFF) {
            /* 启用 ↔ 启用（soft↔full）：**不重置流**。分析窗历史、重叠
             * 相加累加器、门值与时间轴全部沿用，只把 target_alpha 从
             * 0.60↔1.00 切换，让 α 沿既有 30ms τ 斜坡过去，不重放、不跳变。
             * 注：旧实现对任何启用档互切也会 alpha=0 + clear_state 重新预热，
             * 切档瞬间抵消塌陷、居中人声短暂冒出来；本轮是**修正旧实现的
             * 缺陷**，不是复刻旧语义。
             * （重置流会重放前 D 个样本，并在切档瞬间把居中人声放回原声，
             * 正是用户报的"穿插未消声音"。） */
        } else {
            /* 启用 → OFF：目标归零，α 沿 30ms τ 收坡；衰减到 1e-5 以下
             * （约 11.5τ ≈ 345ms；块长 1/1024/4096 实测判定点
             * 344/347/371ms）的第一个
             * process 块首开始 D 个样本的 processed→raw 交叉淡化，随后
             * 逐位直通（见下面的旁路判定）。 */
            g_kar.fade_left = 0;
            g_kar.bypassed = 0;
        }
    }
    /* 不启用档：收坡已走完且旁路交叉淡化也做完 → 一次浮点比较即返回，
     * 且在 STFT 之前，绝不走 FFT 往返（逐位直通）。淡化未做完时不能返回，
     * 否则会丢掉 OLA 里已写入未输出的 pending 样本、造成时间轴前跳。 */
    if (g_kar.alpha == 0.0f && g_kar.target_alpha == 0.0f) {
        if (g_kar.bypassed) return;
        if (g_kar.fade_left <= 0) {
            g_kar.fade_left = g_kar.delay;
            g_kar.seam_left = 0;
        }
    }
    /* 单声道里"居中"就是全部信号，无从分离人声；多于 2 声道时本模块
     * 不做臆测的下混——两者都直通。解码链路已由 swresample 归 1/2 声道。 */
    if (channels != KARAOKE_STEREO_CHANNELS) return;
    if (sample_rate <= 0) return;
    if (!karaoke_buffers_alloc()) return;   /* 分配失败：安全降级为直通 */

    if (g_kar.sample_rate != sample_rate) {
        g_kar.sample_rate = sample_rate;
        g_kar.dirty = 1;
    }
    if (g_kar.dirty) {
        g_kar.dirty = 0;
        karaoke_recalc();
        karaoke_clear_stream();
        g_kar.alpha = 0.0f;   /* 配置变了，旧的逼近值没有意义 */
        if (g_kar.target_alpha > 0.0f) {
            /* 采样率切换 = 重新预热：前 D 个样本再次重放 + 接缝交叉淡化 */
            g_kar.seam_left = g_kar.delay;
            g_kar.fade_left = 0;
            g_kar.bypassed = 0;
        } else {
            /* 收坡/旁路途中换采样率：流已清空，直接旁路 */
            g_kar.seam_left = 0;
            g_kar.fade_left = 0;
            g_kar.bypassed = 1;
        }
    }

    while (frame_count > 0) {
        const int n = frame_count > KARAOKE_STFT_CHUNK ? KARAOKE_STFT_CHUNK : frame_count;
        karaoke_process_chunk(samples, n);
        samples += (size_t)n * KARAOKE_STEREO_CHANNELS;
        frame_count -= n;
    }
}
