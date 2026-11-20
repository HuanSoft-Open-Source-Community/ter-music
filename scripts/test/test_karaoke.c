/**
 * @file test_karaoke.c
 * @brief 卡拉OK（消人声）DSP 的数值单测
 *
 * 覆盖：频带内居中内容的抵消量（full/soft 两档）、频带外（低频/高频）
 * 居中能量的保留、声像两侧（side）内容的原样保留、单声道与"不启用"
 * 的逐位直通、非法档位的防御、采样率切换后的系数重算，档位切换的
 * 30ms 平滑逼近，逐频点门控对立体声伴奏的保护（纯伴奏段该频点 mid
 * 原样保留），以及"降回不启用"的收坡淡出（不硬切、最终逐位直通）。
 *
 * 逐频点 STFT 门控的专项回归（2026-11 漏音修复）：
 *   - test_transient_leak：军鼓+镲式带内多共振旁侧瞬态下，800Hz 居中人声
 *     的 1ms 窗漏出必须为 0、最大增益 ≤ -20dB（生成器为 s10 素材：
 *     旧宽带门控实测漏出 768.3ms/14 次、最长 55ms，漏出瞬间 0.00dB；
 *     s07 素材对应 592.7ms/14 次/43ms）。
 *   - test_steady_wide_cancellation：稳态宽旁侧伴奏下居中人声抵消 ≥ 25dB
 *     （旧实现 0.00dB）；
 *   - test_accompaniment_protection：纯宽伴奏的 mid 分量不得比现状倒退
 *     超过 1dB；
 *   - test_latency_and_bypass：附加算法延迟是显式常量、≤10ms，且旁路仍
 *     逐位直通；
 *   - test_output_robustness / test_block_invariance / test_reset_equivalence：
 *     输出有限性、块长不变性、reset 后与全新会话逐位等价。
 */

#include "audio/karaoke.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define SR            44100
#define TOTAL_FRAMES  (SR * 2)          /* 2 秒：足够暖机 + 测量 */
#define MEASURE_FROM  (SR / 2)          /* 前 0.5 秒留给 α 逼近与滤波器稳定 */
#define MEASURE_TO    (SR + SR / 2)     /* 测量窗 1 秒：整秒保证各分量正交 */
#define BLOCK         1024              /* 与 WRITE_BATCH_FRAMES 一致 */
#define FULL_SCALE    2147483647.0

static int32_t *make_tone(double freq, int centered, double amp, int frames, int channels)
{
    int32_t *buf = calloc((size_t)frames * (size_t)channels, sizeof(int32_t));
    if (!buf) return NULL;
    for (int i = 0; i < frames; i++) {
        double s = amp * sin(2.0 * M_PI * freq * (double)i / (double)SR);
        int32_t v = (int32_t)(s * FULL_SCALE);
        buf[(size_t)i * channels] = v;
        if (channels == 2)
            buf[(size_t)i * channels + 1] = centered ? v : -v;
    }
    return buf;
}

/* 多分量生成器：每个分量按 mid 幅度 mamp_j·sin(θ) 与 side 幅度
 * damp_j·sin(θ+φ_j) 相加；damp=0 即完全居中（单声道内容），
 * mamp=damp 且 φ=90° 即左右去相关的立体声展开（伴奏铺底的典型形态）。
 * L = m + d，R = m - d。各分量频率取整赫兹，1 秒窗内正交。 */
static int32_t *make_md(const double *freq, const double *mamp, const double *damp,
                        const double *dphase_deg, int ncomp, int frames)
{
    int32_t *buf = calloc((size_t)frames * 2, sizeof(int32_t));
    if (!buf) return NULL;
    for (int i = 0; i < frames; i++) {
        double t = (double)i / (double)SR;
        double m = 0.0, d = 0.0;
        for (int j = 0; j < ncomp; j++) {
            double th = 2.0 * M_PI * freq[j] * t;
            m += mamp[j] * sin(th);
            d  += damp[j] * sin(th + dphase_deg[j] * M_PI / 180.0);
        }
        double l = m + d, r = m - d;
        if (l > 1.0) l = 1.0; if (l < -1.0) l = -1.0;
        if (r > 1.0) r = 1.0; if (r < -1.0) r = -1.0;
        buf[(size_t)i * 2]     = (int32_t)(l * FULL_SCALE);
        buf[(size_t)i * 2 + 1] = (int32_t)(r * FULL_SCALE);
    }
    return buf;
}

/* 正交解调提取 buf（立体声交织，取左声道）中 freq 分量的幅度（0..1） */
static double tone_amp_at(const int32_t *buf, int from, int len, double freq)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < len; i++) {
        double th = 2.0 * M_PI * freq * (double)(from + i) / (double)SR;
        double x = (double)buf[((size_t)from + i) * 2] / FULL_SCALE;
        re += x * sin(th);
        im += x * cos(th);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)len;
}

static double rms_channel(const int32_t *buf, int from, int to, int channels, int ch)
{
    double acc = 0.0;
    long n = 0;
    for (int i = from; i < to; i++) {
        double x = (double)buf[(size_t)i * channels + ch] / FULL_SCALE;
        acc += x * x;
        n++;
    }
    return n > 0 ? sqrt(acc / (double)n) : 0.0;
}

/* 返回"处理前/后"的增益变化（dB，负数＝被削弱） */
static double run_gain_db(int mode, double freq, int centered, int channels,
                          int sample_rate, int *first_block_louder)
{
    int32_t *ref = make_tone(freq, centered, 0.8, TOTAL_FRAMES, channels);
    int32_t *out = make_tone(freq, centered, 0.8, TOTAL_FRAMES, channels);
    if (!ref || !out) { free(ref); free(out); return -999.0; }

    karaoke_init();
    karaoke_set_mode(mode);

    double first_block_rms = 0.0;
    int recorded_first = 0;
    for (int off = 0; off < TOTAL_FRAMES; off += BLOCK) {
        int n = (TOTAL_FRAMES - off) < BLOCK ? (TOTAL_FRAMES - off) : BLOCK;
        karaoke_process(out + (size_t)off * channels, n, channels, sample_rate);
        if (!recorded_first) {
            first_block_rms = rms_channel(out, off, off + n, channels, 0);
            recorded_first = 1;
        }
    }

    double in_rms  = rms_channel(ref,  MEASURE_FROM, SR + SR / 2, channels, 0);
    double out_rms = rms_channel(out,  MEASURE_FROM, SR + SR / 2, channels, 0);

    if (first_block_louder)
        *first_block_louder = (first_block_rms > out_rms * 1.3) ? 1 : 0;

    free(ref);
    free(out);
    if (in_rms <= 0.0 || out_rms <= 0.0) return -999.0;
    return 20.0 * log10(out_rms / in_rms);
}

/* ── 抵消量：频带内正中信号 ─────────────────────────────────────── */
static void test_cancellation(void)
{
    int dummy = 0;
    double full = run_gain_db(KARAOKE_MODE_FULL, 1000.0, 1, 2, SR, &dummy);
    CHECK(full <= -30.0 && full > -90.0, "full 档：正中 1kHz 应衰减 >= 30dB");

    double soft = run_gain_db(KARAOKE_MODE_SOFT, 1000.0, 1, 2, SR, &dummy);
    CHECK(soft <= -6.0 && soft >= -12.0, "soft 档：正中 1kHz 应衰减 6~12dB（目标约 -8dB）");

    double soft_less = (soft > full) ? 1 : 0;
    CHECK(soft_less, "soft 档残留必须比 full 档多");
}

/* ── 频带外：居中低频与高频必须保住 ────────────────────────────── */
static void test_band_edges_preserved(void)
{
    int dummy = 0;
    double bass = run_gain_db(KARAOKE_MODE_FULL, 100.0, 1, 2, SR, &dummy);
    CHECK(fabs(bass) <= 2.0, "full 档：居中 100Hz（贝斯/底鼓）应基本保留");

    double air = run_gain_db(KARAOKE_MODE_FULL, 10000.0, 1, 2, SR, &dummy);
    CHECK(fabs(air) <= 2.0, "full 档：居中 10kHz（空气感）应基本保留");
}

/* ── 声像两侧：side 内容原样不动 ───────────────────────────────── */
static void test_side_preserved(void)
{
    int dummy = 0;
    /* L = -R：mid 为 0，全部能量都在 side，不应被动过 */
    double side = run_gain_db(KARAOKE_MODE_FULL, 1000.0, 0, 2, SR, &dummy);
    CHECK(fabs(side) <= 0.5, "full 档：硬声像（L=-R）的 side 内容必须不变");
}

