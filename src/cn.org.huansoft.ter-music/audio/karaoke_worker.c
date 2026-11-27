/**
 * @file karaoke_worker.c
 * @brief 可选 ONNX 模型消人声 — 后台分离 worker（单线程 + 单 job）
 *
 * 一次 job 的流水线（全部流式，内存不随曲目长度线性增长）：
 *
 *   FFmpeg 解码（interrupt callback 支持取消）
 *     → swresample 到 44.1kHz/2ch/float（模型输入域）
 *     → 按 chunk（hop×(dim_t−1)=261120 帧，step=(1−0.25)×chunk）做
 *       STFT → karaoke_model_infer_patch() → iSTFT → 对称 Hann 交叉淡化
 *     → 每个输出样本同时派生 inst 与 soft = 0.4×mix + 0.6×inst
 *     → swresample 回原曲采样率 → 两个 16bit FLAC 编码器
 *     → 原子提交成对缓存（karaoke_cache_commit）
 *
 * DSP 与 audio/karaoke_model.c 的同名实现同源（周期 Hann、reflect pad、
 * Σw² 归一、bins 0..2 清零、dim_f 以上补零）。这里独立成流式版本的原因：
 * t4 的 karaoke_model_separate() 固定 Default overlap 且一次性分配整曲缓冲，
 * 而本任务要求固定 overlap 0.25、>30min 拒绝与可取消的 chunk 粒度。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#include "audio/karaoke_worker.h"

#include "audio/karaoke_cache.h"
#include "audio/karaoke_mode.h"
#include "audio/karaoke_model.h"
#include "i18n/i18n.h"
#include "logger/logger.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WORKER_ERR_CAP KARAOKE_WORKER_ERR_CAP
#define WORKER_EMIT_BLOCK 4096   /* 每块输出样本（交错帧）——编码/取消检查粒度 */

/* ================================================================
 * 1. 小工具
 * ================================================================ */

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* ================================================================
 * 2. FFT / 窗口（与 karaoke_model.c 同源；N = 3×M，M 为 2 的幂）
 * ================================================================ */

typedef struct {
    float re, im;
} WCplx;

typedef struct {
    int n, m;
    float *cos_n, *sin_n, *cos_m, *sin_m;
    int *bitrev;
    WCplx *sub, *full, *wn;
    WCplx w3[3][3], w3_inv[3][3];
} WFft;

static int wfft_init(WFft *p, int n_fft, char *err, size_t err_cap)
{
    memset(p, 0, sizeof(*p));
    if (n_fft <= 0 || (n_fft % 3) != 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.fft_unsupported_fmt"), n_fft);
        return -1;
    }
    int m = n_fft / 3;
    if (m < 2 || (m & (m - 1)) != 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.fft_pow2_fmt"), n_fft);
        return -1;
    }
    p->n = n_fft;
    p->m = m;
    p->cos_n = malloc(sizeof(float) * (size_t)(n_fft / 2));
    p->sin_n = malloc(sizeof(float) * (size_t)(n_fft / 2));
    p->cos_m = malloc(sizeof(float) * (size_t)(m / 2));
    p->sin_m = malloc(sizeof(float) * (size_t)(m / 2));
    p->bitrev = malloc(sizeof(int) * (size_t)m);
    p->sub = calloc((size_t)m * 3, sizeof(WCplx));
    p->full = calloc((size_t)n_fft, sizeof(WCplx));
    p->wn = calloc((size_t)m * 2, sizeof(WCplx));
    if (!p->cos_n || !p->sin_n || !p->cos_m || !p->sin_m || !p->bitrev ||
        !p->sub || !p->full || !p->wn) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "FFT table");
        return -1;
    }
    for (int j = 0; j < n_fft / 2; j++) {
        p->cos_n[j] = (float)cos(2.0 * M_PI * (double)j / (double)n_fft);
        p->sin_n[j] = (float)sin(2.0 * M_PI * (double)j / (double)n_fft);
    }
    for (int j = 0; j < m / 2; j++) {
        p->cos_m[j] = (float)cos(2.0 * M_PI * (double)j / (double)m);
        p->sin_m[j] = (float)sin(2.0 * M_PI * (double)j / (double)m);
    }
    for (int j = 0; j < m; j++) {
        p->wn[j].re = (float)cos(2.0 * M_PI * (double)j / (double)n_fft);
        p->wn[j].im = (float)-sin(2.0 * M_PI * (double)j / (double)n_fft);
        p->wn[m + j].re = (float)cos(4.0 * M_PI * (double)j / (double)n_fft);
        p->wn[m + j].im = (float)-sin(4.0 * M_PI * (double)j / (double)n_fft);
    }
    for (int n1 = 0; n1 < 3; n1++) {
        for (int mm = 0; mm < 3; mm++) {
            double ang = -2.0 * M_PI * (double)(n1 * mm) / 3.0;
            p->w3[n1][mm].re = (float)cos(ang);
            p->w3[n1][mm].im = (float)sin(ang);
            p->w3_inv[n1][mm].re = (float)cos(-ang);
            p->w3_inv[n1][mm].im = (float)sin(-ang);
        }
    }
    int bits = 0;
    while ((1 << bits) < m)
        bits++;
    for (int i = 0; i < m; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            r |= ((i >> b) & 1) << (bits - 1 - b);
        p->bitrev[i] = r;
    }
    return 0;
}

static void wfft_free(WFft *p)
{
    if (!p)
        return;
    free(p->cos_n);
    free(p->sin_n);
    free(p->cos_m);
    free(p->sin_m);
    free(p->bitrev);
    free(p->sub);
    free(p->full);
    free(p->wn);
    memset(p, 0, sizeof(*p));
}

static void wfft_radix2(const WFft *p, WCplx *a, int inverse)
{
    const int n = p->m;
    for (int i = 0; i < n; i++) {
        int j = p->bitrev[i];
        if (j > i) {
            WCplx t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        for (int base = 0; base < n; base += len) {
            for (int j = 0; j < half; j++) {
                const float wr = p->cos_m[j * step];
                const float wi = inverse ? p->sin_m[j * step] : -p->sin_m[j * step];
                const WCplx u = a[base + j];
                const WCplx v = a[base + j + half];
                const float vr = v.re * wr - v.im * wi;
                const float vi = v.re * wi + v.im * wr;
                a[base + j].re = u.re + vr;
                a[base + j].im = u.im + vi;
                a[base + j + half].re = u.re - vr;
                a[base + j + half].im = u.im - vi;
            }
        }
    }
}

static void wfft_forward(const WFft *p, const float *x, WCplx *out_bins, int dim_f)
{
    const int m = p->m;
    const int n = p->n;
    for (int n1 = 0; n1 < 3; n1++) {
        WCplx *a = p->sub + (size_t)n1 * (size_t)m;
        for (int n2 = 0; n2 < m; n2++) {
            a[n2].re = x[(size_t)n1 + (size_t)3 * (size_t)n2];
            a[n2].im = 0.0f;
        }
        wfft_radix2(p, a, 0);
    }
    for (int k = 0; k < dim_f && k < n; k++) {
        const int kp = k % m;
        const int mm = k / m;
        float re = 0.0f, im = 0.0f;
        for (int n1 = 0; n1 < 3; n1++) {
            const WCplx y = p->sub[(size_t)n1 * (size_t)m + (size_t)kp];
            if (n1 == 0) {
                re += y.re;
                im += y.im;
                continue;
            }
            const WCplx tw = p->wn[(size_t)(n1 - 1) * (size_t)m + (size_t)kp];
            const WCplx w3v = p->w3[n1][mm];
            const float tr = w3v.re * tw.re - w3v.im * tw.im;
            const float ti = w3v.re * tw.im + w3v.im * tw.re;
            re += tr * y.re - ti * y.im;
            im += tr * y.im + ti * y.re;
        }
        out_bins[k].re = re;
        out_bins[k].im = im;
    }
}

static void wfft_inverse(WFft *p, const WCplx *bins, int dim_f, float *out_x)
{
    const int m = p->m;
    const int n = p->n;
    const int half = n / 2;
    WCplx *full = p->full;
    for (int k = 0; k <= half; k++) {
        WCplx v = {0.0f, 0.0f};
        if (k < dim_f)
            v = bins[k];
        if (k == 0 || k == half)
            v.im = 0.0f;
        full[k] = v;
    }
    for (int k = half + 1; k < n; k++) {
        const WCplx v = full[n - k];
        full[k].re = v.re;
        full[k].im = -v.im;
    }
    for (int kp = 0; kp < m; kp++) {
        const WCplx x0 = full[kp];
        const WCplx x1 = full[m + kp];
        const WCplx x2 = full[2 * m + kp];
        for (int n1 = 0; n1 < 3; n1++) {
            float zr = 0.0f, zi = 0.0f;
            const WCplx *w = p->w3_inv[n1];
            zr = w[0].re * x0.re - w[0].im * x0.im;
            zi = w[0].re * x0.im + w[0].im * x0.re;
            zr += w[1].re * x1.re - w[1].im * x1.im;
            zi += w[1].re * x1.im + w[1].im * x1.re;
            zr += w[2].re * x2.re - w[2].im * x2.im;
            zi += w[2].re * x2.im + w[2].im * x2.re;
            if (n1 != 0) {
                const WCplx tw = p->wn[(size_t)(n1 - 1) * (size_t)m + (size_t)kp];
                const float nr = zr * tw.re + zi * tw.im;
                const float ni = zi * tw.re - zr * tw.im;
                zr = nr;
                zi = ni;
            }
            p->sub[(size_t)n1 * (size_t)m + (size_t)kp].re = zr;
            p->sub[(size_t)n1 * (size_t)m + (size_t)kp].im = zi;
        }
    }
    for (int n1 = 0; n1 < 3; n1++)
        wfft_radix2(p, p->sub + (size_t)n1 * (size_t)m, 1);
    const float inv_n = 1.0f / (float)n;
    for (int n1 = 0; n1 < 3; n1++) {
        const WCplx *a = p->sub + (size_t)n1 * (size_t)m;
        for (int n2 = 0; n2 < m; n2++)
            out_x[(size_t)n1 + (size_t)3 * (size_t)n2] = a[n2].re * inv_n;
    }
}

static float period_hann(int n, int n_fft)
{
    return (float)(0.5 - 0.5 * cos(2.0 * M_PI * (double)n / (double)n_fft));
}

static float sym_hann(size_t i, size_t n)
{
    if (n <= 1)
        return 1.0f;
    return (float)(0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(n - 1)));
}

/* ================================================================
 * 3. 流式 DSP 上下文
 * ================================================================ */

typedef struct {
    const KaraokeModelSpec *spec;
    int n_fft, hop, dim_f, dim_t, trim, chunk, step;
    WFft fft;
    float *window;
    float *frame;
    WCplx *bins;
    float *part;          /* 2 × chunk（按声道平面） */
    float *ola;           /* 2 × (chunk + n_fft) */
    float *wsum;          /* chunk + n_fft */
    int ring_size;                     /* chunk + step：未发射窗口的最大跨度 */
    float *ring_l, *ring_r, *ring_d;   /* ring_size：跨 chunk 的 OLA 累加 */
    float *in_patch, *out_patch;       /* 模型输入/输出 tensor（8MB each） */
    float *emit_inst, *emit_soft;      /* WORKER_EMIT_BLOCK × 2 */
} Wdsp;

static void wdsp_free(Wdsp *d)
{
    if (!d)
        return;
    wfft_free(&d->fft);
    free(d->window);
    free(d->frame);
    free(d->bins);
    free(d->part);
    free(d->ola);
    free(d->wsum);
    free(d->ring_l);
    free(d->ring_r);
    free(d->ring_d);
    free(d->in_patch);
    free(d->out_patch);
    free(d->emit_inst);
    free(d->emit_soft);
    memset(d, 0, sizeof(*d));
}

