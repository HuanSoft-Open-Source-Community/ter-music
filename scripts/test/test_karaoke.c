/**
 * @file test_karaoke.c
 * @brief 卡拉OK（消人声）DSP 的数值单测
 *
 * 覆盖：频带内居中内容的抵消量（full/soft 两档）、频带外（低频/高频）
 * 居中能量的保留、声像两侧（side）内容的原样保留、单声道与"不启用"
 * 的逐位直通、非法档位的防御、采样率切换后的系数重算，以及档位切换
 * 的 30ms 平滑逼近（不是硬台阶）。
 */

#include "audio/karaoke.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    CHECK(r44 < 0.001 && r22 < 0.001, "两个采样率下 1kHz 正中音都被显著削弱");

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
    test_sample_rate_change();
    test_alpha_ramp();
    test_reset_restarts_ramp();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS test_karaoke\n");
    return 0;
}