/* ── 直通：不启用 / 单声道 ─────────────────────────────────────── */
static void test_passthrough(void)
{
    /* 不启用：逐位不变 */
    int32_t *ref = make_tone(1000.0, 1, 0.8, TOTAL_FRAMES, 2);
    int32_t *out = make_tone(1000.0, 1, 0.8, TOTAL_FRAMES, 2);
    CHECK(ref && out, "测试数据分配成功");
    if (ref && out) {
        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_OFF);
        CHECK(!karaoke_is_active(), "不启用时 is_active 为 0");
        for (int off = 0; off < TOTAL_FRAMES; off += BLOCK) {
            int n = (TOTAL_FRAMES - off) < BLOCK ? (TOTAL_FRAMES - off) : BLOCK;
            karaoke_process(out + (size_t)off * 2, n, 2, SR);
        }
        CHECK(memcmp(ref, out, (size_t)TOTAL_FRAMES * 2 * sizeof(int32_t)) == 0,
              "不启用必须逐位直通");

        /* 单声道：即使开着 full 也不能动 */
        karaoke_set_mode(KARAOKE_MODE_FULL);
        CHECK(karaoke_is_active(), "启用后 is_active 为 1");
        int32_t *mono = make_tone(1000.0, 1, 0.8, TOTAL_FRAMES, 1);
        int32_t *mono_ref = make_tone(1000.0, 1, 0.8, TOTAL_FRAMES, 1);
        CHECK(mono && mono_ref, "单声道测试数据分配成功");
        if (mono && mono_ref) {
            for (int off = 0; off < TOTAL_FRAMES; off += BLOCK) {
                int n = (TOTAL_FRAMES - off) < BLOCK ? (TOTAL_FRAMES - off) : BLOCK;
                karaoke_process(mono + (size_t)off, n, 1, SR);
            }
            CHECK(memcmp(mono_ref, mono, (size_t)TOTAL_FRAMES * sizeof(int32_t)) == 0,
                  "单声道必须逐位直通（无从分离居中人声）");
        }
        free(mono);
        free(mono_ref);
    }
    free(ref);
    free(out);
}

/* ── 相似度门控：纯立体声伴奏段整带放行 ─────────────────────────── */
static void run_md_blocks(int32_t *buf, int channels, int sample_rate)
{
    for (int off = 0; off < TOTAL_FRAMES; off += BLOCK) {
        int n = (TOTAL_FRAMES - off) < BLOCK ? (TOTAL_FRAMES - off) : BLOCK;
        karaoke_process(buf + (size_t)off * channels, n, channels, sample_rate);
    }
}

static void test_wide_accompaniment_preserved(void)
{
    struct { const char *name; double f; double limit_db; } cases[] = {
        { "2.5kHz 去相关铺底", 2500.0, -1.5 },   /* 门控旧版实测 -3.5dB，新版 ~0dB */
        { "500Hz  去相关铺底",  500.0, -1.5 },
    };
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        const double f = cases[c].f;
        const double freq[1]  = { f };
        const double mamp[1]  = { 0.30 };
        const double damp[1]  = { 0.30 };       /* mid/side 同能量、相差90° */
        const double dph[1]   = { 90.0 };

        int32_t *ref = make_md(freq, mamp, damp, dph, 1, TOTAL_FRAMES);
        int32_t *out = make_md(freq, mamp, damp, dph, 1, TOTAL_FRAMES);
        CHECK(ref && out, "宽伴奏保护测试数据分配成功");
        if (!ref || !out) { free(ref); free(out); continue; }

        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        run_md_blocks(out, 2, SR);

        double in_a  = tone_amp_at(ref, MEASURE_FROM, MEASURE_TO - MEASURE_FROM, f);
        double out_a = tone_amp_at(out, MEASURE_FROM, MEASURE_TO - MEASURE_FROM, f);
        double gain = 20.0 * log10(out_a / (in_a > 0.0 ? in_a : 1e-12));
        char msg[128];
        snprintf(msg, sizeof(msg), "full 档：%s应被门控保护（>= %.1f dB）", cases[c].name, cases[c].limit_db);
        CHECK(gain >= cases[c].limit_db, msg);
        fprintf(stderr, "  [info] %s: gain=%.2fdB\n", cases[c].name, gain);

        free(ref);
        free(out);
    }
}

/* ── 半带证据取大：立体声伴奏在场时，居中人声抵消量不打折 ───────── */
static void test_gate_keeps_vocal_cancellation(void)
{
    const double freq[2] = { 800.0, 3000.0 };
    const double mamp[2] = { 0.45, 0.30 };
    const double damp[2] = { 0.00, 0.30 };      /* 人声正中；伴奏立体声展开 */
    const double dph[2]  = { 0.0, 90.0 };

    int32_t *ref = make_md(freq, mamp, damp, dph, 2, TOTAL_FRAMES);
    int32_t *out = make_md(freq, mamp, damp, dph, 2, TOTAL_FRAMES);
    CHECK(ref && out, "门控人声测试数据分配成功");
    if (!ref || !out) { free(ref); free(out); return; }

    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    run_md_blocks(out, 2, SR);

    int len = MEASURE_TO - MEASURE_FROM;
    double v_in  = tone_amp_at(ref, MEASURE_FROM, len, 800.0);
    double v_out = tone_amp_at(out, MEASURE_FROM, len, 800.0);
    double a_in  = tone_amp_at(ref, MEASURE_FROM, len, 3000.0);
    double a_out = tone_amp_at(out, MEASURE_FROM, len, 3000.0);

    double vgain = 20.0 * log10(v_out / (v_in > 0 ? v_in : 1e-12));
    double again = 20.0 * log10(a_out / (a_in > 0 ? a_in : 1e-12));
    CHECK(vgain <= -25.0, "3kHz 立体声伴奏在场时，800Hz 居中人声仍须抵消 >= 25dB");
    CHECK(again >= -8.0,  "人声+伴奏混叠段允许伴奏受损，但不得反常放大（>= -8dB）");
    fprintf(stderr, "  [info] vocal-in-mix: vocal=%.2fdB accompaniment=%.2fdB\n", vgain, again);

    free(ref);
    free(out);
}

/* ── 降回不启用：沿同一曲线收坡，走完逐位直通 ──────────────────── */
static void test_off_ramps_out(void)
{
    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_SOFT);

    int32_t *ref = make_tone(1000.0, 1, 0.8, TOTAL_FRAMES, 2);
    int32_t *out = make_tone(1000.0, 1, 0.8, TOTAL_FRAMES, 2);
    CHECK(ref && out, "收坡测试数据分配成功");
    if (!ref || !out) { free(ref); free(out); return; }

    int half = SR / 2;                    /* 前 0.5s 稳态，中段切 OFF */
    int off_at = (half / BLOCK) * BLOCK;
    int32_t *tail_ref = make_tone(1000.0, 1, 0.8, TOTAL_FRAMES, 2);
    CHECK(tail_ref, "收坡参考数据分配成功");

    double steady = -1.0, first = -1.0;
    for (int pos = 0; pos + BLOCK <= TOTAL_FRAMES; pos += BLOCK) {
        if (pos == off_at) {
            steady = rms_channel(out, pos - BLOCK, pos, 2, 0);
            karaoke_set_mode(KARAOKE_MODE_OFF);   /* 播放中切回不启用 */
            first = -1.0;
        }
        karaoke_process(out + (size_t)pos * 2, BLOCK, 2, SR);
        if (pos == off_at) first = rms_channel(out, pos, pos + BLOCK, 2, 0);
    }
    /* 收坡语义：切档首块比稳态响（α>0 还在削弱），但远低于直通 */
    double full_rms = rms_channel(tail_ref, off_at, off_at + BLOCK, 2, 0);
    CHECK(steady > 0.0 && first > steady, "切到不启用的首块必须仍在淡出（非硬切直通）");
    CHECK(first < full_rms * 0.9, "切档首块不得直接回到满幅（必须经过斜坡）");

    /* 坡走完（>0.5s）之后必须逐位直通 */
    int late = off_at + SR / 2;
    late = (late / BLOCK) * BLOCK;
    if (late + BLOCK <= TOTAL_FRAMES) {
        CHECK(memcmp(out + (size_t)late * 2, tail_ref + (size_t)late * 2,
                     (size_t)BLOCK * 2 * sizeof(int32_t)) == 0,
              "收坡结束后必须逐位直通（旁路生效）");
    }
    fprintf(stderr, "  [info] off-ramp: steady=%.4f first=%.4f full=%.4f\n",
            steady, first, full_rms);

    free(ref); free(out); free(tail_ref);
}

/* ── 逐频点门控专项回归的素材与测量辅助 ───────────────────────── */

/* 军鼓 body(300Hz) + 镲(2.6k/6.5k) 的 14 次硬声像旁侧瞬态 + 800Hz 居中人声。
 * 这是实测能冲开旧宽带门控的构型：三者的能量都落在 200–5000Hz 抵消带内，
 * 且分别占据低/高半带，旧实现"两半带取大"后整带放行；逐频点门控下
 * 这些能量只影响各自频点，人声所在频点不受影响。 */