static int wdsp_init(Wdsp *d, const KaraokeModelSpec *spec, char *err, size_t err_cap)
{
    memset(d, 0, sizeof(*d));
    d->spec = spec;
    d->n_fft = spec->n_fft;
    d->hop = spec->hop;
    d->dim_f = spec->dim_f;
    d->dim_t = spec->dim_t;
    d->trim = spec->n_fft / 2;
    d->chunk = (int)karaoke_model_chunk_frames(spec);
    d->step = (int)((double)d->chunk * (1.0 - KARAOKE_CACHE_OVERLAP));
    if (d->chunk <= 0 || d->step <= 0 || d->step >= d->chunk) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.model_table_invalid"));
        return -1;
    }
    if (wfft_init(&d->fft, d->n_fft, err, err_cap) != 0)
        return -1;
    size_t cyl = (size_t)d->chunk + (size_t)d->n_fft;
    d->window = calloc((size_t)d->n_fft, sizeof(float));
    d->frame = calloc((size_t)d->n_fft, sizeof(float));
    d->bins = calloc((size_t)d->dim_f, sizeof(WCplx));
    d->part = calloc((size_t)d->chunk * 2, sizeof(float));
    d->ola = calloc(cyl * 2, sizeof(float));
    d->wsum = calloc(cyl, sizeof(float));
    d->ring_size = d->chunk + d->step;
    d->ring_l = calloc((size_t)d->ring_size, sizeof(float));
    d->ring_r = calloc((size_t)d->ring_size, sizeof(float));
    d->ring_d = calloc((size_t)d->ring_size, sizeof(float));
    d->emit_inst = calloc(WORKER_EMIT_BLOCK * 2, sizeof(float));
    d->emit_soft = calloc(WORKER_EMIT_BLOCK * 2, sizeof(float));
    size_t patch = karaoke_model_patch_elems(spec);
    d->in_patch = calloc(patch, sizeof(float));
    d->out_patch = calloc(patch, sizeof(float));
    if (!d->window || !d->frame || !d->bins || !d->part || !d->ola || !d->wsum ||
        !d->ring_l || !d->ring_r || !d->ring_d || !d->in_patch || !d->out_patch ||
        !d->emit_inst || !d->emit_soft) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "DSP scratch");
        wdsp_free(d);
        return -1;
    }
    for (int i = 0; i < d->n_fft; i++)
        d->window[i] = period_hann(i, d->n_fft);
    return 0;
}

static inline size_t reflect_index(size_t x, size_t chunk, size_t trim)
{
    if (x < trim)
        return trim - x;
    if (x < trim + chunk)
        return x - trim;
    return chunk - 2 - (x - trim - chunk);
}

/* 把 chunk（从全局 mixture 位置 s 起）搬进 d->part：part 是 mixture 的
 * [s, s+chunk) 片段；pcm 里只有 mix44（不含两端补零）。 */
typedef struct {
    float *data;
    size_t cap_frames;
    long long start;      /* data[0] 对应的 44.1k 绝对帧号（mixture 坐标需减 trim） */
    size_t frames;
} PcmQueue;

static void pcm_free(PcmQueue *q)
{
    free(q->data);
    memset(q, 0, sizeof(*q));
}

/* 丢弃绝对位置 < abs 的样本，保留其后数据。 */
static void pcm_drop_before(PcmQueue *q, long long abs)
{
    if (abs <= q->start)
        return;
    if ((unsigned long long)abs >= (unsigned long long)(q->start + (long long)q->frames)) {
        q->start = abs;
        q->frames = 0;
        return;
    }
    size_t drop = (size_t)(abs - q->start);
    size_t left = q->frames - drop;
    memmove(q->data, q->data + drop * 2, left * 2 * sizeof(float));
    q->start = abs;
    q->frames = left;
}

/* ================================================================
 * 4. 解码器
 * ================================================================ */

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext *codec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int stream_index;
    int src_rate;
    int src_channels;
    int eof;
    int sent_null;
    long long src_frames;    /* 原采样率下已解码帧数 */
    int reanchor;            /* seek 后首帧：用其 pts 校正 pcm.start 的绝对位置 */
    long long expect_src;    /* 期望的绝对起始帧（无 pts 时使用） */
    AVRational tb;           /* 流时间基（pts → 44.1k 帧号） */
    PcmQueue pcm;            /* 44.1kHz 立体声 float 交织 */
} WDecoder;

static void decoder_close(WDecoder *d)
{
    if (!d)
        return;
    if (d->pkt)
        av_packet_free(&d->pkt);
    if (d->frame)
        av_frame_free(&d->frame);
    if (d->swr)
        swr_free(&d->swr);
    if (d->codec)
        avcodec_free_context(&d->codec);
    if (d->fmt)
        avformat_close_input(&d->fmt);
    pcm_free(&d->pcm);
    memset(d, 0, sizeof(*d));
}

static int decode_interrupt_cb(void *opaque)
{
    volatile int *cancelled = (volatile int *)opaque;
    return cancelled && *cancelled ? 1 : 0;
}

static int decoder_open(WDecoder *d, const char *path, volatile int *cancelled,
                        char *err, size_t err_cap)
{
    memset(d, 0, sizeof(*d));
    AVIOInterruptCB icb;
    icb.callback = decode_interrupt_cb;
    icb.opaque = (void *)cancelled;

    d->fmt = avformat_alloc_context();
    if (!d->fmt) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "AVFormatContext");
        return -1;
    }
    d->fmt->interrupt_callback = icb;
    int rc = avformat_open_input(&d->fmt, path, NULL, NULL);
    if (rc < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.open_audio_fmt"), path, rc);
        decoder_close(d);
        return -1;
    }
    if (avformat_find_stream_info(d->fmt, NULL) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.read_stream_fmt"), path);
        decoder_close(d);
        return -1;
    }
    d->stream_index = -1;
    for (unsigned i = 0; i < d->fmt->nb_streams; i++) {
        if (d->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            d->stream_index = (int)i;
            break;
        }
    }
    if (d->stream_index < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.no_stream_fmt"), path);
        decoder_close(d);
        return -1;
    }
    AVCodecParameters *par = d->fmt->streams[d->stream_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.codec_unsupported_fmt"), par->codec_id);
        decoder_close(d);
        return -1;
    }
    d->codec = avcodec_alloc_context3(codec);
    if (!d->codec || avcodec_parameters_to_context(d->codec, par) < 0 ||
        avcodec_open2(d->codec, codec, NULL) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.codec_init_fmt"), path);
        decoder_close(d);
        return -1;
    }
    d->src_rate = d->codec->sample_rate > 0 ? d->codec->sample_rate : par->sample_rate;
    d->tb = d->fmt->streams[d->stream_index]->time_base;
    d->src_channels = d->codec->ch_layout.nb_channels > 0
                          ? d->codec->ch_layout.nb_channels
                          : par->ch_layout.nb_channels;
    if (d->src_rate <= 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.sample_rate_unknown_fmt"), path);
        decoder_close(d);
        return -1;
    }
    if (d->src_channels != 2) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.stereo_only_fmt"), d->src_channels, path);
        decoder_close(d);
        return -1;
    }
    d->swr = swr_alloc();
    if (!d->swr) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "swresample");
        decoder_close(d);
        return -1;
    }
    AVChannelLayout in_layout;
    AVChannelLayout out_layout;
    av_channel_layout_copy(&in_layout, &d->codec->ch_layout);
    if (in_layout.nb_channels <= 0)
        av_channel_layout_default(&in_layout, 2);
    av_channel_layout_default(&out_layout, 2);
    av_opt_set_chlayout(d->swr, "in_chlayout", &in_layout, 0);
    av_opt_set_chlayout(d->swr, "out_chlayout", &out_layout, 0);
    av_opt_set_int(d->swr, "in_sample_rate", d->src_rate, 0);
    av_opt_set_int(d->swr, "out_sample_rate", KARAOKE_MODEL_SAMPLE_RATE, 0);
    av_opt_set_sample_fmt(d->swr, "in_sample_fmt", d->codec->sample_fmt, 0);
    av_opt_set_sample_fmt(d->swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    if (swr_init(d->swr) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.resampler_init_fmt"), path);
        decoder_close(d);
        return -1;
    }
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->pkt || !d->frame) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "decode buffer");
        decoder_close(d);
        return -1;
    }
    return 0;
}

/* 把一帧解码输出经 swr 追加进 pcm（44.1k 立体声 float）。 */
static int decoder_append_frame(WDecoder *d, AVFrame *frame, char *err, size_t err_cap)
{
    if (!frame || frame->nb_samples <= 0)
        return 0;
    if (d->reanchor) {
        /* seek 后的第一个解码帧：用 pts 校正绝对位置（demuxer 落点可能早于
         * 请求点）。没有 pts 时退回 expect_src。 */
        d->reanchor = 0;
        if (frame->pts != AV_NOPTS_VALUE && d->tb.den > 0) {
            long long q = (long long)av_rescale_q(frame->pts, d->tb,
                                                  (AVRational){1, KARAOKE_MODEL_SAMPLE_RATE});
            if (q >= 0) {
                d->pcm.start = q;
                if (q > d->expect_src + 8192) {
                    /* 落点比请求点晚太多：宁可报错也不静默错位。 */
                    set_err(err, err_cap, i18n_get("karaoke.worker.err.seek_position_fmt"), q, d->expect_src);
                    return -1;
                }
            }
        }
    }
    d->src_frames += frame->nb_samples;
    int out_max = (int)av_rescale_rnd(
        swr_get_delay(d->swr, d->src_rate) + frame->nb_samples,
        KARAOKE_MODEL_SAMPLE_RATE, d->src_rate, AV_ROUND_UP);
    if (out_max <= 0)
        return 0;
    size_t need = d->pcm.frames + (size_t)out_max;
    if (need > d->pcm.cap_frames) {
        size_t cap = d->pcm.cap_frames ? d->pcm.cap_frames : 262144;
        while (cap < need)
            cap *= 2;
        float *n = realloc(d->pcm.data, cap * 2 * sizeof(float));
        if (!n) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "PCM buffer");
            return -1;
        }
        d->pcm.data = n;
        d->pcm.cap_frames = cap;
    }
    uint8_t *out_planes[1] = {
        (uint8_t *)(d->pcm.data + d->pcm.frames * 2)
    };
    int produced = swr_convert(d->swr, out_planes, out_max,
                               (const uint8_t **)frame->extended_data,
                               frame->nb_samples);
    if (produced < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.resample_failed_fmt"), produced);
        return -1;
    }
    d->pcm.frames += (size_t)produced;
    return 0;
}

static int decoder_flush_swr(WDecoder *d, char *err, size_t err_cap)
{
    for (;;) {
        int out_max = (int)av_rescale_rnd(swr_get_delay(d->swr, KARAOKE_MODEL_SAMPLE_RATE),
                                          KARAOKE_MODEL_SAMPLE_RATE,
                                          KARAOKE_MODEL_SAMPLE_RATE, AV_ROUND_UP);
        if (out_max <= 0)
            return 0;
        size_t need = d->pcm.frames + (size_t)out_max + 64;
        if (need > d->pcm.cap_frames) {
            size_t cap = d->pcm.cap_frames ? d->pcm.cap_frames : 262144;
            while (cap < need)
                cap *= 2;
            float *n = realloc(d->pcm.data, cap * 2 * sizeof(float));
            if (!n) {
                set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "PCM buffer");
                return -1;
            }
            d->pcm.data = n;
            d->pcm.cap_frames = cap;
        }
        uint8_t *out_planes[1] = {
            (uint8_t *)(d->pcm.data + d->pcm.frames * 2)
        };
        int produced = swr_convert(d->swr, out_planes, out_max, NULL, 0);
        if (produced <= 0)
            return 0;
        d->pcm.frames += (size_t)produced;
    }
}