static int32_t *make_scene_transient(int frames, double vocal_amp, double hit_scale)
{
    int32_t *buf = calloc((size_t)frames * 2, sizeof(int32_t));
    if (!buf) return NULL;
    for (int i = 0; i < frames; i++) {
        const double t = (double)i / (double)SR;
        double m = vocal_amp * sin(2.0 * M_PI * 800.0 * t);
        double d = 0.0;
        for (int k = 0; k < 14; k++) {
            const double t0 = 0.40 + 0.18 * (double)k;
            if (t < t0 || t > t0 + 0.32) continue;
            const double l = t - t0;
            const double e1 = (l < 0.0005) ? l / 0.0005 : exp(-(l - 0.0005) / 0.030);
            const double e2 = (l < 0.0005) ? l / 0.0005 : exp(-(l - 0.0005) / 0.020);
            d += hit_scale * (0.30 * e1 * sin(2.0 * M_PI * 300.0 * t)
                            + 0.25 * e2 * sin(2.0 * M_PI * 2600.0 * t + M_PI / 2.0)
                            + 0.25 * e2 * sin(2.0 * M_PI * 6500.0 * t));
        }
        double l = m + d, r = m - d;
        if (l > 1.0) l = 1.0;
        if (l < -1.0) l = -1.0;
        if (r > 1.0) r = 1.0;
        if (r < -1.0) r = -1.0;
        buf[(size_t)i * 2]     = (int32_t)(l * FULL_SCALE);
        buf[(size_t)i * 2 + 1] = (int32_t)(r * FULL_SCALE);
    }
    return buf;
}

/* 稳态宽旁侧伴奏：300Hz 与 2400Hz 两个半带都铺 mid=side 的去相关宽立体声
 * （相差 90°），外加 800Hz 居中人声。旧实现在这里 0.00dB 完全没消。 */
static int32_t *make_scene_steady_wide(int frames, double vocal_amp, double acc_amp)
{
    int32_t *buf = calloc((size_t)frames * 2, sizeof(int32_t));
    if (!buf) return NULL;
    for (int i = 0; i < frames; i++) {
        const double t = (double)i / (double)SR;
        const double m = vocal_amp * sin(2.0 * M_PI * 800.0 * t)
                       + acc_amp * sin(2.0 * M_PI * 300.0 * t)
                       + acc_amp * sin(2.0 * M_PI * 2400.0 * t);
        const double d = acc_amp * sin(2.0 * M_PI * 300.0 * t + M_PI / 2.0)
                       + acc_amp * sin(2.0 * M_PI * 2400.0 * t + M_PI / 2.0);
        double l = m + d, r = m - d;
        if (l > 1.0) l = 1.0;
        if (l < -1.0) l = -1.0;
        if (r > 1.0) r = 1.0;
        if (r < -1.0) r = -1.0;
        buf[(size_t)i * 2]     = (int32_t)(l * FULL_SCALE);
        buf[(size_t)i * 2 + 1] = (int32_t)(r * FULL_SCALE);
    }
    return buf;
}

/* 纯宽伴奏（无人声）：用于"伴奏保护不倒退"回归。 */
static int32_t *make_scene_pure_wide(int frames, double acc_amp)
{
    return make_scene_steady_wide(frames, 0.0, acc_amp);
}

/* 从输出恢复中置通道 m'=(L'+R')/2 并做正交解调（1ms 窗同一口径）。 */
static double demod_mid_at(const int32_t *buf, int from, int len, double freq)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < len; i++) {
        const double th = 2.0 * M_PI * freq * (double)(from + i) / (double)SR;
        const double mid = 0.5 * ((double)buf[((size_t)from + i) * 2]
                                + (double)buf[((size_t)from + i) * 2 + 1]) / FULL_SCALE;
        re += mid * sin(th);
        im += mid * cos(th);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)len;
}

static double demod_left_at(const int32_t *buf, int from, int len, double freq)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < len; i++) {
        const double th = 2.0 * M_PI * freq * (double)(from + i) / (double)SR;
        const double x = (double)buf[((size_t)from + i) * 2] / FULL_SCALE;
        re += x * sin(th);
        im += x * cos(th);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)len;
}

static void run_blocks_n(int32_t *buf, int frames, int channels, int sample_rate)
{
    for (int off = 0; off < frames; off += BLOCK) {
        int n = (frames - off) < BLOCK ? (frames - off) : BLOCK;
        karaoke_process(buf + (size_t)off * channels, n, channels, sample_rate);
    }
}

/* 逐 1ms 窗统计 800Hz 人声增益：返回最大增益，并回填漏出窗数/最长连续。 */
typedef struct { double max_gain; int leak_windows; int longest_ms; int groups; } LeakStat;

static LeakStat measure_transient_leak(int mode, double from_s, double to_s,
                                       int frames, const int32_t *ref, int32_t *out)
{
    const int win = 44, hop = 44;          /* 1ms 窗 / 1ms 跳距 */
    LeakStat st = { -1e9, 0, 0, 0 };
    const int from = (int)(from_s * (double)SR);
    const int to = (int)(to_s * (double)SR) < frames ? (int)(to_s * (double)SR) : frames;
    int run = 0;
    for (int pos = from; pos + win <= to; pos += hop) {
        const double a_in = demod_mid_at(ref, pos, win, 800.0);
        const double a_out = demod_mid_at(out, pos, win, 800.0);
        const double db = 20.0 * log10((a_out > 1e-12 ? a_out : 1e-12) /
                                       (a_in > 1e-12 ? a_in : 1e-12));
        if (db > st.max_gain) st.max_gain = db;
        if (db > -20.0) {
            st.leak_windows++;
            run++;
            if (run > st.longest_ms) st.longest_ms = run;
        } else if (run > 0) {
            st.groups++;
            run = 0;
        }
    }
    if (run > 0) st.groups++;
    (void)mode;
    return st;
}

/* ── 瞬态旁侧漏音：1ms 窗漏出必须为 0、最大增益 ≤ -20dB ─────────── */
static void test_transient_leak(void)
{
    const int frames = SR * 3;
    int32_t *ref = make_scene_transient(frames, 0.10, 1.0);
    int32_t *out = make_scene_transient(frames, 0.10, 1.0);
    CHECK(ref && out, "瞬态旁侧测试数据分配成功");
    if (!ref || !out) { free(ref); free(out); return; }

    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    run_blocks_n(out, frames, 2, SR);
    LeakStat f = measure_transient_leak(KARAOKE_MODE_FULL, 0.30, 2.999, frames, ref, out);
    fprintf(stderr, "  [info] transient full: max=%.2fdB leaks=%d longest=%dms groups=%d\n",
            f.max_gain, f.leak_windows, f.longest_ms, f.groups);
    CHECK(f.leak_windows == 0, "full 档：瞬态旁侧不得有 1ms 窗人声增益 > -20dB（漏出窗口数必须为 0）");
    CHECK(f.max_gain <= -20.0, "full 档：瞬态旁侧 1ms 窗人声增益最大不得高于 -20dB");

    /* soft 档：人声本来就留 ~-8dB，用"不得回到距完全不消 5dB 以内"判据 */
    int32_t *out_s = make_scene_transient(frames, 0.10, 1.0);
    CHECK(out_s, "瞬态旁侧 soft 测试数据分配成功");
    if (out_s) {
        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_SOFT);
        run_blocks_n(out_s, frames, 2, SR);
        LeakStat sf = measure_transient_leak(KARAOKE_MODE_SOFT, 0.30, 2.999, frames, ref, out_s);
        fprintf(stderr, "  [info] transient soft: max=%.2fdB\n", sf.max_gain);
        CHECK(sf.max_gain <= -3.0, "soft 档：瞬态旁侧 1ms 窗人声增益不得高于 -3dB");
        free(out_s);
    }
    free(ref);
    free(out);
}

/* ── 稳态宽旁侧伴音下居中人声仍须抵消 ≥ 25dB ───────────────────── */
static void test_steady_wide_cancellation(void)
{
    const int frames = SR * 2;
    int32_t *ref = make_scene_steady_wide(frames, 0.10, 0.22);
    int32_t *out = make_scene_steady_wide(frames, 0.10, 0.22);
    CHECK(ref && out, "稳态宽旁侧测试数据分配成功");
    if (!ref || !out) { free(ref); free(out); return; }

    const int lf = SR / 2, len = SR;   /* 0.5–1.5s，整秒保证各分量正交 */
    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    run_blocks_n(out, frames, 2, SR);
    const double ai = demod_mid_at(ref, lf, len, 800.0);
    const double ao = demod_mid_at(out, lf, len, 800.0);
    const double full_db = 20.0 * log10((ao > 1e-12 ? ao : 1e-12) /
                                        (ai > 1e-12 ? ai : 1e-12));
    fprintf(stderr, "  [info] steady-wide full: vocal=%.2fdB body=%.2fdB\n", full_db, 0.0);
    CHECK(full_db <= -25.0, "full 档：稳态宽旁侧伴奏在场时 800Hz 居中人声仍须抵消 >= 25dB");

    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_SOFT);
    int32_t *out_s = make_scene_steady_wide(frames, 0.10, 0.22);
    CHECK(out_s, "稳态宽旁侧 soft 测试数据分配成功");
    if (out_s) {
        run_blocks_n(out_s, frames, 2, SR);
        const double so = demod_mid_at(out_s, lf, len, 800.0);
        const double soft_db = 20.0 * log10((so > 1e-12 ? so : 1e-12) /
                                            (ai > 1e-12 ? ai : 1e-12));
        fprintf(stderr, "  [info] steady-wide soft: vocal=%.2fdB\n", soft_db);
        CHECK(soft_db <= -6.0, "soft 档：稳态宽旁侧伴奏在场时人声仍须按 α=0.6 削弱（<= -6dB）");
        free(out_s);
    }
    free(ref);
    free(out);
}

/* ── 纯伴奏保护不倒退：门控的原始目的，逐频点实现不得回退 ──────── */
static void test_accompaniment_protection(void)
{
    const int frames = SR * 2;
    int32_t *ref = make_scene_pure_wide(frames, 0.22);
    int32_t *out = make_scene_pure_wide(frames, 0.22);
    CHECK(ref && out, "纯伴奏保护测试数据分配成功");
    if (!ref || !out) { free(ref); free(out); return; }

    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    run_blocks_n(out, frames, 2, SR);

    const int lf = SR / 2, len = SR;
    static const double probe[2] = { 300.0, 2400.0 };
    for (int j = 0; j < 2; j++) {
        const double ai = demod_left_at(ref, lf, len, probe[j]);
        const double ao = demod_left_at(out, lf, len, probe[j]);
        const double db = 20.0 * log10((ao > 1e-12 ? ao : 1e-12) /
                                       (ai > 1e-12 ? ai : 1e-12));
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "full 档：纯宽伴奏 %.0fHz 的 mid 分量不得比现状（0.000dB）低超过 1dB",
                 probe[j]);
        CHECK(db >= -1.0, msg);
        fprintf(stderr, "  [info] accompaniment %.0fHz: gain=%.3fdB\n", probe[j], db);
    }
    free(ref);
    free(out);
}

/* ── 附加算法延迟是显式常量 ≤ 10ms；旁路仍逐位直通 ─────────────── */
static int measure_added_latency(int sample_rate)
{
    const int frames = sample_rate;          /* 1s */
    int32_t *buf = calloc((size_t)frames * 2, sizeof(int32_t));
    if (!buf) return -1;
    const int at = frames / 2;
    /* 纯 side 脉冲（mid=0 → 逐频点门恒为 0、处理流原样延时通过）。
     * 居中脉冲会被带内抵消淹掉，读不出干净的时间轴映射（t4 F6）。 */
    buf[(size_t)at * 2]     = (int32_t)(0.5 * FULL_SCALE);
    buf[(size_t)at * 2 + 1] = (int32_t)(-0.5 * FULL_SCALE);

    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    for (int off = 0; off < frames; off += BLOCK) {
        const int n = (frames - off) < BLOCK ? (frames - off) : BLOCK;
        karaoke_process(buf + (size_t)off * 2, n, 2, sample_rate);
    }
    int peak = 0;
    double peak_v = -1.0;
    for (int i = 0; i < frames; i++) {
        const double v = fabs((double)buf[(size_t)i * 2]);
        if (v > peak_v) { peak_v = v; peak = i; }
    }
    const int latency = peak - at;
    free(buf);
    return latency;
}

static void test_latency_and_bypass(void)
{
    /* 头文件承诺：44.1k 附加延迟 = 383 样本（8.69ms），所有采样率 ≤ 10ms */
    CHECK(KARAOKE_STFT_DELAY_44100 == KARAOKE_STFT_WIN_44100 - 1,
          "44.1k 附加延迟常量必须等于窗长-1");
    CHECK((long)KARAOKE_STFT_DELAY_44100 * 1000 / 44100 < 10,
          "44.1k 附加延迟必须 < 10ms");
    CHECK(KARAOKE_STFT_LATENCY_MAX_MS <= 10, "附加延迟上界常量必须 <= 10ms");

    const int lat44 = measure_added_latency(44100);
    const int lat48 = measure_added_latency(48000);
    const int lat22 = measure_added_latency(22050);
    fprintf(stderr, "  [info] latency: 44.1k=%d smp (%.2fms), 48k=%d smp (%.2fms), "
                    "22.05k=%d smp (%.2fms)\n",
            lat44, (double)lat44 * 1000.0 / 44100.0,
            lat48, (double)lat48 * 1000.0 / 48000.0,
            lat22, (double)lat22 * 1000.0 / 22050.0);
    CHECK(lat44 == KARAOKE_STFT_DELAY_44100,
          "44.1k 实测附加延迟必须等于常量");
    CHECK(lat48 == KARAOKE_STFT_DELAY_44100,
          "48k 实测附加延迟必须等于常量");
    CHECK(lat22 == (KARAOKE_STFT_WIN_44100 / 2) - 1,
          "22.05k 实测附加延迟必须等于该采样率的窗长-1");
    CHECK((double)lat44 * 1000.0 / 44100.0 <= (double)KARAOKE_STFT_LATENCY_MAX_MS, "44.1k 延迟 <= 10ms");
    CHECK((double)lat48 * 1000.0 / 48000.0 <= (double)KARAOKE_STFT_LATENCY_MAX_MS, "48k 延迟 <= 10ms");
    CHECK((double)lat22 * 1000.0 / 22050.0 <= (double)KARAOKE_STFT_LATENCY_MAX_MS, "22.05k 延迟 <= 10ms");

    /* 旁路：处理过一段之后收坡，坡走完必须逐位直通 */
    const int frames = SR * 2;
    int32_t *ref = make_tone(1000.0, 1, 0.8, frames, 2);
    int32_t *out = make_tone(1000.0, 1, 0.8, frames, 2);
    CHECK(ref && out, "旁路直通测试数据分配成功");
    if (ref && out) {
        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        const int off_at = (SR / BLOCK) * BLOCK;   /* 1s 附近，按块对齐 */
        for (int off = 0; off + BLOCK <= frames; off += BLOCK) {
            if (off == off_at) karaoke_set_mode(KARAOKE_MODE_OFF);   /* 播放中切回不启用 */
            karaoke_process(out + (size_t)off * 2, BLOCK, 2, SR);
        }
        const int late = off_at + SR / 2;   /* 切档 0.5s 后 */
        CHECK(memcmp(out + (size_t)late * 2, ref + (size_t)late * 2,
                     (size_t)BLOCK * 2 * sizeof(int32_t)) == 0,
              "处理流收坡结束后必须逐位直通（不含任何 STFT 残留）");
    }
    free(ref);
    free(out);
}

/* ── 输出有限性：极值/静音/超小输入不得产生 NaN 指纹或残留 ──────── */
static void test_output_robustness(void)
{
    const int frames = SR;
    int32_t *buf = calloc((size_t)frames * 2, sizeof(int32_t));
    CHECK(buf, "输出有限性测试数据分配成功");
    if (!buf) return;

    /* ±满幅方波 + DC + INT32_MIN/MAX 混排 */
    for (int i = 0; i < frames; i++) {
        int32_t v;
        switch (i % 4) {
            case 0:  v = (int32_t)0x7FFFFFFF; break;
            case 1:  v = (int32_t)0x80000000; break;   /* 真实 INT32_MIN 样本 */
            case 2:  v = (i & 1) ? (int32_t)0x7FFFFFFF : (int32_t)0x80000000; break;
            default: v = (i / 4) % 2 ? 1 : -1; break;
        }
        buf[(size_t)i * 2]     = v;
        buf[(size_t)i * 2 + 1] = (i % 8 < 4) ? v : (int32_t)(0u - (uint32_t)v);
    }
    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    run_blocks_n(buf, frames, 2, SR);
    int intmin = 0;
    for (int i = 0; i < frames * 2; i++)
        if (buf[i] == (int32_t)0x80000000) intmin++;
    CHECK(intmin == 0, "极值输入不得产生 INT32_MIN（NaN/UB 指纹）");

    /* 1 LSB 居中输入后接静音：静音块必须逐位为 0（状态无残留） */
    int32_t *tiny = calloc((size_t)frames * 2, sizeof(int32_t));
    CHECK(tiny, "1 LSB 测试数据分配成功");
    if (tiny) {
        for (int i = 0; i < frames / 2; i++)
            tiny[(size_t)i * 2] = tiny[(size_t)i * 2 + 1] = 1;
        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        run_blocks_n(tiny, frames, 2, SR);
        /* 模块有 窗长-1 的算法延迟：静音段最前面那截仍是对应输入的延迟内容，
         * 之后（延迟 + 一个窗长之后）必须逐位为 0。 */
        int nonzero_tail = 0;
        const int check_from = frames / 2 + 2 * KARAOKE_STFT_WIN_44100;
        for (int i = check_from; i < frames; i++)
            if (tiny[(size_t)i * 2] != 0 || tiny[(size_t)i * 2 + 1] != 0) nonzero_tail++;
        CHECK(nonzero_tail == 0, "1 LSB 输入之后的静音块必须逐位为 0（无 denormal 残留）");
        free(tiny);
    }
    free(buf);
}