/* 解出至少一帧；返回 1=有进展，0=EOF，-1=错误。 */
static int decoder_step(WDecoder *d, char *err, size_t err_cap)
{
    for (;;) {
        int r = avcodec_receive_frame(d->codec, d->frame);
        if (r == 0) {
            int rc = decoder_append_frame(d, d->frame, err, err_cap);
            av_frame_unref(d->frame);
            return rc == 0 ? 1 : -1;
        }
        if (r == AVERROR(EAGAIN))
            break;
        if (r == AVERROR_EOF) {
            d->eof = 1;
            return decoder_flush_swr(d, err, err_cap) == 0 ? 0 : -1;
        }
        set_err(err, err_cap, i18n_get("karaoke.worker.err.decode_failed_fmt"), r);
        return -1;
    }
    while (!d->sent_null) {
        int r = av_read_frame(d->fmt, d->pkt);
        if (r == AVERROR_EOF) {
            d->sent_null = 1;
            avcodec_send_packet(d->codec, NULL);
            return 1;
        }
        if (r < 0) {
            if (r == AVERROR(EINTR) || r == AVERROR_EXIT) {
                set_err(err, err_cap, i18n_get("karaoke.worker.err.decode_interrupted"));
                return -1;
            }
            set_err(err, err_cap, i18n_get("karaoke.worker.err.read_packet_fmt"), r);
            return -1;
        }
        if (d->pkt->stream_index != d->stream_index) {
            av_packet_unref(d->pkt);
            continue;
        }
        r = avcodec_send_packet(d->codec, d->pkt);
        av_packet_unref(d->pkt);
        if (r < 0 && r != AVERROR(EAGAIN)) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.send_packet_fmt"), r);
            return -1;
        }
        return 1;
    }
    return 0;
}

/* 解到 pcm 覆盖绝对位置 target（44.1k 帧）或 EOF。 */
static int decoder_fill_to(WDecoder *d, long long target, volatile int *cancelled,
                           char *err, size_t err_cap)
{
    while (d->pcm.start + (long long)d->pcm.frames < target) {
        if (cancelled && *cancelled) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.cancelled"));
            return -1;
        }
        int rc = decoder_step(d, err, err_cap);
        if (rc < 0)
            return -1;
        if (rc == 0)
            break;
        if (d->src_frames > (long long)KARAOKE_WORKER_MAX_SECONDS * d->src_rate) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.too_long_limit_fmt"),
                    KARAOKE_WORKER_MAX_SECONDS);
            return -1;
        }
    }
    return 0;
}

/* ================================================================
 * 5. FLAC 写器（16bit / 2ch / 指定采样率）
 * ================================================================ */

typedef struct {
    AVFormatContext *oc;
    AVCodecContext *cc;
    AVStream *st;
    AVFrame *frame;
    AVPacket *pkt;
    int64_t pts;
    long long written;
    int open;
    int frame_size;
    int16_t *pending;     /* 攒满 frame_size 再送编码器（非末帧必须整帧） */
    int pending_count;
} WFlacWriter;

static int flac_writer_open(WFlacWriter *w, const char *path, int sample_rate,
                            char *err, size_t err_cap)
{
    memset(w, 0, sizeof(*w));
    if (avformat_alloc_output_context2(&w->oc, NULL, "flac", path) < 0 || !w->oc) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_create_fmt"), path);
        return -1;
    }
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!codec) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_no_encoder"));
        return -1;
    }
    w->cc = avcodec_alloc_context3(codec);
    if (!w->cc) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "FLAC encoder");
        return -1;
    }
    w->cc->sample_rate = sample_rate;
    w->cc->sample_fmt = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&w->cc->ch_layout, 2);
    w->cc->time_base = (AVRational){1, sample_rate};
    if (avcodec_open2(w->cc, codec, NULL) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_encoder_open"));
        return -1;
    }
    w->st = avformat_new_stream(w->oc, NULL);
    if (!w->st || avcodec_parameters_from_context(w->st->codecpar, w->cc) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_stream_create"));
        return -1;
    }
    w->st->time_base = w->cc->time_base;
    if (avio_open(&w->oc->pb, path, AVIO_FLAG_WRITE) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_write_file_fmt"), path);
        return -1;
    }
    if (avformat_write_header(w->oc, NULL) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_header"));
        return -1;
    }
    w->frame = av_frame_alloc();
    w->pkt = av_packet_alloc();
    if (!w->frame || !w->pkt) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "FLAC frame");
        return -1;
    }
    w->frame->format = AV_SAMPLE_FMT_S16;
    w->frame->sample_rate = sample_rate;
    av_channel_layout_default(&w->frame->ch_layout, 2);
    w->frame_size = w->cc->frame_size > 0 ? w->cc->frame_size : 4608;
    w->frame->nb_samples = w->frame_size;
    w->pending = calloc((size_t)w->frame_size * 2, sizeof(int16_t));
    if (!w->pending || av_frame_get_buffer(w->frame, 0) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_frame_alloc"));
        return -1;
    }
    w->open = 1;
    return 0;
}

static int flac_writer_drain(WFlacWriter *w, char *err, size_t err_cap)
{
    for (;;) {
        int r = avcodec_receive_packet(w->cc, w->pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return 0;
        if (r < 0) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_encode_fmt"), r);
            return -1;
        }
        av_packet_rescale_ts(w->pkt, w->cc->time_base, w->st->time_base);
        w->pkt->stream_index = w->st->index;
        int wr = av_interleaved_write_frame(w->oc, w->pkt);
        av_packet_unref(w->pkt);
        if (wr < 0) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_write_frame_fmt"), wr);
            return -1;
        }
    }
}

/* 送一整帧（frames <= frame_size；只有最后一帧可以更短）。 */
static int flac_writer_send_frame(WFlacWriter *w, const int16_t *data, int frames,
                                  char *err, size_t err_cap)
{
    if (av_frame_make_writable(w->frame) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_frame_unwritable"));
        return -1;
    }
    w->frame->nb_samples = frames;
    w->frame->pts = w->pts;
    memcpy(w->frame->data[0], data, (size_t)frames * 2 * sizeof(int16_t));
    int r = avcodec_send_frame(w->cc, w->frame);
    if (r < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_send_frame_fmt"), r);
        return -1;
    }
    w->pts += frames;
    w->written += frames;
    return flac_writer_drain(w, err, err_cap);
}

static int flac_writer_feed_s16(WFlacWriter *w, const int16_t *interleaved, int frames,
                                char *err, size_t err_cap)
{
    while (frames > 0) {
        int room = w->frame_size - w->pending_count;
        int n = frames < room ? frames : room;
        memcpy(w->pending + (size_t)w->pending_count * 2, interleaved,
               (size_t)n * 2 * sizeof(int16_t));
        w->pending_count += n;
        interleaved += (size_t)n * 2;
        frames -= n;
        if (w->pending_count == w->frame_size) {
            if (flac_writer_send_frame(w, w->pending, w->frame_size, err, err_cap) != 0)
                return -1;
            w->pending_count = 0;
        }
    }
    return 0;
}

static int flac_writer_feed(WFlacWriter *w, const float *interleaved, int frames,
                            char *err, size_t err_cap)
{
    /* 逐帧转换到 pending（避免为每小块分配整帧缓冲）。 */
    while (frames > 0) {
        int room = w->frame_size - w->pending_count;
        int n = frames < room ? frames : room;
        int16_t *dst = w->pending + (size_t)w->pending_count * 2;
        for (int i = 0; i < n * 2; i++) {
            float v = interleaved[i];
            if (v > 1.0f)
                v = 1.0f;
            if (v < -1.0f)
                v = -1.0f;
            dst[i] = (int16_t)lrintf(v * 32767.0f);
        }
        w->pending_count += n;
        interleaved += (size_t)n * 2;
        frames -= n;
        if (w->pending_count == w->frame_size) {
            if (flac_writer_send_frame(w, w->pending, w->frame_size, err, err_cap) != 0)
                return -1;
            w->pending_count = 0;
        }
    }
    return 0;
}

static int flac_writer_finish(WFlacWriter *w, char *err, size_t err_cap)
{
    if (!w->open)
        return 0;
    if (w->pending_count > 0) {
        if (flac_writer_send_frame(w, w->pending, w->pending_count, err, err_cap) != 0)
            return -1;
        w->pending_count = 0;
    }
    int r = avcodec_send_frame(w->cc, NULL);
    if (r < 0 && r != AVERROR_EOF) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_flush_fmt"), r);
        return -1;
    }
    if (flac_writer_drain(w, err, err_cap) != 0)
        return -1;
    if (av_write_trailer(w->oc) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.flac_trailer"));
        return -1;
    }
    w->open = 0;
    return 0;
}

static void flac_writer_close(WFlacWriter *w)
{
    if (!w)
        return;
    if (w->oc && w->oc->pb)
        avio_closep(&w->oc->pb);
    if (w->pkt)
        av_packet_free(&w->pkt);
    if (w->frame)
        av_frame_free(&w->frame);
    if (w->cc)
        avcodec_free_context(&w->cc);
    if (w->oc)
        avformat_free_context(w->oc);
    free(w->pending);
    memset(w, 0, sizeof(*w));
}

/* ================================================================
 * 6. 输出重采样（44.1k float → 原采样率 s16 流）
 * ================================================================ */

typedef struct {
    SwrContext *swr;
    int dst_rate;
    int16_t *buf;
    int buf_frames;
} WResampler;

static int resampler_open(WResampler *r, int dst_rate, char *err, size_t err_cap)
{
    memset(r, 0, sizeof(*r));
    r->dst_rate = dst_rate;
    if (dst_rate == KARAOKE_MODEL_SAMPLE_RATE)
        return 0;   /* 直通 */
    r->swr = swr_alloc();
    if (!r->swr) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "output resampler");
        return -1;
    }
    AVChannelLayout in_layout, out_layout;
    av_channel_layout_default(&in_layout, 2);
    av_channel_layout_default(&out_layout, 2);
    av_opt_set_chlayout(r->swr, "in_chlayout", &in_layout, 0);
    av_opt_set_chlayout(r->swr, "out_chlayout", &out_layout, 0);
    av_opt_set_int(r->swr, "in_sample_rate", KARAOKE_MODEL_SAMPLE_RATE, 0);
    av_opt_set_int(r->swr, "out_sample_rate", dst_rate, 0);
    av_opt_set_sample_fmt(r->swr, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    av_opt_set_sample_fmt(r->swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    if (swr_init(r->swr) < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.out_resampler_init"));
        return -1;
    }
    return 0;
}

static void resampler_close(WResampler *r)
{
    if (!r)
        return;
    if (r->swr)
        swr_free(&r->swr);
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

/* 把 44.1k 交织 float 送进 writer（dst_rate==44100 时直通 s16 转换）。 */
static int resampler_feed(WResampler *r, WFlacWriter *w, const float *in,
                          int frames, char *err, size_t err_cap)
{
    if (frames <= 0)
        return 0;
    if (!r->swr)
        return flac_writer_feed(w, in, frames, err, err_cap);
    int out_max = (int)av_rescale_rnd(
        swr_get_delay(r->swr, KARAOKE_MODEL_SAMPLE_RATE) + frames, r->dst_rate,
        KARAOKE_MODEL_SAMPLE_RATE, AV_ROUND_UP);
    if (out_max <= 0)
        return 0;
    if (out_max > r->buf_frames) {
        int16_t *n = realloc(r->buf, (size_t)out_max * 2 * sizeof(int16_t));
        if (!n) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "output resampler buffer");
            return -1;
        }
        r->buf = n;
        r->buf_frames = out_max;
    }
    uint8_t *out_planes[1] = {(uint8_t *)r->buf};
    int produced = swr_convert(r->swr, out_planes, out_max,
                               (const uint8_t **)&in, frames);
    if (produced < 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.out_resample_fmt"), produced);
        return -1;
    }
    if (produced == 0)
        return 0;
    return flac_writer_feed_s16(w, r->buf, produced, err, err_cap);
}