/* ── 块长不变性：内部按绝对样本序号推进，块边界不得影响输出 ─────── */
static void test_block_invariance(void)
{
    const int frames = SR;
    static const int sizes[5] = { 1, 37, 1024, 4096, 7919 };
    int32_t *ref = make_scene_transient(frames, 0.10, 1.0);
    int32_t *base = calloc((size_t)frames * 2, sizeof(int32_t));
    CHECK(ref && base, "块长不变性测试数据分配成功");
    if (!ref || !base) { free(ref); free(base); return; }
    memcpy(base, ref, (size_t)frames * 2 * sizeof(int32_t));
    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    run_blocks_n(base, frames, 2, SR);

    for (int k = 0; k < 5; k++) {
        int32_t *tmp = calloc((size_t)frames * 2, sizeof(int32_t));
        CHECK(tmp, "块长不变性临时缓冲分配成功");
        if (!tmp) continue;
        memcpy(tmp, ref, (size_t)frames * 2 * sizeof(int32_t));
        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        for (int off = 0; off < frames; off += sizes[k]) {
            const int n = (frames - off) < sizes[k] ? (frames - off) : sizes[k];
            karaoke_process(tmp + (size_t)off * 2, n, 2, SR);
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "块长 %d 的处理结果必须与块长 1024 逐位相同", sizes[k]);
        CHECK(memcmp(tmp, base, (size_t)frames * 2 * sizeof(int32_t)) == 0, msg);
        free(tmp);
    }
    free(ref);
    free(base);
}

/* ── reset 等价：reset 后的处理必须与全新会话逐位一致 ───────────── */
static void test_reset_equivalence(void)
{
    const int frames = SR;
    int32_t *ref = make_scene_transient(frames, 0.10, 1.0);
    int32_t *a = calloc((size_t)frames * 2, sizeof(int32_t));
    int32_t *b = calloc((size_t)frames * 2, sizeof(int32_t));
    CHECK(ref && a && b, "reset 等价测试数据分配成功");
    if (!ref || !a || !b) { free(ref); free(a); free(b); return; }

    /* 先用脏信号把内部状态跑脏 */
    int32_t *dirty = make_scene_steady_wide(frames, 0.10, 0.22);
    CHECK(dirty, "reset 等价脏信号分配成功");
    if (dirty) {
        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        run_blocks_n(dirty, frames, 2, SR);
        karaoke_reset();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        memcpy(a, ref, (size_t)frames * 2 * sizeof(int32_t));
        run_blocks_n(a, frames, 2, SR);

        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        memcpy(b, ref, (size_t)frames * 2 * sizeof(int32_t));
        run_blocks_n(b, frames, 2, SR);
        CHECK(memcmp(a, b, (size_t)frames * 2 * sizeof(int32_t)) == 0,
              "karaoke_reset 之后处理同一段音频必须与全新会话逐位一致（无残留）");
        free(dirty);
    }
    free(ref); free(a); free(b);
}

/* ── 频点门控的相位/幅度处理：实数缩放，左右严格一致 ────────────── */
static void test_center_phase_coherence(void)
{
    const int frames = SR;
    int32_t *buf = make_tone(1000.0, 1, 0.8, frames, 2);
    CHECK(buf, "相位一致性测试数据分配成功");
    if (!buf) return;
    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    run_blocks_n(buf, frames, 2, SR);

    /* 居中输入（d=0）→ m' 对两声道相同 → L' 与 R' 必须逐位相同，
     * 说明门控是实数缩放、没有引入任何相位旋转或左右不一致。 */
    int diff = 0;
    for (int i = 0; i < frames; i++)
        if (buf[(size_t)i * 2] != buf[(size_t)i * 2 + 1]) diff++;
    CHECK(diff == 0, "居中输入时 L' 与 R' 必须逐位相同（门控不得引入相位旋转）");
    fprintf(stderr, "  [info] center phase: L/R differing samples=%d\n", diff);
    free(buf);
}

/* ── 采样率往返：44.1k→22.05k→44.1k 后必须与全新 44.1k 会话逐位一致 ── */
static void test_sample_rate_roundtrip(void)
{
    const int frames44 = SR;
    const int frames22 = SR / 2;
    int32_t *ref44 = make_scene_steady_wide(frames44, 0.10, 0.22);
    int32_t *out = calloc((size_t)frames44 * 2, sizeof(int32_t));
    int32_t *fresh = calloc((size_t)frames44 * 2, sizeof(int32_t));
    int32_t *mid22 = calloc((size_t)frames22 * 2, sizeof(int32_t));
    CHECK(ref44 && out && fresh && mid22, "采样率往返测试数据分配成功");
    if (!ref44 || !out || !fresh || !mid22) {
        free(ref44); free(out); free(fresh); free(mid22); return;
    }
    for (int i = 0; i < frames22; i++) {
        const double t = (double)i / 22050.0;
        const double v = 0.5 * sin(2.0 * M_PI * 1000.0 * t);
        mid22[(size_t)i * 2] = mid22[(size_t)i * 2 + 1] = (int32_t)(v * FULL_SCALE);
    }

    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    memcpy(out, ref44, (size_t)frames44 * 2 * sizeof(int32_t));
    run_blocks_n(out, frames44, 2, SR);            /* 44.1k */
    run_blocks_n(mid22, frames22, 2, 22050);       /* 切 22.05k */
    karaoke_set_mode(KARAOKE_MODE_FULL);
    memcpy(out, ref44, (size_t)frames44 * 2 * sizeof(int32_t));
    run_blocks_n(out, frames44, 2, SR);            /* 再切回 44.1k */

    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    memcpy(fresh, ref44, (size_t)frames44 * 2 * sizeof(int32_t));
    run_blocks_n(fresh, frames44, 2, SR);

    CHECK(memcmp(out, fresh, (size_t)frames44 * 2 * sizeof(int32_t)) == 0,
          "44.1k→22.05k→44.1k 往返后必须与全新 44.1k 会话逐位一致（采样率变化必须重算并清零）");
    free(ref44); free(out); free(fresh); free(mid22);
}

/* ── 切档时间轴 / 接缝 / 旁路跳变 的素材与辅助 ─────────────────── */

/* 纯 side 斜坡（mid=0 → 门恒为 0，处理流原样延时通过）：用于精确读时间轴。
 * step 取 5e-6~1e-5（约 1–2 万 LSB/样本），float32 往返仍精确，便于断言逐位；
 * 注意 step*frames 不得超过 1.0FS，否则 (int32_t) 转换会溢出（UB）。 */
static int32_t *make_side_ramp(int frames, double step)
{
    int32_t *b = calloc((size_t)frames * 2, sizeof(int32_t));
    if (!b) return NULL;
    for (int i = 0; i < frames; i++) {
        const double v = step * (double)i;
        b[(size_t)i * 2]     = (int32_t)( v * FULL_SCALE);
        b[(size_t)i * 2 + 1] = (int32_t)(-v * FULL_SCALE);
    }
    return b;
}

/* 纯 side 稳态单音（mid=0）：复审用的旁路跳变探针（880Hz/0.5），
 * 自然最大步进 = 2π·880/44100·0.5 ≈ 0.0627 FS。 */
static int32_t *make_side_tone(int frames, double freq, double amp)
{
    int32_t *b = calloc((size_t)frames * 2, sizeof(int32_t));
    if (!b) return NULL;
    for (int i = 0; i < frames; i++) {
        const double v = amp * sin(2.0 * M_PI * freq * (double)i / (double)SR);
        b[(size_t)i * 2]     = (int32_t)( v * FULL_SCALE);
        b[(size_t)i * 2 + 1] = (int32_t)(-v * FULL_SCALE);
    }
    return b;
}

/* 纯 side 鼓击：2kHz、0.5ms 起振、20ms 指数衰减，正中放在升档点（t4 F2 探针）。 */
static int32_t *make_side_hit(int frames, double freq, double amp)
{
    int32_t *b = calloc((size_t)frames * 2, sizeof(int32_t));
    if (!b) return NULL;
    for (int i = 0; i < frames; i++) {
        const double t = (double)i / (double)SR;
        const double l = t;
        const double e = (l < 0.0005) ? l / 0.0005 : exp(-(l - 0.0005) / 0.020);
        const double v = amp * e * sin(2.0 * M_PI * freq * t);
        b[(size_t)i * 2]     = (int32_t)( v * FULL_SCALE);
        b[(size_t)i * 2 + 1] = (int32_t)(-v * FULL_SCALE);
    }
    return b;
}

/* 左声道相邻样本最大步进（FS 归一） */
static double max_abs_step(const int32_t *buf, int from, int to)
{
    if (to > from + 1) { /* ok */ } else return 0.0;
    double mx = 0.0;
    for (int i = from + 1; i < to; i++) {
        const double d = fabs((double)buf[(size_t)i * 2]
                            - (double)buf[(size_t)(i - 1) * 2]) / FULL_SCALE;
        if (d > mx) mx = d;
    }
    return mx;
}

/* 在 [from, frames-64) 里找第一个"连续 64 样本与输入逐位相同"的点（旁路点） */
static int find_bypass(const int32_t *out, const int32_t *ref, int from, int frames)
{
    for (int i = from; i + 64 < frames; i++) {
        int eq = 1;
        for (int k = 0; k < 64; k++)
            if (out[(size_t)(i + k) * 2] != ref[(size_t)(i + k) * 2]) { eq = 0; break; }
        if (eq) return i;
    }
    return -1;
}

/* ── 激活预热：首块用当前输入填充，不得出现静音缺口 ─────────────── */
static void test_activation_priming(void)
{
    const int frames = SR;
    int32_t *ref = make_tone(1000.0, 1, 0.8, frames, 2);
    int32_t *out = make_tone(1000.0, 1, 0.8, frames, 2);
    CHECK(ref && out, "激活预热测试数据分配成功");
    if (!ref || !out) { free(ref); free(out); return; }

    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    karaoke_process(out, BLOCK, 2, SR);

    /* 最前面的 delay 个输出样本是原始输入的预热段（不是静音） */
    const int n = KARAOKE_STFT_DELAY_44100;
    const double r_in  = rms_channel(ref, 0, n, 2, 0);
    const double r_out = rms_channel(out, 0, n, 2, 0);
    CHECK(r_out > r_in * 0.99, "激活首块不得出现静音缺口（预热段必须用当前输入填充）");

    /* 预热段只允许 float32 量化误差，不允许出现台阶/错位 */
    int bad = 0;
    for (int i = 0; i < n; i++) {
        const long dl = (long)out[(size_t)i * 2] - (long)ref[(size_t)i * 2];
        const long dr = (long)out[(size_t)i * 2 + 1] - (long)ref[(size_t)i * 2 + 1];
        if (labs(dl) > 256 || labs(dr) > 256) bad++;
    }
    CHECK(bad == 0, "预热段必须与输入一致（仅允许 float32 量化误差）");

    /* 接缝 [D, 2D)：必须是与 processed 的交叉淡化，而不是把前 D 个样本
     * 原样再放一遍；接缝处也不得出现硬跳变。 */
    int replay = 0;
    for (int i = n; i < 2 * n && i < frames; i++)
        if (out[(size_t)i * 2] == ref[(size_t)(i - n) * 2]) replay++;
    const double nat = max_abs_step(ref, 0, frames);
    const double seam = max_abs_step(out, n - 2, 2 * n + 3);
    fprintf(stderr, "  [info] activation priming: r_in=%.4f r_out=%.4f bad=%d "
                    "seam_replay=%d/%d seam_step=%.3g nat=%.3g\n",
            r_in, r_out, bad, replay, n, seam, nat);
    CHECK(replay < n / 10, "升档接缝必须是交叉淡化，不得逐样本重放前 D 个样本");
    CHECK(seam <= 1.5 * nat, "升档接缝处的单样本跳变不得超过自然步进 1.5 倍");
    free(ref);
    free(out);
}

/* ── 切档时间轴：预热/接缝/稳态/降档旁路（块长 1/1024/4096）───────── */
static void test_switch_timeline(void)
{
    const int D = KARAOKE_STFT_DELAY_44100;
    const int frames = SR * 2;
    static const int sizes[3] = { 1, 1024, 4096 };
    for (int k = 0; k < 3; k++) {
        const int block = sizes[k];
        int32_t *ref = make_side_ramp(frames, 1e-5);
        int32_t *out = make_side_ramp(frames, 1e-5);
        CHECK(ref && out, "切档时间轴测试数据分配成功");
        if (!ref || !out) { free(ref); free(out); continue; }

        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        for (int off = 0; off < frames; off += block) {
            const int n = (frames - off) < block ? (frames - off) : block;
            karaoke_process(out + (size_t)off * 2, n, 2, SR);
        }

        int raw_diff = 0;
        for (int i = 0; i < D; i++)
            if (out[(size_t)i * 2] != ref[(size_t)i * 2]) raw_diff++;
        CHECK(raw_diff == 0, "升档后前 D 个样本必须是 raw（逐位）");

        int replay = 0;
        for (int i = D; i < 2 * D; i++)
            if (out[(size_t)i * 2] == ref[(size_t)(i - D) * 2]) replay++;
        CHECK(replay < D / 10, "升档接缝不得逐样本重放前 D 个样本");

        int steady_bad = 0, steady_max = 0;
        for (int i = 2 * D + 2048; i < frames - 64; i++) {
            const long d = labs((long)out[(size_t)i * 2] - (long)ref[(size_t)(i - D) * 2]);
            if (d > 1024) steady_bad++;   /* float32 OLA 精度上界；实测 ≤65 LSB */
            if (d > steady_max) steady_max = d;
        }
        CHECK(steady_bad == 0, "启用稳态必须满足 out[n] = in[n-D]（误差 ≤1024 LSB）");

        const double nat = max_abs_step(ref, 0, frames);
        const double seam_step = max_abs_step(out, D - 2, 2 * D + 3);
        CHECK(seam_step <= 1.5 * nat, "升档接缝处的单样本跳变不得超过自然步进 1.5 倍");

        /* 降档：1s 处切 OFF，扫描旁路点并检查步进与逐位直通 */
        int32_t *ref2 = make_side_ramp(frames, 1e-5);
        int32_t *out2 = make_side_ramp(frames, 1e-5);
        CHECK(ref2 && out2, "切档时间轴降档数据分配成功");
        if (ref2 && out2) {
            karaoke_init();
            karaoke_set_mode(KARAOKE_MODE_FULL);
            const int off_at = (SR / block) * block;
            for (int off = 0; off < frames; off += block) {
                if (off == off_at) karaoke_set_mode(KARAOKE_MODE_OFF);
                const int n = (frames - off) < block ? (frames - off) : block;
                karaoke_process(out2 + (size_t)off * 2, n, 2, SR);
            }
            const int bypass = find_bypass(out2, ref2, off_at + block, frames);
            CHECK(bypass > 0, "降档收坡后必须最终逐位直通");
            if (bypass > 0) {
                const int ms = (bypass - off_at) * 1000 / SR;
                const int tail = block * 1000 / SR;
                char msg[160];
                /* 实测口径（t8，块长 1/1024/4096）：α 判定点 344/347/371ms
                 * （≈11.5τ，τ=30ms），再晚 D=8.69ms 逐位直通 → 353/356/380ms。 */
                snprintf(msg, sizeof(msg),
                         "降档旁路应在收坡后约 11.5τ≈345ms 判旁路、再晚 D=8.69ms "
                         "逐位直通（实测 %dms，块长 %d）", ms, block);
                CHECK(ms >= 300 && ms <= 420 + tail, msg);
                CHECK(memcmp(out2 + (size_t)(bypass + 1024) * 2,
                             ref2 + (size_t)(bypass + 1024) * 2,
                             (size_t)BLOCK * 2 * sizeof(int32_t)) == 0,
                      "旁路后输出必须逐位直通");
                const double nat2 = max_abs_step(ref2, 0, frames);
                const double near = max_abs_step(out2, bypass - 2 * D, bypass + 2 * D);
                /* 斜坡上 D 样本内抹掉 D 样本时间偏移，斜率下界是 2×（线性
                 * 交叉淡化最优）；音乐/单音素材见 test_bypass_click 的 1.5×。 */
                CHECK(near <= 2.1 * nat2,
                      "降档旁路邻域的最大单样本跳变不得超过自然步进 2.1 倍（斜坡数学下界 2×）");
                fprintf(stderr, "  [info] timeline block=%4d: raw_diff=%d replay=%d/%d "
                                "steady_bad=%d max=%dLSB seam_step=%.2fx off@%d bypass=+%dms "
                                "near_step=%.2fx\n",
                        block, raw_diff, replay, D, steady_bad, steady_max,
                        seam_step / nat, off_at, ms, near / nat2);
            }
            free(ref2);
            free(out2);
        }
        free(ref);
        free(out);
    }
}