static int resampler_flush(WResampler *r, WFlacWriter *w, char *err, size_t err_cap)
{
    if (!r->swr)
        return 0;
    for (;;) {
        int out_max = 4096;
        if (out_max > r->buf_frames) {
            int16_t *n = realloc(r->buf, (size_t)out_max * 2 * sizeof(int16_t));
            if (!n) {
                set_err(err, err_cap, i18n_get("karaoke.worker.err.oom_fmt"), "output resampler buffer");
                return -1;
            }
            r->buf = n;
            r->buf_frames = out_max;
        }
        uint8_t *out_planes[1] = {(uint8_t *)r->buf};
        int produced = swr_convert(r->swr, out_planes, out_max, NULL, 0);
        if (produced <= 0)
            return 0;
        if (flac_writer_feed_s16(w, r->buf, produced, err, err_cap) != 0)
            return -1;
    }
}

/* ================================================================
 * 7. job 执行
 * ================================================================ */

typedef struct {
    KaraokeWorkerJob req;
    unsigned generation;      /* 取消代数：与 g_cancel_gen 不同即取消 */
    volatile int cancelled;
    WDecoder dec;
    Wdsp dsp;
    KaraokeModelSession *session;
    WFlacWriter w_inst, w_soft;
    WResampler rs_inst, rs_soft;
    char inst_tmp[KARAOKE_CACHE_PATH_CAP];
    char soft_tmp[KARAOKE_CACHE_PATH_CAP];
    char meta_tmp[KARAOKE_CACHE_PATH_CAP];
    KaraokeCachePaths paths;
    KaraokeCacheConfig cache_cfg;
    long long emitted;
    long long chunks_done;
    long long chunks_total_est;
} WJob;

/* 建 ONNX 会话会触发 Debian ORT 的 ~116KB Schema error 噪音；这里临时把
 * fd2 指到 /dev/null（进程内仅此一处，且只在建会话的短窗口）。 */
static pthread_mutex_t g_quiet_lock = PTHREAD_MUTEX_INITIALIZER;

static int quiet_begin(int *saved_fd)
{
    pthread_mutex_lock(&g_quiet_lock);
    *saved_fd = dup(STDERR_FILENO);
    int dn = open("/dev/null", O_WRONLY);
    if (*saved_fd < 0 || dn < 0) {
        if (*saved_fd >= 0)
            close(*saved_fd);
        if (dn >= 0)
            close(dn);
        pthread_mutex_unlock(&g_quiet_lock);
        *saved_fd = -1;
        return -1;
    }
    dup2(dn, STDERR_FILENO);
    close(dn);
    return 0;
}

static void quiet_end(int saved_fd)
{
    if (saved_fd >= 0) {
        dup2(saved_fd, STDERR_FILENO);
        close(saved_fd);
    }
    pthread_mutex_unlock(&g_quiet_lock);
}

/* 填充 dsp->part：mixture[s .. s+chunk)（mixture = trim 个 0 + mix + 0）。 */
static int job_fill_part(WJob *j, long long s, char *err, size_t err_cap)
{
    (void)err;
    (void)err_cap;
    Wdsp *d = &j->dsp;
    const int trim = d->trim;
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < d->chunk; k++) {
            long long p = s + k;
            float v = 0.0f;
            if (p >= trim) {
                long long q = p - trim;
                long long idx = q - j->dec.pcm.start;
                if (idx >= 0 && idx < (long long)j->dec.pcm.frames)
                    v = j->dec.pcm.data[(size_t)idx * 2 + (size_t)ch];
            }
            d->part[(size_t)ch * (size_t)d->chunk + (size_t)k] = v;
        }
    }
    return 0;
}

static void job_stft(WJob *j)
{
    Wdsp *d = &j->dsp;
    memset(d->in_patch, 0, sizeof(float) * karaoke_model_patch_elems(d->spec));
    for (int t = 0; t < d->dim_t; t++) {
        for (int ch = 0; ch < 2; ch++) {
            for (int k = 0; k < d->n_fft; k++) {
                size_t idx = reflect_index((size_t)t * (size_t)d->hop + (size_t)k,
                                           (size_t)d->chunk, (size_t)d->trim);
                d->frame[k] = d->part[(size_t)ch * (size_t)d->chunk + idx] * d->window[k];
            }
            wfft_forward(&d->fft, d->frame, d->bins, d->dim_f);
            const size_t base = ((size_t)ch * 2u) * (size_t)d->dim_f * (size_t)d->dim_t;
            for (int f = 3; f < d->dim_f; f++) {   /* UVR：bins 0..2 清零 */
                d->in_patch[base + (size_t)f * (size_t)d->dim_t + (size_t)t] = d->bins[f].re;
                d->in_patch[base + (size_t)d->dim_f * (size_t)d->dim_t +
                            (size_t)f * (size_t)d->dim_t + (size_t)t] = d->bins[f].im;
            }
        }
    }
}

static void job_istft_into_ring(WJob *j, long long s)
{
    Wdsp *d = &j->dsp;
    const int n_fft = d->n_fft;
    const size_t cyl = (size_t)d->chunk + (size_t)n_fft;
    memset(d->ola, 0, sizeof(float) * cyl * 2);
    memset(d->wsum, 0, sizeof(float) * cyl);
    for (int t = 0; t < d->dim_t; t++) {
        for (int ch = 0; ch < 2; ch++) {
            const size_t base = ((size_t)ch * 2u) * (size_t)d->dim_f * (size_t)d->dim_t;
            for (int f = 0; f < d->dim_f; f++) {
                d->bins[f].re = d->out_patch[base + (size_t)f * (size_t)d->dim_t + (size_t)t];
                d->bins[f].im = d->out_patch[base + (size_t)d->dim_f * (size_t)d->dim_t +
                                             (size_t)f * (size_t)d->dim_t + (size_t)t];
            }
            wfft_inverse(&d->fft, d->bins, d->dim_f, d->frame);
            for (int k = 0; k < n_fft; k++) {
                size_t pos = (size_t)t * (size_t)d->hop + (size_t)k;
                d->ola[pos * 2 + (size_t)ch] += d->frame[k] * d->window[k];
            }
        }
        for (int k = 0; k < n_fft; k++) {
            size_t pos = (size_t)t * (size_t)d->hop + (size_t)k;
            d->wsum[pos] += d->window[k] * d->window[k];
        }
    }
    for (size_t p = 0; p < cyl; p++) {
        float den = d->wsum[p];
        float inv = den > 1e-11f ? 1.0f / den : 0.0f;
        d->ola[p * 2] *= inv;
        d->ola[p * 2 + 1] *= inv;
    }
    /* 裁掉两端 n_fft/2，按对称 Hann 交叉淡化累加进 ring（chunk 对齐）。 */
    for (int k = 0; k < d->chunk; k++) {
        float w = sym_hann((size_t)k, (size_t)d->chunk);
        size_t idx = (size_t)((s + k) % d->ring_size);
        d->ring_l[idx] += d->ola[((size_t)d->trim + (size_t)k) * 2] * w;
        d->ring_r[idx] += d->ola[((size_t)d->trim + (size_t)k) * 2 + 1] * w;
        d->ring_d[idx] += w;
    }
}

/* 取一个已完成的输出样本（ring 下标 p%chunk）。 */
static inline void ring_take(Wdsp *d, long long p, float *inst_l, float *inst_r)
{
    size_t idx = (size_t)(p % d->ring_size);
    float den = d->ring_d[idx];
    float inv = den > 1e-12f ? 1.0f / den : 0.0f;
    *inst_l = d->ring_l[idx] * inv;
    *inst_r = d->ring_r[idx] * inv;
    d->ring_l[idx] = d->ring_r[idx] = d->ring_d[idx] = 0.0f;
}

static int job_feed_block(WJob *j, const float *inst, const float *soft, int frames,
                          char *err, size_t err_cap)
{
    if (resampler_feed(&j->rs_inst, &j->w_inst, inst, frames, err, err_cap) != 0)
        return -1;
    return resampler_feed(&j->rs_soft, &j->w_soft, soft, frames, err, err_cap);
}

/* 发出 [*flushed, end) 的输出样本（end 为 mixture 坐标）；返回 -1 失败。 */
static int job_emit_until(WJob *j, long long *flushed, long long end,
                          volatile int *cancelled,
                          char *err, size_t err_cap)
{
    Wdsp *d = &j->dsp;
    float *inst_blk = d->emit_inst;
    float *soft_blk = d->emit_soft;
    int fill = 0;
    const float inst_mix = j->cache_cfg.soft_inst_mix > 0.0f
                               ? j->cache_cfg.soft_inst_mix
                               : KARAOKE_CACHE_SOFT_INST_MIX;
    const float orig_mix = 1.0f - inst_mix;
    while (*flushed < end) {
        if (cancelled && *cancelled) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.cancelled"));
            return -1;
        }
        long long p = *flushed;
        long long q = p - d->trim;   /* p >= trim 时才输出 */
        if (p >= d->trim) {
            long long idx = q - j->dec.pcm.start;
            if (idx < 0 || idx >= (long long)j->dec.pcm.frames) {
                set_err(err, err_cap, i18n_get("karaoke.worker.err.internal_output_pos"));
                return -1;
            }
            float vl, vr;
            ring_take(d, p, &vl, &vr);
            /* 模型直出是人声 stem：voc = ring × compensate；
             * inst = mixture − voc（UVR 语义，与 karaoke_model.c 一致）。 */
            const float comp = d->spec->compensate;
            vl *= comp;
            vr *= comp;
            float ml = j->dec.pcm.data[(size_t)idx * 2];
            float mr = j->dec.pcm.data[(size_t)idx * 2 + 1];
            float il = ml - vl;
            float ir = mr - vr;
            float sl = orig_mix * ml + inst_mix * il;
            float sr = orig_mix * mr + inst_mix * ir;
            inst_blk[fill * 2] = il;
            inst_blk[fill * 2 + 1] = ir;
            soft_blk[fill * 2] = sl;
            soft_blk[fill * 2 + 1] = sr;
            fill++;
        } else {
            /* trim 之前的补零段：丢弃 ring 内容即可（不属于输出） */
            size_t ridx = (size_t)(p % d->ring_size);
            d->ring_l[ridx] = d->ring_r[ridx] = d->ring_d[ridx] = 0.0f;
        }
        (*flushed)++;
        if (fill == WORKER_EMIT_BLOCK || (*flushed == end && fill > 0)) {
            if (job_feed_block(j, inst_blk, soft_blk, fill, err, err_cap) != 0)
                return -1;
            fill = 0;
        }
    }
    if (fill > 0) {
        if (job_feed_block(j, inst_blk, soft_blk, fill, err, err_cap) != 0)
            return -1;
    }
    return 0;
}

/* 当前取消代数（定义在文件后部）；job 内通过该函数检查。 */
static unsigned worker_cancel_generation(void);

static int job_check_cancel(WJob *j, char *err, size_t err_cap)
{
    if (j->cancelled || j->generation != worker_cancel_generation()) {
        j->cancelled = 1;
        set_err(err, err_cap, i18n_get("karaoke.worker.err.cancelled"));
        return -1;
    }
    return 0;
}

static void job_dsp_free(WJob *j)
{
    wdsp_free(&j->dsp);
}

static void job_cleanup(WJob *j)
{
    if (j->session)
        karaoke_model_session_close(j->session);
    resampler_close(&j->rs_inst);
    resampler_close(&j->rs_soft);
    flac_writer_close(&j->w_inst);
    flac_writer_close(&j->w_soft);
    decoder_close(&j->dec);
    job_dsp_free(j);
}

/* 进度/取消的跨线程通道（worker 主循环会读取；只由本文件的线程写）。 */
static void worker_note_progress(long long done, long long total);

/* v1 主 job（旧版整曲成对缓存；仅 test_karaoke_model.c 兼容夹具使用）：
 * 成功返回 0 并已提交缓存；失败/取消返回 -1（err 写原因）。 */