/* ── 降档旁路 tick / 升档 flam：可闻性判据（t4 F1、F2）────────────── */
static void test_bypass_click(void)
{
    const int D = KARAOKE_STFT_DELAY_44100;
    const int frames = SR * 3;

    /* 降档：880Hz/0.5 纯 side 单音（mid=0），自然最大步进 0.0627FS。
     * 修复前实测旁路处 0.8426FS = 13.4×；交叉淡化后应 ≤1.5×。 */
    int32_t *ref = make_side_tone(frames, 880.0, 0.5);
    int32_t *out = make_side_tone(frames, 880.0, 0.5);
    CHECK(ref && out, "旁路跳变测试数据分配成功");
    if (ref && out) {
        const int off_at = (2 * SR / BLOCK) * BLOCK;
        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        for (int off = 0; off < frames; off += BLOCK) {
            if (off == off_at) karaoke_set_mode(KARAOKE_MODE_OFF);
            karaoke_process(out + (size_t)off * 2, BLOCK, 2, SR);
        }
        const int bypass = find_bypass(out, ref, off_at + BLOCK, frames);
        CHECK(bypass > 0, "降档后必须最终逐位直通");
        if (bypass > 0) {
            const double nat = max_abs_step(ref, 0, frames);
            const double near = max_abs_step(out, bypass - 2 * D, bypass + 2 * D);
            fprintf(stderr, "  [info] bypass click: off@%d bypass=+%dms step=%.4fFS nat=%.4fFS "
                            "ratio=%.2fx\n", off_at, (bypass - off_at) * 1000 / SR, near, nat,
                    near / nat);
            CHECK(near <= 1.5 * nat,
                  "降档旁路处最大单样本跳变不得超过该素材自然最大步进的 1.5 倍");
            CHECK(memcmp(out + (size_t)(bypass + 1024) * 2, ref + (size_t)(bypass + 1024) * 2,
                         (size_t)BLOCK * 2 * sizeof(int32_t)) == 0,
                  "旁路后必须逐位直通（memcmp）");
        }
        free(ref);
        free(out);
    }

    /* 升档：2kHz/0.5ms 起振的 side 鼓击正好放在升档点。
     * 修复前 [0,D) 与 [D,2D) 逐样本相同（383/383，峰值都 0.7885FS）；
     * 修复后接缝是交叉淡化：0 个样本相同、重放峰值被压低。 */
    int32_t *hit_ref = make_side_hit(SR * 2, 2000.0, 0.8);
    int32_t *hit_out = make_side_hit(SR * 2, 2000.0, 0.8);
    CHECK(hit_ref && hit_out, "升档接缝测试数据分配成功");
    if (hit_ref && hit_out) {
        karaoke_init();
        karaoke_set_mode(KARAOKE_MODE_FULL);
        run_blocks_n(hit_out, SR * 2, 2, SR);
        int same = 0;
        for (int i = 0; i < D; i++)
            if (hit_out[(size_t)i * 2] == hit_out[(size_t)(i + D) * 2]) same++;
        double p0 = 0.0, p1 = 0.0;
        for (int i = 0; i < D; i++) {
            double a = fabs((double)hit_out[(size_t)i * 2]) / FULL_SCALE;
            if (a > p0) p0 = a;
        }
        for (int i = D; i < 2 * D; i++) {
            double a = fabs((double)hit_out[(size_t)i * 2]) / FULL_SCALE;
            if (a > p1) p1 = a;
        }
        const double ratio_db = 20.0 * log10((p1 > 1e-9 ? p1 : 1e-9) / (p0 > 1e-9 ? p0 : 1e-9));
        const double nat = max_abs_step(hit_ref, 0, SR * 2);
        const double seam = max_abs_step(hit_out, 1, 2 * D + 2);
        fprintf(stderr, "  [info] upshift flam: identical=%d/%d peak raw=%.4fFS seam=%.4fFS "
                        "(%.2fdB) seam_step=%.2fx\n", same, D, p0, p1, ratio_db, seam / nat);
        CHECK(same < D / 20, "升档接缝不得把前 D 个样本原样重放（flam，t4 F2）");
        CHECK(ratio_db <= -1.5, "接缝重放峰值必须被交叉淡化压低至少 1.5dB");
        CHECK(seam <= 1.5 * nat, "升档接缝处的单样本跳变不得超过自然步进 1.5 倍");
        free(hit_ref);
        free(hit_out);
    }
}

/* ── 启用档互切（soft↔full）：不得重置流/重放/跳变（t7 F1）──────── */
static void test_active_mode_switch(void)
{
    const int D = KARAOKE_STFT_DELAY_44100;

    /* 1) 纯 side 斜坡（mid=0 → 门恒 0，处理流原样延时）：时间轴必须全程保持 D */
    {
        const int frames = SR * 3;
        int32_t *ref = make_side_ramp(frames, 5e-6);   /* 3s 末尾 0.66FS，避免越界 */
        int32_t *out = make_side_ramp(frames, 5e-6);
        CHECK(ref && out, "启用档互切测试数据分配成功");
        if (ref && out) {
            const int off1 = (SR / BLOCK) * BLOCK;       /* 1s：full→soft */
            const int off2 = (2 * SR / BLOCK) * BLOCK;   /* 2s：soft→full */
            karaoke_init();
            karaoke_set_mode(KARAOKE_MODE_FULL);
            for (int off = 0; off < frames; off += BLOCK) {
                if (off == off1) karaoke_set_mode(KARAOKE_MODE_SOFT);
                if (off == off2) karaoke_set_mode(KARAOKE_MODE_FULL);
                const int n = (frames - off) < BLOCK ? (frames - off) : BLOCK;
                karaoke_process(out + (size_t)off * 2, n, 2, SR);
            }
            int lag_bad = 0;
            for (int i = 2 * D + 2048; i < frames - 64; i += 97)
                if (labs((long)out[(size_t)i * 2] - (long)ref[(size_t)(i - D) * 2]) > 1024)
                    lag_bad++;
            CHECK(lag_bad == 0, "soft↔full 互切不得改变时间轴（out[n] 必须仍是 in[n-D]）");

            const double nat = max_abs_step(ref, 0, frames);
            const double s1 = max_abs_step(out, off1 - 2, off1 + 2 * D + 2);
            const double s2 = max_abs_step(out, off2 - 2, off2 + 2 * D + 2);
            CHECK(s1 <= 1.5 * nat && s2 <= 1.5 * nat,
                  "soft↔full 切档邻域的单样本跳变不得超过自然步进 1.5 倍");

            int replay = 0;
            for (int i = 0; i < D; i++) {
                if (out[(size_t)(off1 + i) * 2] == ref[(size_t)(off1 + i) * 2]) replay++;
                if (out[(size_t)(off2 + i) * 2] == ref[(size_t)(off2 + i) * 2]) replay++;
            }
            CHECK(replay < D / 5, "soft↔full 互切不得重放前 D 个样本（不得重置流）");
            fprintf(stderr, "  [info] active-switch side ramp: lag_bad=%d step=%.2fx/%.2fx "
                            "raw_replay=%d/%d\n", lag_bad, s1 / nat, s2 / nat, replay, 2 * D);
            free(ref);
            free(out);
        }
    }

    /* 2) 居中人声 800Hz：full→soft 不得在切档瞬间把抵消放掉（不得回到 0dB），
     *    soft→full 应沿 30ms τ 斜坡加深；两个方向都不得出现 raw 爆发。 */
    {
        const int frames = SR * 2;
        int32_t *ref = make_tone(800.0, 1, 0.10, frames, 2);
        int32_t *out = make_tone(800.0, 1, 0.10, frames, 2);
        CHECK(ref && out, "启用档互切人声测试数据分配成功");
        if (ref && out) {
            const int off1 = (SR / 2 / BLOCK) * BLOCK;   /* 0.5s：full→soft */
            const int off2 = (SR / BLOCK) * BLOCK;       /* 1.0s：soft→full */
            karaoke_init();
            karaoke_set_mode(KARAOKE_MODE_FULL);
            for (int off = 0; off < frames; off += BLOCK) {
                if (off == off1) karaoke_set_mode(KARAOKE_MODE_SOFT);
                if (off == off2) karaoke_set_mode(KARAOKE_MODE_FULL);
                const int n = (frames - off) < BLOCK ? (frames - off) : BLOCK;
                karaoke_process(out + (size_t)off * 2, n, 2, SR);
            }
            const int win = 44;
            double g_full = -999.0, g_soft = -999.0, g_full2 = -999.0;
            double max_after1 = -999.0, max_after2 = -999.0;
            for (int pos = (int)(0.30 * SR); pos + win <= frames; pos += win) {
                const double a_in  = demod_mid_at(ref, pos, win, 800.0);
                const double a_out = demod_mid_at(out, pos, win, 800.0);
                const double db = 20.0 * log10((a_out > 1e-12 ? a_out : 1e-12) /
                                               (a_in  > 1e-12 ? a_in  : 1e-12));
                if (pos < off1 && pos >= off1 - 200 * win) { if (db > g_full) g_full = db; }
                if (pos >= off1 && pos < off1 + 30 * win)  { if (db > max_after1) max_after1 = db; }
                if (pos >= off1 + 200 * win && pos < off2) { if (db > g_soft) g_soft = db; }
                if (pos >= off2 && pos < off2 + 30 * win)  { if (db > max_after2) max_after2 = db; }
                if (pos >= off2 + 200 * win)               { if (db > g_full2) g_full2 = db; }
            }
            fprintf(stderr,
                    "  [info] active-switch vocal: full=%.2fdB soft=%.2fdB "
                    "after_full2soft=%.2fdB after_soft2full=%.2fdB full_again=%.2fdB\n",
                    g_full, g_soft, max_after1, max_after2, g_full2);
            /* 切档瞬间不得回到"完全没消"：修复前会 0.00dB */
            CHECK(max_after1 <= -3.0,
                  "full→soft 后 30ms 内人声不得回到 0dB（不得重置流、不得 raw 爆发）");
            CHECK(max_after2 <= -3.0,
                  "soft→full 后 30ms 内人声不得回到 0dB（不得重置流、不得 raw 爆发）");
            /* 斜坡收敛：soft 残留约 -8dB，full 恢复满额抵消 */
            CHECK(g_soft <= -6.0 && g_soft >= -12.0,
                  "full→soft 应收敛到 soft 档的 -8dB 左右（30ms τ 斜坡）");
            CHECK(g_full2 <= -30.0, "soft→full 应收敛回满额抵消（沿 30ms τ 斜坡加深）");
            CHECK(g_full <= -30.0, "切档前 full 档本身应是满额抵消");
            free(ref);
            free(out);
        }
    }
}