static int job_run_v1(WJob *j, char *err, size_t err_cap)
{
    const KaraokeWorkerJob *req = &j->req;
    const KaraokeModelSpec *spec = karaoke_model_by_id(req->model_id);
    if (!spec) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.unsupported_model_fmt"), req->model_id);
        return -1;
    }
    if (req->model_sha256[0] == '\0') {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.model_sha_empty"));
        return -1;
    }
    memset(&j->cache_cfg, 0, sizeof(j->cache_cfg));
    j->cache_cfg.root = req->cache_root;
    j->cache_cfg.model_id = spec->id;
    j->cache_cfg.model_sha256 = spec->sha256;
    j->cache_cfg.overlap = req->overlap > 0.0f ? req->overlap : KARAOKE_CACHE_OVERLAP;
    j->cache_cfg.soft_inst_mix =
        req->soft_inst_mix > 0.0f ? req->soft_inst_mix : KARAOKE_CACHE_SOFT_INST_MIX;
    j->cache_cfg.limit_mb = KARAOKE_CACHE_LIMIT_MB;

    if (req->model_sha256[0] && strcmp(req->model_sha256, spec->sha256) != 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.model_sha_mismatch"));
        return -1;
    }
    /* S2：模型文件的完整 SHA256 校验在 worker 线程做（结果按 路径+大小+
     * mtime 缓存，重复 job 不重读 30MiB）；UI/播放线程只做 stat 级定位。 */
    if (karaoke_model_verify_file_cached(spec, req->model_path, err, err_cap) != 0)
        return -1;

    if (decoder_open(&j->dec, req->track_path, &j->cancelled, err, err_cap) != 0)
        return -1;

    /* 时长上限：优先用容器/流时长，其次用解码帧数在 fill 里兜底。 */
    {
        int64_t dur = 0;
        AVStream *st = j->dec.fmt->streams[j->dec.stream_index];
        if (st->duration > 0 && st->time_base.den > 0)
            dur = av_rescale_q(st->duration, st->time_base, (AVRational){1, 1});
        else if (j->dec.fmt->duration > 0)
            dur = j->dec.fmt->duration / AV_TIME_BASE;
        if (dur > KARAOKE_WORKER_MAX_SECONDS) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.too_long_fmt"),
                    (long long)dur, KARAOKE_WORKER_MAX_SECONDS);
            return -1;
        }
        j->chunks_total_est = 0;
        if (dur > 0) {
            long long n44_est = (long long)((double)dur * KARAOKE_MODEL_SAMPLE_RATE);
            long long step = (long long)((double)(spec->hop * (spec->dim_t - 1)) *
                                         (1.0 - (double)j->cache_cfg.overlap));
            if (step > 0)
                j->chunks_total_est = (n44_est + 2 * (long long)spec->n_fft) / step + 1;
        }
    }

    if (karaoke_model_runtime_available(err, err_cap) != 0)
        return -1;
    int saved_stderr = -1;
    quiet_begin(&saved_stderr);
    j->session = karaoke_model_session_open(spec, req->model_path, 0, err, err_cap);
    quiet_end(saved_stderr);
    if (!j->session)
        return -1;

    if (wdsp_init(&j->dsp, spec, err, err_cap) != 0)
        return -1;

    if (karaoke_cache_begin(&j->cache_cfg, req->track_path, &j->paths,
                            j->inst_tmp, sizeof(j->inst_tmp), j->soft_tmp,
                            sizeof(j->soft_tmp), j->meta_tmp, sizeof(j->meta_tmp),
                            err, err_cap) != 0)
        return -1;

    if (flac_writer_open(&j->w_inst, j->inst_tmp, j->dec.src_rate, err, err_cap) != 0 ||
        flac_writer_open(&j->w_soft, j->soft_tmp, j->dec.src_rate, err, err_cap) != 0)
        return -1;
    if (resampler_open(&j->rs_inst, j->dec.src_rate, err, err_cap) != 0 ||
        resampler_open(&j->rs_soft, j->dec.src_rate, err, err_cap) != 0)
        return -1;

    /* 逐 chunk：解码→STFT→推理→iSTFT→交叉淡化→发射。 */
    long long s = 0;
    long long flushed = 0;
    for (;;) {
        if (job_check_cancel(j, err, err_cap) != 0)
            return -1;
        if (decoder_fill_to(&j->dec, s + j->dsp.chunk - j->dsp.trim, &j->cancelled,
                            err, err_cap) != 0)
            return -1;
        long long n44 = j->dec.pcm.start + (long long)j->dec.pcm.frames;
        if (j->dec.eof && s >= (long long)j->dsp.trim + n44)
            break;
        job_fill_part(j, s, err, err_cap);
        job_stft(j);
        if (karaoke_model_infer_patch(j->session, j->dsp.in_patch, j->dsp.out_patch,
                                      err, err_cap) != 0)
            return -1;
        job_istft_into_ring(j, s);
        long long emit_end = s;
        if (j->dec.eof) {
            long long ce = (long long)j->dsp.trim + n44;
            if (emit_end > ce)
                emit_end = ce;
        }
        if (job_emit_until(j, &flushed, emit_end, &j->cancelled, err, err_cap) != 0)
            return -1;
        pcm_drop_before(&j->dec.pcm, flushed - j->dsp.trim);
        s += j->dsp.step;
        j->chunks_done++;
        j->emitted = flushed;
        worker_note_progress(j->chunks_done, j->chunks_total_est);
    }
    if (job_check_cancel(j, err, err_cap) != 0)
        return -1;

    long long n44_final = j->dec.pcm.start + (long long)j->dec.pcm.frames;
    long long content_end = (long long)j->dsp.trim + n44_final;
    if (job_emit_until(j, &flushed, content_end, &j->cancelled, err, err_cap) != 0)
        return -1;

    if (resampler_flush(&j->rs_inst, &j->w_inst, err, err_cap) != 0 ||
        resampler_flush(&j->rs_soft, &j->w_soft, err, err_cap) != 0)
        return -1;
    if (flac_writer_finish(&j->w_inst, err, err_cap) != 0 ||
        flac_writer_finish(&j->w_soft, err, err_cap) != 0)
        return -1;
    if (j->w_inst.written != j->w_soft.written) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.internal_frames_mismatch_fmt"),
                j->w_inst.written, j->w_soft.written);
        return -1;
    }
    long long frames = j->w_inst.written;
    if (frames <= 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.empty_result"));
        return -1;
    }
    if (karaoke_cache_write_meta(j->meta_tmp, &j->cache_cfg, frames,
                                 j->dec.src_rate, frames, frames, err, err_cap) != 0)
        return -1;
    if (karaoke_cache_commit(&j->cache_cfg, &j->paths, j->inst_tmp, j->soft_tmp,
                             j->meta_tmp, err, err_cap) != 0)
        return -1;
    log_info("karaoke", "cache ready: %s (%lld frames @ %dHz)", req->track_path,
             frames, j->dec.src_rate);
    return 0;
}


/* ================================================================
 * 7b. v2 分段 job（15s 段、/tmp 会话缓存、seek 优先/回填）
 * ================================================================ */

/* 段 job 的跨函数状态。 */
typedef struct {
    WJob *j;
    KaraokeSegmentPaths paths;
    KaraokeCacheConfig cfg;
    KaraokeSegmentManifest manifest;
    WFlacWriter w_inst, w_soft;
    int cur_seg;              /* 正在写的段号；-1 = 未打开 */
    int writer_open;
    long long seg_frames;     /* 当前段已写帧数 */
    long long flushed;        /* 下一个待发射的 mixture 坐标 */
    long long total_frames;   /* EOF 后已知的模型输出总帧数 */
    int total_segments;       /* EOF 后已知的总段数；0 = 未知 */
    int produced;             /* 本次 job 新产出的段数 */
    /* 会话缓存预算淘汰的保护目录：本次 job 曲目 + 当前播放曲目（预取场景）。 */
    const char *exclude_dirs[2];
    int exclude_count;
} WSegCtx;

/* 前向声明（实现在第 8 节；worker 线程内调用）。 */
static void worker_note_segments(int done, int total, int first_ready, int phase);
static int worker_hint_take(const char *track_path, unsigned *out_gen);

static int fsync_file_path(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static void seg_clear_dsp_ring(Wdsp *d)
{
    memset(d->ring_l, 0, sizeof(float) * (size_t)d->ring_size);
    memset(d->ring_r, 0, sizeof(float) * (size_t)d->ring_size);
    memset(d->ring_d, 0, sizeof(float) * (size_t)d->ring_size);
}

static int seg_open_writer(WSegCtx *c, int seg, char *err, size_t err_cap)
{
    if (seg < 0 || seg >= KARAOKE_SEGMENT_MAX_COUNT) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.segment_range_fmt"), seg);
        return -1;
    }
    char inst_path[KARAOKE_CACHE_PATH_CAP];
    char soft_path[KARAOKE_CACHE_PATH_CAP];
    if (karaoke_cache_segment_file(&c->paths, seg, 0, inst_path,
                                   sizeof(inst_path)) != 0 ||
        karaoke_cache_segment_file(&c->paths, seg, 1, soft_path,
                                   sizeof(soft_path)) != 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.path_too_long"));
        return -1;
    }
    /* S1：每段写入前做空间/预算闸门（/tmp 可能是 tmpfs）。先淘汰最旧的
     * 非保护曲目段，仍不足则失败回退原声，绝不写到 ENOSPC 才报错。 */
    if (karaoke_cache_segment_reserve(&c->cfg, c->exclude_dirs,
                                      c->exclude_count, 0, err, err_cap) != 0)
        return -1;
    /* 直接写最终文件：ready 标记最后写，未完成段永远不可见。 */
    if (flac_writer_open(&c->w_inst, inst_path, KARAOKE_SEGMENT_SAMPLE_RATE, err,
                         err_cap) != 0) {
        flac_writer_close(&c->w_inst);
        unlink(inst_path);
        return -1;
    }
    if (flac_writer_open(&c->w_soft, soft_path, KARAOKE_SEGMENT_SAMPLE_RATE, err,
                         err_cap) != 0) {
        flac_writer_close(&c->w_soft);
        flac_writer_close(&c->w_inst);
        unlink(inst_path);
        unlink(soft_path);
        return -1;
    }
    c->cur_seg = seg;
    c->seg_frames = 0;
    c->writer_open = 1;
    return 0;
}

/* 收尾当前段：frames>0 → finish+fsync 两份 FLAC → 原子写 ready 标记；
 * frames==0 → 关闭并删除空文件（EOF 恰落在段边界）。 */
static int seg_finish_writer(WSegCtx *c, long long frames, char *err, size_t err_cap)
{
    if (!c->writer_open)
        return 0;
    char inst_path[KARAOKE_CACHE_PATH_CAP];
    char soft_path[KARAOKE_CACHE_PATH_CAP];
    (void)karaoke_cache_segment_file(&c->paths, c->cur_seg, 0, inst_path,
                                     sizeof(inst_path));
    (void)karaoke_cache_segment_file(&c->paths, c->cur_seg, 1, soft_path,
                                     sizeof(soft_path));
    int rc = 0;
    if (flac_writer_finish(&c->w_inst, err, err_cap) != 0)
        rc = -1;
    if (rc == 0 && flac_writer_finish(&c->w_soft, err, err_cap) != 0)
        rc = -1;
    if (rc == 0 &&
        (c->w_inst.written != frames || c->w_soft.written != frames)) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.segment_frames_mismatch_fmt"), frames,
                c->w_inst.written, c->w_soft.written);
        rc = -1;
    }
    flac_writer_close(&c->w_inst);
    flac_writer_close(&c->w_soft);
    c->writer_open = 0;
    int seg = c->cur_seg;
    c->cur_seg = -1;
    if (rc != 0) {
        unlink(inst_path);
        unlink(soft_path);
        return -1;
    }
    if (frames <= 0) {
        unlink(inst_path);
        unlink(soft_path);
        return 0;
    }
    if (fsync_file_path(inst_path) != 0 || fsync_file_path(soft_path) != 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.segment_fsync_fmt"), strerror(errno));
        unlink(inst_path);
        unlink(soft_path);
        return -1;
    }
    if (karaoke_cache_segment_mark_ready(&c->paths, seg, frames, err, err_cap) != 0) {
        unlink(inst_path);
        unlink(soft_path);
        return -1;
    }
    c->produced++;
    /* S1：本段已可见后再按会话预算淘汰最旧的非保护曲目段（当前曲目与
     * 正在播放曲目受保护，manifest/段布局语义不变）。 */
    (void)karaoke_cache_segment_evict(&c->cfg, c->exclude_dirs,
                                      c->exclude_count, NULL, NULL, 0);
    return 0;
}

static void seg_abandon_writer(WSegCtx *c)
{
    if (!c->writer_open)
        return;
    char inst_path[KARAOKE_CACHE_PATH_CAP];
    char soft_path[KARAOKE_CACHE_PATH_CAP];
    (void)karaoke_cache_segment_file(&c->paths, c->cur_seg, 0, inst_path,
                                     sizeof(inst_path));
    (void)karaoke_cache_segment_file(&c->paths, c->cur_seg, 1, soft_path,
                                     sizeof(soft_path));
    flac_writer_close(&c->w_inst);
    flac_writer_close(&c->w_soft);
    unlink(inst_path);
    unlink(soft_path);
    c->writer_open = 0;
    c->cur_seg = -1;
    c->seg_frames = 0;
}

static int seg_ready(WSegCtx *c, int seg)
{
    return karaoke_cache_segment_ready(&c->paths, &c->manifest, seg, NULL, NULL, 0);
}

/* 把 [*flushed, end) 的样本发出去：p<output_from 的样本只清 ring（预热段），
 * 真正输出从 output_from 开始。调用方保证不跨段边界。 */
static int seg_emit_until(WSegCtx *c, long long end, long long output_from,
                          char *err, size_t err_cap)
{
    WJob *j = c->j;
    Wdsp *d = &j->dsp;
    float *inst_blk = d->emit_inst;
    float *soft_blk = d->emit_soft;
    int fill = 0;
    const float inst_mix = c->cfg.soft_inst_mix > 0.0f ? c->cfg.soft_inst_mix
                                                       : KARAOKE_CACHE_SOFT_INST_MIX;
    const float orig_mix = 1.0f - inst_mix;
    while (c->flushed < end) {
        if (j->cancelled) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.cancelled"));
            return -1;
        }
        long long p = c->flushed;
        if (p < (long long)d->trim || p < output_from) {
            size_t ridx = (size_t)(p % d->ring_size);
            d->ring_l[ridx] = d->ring_r[ridx] = d->ring_d[ridx] = 0.0f;
            c->flushed++;
            continue;
        }
        long long q = p - (long long)d->trim;
        long long idx = q - j->dec.pcm.start;
        if (idx < 0 || idx >= (long long)j->dec.pcm.frames) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.internal_output_pos"));
            return -1;
        }
        float vl, vr;
        ring_take(d, p, &vl, &vr);
        const float comp = d->spec->compensate;
        vl *= comp;
        vr *= comp;
        float ml = j->dec.pcm.data[(size_t)idx * 2];
        float mr = j->dec.pcm.data[(size_t)idx * 2 + 1];
        float il = ml - vl;
        float ir = mr - vr;
        inst_blk[fill * 2] = il;
        inst_blk[fill * 2 + 1] = ir;
        soft_blk[fill * 2] = orig_mix * ml + inst_mix * il;
        soft_blk[fill * 2 + 1] = orig_mix * mr + inst_mix * ir;
        fill++;
        c->flushed++;
        if (fill == WORKER_EMIT_BLOCK || c->flushed == end) {
            if (flac_writer_feed(&c->w_inst, inst_blk, fill, err, err_cap) != 0 ||
                flac_writer_feed(&c->w_soft, soft_blk, fill, err, err_cap) != 0)
                return -1;
            c->seg_frames += fill;
            fill = 0;
        }
    }
    return 0;
}

/* 把解码器/DSP 定位到段 seg 的起点：回退一个模型 chunk 预热 OLA 上下文。 */
static int seg_reposition(WSegCtx *c, int seg, long long *out_output_from,
                          long long *out_chunk_start, char *err, size_t err_cap)
{
    WJob *j = c->j;
    Wdsp *d = &j->dsp;
    long long q_start = (long long)seg * KARAOKE_SEGMENT_FRAMES;
    long long p_start = q_start + (long long)d->trim;
    long long c0 = p_start / d->step;
    long long start_chunk = c0 > 0 ? c0 - 1 : 0;
    long long s = start_chunk * d->step;
    long long src = s - (long long)d->trim;
    if (src < 0)
        src = 0;
    /* 向后多留一点余量，抵消 demuxer/解码器落点误差（pts 会再校正）。 */
    long long seek_src = src > 8192 ? src - 8192 : 0;

    seg_clear_dsp_ring(d);
    pcm_free(&j->dec.pcm);
    j->dec.pcm.start = seek_src;
    j->dec.pcm.frames = 0;
    j->dec.eof = 0;
    j->dec.sent_null = 0;
    j->dec.src_frames = seek_src;
    j->dec.reanchor = 1;
    j->dec.expect_src = seek_src;
    if (seek_src > 0) {
        AVStream *st = j->dec.fmt->streams[j->dec.stream_index];
        int64_t ts = av_rescale_q(seek_src,
                                  (AVRational){1, KARAOKE_MODEL_SAMPLE_RATE},
                                  st->time_base);
        if (av_seek_frame(j->dec.fmt, j->dec.stream_index, ts,
                          AVSEEK_FLAG_BACKWARD) < 0) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.segment_seek_fmt"), seg);
            return -1;
        }
        avcodec_flush_buffers(j->dec.codec);
        if (j->dec.swr)
            swr_init(j->dec.swr);
    }
    c->flushed = s;
    *out_output_from = p_start;
    *out_chunk_start = s;
    return 0;
}

/* 从 start_seg 起连续产出段，直到：EOF（reason=1）、
 * 段边界处下一段已就绪或收到 seek 提示（reason=0，段边界干净退出）。
 * 返回 -1 失败（err 写原因）。 */
static int seg_produce_from(WSegCtx *c, int start_seg, int *out_reason,
                            char *err, size_t err_cap)
{
    WJob *j = c->j;
    Wdsp *d = &j->dsp;
    long long output_from = 0;
    long long s = 0;
    if (seg_reposition(c, start_seg, &output_from, &s, err, err_cap) != 0)
        return -1;
    if (seg_open_writer(c, start_seg, err, err_cap) != 0)
        return -1;
    for (;;) {
        if (job_check_cancel(j, err, err_cap) != 0)
            return -1;
        /* seek 优先提示：在 chunk 边界放弃当前半成品段（不写 ready）。 */
        unsigned hgen = 0;
        int hs = worker_hint_take(j->req.track_path, &hgen);
        if (hs >= 0 && hs != c->cur_seg && !seg_ready(c, hs)) {
            seg_abandon_writer(c);
            *out_reason = 0;
            return 0;
        }
        if (decoder_fill_to(&j->dec, s + d->chunk - d->trim, &j->cancelled,
                            err, err_cap) != 0)
            return -1;
        long long n44 = j->dec.pcm.start + (long long)j->dec.pcm.frames;
        if (j->dec.eof && s >= (long long)d->trim + n44)
            break;
        job_fill_part(j, s, err, err_cap);
        job_stft(j);
        if (karaoke_model_infer_patch(j->session, d->in_patch, d->out_patch, err,
                                      err_cap) != 0)
            return -1;
        job_istft_into_ring(j, s);
        long long emit_end = s;
        if (j->dec.eof) {
            long long ce = (long long)d->trim + n44;
            if (emit_end > ce)
                emit_end = ce;
        }
        /* 不跨段界：最多发到当前段满。 */
        long long boundary = c->flushed +
                             (KARAOKE_SEGMENT_FRAMES - c->seg_frames);
        long long to = emit_end < boundary ? emit_end : boundary;
        if (to > c->flushed && seg_emit_until(c, to, output_from, err, err_cap) != 0)
            return -1;
        pcm_drop_before(&j->dec.pcm, c->flushed - (long long)d->trim);
        j->chunks_done++;
        worker_note_progress(j->chunks_done, j->chunks_total_est);
        s += d->step;
        if (c->seg_frames == KARAOKE_SEGMENT_FRAMES) {
            int finished_seg = c->cur_seg;
            if (seg_finish_writer(c, KARAOKE_SEGMENT_FRAMES, err, err_cap) != 0)
                return -1;
            int next = finished_seg + 1;
            if (next >= KARAOKE_SEGMENT_MAX_COUNT) {
                set_err(err, err_cap, i18n_get("karaoke.worker.err.segment_limit"));
                return -1;
            }
            if (seg_ready(c, next)) {
                *out_reason = 0;
                return 0;
            }
            unsigned hg2 = 0;
            int h2 = worker_hint_take(j->req.track_path, &hg2);
            if (h2 >= 0 && h2 != next) {
                *out_reason = 0;
                return 0;
            }
            if (seg_open_writer(c, next, err, err_cap) != 0)
                return -1;
            output_from = (long long)next * KARAOKE_SEGMENT_FRAMES + d->trim;
        }
    }
    /* EOF：把剩余输出发完，收尾末段。 */
    long long n44 = j->dec.pcm.start + (long long)j->dec.pcm.frames;
    long long content_end = (long long)d->trim + n44;
    if (c->flushed < content_end &&
        seg_emit_until(c, content_end, output_from, err, err_cap) != 0)
        return -1;
    long long last_frames = c->seg_frames;
    int last_seg = c->cur_seg;
    if (seg_finish_writer(c, last_frames, err, err_cap) != 0)
        return -1;
    if (last_seg < 0)
        last_seg = (int)((c->flushed - (long long)d->trim) /
                         KARAOKE_SEGMENT_FRAMES) - 1;
    int total_segments = last_frames > 0 ? last_seg + 1 : last_seg;
    if (total_segments <= 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.empty_result"));
        return -1;
    }
    c->total_segments = total_segments;
    c->total_frames = (long long)last_seg * KARAOKE_SEGMENT_FRAMES + last_frames;
    c->manifest.total_segments = c->total_segments;
    c->manifest.source_frames = c->total_frames;
    /* 末段可能短于 661500 帧：在 total/source 落盘之前，磁盘上的旧 manifest
     * 还无法判断它；这里只检查除末段外的段（它们必然是满段），末段已在
     * seg_finish_writer 里原子写 ready。manifest 必须先于"末段可被校验"
     * 落盘，否则 ready 校验会反复判末段未就绪（无限重做）。 */
    int all = 1;
    for (int k = 0; k < last_seg; k++) {
        if (!seg_ready(c, k)) {
            all = 0;
            break;
        }
    }
    c->manifest.complete = all ? 1 : 0;
    if (karaoke_cache_segment_manifest_write(&c->paths, &c->manifest, err,
                                             err_cap) != 0)
        return -1;
    *out_reason = 1;
    return 0;
}

/* 统计已就绪段数（total_known>0 时全扫；否则数从 0 开始的前缀）。 */
static int seg_count_ready(WSegCtx *c, int total_known)
{
    if (total_known > 0) {
        int n = 0;
        for (int k = 0; k < total_known; k++)
            if (seg_ready(c, k))
                n++;
        return n;
    }
    int k = 0;
    while (k < KARAOKE_SEGMENT_MAX_COUNT && seg_ready(c, k))
        k++;
    return k;
}

static void seg_note_status(WSegCtx *c, int phase)
{
    int total_known = c->total_segments;
    int first_ready = seg_ready(c, 0);
    int done = seg_count_ready(c, total_known);
    worker_note_segments(done, total_known, first_ready, phase);
}