/* ── 频带过渡带：实测值固化（t4 F4）────────────────────────────── */
static void test_band_transition(void)
{
    /* 频带边界用频点选择实现，边界附近存在约 ±230Hz 的过渡带。下表把
     * 过渡带/带外的实测衰减固化成回归：带外（100Hz/5.2k/5.5k/10k）必须
     * 基本保留；带内（4.8k）必须深度抵消；边界点（200/5.0k）允许部分抵消。 */
    struct { double f, lo, hi; const char *msg; } cases[] = {
        {   100.0, -1.0,   0.2, "100Hz 带外居中能量必须基本保留（实测 -0.25dB）" },
        {   150.0, -3.0,   0.2, "150Hz 已进入过渡带（实测 -1.42dB）" },
        {   200.0, -8.0,  -2.0, "200Hz 带边界落在过渡带内（实测 -4.52dB）" },
        {  4800.0, -120.0, -20.0, "4.8kHz 带内必须深度抵消（实测 -68.7dB）" },
        {  5000.0, -20.0,  -5.0, "5.0kHz 带边界落在过渡带内（实测 -11.3dB）" },
        {  5200.0, -0.5,   0.2, "5.2kHz 已在带外，必须基本保留（实测 -0.015dB）" },
        {  5500.0, -0.2,   0.2, "5.5kHz 带外必须保留（实测 0.000dB）" },
        { 10000.0, -0.2,   0.2, "10kHz 带外必须保留（实测 0.000dB）" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int dummy = 0;
        const double g = run_gain_db(KARAOKE_MODE_FULL, cases[c].f, 1, 2, SR, &dummy);
        CHECK(g >= cases[c].lo && g <= cases[c].hi, cases[c].msg);
        fprintf(stderr, "  [info] band transition %.0fHz: %+.3fdB\n", cases[c].f, g);
    }
}

/* ── 处理耗时护栏：2s 44.1kHz 立体声必须远低于实时 ─────────────── */
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void test_processing_budget(void)
{
    const int frames = SR * 2;
    int32_t *buf = make_scene_steady_wide(frames, 0.10, 0.22);
    CHECK(buf, "耗时测试数据分配成功");
    if (!buf) return;

    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);
    karaoke_process(buf, BLOCK, 2, SR);          /* 预热：建表/分配/预热窗 */
    const double t0 = now_ms();
    for (int off = BLOCK; off < frames; off += BLOCK) {
        const int n = (frames - off) < BLOCK ? (frames - off) : BLOCK;
        karaoke_process(buf + (size_t)off * 2, n, 2, SR);
    }
    const double ms = now_ms() - t0;
    fprintf(stderr, "  [info] processing 2s/44.1k stereo: %.2fms (rtf=%.4f)\n",
            ms, ms / 2000.0);
    /* 本用例在单测构建（-O0、无优化）下运行，实测约 79ms；验收口径是
     * -O3/Release 构建（t6 报告实测 15.9ms，RTF 0.008），单测里无法保证
     * 优化级别，所以 200ms（RTF 0.1）只是**回归护栏**、不是验收值。 */
    CHECK(ms < 200.0, "2s/44.1k 立体声处理耗时回归护栏（200ms；验收值见 -O3 报告）");
    free(buf);
}

/* ── 档位取值与非法值防御 ─────────────────────────────────────── */
static void test_mode_accessors(void)
{
    karaoke_init();
    CHECK(karaoke_get_mode() == KARAOKE_MODE_OFF, "初值为不启用");

    karaoke_set_mode(KARAOKE_MODE_FULL);
    CHECK(karaoke_get_mode() == KARAOKE_MODE_FULL, "设置 full 生效");
    CHECK(karaoke_alpha_for_mode(KARAOKE_MODE_FULL) > 0.99f, "full 档 α≈1");
    CHECK(fabs(karaoke_alpha_for_mode(KARAOKE_MODE_SOFT) - KARAOKE_ALPHA_SOFT) < 1e-6f,
          "soft 档 α 取常量");
    CHECK(karaoke_alpha_for_mode(KARAOKE_MODE_OFF) == 0.0f, "不启用档 α=0");

    karaoke_set_mode(999);
    CHECK(karaoke_get_mode() == KARAOKE_MODE_OFF, "越界档位退回不启用");
    karaoke_set_mode(-1);
    CHECK(karaoke_get_mode() == KARAOKE_MODE_OFF, "负数档位退回不启用");
    karaoke_set_mode(KARAOKE_MODE_COUNT);
    CHECK(karaoke_get_mode() == KARAOKE_MODE_OFF, "等于 COUNT 的档位退回不启用");
}

/* ── 采样率切换：系数重算后仍能工作 ───────────────────────────── */
static void test_sample_rate_change(void)
{
    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);

    int32_t *buf44 = make_tone(1000.0, 1, 0.8, SR, 2);
    /* 22050Hz 下 1 秒（帧数 22050，交织 2 声道） */
    int32_t *buf22 = calloc((size_t)22050 * 2, sizeof(int32_t));
    CHECK(buf44 && buf22, "变采样率测试数据分配成功");
    if (buf22) {
        for (int i = 0; i < 22050; i++) {
            double s = 0.8 * sin(2.0 * M_PI * 1000.0 * (double)i / 22050.0);
            int32_t v = (int32_t)(s * FULL_SCALE);
            buf22[(size_t)i * 2] = v;
            buf22[(size_t)i * 2 + 1] = v;
        }
    }

    for (int off = 0; off < SR;) {
        int n = (SR - off) < BLOCK ? (SR - off) : BLOCK;
        karaoke_process(buf44 + (size_t)off * 2, n, 2, SR);
        off += n;
    }
    double r44 = rms_channel(buf44, SR / 2, SR, 2, 0);
    /* 换采样率：内部必须重算系数，且输出仍是有限值、仍在削弱 */
    for (int off = 0; off < 22050;) {
        int n = (22050 - off) < BLOCK ? (22050 - off) : BLOCK;
        karaoke_process(buf22 + (size_t)off * 2, n, 2, 22050);
        off += n;
    }
    double r22 = rms_channel(buf22, 11000, 20000, 2, 0);

    CHECK(isfinite(r44) && isfinite(r22), "切换采样率后输出有限");
    /* 阈值为"削弱 >= 约31dB"（残差 0.005 相对 0.566 的直通 RMS）：
     * 频带边界 200/5000 在 1kHz 处的 skirt 泄漏约 -49dB 是设计预期，
     * 此处校验的是重算系数后功能仍在起作用，不是抵消深度（深度由
     * test_cancellation 以 <= -30dB 把关）。 */
    CHECK(r44 < 0.005 && r22 < 0.005, "两个采样率下 1kHz 正中音都被显著削弱");

    free(buf44);
    free(buf22);
}

/* ── 档位切换是平滑逼近，不是硬台阶 ───────────────────────────── */
static void test_alpha_ramp(void)
{
    int louder = 0;
    run_gain_db(KARAOKE_MODE_FULL, 1000.0, 1, 2, SR, &louder);
    CHECK(louder, "首个处理块的响度必须高于稳态（说明 α 在爬升而非台阶）");
}

/* ── reset：清历史，重新起播不残留 ────────────────────────────── */
static void test_reset_restarts_ramp(void)
{
    karaoke_init();
    karaoke_set_mode(KARAOKE_MODE_FULL);

    int32_t *buf = make_tone(1000.0, 1, 0.8, TOTAL_FRAMES, 2);
    CHECK(buf, "reset 测试数据分配成功");
    if (!buf) return;

    /* 先跑到稳态 */
    for (int off = 0; off + BLOCK <= TOTAL_FRAMES; off += BLOCK)
        karaoke_process(buf + (size_t)off * 2, BLOCK, 2, SR);
    double steady = rms_channel(buf, TOTAL_FRAMES - BLOCK, TOTAL_FRAMES - 1, 2, 0);

    /* reset 后同一输入的首块应明显更响（α 从头爬升） */
    karaoke_reset();
    int32_t *again = make_tone(1000.0, 1, 0.8, BLOCK, 2);
    CHECK(again, "reset 后二次运行数据分配成功");
    if (again) {
        karaoke_process(again, BLOCK, 2, SR);
        double first = rms_channel(again, 0, BLOCK - 1, 2, 0);
        CHECK(first > steady * 1.3, "karaoke_reset 之后必须重新平滑起振");
    }
    free(buf);
    free(again);
}

int main(void)
{
    test_mode_accessors();
    test_passthrough();
    test_cancellation();
    test_band_edges_preserved();
    test_side_preserved();
    test_wide_accompaniment_preserved();
    test_gate_keeps_vocal_cancellation();
    test_off_ramps_out();
    test_sample_rate_change();
    test_alpha_ramp();
    test_reset_restarts_ramp();
    test_transient_leak();
    test_steady_wide_cancellation();
    test_accompaniment_protection();
    test_latency_and_bypass();
    test_output_robustness();
    test_block_invariance();
    test_reset_equivalence();
    test_activation_priming();
    test_switch_timeline();
    test_bypass_click();
    test_active_mode_switch();
    test_band_transition();
    test_center_phase_coherence();
    test_sample_rate_roundtrip();
    test_processing_budget();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS test_karaoke\n");
    return 0;
}