/* v2 作业入口：成功返回 0（全部段就绪、manifest complete=1）。 */
static int job_run_v2(WJob *j, char *err, size_t err_cap)
{
    const KaraokeWorkerJob *req = &j->req;
    const KaraokeModelSpec *spec = karaoke_model_by_id(req->model_id);
    if (!spec) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.unsupported_model_fmt"), req->model_id);
        return -1;
    }
    if (req->model_sha256[0] == '\0' ||
        strcmp(req->model_sha256, spec->sha256) != 0) {
        set_err(err, err_cap, i18n_get("karaoke.worker.err.model_sha_mismatch"));
        return -1;
    }
    /* S2：同 v1，完整 SHA256 校验放后台 worker；缓存命中后每次 job 仅 stat。 */
    if (karaoke_model_verify_file_cached(spec, req->model_path, err, err_cap) != 0)
        return -1;
    char session_root[KARAOKE_CACHE_PATH_CAP];
    if (karaoke_cache_session_ensure(session_root, sizeof(session_root), err,
                                     err_cap) != 0)
        return -1;

    memset(&j->cache_cfg, 0, sizeof(j->cache_cfg));
    j->cache_cfg.root = session_root;
    j->cache_cfg.model_id = spec->id;
    j->cache_cfg.model_sha256 = spec->sha256;
    j->cache_cfg.overlap = req->overlap > 0.0f ? req->overlap : KARAOKE_CACHE_OVERLAP;
    j->cache_cfg.soft_inst_mix =
        req->soft_inst_mix > 0.0f ? req->soft_inst_mix : KARAOKE_CACHE_SOFT_INST_MIX;
    j->cache_cfg.limit_mb = 0;

    if (decoder_open(&j->dec, req->track_path, &j->cancelled, err, err_cap) != 0)
        return -1;
    /* 时长上限 + 进度估算（与 v1 相同口径）。 */
    int64_t track_dur = 0;
    {
        int64_t dur = 0;
        AVStream *st = j->dec.fmt->streams[j->dec.stream_index];
        if (st->duration > 0 && st->time_base.den > 0)
            dur = av_rescale_q(st->duration, st->time_base, (AVRational){1, 1});
        else if (j->dec.fmt->duration > 0)
            dur = j->dec.fmt->duration / AV_TIME_BASE;
        if (dur > KARAOKE_WORKER_MAX_SECONDS) {
            set_err(err, err_cap, i18n_get("karaoke.worker.err.too_long_fmt"),
                    (long long)dur, KARAOKE_WORKER_MAX_SECONDS);
            return -1;
        }
        track_dur = dur;
        j->chunks_total_est = 0;
        if (dur > 0) {
            long long n44_est = (long long)((double)dur * KARAOKE_MODEL_SAMPLE_RATE);
            long long step = (long long)((double)(spec->hop * (spec->dim_t - 1)) *
                                         (1.0 - (double)j->cache_cfg.overlap));
            if (step > 0)
                j->chunks_total_est = (n44_est + 2 * (long long)spec->n_fft) / step + 1;
        }
    }
    if (karaoke_model_runtime_available(err, err_cap) != 0)
        return -1;
    int saved_stderr = -1;
    quiet_begin(&saved_stderr);
    j->session = karaoke_model_session_open(spec, req->model_path, 0, err, err_cap);
    quiet_end(saved_stderr);
    if (!j->session)
        return -1;
    if (wdsp_init(&j->dsp, spec, err, err_cap) != 0)
        return -1;

    WSegCtx c;
    memset(&c, 0, sizeof(c));
    c.j = j;
    c.cur_seg = -1;
    memset(&c.cfg, 0, sizeof(c.cfg));
    c.cfg = j->cache_cfg;
    if (karaoke_cache_segment_paths(&c.cfg, req->track_path, &c.paths, err,
                                    err_cap) != 0)
        return -1;
    if (karaoke_cache_segment_mkdir(&c.paths, err, err_cap) != 0)
        return -1;

    /* S1：预算淘汰的保护集合 = 本次 job 曲目 + 请求方指定的当前播放曲目
     * （预取下一曲时 audio.c 会填 protect_track，避免删掉正在播放的段）。 */
    c.exclude_dirs[0] = c.paths.dir;
    c.exclude_count = 1;
    char protect_dir[KARAOKE_CACHE_PATH_CAP] = {0};
    if (req->protect_track[0] &&
        strcmp(req->protect_track, req->track_path) != 0) {
        KaraokeSegmentPaths protect_paths;
        if (karaoke_cache_segment_paths(&c.cfg, req->protect_track,
                                        &protect_paths, NULL, 0) == 0) {
            snprintf(protect_dir, sizeof(protect_dir), "%s", protect_paths.dir);
            c.exclude_dirs[1] = protect_dir;
            c.exclude_count = 2;
        }
    }
    /* 整曲空间预检：/tmp 可能是小 tmpfs；按保守下界一次确认容得下，
     * 不足时先淘汰其它曲目段，仍不足则提前失败（回退原声，不白跑模型）。 */
    if (track_dur > 0) {
        long long est = (long long)track_dur * KARAOKE_SEGMENT_EST_BYTES_PER_SEC;
        if (karaoke_cache_segment_reserve(&c.cfg, c.exclude_dirs,
                                          c.exclude_count, est, err,
                                          err_cap) != 0)
            return -1;
    }

    /* manifest：能读到旧的就保留已完成的 total/source（上次 job 到过 EOF）。 */
    memset(&c.manifest, 0, sizeof(c.manifest));
    c.manifest.format = KARAOKE_SEGMENT_FORMAT_VERSION;
    snprintf(c.manifest.model_id, sizeof(c.manifest.model_id), "%s", spec->id);
    snprintf(c.manifest.model_sha256, sizeof(c.manifest.model_sha256), "%s",
             spec->sha256);
    c.manifest.overlap = c.cfg.overlap;
    c.manifest.soft_inst = c.cfg.soft_inst_mix;
    c.manifest.sample_rate = KARAOKE_SEGMENT_SAMPLE_RATE;
    c.manifest.segment_seconds = KARAOKE_SEGMENT_SECONDS;
    c.manifest.total_segments = 0;
    c.manifest.source_frames = 0;
    c.manifest.complete = 0;
    {
        KaraokeSegmentManifest old;
        if (karaoke_cache_segment_manifest_read(c.paths.manifest_path, &old, NULL,
                                                0) == 0 &&
            old.format == KARAOKE_SEGMENT_FORMAT_VERSION &&
            strcmp(old.model_id, spec->id) == 0 &&
            strcmp(old.model_sha256, spec->sha256) == 0 && old.total_segments > 0 &&
            old.source_frames > 0) {
            c.manifest.total_segments = old.total_segments;
            c.manifest.source_frames = old.source_frames;
            c.total_segments = old.total_segments;
            c.total_frames = old.source_frames;
        }
    }
    if (karaoke_cache_segment_manifest_write(&c.paths, &c.manifest, err,
                                             err_cap) != 0)
        return -1;
    seg_note_status(&c, KARAOKE_PHASE_FORWARD);

    int cursor = 0;
    int phase = KARAOKE_PHASE_FORWARD;
    for (;;) {
        if (job_check_cancel(j, err, err_cap) != 0)
            return -1;
        if (c.total_segments > 0) {
            int missing = -1;
            for (int k = 0; k < c.total_segments; k++) {
                if (!seg_ready(&c, k)) {
                    missing = k;
                    break;
                }
            }
            if (missing < 0)
                break;   /* 全部完成 */
            cursor = missing;
        } else {
            /* 还不知道总段数：从 cursor 起找第一个未就绪段。 */
            while (cursor < KARAOKE_SEGMENT_MAX_COUNT && seg_ready(&c, cursor))
                cursor++;
            if (cursor >= KARAOKE_SEGMENT_MAX_COUNT) {
                set_err(err, err_cap, i18n_get("karaoke.worker.err.segment_limit"));
                return -1;
            }
        }
        int target = cursor;
        unsigned hgen = 0;
        int hint = worker_hint_take(req->track_path, &hgen);
        if (hint >= 0 &&
            (c.total_segments <= 0 || hint < c.total_segments) &&
            !seg_ready(&c, hint))
            target = hint;
        int reason = 0;
        if (c.total_segments > 0 && target >= c.total_segments)
            target = cursor;
        if (seg_produce_from(&c, target, &reason, err, err_cap) != 0) {
            seg_abandon_writer(&c);
            return -1;
        }
        if (reason == 1) {
            /* EOF：总段数/总帧数已知；若还有缺失段则进入回填阶段。 */
            cursor = 0;
            phase = KARAOKE_PHASE_BACKFILL;
        } else {
            cursor = target + 1;
        }
        seg_note_status(&c, phase);
    }

    /* 回填完成后重写 manifest（complete=1 必须等全部段 ready 才可见）。 */
    c.manifest.total_segments = c.total_segments;
    c.manifest.source_frames = c.total_frames;
    c.manifest.complete = 1;
    {
        int all = 1;
        for (int k = 0; k < c.total_segments; k++) {
            if (!seg_ready(&c, k)) {
                all = 0;
                break;
            }
        }
        if (!all) {
            c.manifest.complete = 0;
            set_err(err, err_cap, i18n_get("karaoke.worker.err.internal_segment_set"));
            return -1;
        }
    }
    if (karaoke_cache_segment_manifest_write(&c.paths, &c.manifest, err,
                                             err_cap) != 0)
        return -1;
    seg_note_status(&c, KARAOKE_PHASE_COMPLETE);
    log_info("karaoke", "segments ready: %s (%d segs, %lld frames @44.1k)",
             req->track_path, c.total_segments, c.total_frames);
    return 0;
}

/* ================================================================
 * 8. worker 线程与状态
 * ================================================================ */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static int g_thread_started = 0;
static int g_shutdown = 0;
static int g_pending_valid = 0;
static KaraokeWorkerJob g_pending;
static volatile int g_cancel_gen = 0;
static unsigned g_job_gen = 0;
static volatile int *g_running_cancel = NULL;   /* 指向当前 job 的取消旗标 */
static KaraokeWorkerStatus g_status;
static KaraokeWorkerStatusCb g_status_cb = NULL;

static unsigned worker_cancel_generation(void)
{
    return (unsigned)__atomic_load_n(&g_cancel_gen, __ATOMIC_ACQUIRE);
}

static void status_reset_locked(void)
{
    g_status.state = KARAOKE_JOB_IDLE;
    g_status.percent = 0;
    g_status.chunks_done = 0;
    g_status.chunks_total = 0;
    g_status.segments_done = 0;
    g_status.segments_total = 0;
    g_status.first_segment_ready = 0;
    g_status.phase = KARAOKE_PHASE_NONE;
    g_status.track_path[0] = '\0';
    g_status.error[0] = '\0';
}

static void worker_note_progress(long long done, long long total)
{
    pthread_mutex_lock(&g_lock);
    if (g_status.state == KARAOKE_JOB_RUNNING) {
        g_status.chunks_done = done;
        g_status.chunks_total = total;
        int pct = 0;
        if (total > 0) {
            pct = (int)(done * 100 / (total + 1));
            if (pct > 99)
                pct = 99;
        }
        g_status.percent = pct;
    }
    pthread_mutex_unlock(&g_lock);
}

/* v2：段进度快照（segments_done/total/first_ready/phase）。 */
static void worker_note_segments(int done, int total, int first_ready, int phase)
{
    pthread_mutex_lock(&g_lock);
    if (g_status.state == KARAOKE_JOB_RUNNING) {
        g_status.segments_done = done;
        g_status.segments_total = total;
        g_status.first_segment_ready = first_ready;
        g_status.phase = phase;
    }
    pthread_mutex_unlock(&g_lock);
}

/* ── seek 优先提示（D4）：只对当前正在跑同一曲目的 job 生效 ── */
static int g_hint_seg = -1;
static unsigned g_hint_gen = 0;
static char g_hint_track[KARAOKE_WORKER_PATH_CAP];

static int worker_hint_take(const char *track_path, unsigned *out_gen)
{
    if (!track_path || !track_path[0])
        return -1;
    int seg = -1;
    pthread_mutex_lock(&g_lock);
    if (g_hint_seg >= 0 && g_status.state == KARAOKE_JOB_RUNNING &&
        strcmp(g_hint_track, track_path) == 0) {
        seg = g_hint_seg;
        if (out_gen)
            *out_gen = g_hint_gen;
    }
    pthread_mutex_unlock(&g_lock);
    return seg;
}

int karaoke_worker_hint_segment(const char *track_path, int segment)
{
    if (!track_path || !track_path[0] || segment < 0)
        return -1;
    pthread_mutex_lock(&g_lock);
    if (g_status.state == KARAOKE_JOB_RUNNING &&
        strcmp(g_status.track_path, track_path) == 0) {
        if (segment != g_hint_seg || strcmp(g_hint_track, track_path) != 0) {
            g_hint_seg = segment;
            snprintf(g_hint_track, sizeof(g_hint_track), "%s", track_path);
            __atomic_add_fetch(&g_hint_gen, 1, __ATOMIC_ACQ_REL);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void karaoke_worker_init(void)
{
    pthread_mutex_lock(&g_lock);
    status_reset_locked();
    g_status.completed_jobs = 0;
    g_status.cancelled_jobs = 0;
    g_status.failed_jobs = 0;
    g_status.chunks_done = 0;
    g_status.chunks_total = 0;
    g_status.segments_done = 0;
    g_status.segments_total = 0;
    g_status.first_segment_ready = 0;
    g_status.phase = KARAOKE_PHASE_NONE;
    g_hint_seg = -1;
    g_hint_gen = 0;
    g_hint_track[0] = '\0';
    g_pending_valid = 0;
    g_shutdown = 0;
    pthread_mutex_unlock(&g_lock);
}

/* 在锁外调用状态回调（回调方可能再取 core 的锁，避免锁序问题）。 */
static void worker_notify(int state, const char *message)
{
    pthread_mutex_lock(&g_lock);
    KaraokeWorkerStatusCb cb = g_status_cb;
    pthread_mutex_unlock(&g_lock);
    if (cb)
        cb(state, message ? message : "");
}

void karaoke_worker_set_status_callback(KaraokeWorkerStatusCb cb)
{
    pthread_mutex_lock(&g_lock);
    g_status_cb = cb;
    pthread_mutex_unlock(&g_lock);
}

static void *worker_main(void *arg)
{
    (void)arg;
    /* 低优先级：模型推理/FLAC 编码不能和播放线程抢 CPU（Linux nice 按线程生效）。 */
    (void)setpriority(PRIO_PROCESS, 0, 10);
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_pending_valid && !g_shutdown)
            pthread_cond_wait(&g_cond, &g_lock);
        if (g_shutdown) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        KaraokeWorkerJob job = g_pending;
        g_pending_valid = 0;
        unsigned gen = worker_cancel_generation();
        g_job_gen = gen;
        status_reset_locked();
        g_hint_seg = -1;
        g_hint_gen = 0;
        g_hint_track[0] = '\0';
        g_status.state = KARAOKE_JOB_RUNNING;
        g_status.chunks_total = 0;
        snprintf(g_status.track_path, sizeof(g_status.track_path), "%s", job.track_path);
        pthread_mutex_unlock(&g_lock);
        worker_notify(KARAOKE_JOB_RUNNING, "");

        WJob *j = calloc(1, sizeof(*j));
        char err[WORKER_ERR_CAP] = {0};
        int rc = -1;
        int cancelled_flag = 0;
        if (j) {
            j->req = job;
            pthread_mutex_lock(&g_lock);
            g_running_cancel = &j->cancelled;
            pthread_mutex_unlock(&g_lock);
            j->generation = gen;
            j->cache_cfg.root = job.cache_root;
            j->cache_cfg.model_id = job.model_id;
            j->cache_cfg.model_sha256 = job.model_sha256;
            j->cache_cfg.overlap = job.overlap > 0.0f ? job.overlap : KARAOKE_CACHE_OVERLAP;
            j->cache_cfg.soft_inst_mix =
                job.soft_inst_mix > 0.0f ? job.soft_inst_mix
                                         : KARAOKE_CACHE_SOFT_INST_MIX;
            j->cache_cfg.limit_mb = KARAOKE_CACHE_LIMIT_MB;
            if (job.format == KARAOKE_JOB_FORMAT_SEGMENTS_V2) {
                rc = job_run_v2(j, err, sizeof(err));
            } else if (job.format == KARAOKE_JOB_FORMAT_LEGACY_V1) {
                rc = job_run_v1(j, err, sizeof(err));
            } else {
                /* R2：未指定/未知 format 不允许静默落到某条写路径。 */
                set_err(err, sizeof(err), i18n_get("karaoke.worker.err.job_format_fmt"), job.format);
                rc = -1;
            }
            cancelled_flag = j->cancelled;
            /* 必须在释放 j 之前摘掉取消旗标指针：request/cancel 可能在
             * 清理窗口里写 *g_running_cancel（否则 use-after-free）。 */
            pthread_mutex_lock(&g_lock);
            g_running_cancel = NULL;
            pthread_mutex_unlock(&g_lock);
            if (rc != 0) {
                if (job.format != KARAOKE_JOB_FORMAT_SEGMENTS_V2) {
                    karaoke_cache_abort_temps(j->inst_tmp, j->soft_tmp,
                                              j->meta_tmp);
                    if (j->paths.dir[0] &&
                        !karaoke_cache_complete(&j->cache_cfg, job.track_path))
                        (void)karaoke_cache_remove(&j->cache_cfg, job.track_path);
                }
                /* v2：半成品段没有 ready 标记，天然不可见；保留已就绪段，
                 * 下次 request 会从缺失段继续（不产生"坏缓存"）。 */
            }
            job_cleanup(j);
            free(j);
            j = NULL;
            (void)cancelled_flag;
        } else {
            set_err(err, sizeof(err), i18n_get("karaoke.worker.err.oom_fmt"), "worker job");
        }

        pthread_mutex_lock(&g_lock);
        /* 取消判定只认显式旗标/世代：绝不能靠匹配错误文案（i18n 后
         * 文案随语言变化，英文界面下会漏判）。 */
        int was_cancelled = cancelled_flag ||
                            gen != worker_cancel_generation();
        if (rc == 0) {
            g_status.state = KARAOKE_JOB_READY;
            g_status.percent = 100;
            g_status.completed_jobs++;
            g_status.error[0] = '\0';
        } else if (was_cancelled) {
            g_status.state = KARAOKE_JOB_CANCELLED;
            g_status.cancelled_jobs++;
            snprintf(g_status.error, sizeof(g_status.error), "%s", err);
        } else {
            g_status.state = KARAOKE_JOB_FAILED;
            g_status.failed_jobs++;
            snprintf(g_status.error, sizeof(g_status.error), "%s", err);
            log_warn("karaoke", "job failed for '%s': %s", job.track_path, err);
        }
        snprintf(g_status.track_path, sizeof(g_status.track_path), "%s", job.track_path);
        if (rc == 0 && job.format == KARAOKE_JOB_FORMAT_SEGMENTS_V2) {
            g_status.first_segment_ready = 1;
            g_status.phase = KARAOKE_PHASE_COMPLETE;
        }
        int final_state = g_status.state;
        char final_msg[WORKER_ERR_CAP];
        snprintf(final_msg, sizeof(final_msg), "%s", g_status.error);
        pthread_mutex_unlock(&g_lock);
        worker_notify(final_state, final_msg);
    }
    return NULL;
}

int karaoke_worker_request(const KaraokeWorkerJob *job)
{
    if (!job || !job->track_path[0] || !job->model_id[0] || !job->model_path[0] ||
        !job->cache_root[0] || !job->model_sha256[0])
        return -1;
    if (job->format != KARAOKE_JOB_FORMAT_SEGMENTS_V2 &&
        job->format != KARAOKE_JOB_FORMAT_LEGACY_V1) {
        log_warn("karaoke", "worker request rejected: unspecified/invalid format=%d",
                 (int)job->format);
        return -1;
    }
    /* 已完成 → 不需要再跑模型（"第二次起播不触发第二次推理"）。
     * v2：查 /tmp 会话分段缓存；v1：仅旧版兼容夹具。 */
    {
        KaraokeCacheConfig done_cfg;
        memset(&done_cfg, 0, sizeof(done_cfg));
        done_cfg.model_id = job->model_id;
        done_cfg.model_sha256 = job->model_sha256;
        done_cfg.overlap = job->overlap > 0.0f ? job->overlap : KARAOKE_CACHE_OVERLAP;
        done_cfg.soft_inst_mix =
            job->soft_inst_mix > 0.0f ? job->soft_inst_mix : KARAOKE_CACHE_SOFT_INST_MIX;
        if (job->format == KARAOKE_JOB_FORMAT_SEGMENTS_V2) {
            char session_root[KARAOKE_CACHE_PATH_CAP];
            if (karaoke_cache_session_root(session_root, sizeof(session_root)) == 0) {
                done_cfg.root = session_root;
                done_cfg.limit_mb = 0;
                if (karaoke_cache_segment_complete(&done_cfg, job->track_path))
                    return 0;
            }
        } else {
            done_cfg.root = job->cache_root;
            done_cfg.limit_mb = KARAOKE_CACHE_LIMIT_MB;
            if (karaoke_cache_complete(&done_cfg, job->track_path))
                return 0;
        }
    }
    pthread_mutex_lock(&g_lock);
    if (g_shutdown) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    /* 同一曲目/模型：正在跑或已有待跑请求 → 不重复触发。 */
    if (g_status.state == KARAOKE_JOB_RUNNING &&
        strcmp(g_status.track_path, job->track_path) == 0) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (g_pending_valid && strcmp(g_pending.track_path, job->track_path) == 0) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_pending = *job;
    g_pending_valid = 1;
    if (g_running_cancel)
        *g_running_cancel = 1;
    __atomic_add_fetch(&g_cancel_gen, 1, __ATOMIC_ACQ_REL);   /* 取消旧 job */
    if (!g_thread_started) {
        if (pthread_create(&g_thread, NULL, worker_main, NULL) != 0) {
            g_pending_valid = 0;
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
        g_thread_started = 1;
    }
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void karaoke_worker_cancel(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_running_cancel)
        *g_running_cancel = 1;
    g_hint_seg = -1;
    g_hint_gen = 0;
    g_hint_track[0] = '\0';
    __atomic_add_fetch(&g_cancel_gen, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_unlock(&g_lock);
}

void karaoke_worker_shutdown(void)
{
    pthread_t th;
    int have = 0;
    pthread_mutex_lock(&g_lock);
    g_shutdown = 1;
    g_pending_valid = 0;
    g_hint_seg = -1;
    g_hint_gen = 0;
    g_hint_track[0] = '\0';
    if (g_running_cancel)
        *g_running_cancel = 1;
    __atomic_add_fetch(&g_cancel_gen, 1, __ATOMIC_ACQ_REL);
    pthread_cond_signal(&g_cond);
    if (g_thread_started) {
        th = g_thread;
        have = 1;
        g_thread_started = 0;
    }
    pthread_mutex_unlock(&g_lock);
    if (have) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        int rc = pthread_timedjoin_np(th, NULL, &ts);
        if (rc == ETIMEDOUT) {
            /* 一次 chunk 推理可能还在跑：不阻塞进程退出，交给 OS 回收。 */
            pthread_detach(th);
            log_warn("karaoke", "worker shutdown timed out; detached");
        }
    }
}

int karaoke_worker_busy(void)
{
    pthread_mutex_lock(&g_lock);
    int busy = g_status.state == KARAOKE_JOB_RUNNING;
    pthread_mutex_unlock(&g_lock);
    return busy;
}

int karaoke_worker_status(KaraokeWorkerStatus *out)
{
    if (!out)
        return -1;
    pthread_mutex_lock(&g_lock);
    *out = g_status;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int karaoke_worker_current_track(char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    pthread_mutex_lock(&g_lock);
    if (g_status.track_path[0] && g_status.state == KARAOKE_JOB_RUNNING)
        snprintf(out, cap, "%s", g_status.track_path);
    pthread_mutex_unlock(&g_lock);
    return out[0] ? 1 : 0;
}
