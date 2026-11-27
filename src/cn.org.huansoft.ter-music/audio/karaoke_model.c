/**
 * @file karaoke_model.c
 * @brief 可选 ONNX 模型消人声 — 模型表、校验/安装、dlopen 运行时会话与单段分离
 *
 * 结构（自上而下）：
 *   1. 受支持模型表（静态只读，唯一事实来源）
 *   2. SHA-256（自带实现，避免为 30MiB 校验引入新依赖）
 *   3. ONNX Runtime 装载：静态构建直接调用 OrtGetApiBase()（官方包内置），
 *      动态构建运行期 dlopen（编译期零链接，缺库时功能不可用）
 *   4. 会话：CPU EP + DisableCpuMemArena + 线程钳制 + I/O 形状校验
 *   5. DSP：周期 Hann STFT → 逐 chunk 推理 → iSTFT(Σw² 归一) → 对称 Hann
 *      交叉淡化（逐字复刻 UVR 5.6 的 MDX-Net 语义，见团队研究 REPORT §2.3）
 *   6. 校验 / 原子安装
 *
 * 关键实现事实（为什么这么做，改之前先读）：
 *   - **只用 CPU EP**：Debian 的 DNNL provider 在本机 `AppendExecutionProvider_Dnnl`
 *     直接段错误（139），不碰。
 *   - **DisableCpuMemArena**：默认 arena 峰值 1.11GiB，关掉后 0.68GiB（慢 2–8%）。
 *   - STFT/FFT **自带实现**，不依赖 libavutil。原因（实测，别再踩）：
 *     本机 libavutil 58.29 的 `AV_TX_FLOAT_RDFT` 初始化在任何长度都栈上踩
 *     内存（n=8/2048/6144 全部 stack smashing）；`AV_TX_FLOAT_FFT` 对
 *     n_fft=6144（= 3×2048，走 PFA 分解）返回的是 *置换顺序* 的频点（实部
 *     逐点对得上、虚部整体错位），不能直接当 rfft 用。这里用固定分解
 *     N = 3 × M（M=2048 为 2 的幂）的混合基 Cooley-Tukey：3 个 2048 点基 2
 *     FFT + 3 点 DFT 组合，数学与 numpy/torch 的 rfft/irfft 逐点一致。
 *     逆变换未归一化，最后除以 n_fft。
 *   - 频谱只保留前 dim_f（2048）个频点（0–14.7kHz），>14.7kHz 内容不参与
 *     人声预测、原样留在伴奏里，这是上游 MDX 的设计，不是 bug。
 *   - Debian 的 libonnxruntime 会在建会话时向 stderr 打约 116KB 的
 *     `Schema error: ... already registered ...` 噪音；本文件不劫持进程 fd，
 *     调用方（worker）必须自行重定向/排空 stderr（见 karaoke_model.h）。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#include "audio/karaoke_model.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ONNX Runtime C API 头文件是 vendored 的（MIT，v1.21.0，见同目录 NOTICE.md）。
 * 用相对路径而不是 target include dir：本文件既要进 CMake 构建，也要被
 * scripts/test/run-unit-tests.sh 以仓库根为工作目录单独编译，相对包含在两种
 * 构建下都稳定。 */
#include "../../../include/third_party/onnxruntime/onnxruntime_c_api.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 单元测试（scripts/test/run-unit-tests.sh）直接编译本文件时不带 CMake 的
 * -D，这里给空定义：空数据目录等于"跳过随包安装位置"，只走旧用户 XDG 回退，
 * 不会让测试构建失败。生产构建由 CMakeLists.txt 注入真实安装前缀。 */
#ifndef TER_MUSIC_DATA_DIR
#define TER_MUSIC_DATA_DIR ""
#endif

/* ================================================================
 * 1. 受支持模型表
 * ================================================================ */

/* UVR（Ultimate Vocal Remover GUI）官方 model_data.json 的条目：
 *   {"compensate": 1.035, "mdx_dim_f_set": 2048, "mdx_dim_t_set": 8,
 *    "mdx_n_fft_scale_set": 6144, "primary_stem": "Vocals"}
 * 其中 dim_t = 8 * 32 = 256（UVR 里 mdx_dim_t_set 是"秒"的因子，实际帧数
 * 由 dim_t_set * 32 得到），n_fft=6144、hop=n_fft/6=1024。
 * UVR model hash 是**文件末 10MiB 的 MD5**（不是整文件），这里只用于和上游
 * 对照；应用内校验用整文件 SHA256。 */
static const KaraokeModelSpec g_models[] = {
    {
        .id            = "uvr-mdxnet-3-9662",
        .display_name  = "UVR-MDX-NET 3",
        .url           = "https://github.com/TRvlvr/model_repo/releases/download/"
                         "all_public_uvr_models/UVR_MDXNET_3_9662.onnx",
        .sha256        = "e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc",
        .uvr_hash      = "d7bff498db9324db933d913388cba6be",
        .size          = 29704436LL,
        .n_fft         = 6144,
        .hop           = 1024,
        .dim_f         = 2048,
        .dim_t         = 256,
        .compensate    = 1.035f,
        .primary_stem  = KARAOKE_STEM_VOCALS,
        .input_name    = "input",
        .output_name   = "output",
        .input_rank    = 4,
        .input_channels = 4,   /* [L.re, L.im, R.re, R.im] */
        .batch_dynamic = 1,
        .license       = "MIT (model weights); bundled unmodified as a data file "
                         "with this GPL application under the MIT notice terms; "
                         "see resources/models/NOTICE.md",
        .credit        = "Vocal separation model: UVR-MDX-NET 3, from Ultimate Vocal "
                         "Remover GUI (UVR) by Anjok07, TRvlvr and contributors - "
                         "MIT, used with credit.",
    },
};

#define KARAOKE_MODEL_COUNT (sizeof(g_models) / sizeof(g_models[0]))

const KaraokeModelSpec *karaoke_model_supported(size_t *count)
{
    if (count)
        *count = KARAOKE_MODEL_COUNT;
    return g_models;
}

const KaraokeModelSpec *karaoke_model_by_id(const char *id)
{
    if (!id || !*id)
        return NULL;
    for (size_t i = 0; i < KARAOKE_MODEL_COUNT; i++) {
        if (strcmp(g_models[i].id, id) == 0)
            return &g_models[i];
    }
    return NULL;
}

/* 大小写不敏感的 64 位 hex 比较（表里是小写；用户/文件来源可能给大写）。 */
static int hex64_equal_ci(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    for (int i = 0; i < 64; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (!ca || !cb)
            return 0;
        if (ca >= 'A' && ca <= 'F')
            ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'F')
            cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
    }
    return a[64] == '\0' && b[64] == '\0';
}

const KaraokeModelSpec *karaoke_model_by_sha256(const char *sha256_hex)
{
    if (!sha256_hex || strlen(sha256_hex) != 64)
        return NULL;
    for (size_t i = 0; i < KARAOKE_MODEL_COUNT; i++) {
        if (hex64_equal_ci(g_models[i].sha256, sha256_hex))
            return &g_models[i];
    }
    return NULL;
}

/* ================================================================
 * 2. 错误文本与通用小工具
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

/* 有界字符串追加（strncat 的长度参数在接近满缓冲时会下溢，别直接用）。 */
static void append_str(char *dst, size_t cap, const char *src)
{
    size_t d = strlen(dst);
    if (d + 1 >= cap)
        return;
    strncat(dst, src, cap - d - 1);
}

static void *xaligned(size_t bytes)
{
    if (bytes == 0)
        return NULL;
    void *p = NULL;
    if (posix_memalign(&p, 64, bytes) != 0)
        return NULL;
    memset(p, 0, bytes);
    return p;
}

/* ================================================================
 * 3. SHA-256（自包含实现；只用于模型文件校验，不用于安全敏感场景）
 * ================================================================ */

typedef struct {
    uint32_t state[8];
    uint64_t total_bits;
    unsigned char block[64];
    size_t block_len;
} Sha256Ctx;

static const uint32_t g_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t ror32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(Sha256Ctx *ctx, const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + g_sha256_k[i] + w[i];
        uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->total_bits = 0;
    ctx->block_len = 0;
}

static void sha256_update(Sha256Ctx *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    ctx->total_bits += (uint64_t)len * 8u;
    if (ctx->block_len) {
        size_t take = 64 - ctx->block_len;
        if (take > len)
            take = len;
        memcpy(ctx->block + ctx->block_len, p, take);
        ctx->block_len += take;
        p += take;
        len -= take;
        if (ctx->block_len == 64) {
            sha256_block(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
    while (len >= 64) {
        sha256_block(ctx, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(ctx->block, p, len);
        ctx->block_len = len;
    }
}

static void sha256_final_hex(Sha256Ctx *ctx, char out_hex[65])
{
    uint64_t bits = ctx->total_bits;
    unsigned char pad[72];
    size_t pad_len = (ctx->block_len < 56) ? (56 - ctx->block_len) : (120 - ctx->block_len);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++)
        pad[pad_len + (size_t)i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(ctx, pad, pad_len + 8);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        uint32_t v = ctx->state[i];
        for (int j = 0; j < 4; j++) {
            unsigned char byte = (unsigned char)(v >> (24 - 8 * j));
            out_hex[i * 8 + j * 2] = hexd[byte >> 4];
            out_hex[i * 8 + j * 2 + 1] = hexd[byte & 0x0f];
        }
    }
    out_hex[64] = '\0';
}

/* 计算文件 SHA256；顺带返回字节数（out_size 可空）。 */
static int sha256_file(const char *path, char out_hex[65], long long *out_size,
                       char *err, size_t err_cap)
{
    if (!path || !*path) {
        set_err(err, err_cap, "模型文件路径为空");
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_err(err, err_cap, "无法打开模型文件 %s：%s", path, strerror(errno));
        return -1;
    }
    Sha256Ctx ctx;
    sha256_init(&ctx);
    unsigned char *buf = malloc(256 * 1024);
    if (!buf) {
        close(fd);
        set_err(err, err_cap, "内存不足（SHA256 缓冲）");
        return -1;
    }
    long long total = 0;
    int rc = 0;
    for (;;) {
        ssize_t n = read(fd, buf, 256 * 1024);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            set_err(err, err_cap, "读取模型文件失败 %s：%s", path, strerror(errno));
            rc = -1;
            break;
        }
        if (n == 0)
            break;
        sha256_update(&ctx, buf, (size_t)n);
        total += n;
    }
    free(buf);
    close(fd);
    if (rc != 0)
        return -1;
    sha256_final_hex(&ctx, out_hex);
    if (out_size)
        *out_size = total;
    return 0;
}

int karaoke_model_sha256_file(const char *path, char out_hex[65],
                              char *err, size_t err_cap)
{
    if (!out_hex) {
        set_err(err, err_cap, "输出缓冲为空");
        return -1;
    }
    return sha256_file(path, out_hex, NULL, err, err_cap);
}

/* ================================================================
 * 4. ONNX Runtime 装载
 *
 * 两种构建方式，行为对调用方完全一致（只有"运行库名"文案不同）：
 *   静态（-DTER_MUSIC_ONNXRUNTIME_STATIC=ON，官方 deb/rpm/AppImage）：
 *     ORT 静态链接进 ter-music，直接调用 OrtGetApiBase()，不碰 dlopen。
 *     发布矩阵里的 Debian 10/11/12 与 EL7–10 都没有系统 libonnxruntime，
 *     内置模型只有配合静态运行库才真正"装得上就能消音"。
 *   动态（默认，源码构建/开发机）：编译期零链接，运行期 dlopen
 *     libonnxruntime.so.1.21 -> .so.1 -> .so；缺库时本功能不可用但不影响
 *     原声播放（可读提示见 karaoke_model_runtime_available 的 err）。
 * ================================================================ */

#ifdef TER_MUSIC_ONNXRUNTIME_STATIC
/* 静态链接：ORT C API 的入口在 onnxruntime_c_api.h 里已声明（ORT_EXPORT 在
 * Linux 为空、NO_EXCEPTION 在 C 下为空）。这里再显式 extern 一次，配合注释
 * 让"静态构建直接调用、不进 dlopen"这件事在代码里一眼可见。 */
extern const OrtApiBase *ORT_API_CALL OrtGetApiBase(void);

static pthread_once_t g_ort_once = PTHREAD_ONCE_INIT;
static int g_ort_state = -1;              /* 1=可用，-1=不可用（pthread_once 后只读） */
static const OrtApi *g_ort = NULL;
static char g_ort_lib[64];
static char g_ort_version[64];
static char g_ort_err[KARAOKE_MODEL_ERR_CAP];

static void ort_load_once(void)
{
    const OrtApiBase *base = OrtGetApiBase();
    if (!base || !base->GetApi || !base->GetVersionString) {
        g_ort_state = -1;
        snprintf(g_ort_err, sizeof(g_ort_err),
                 "内置 ONNX Runtime 静态库异常：OrtGetApiBase() 返回的 API 表无效"
                 "（二进制可能链接到了不完整的 libonnxruntime.a）");
        return;
    }
    /* 头部按 ORT_API_VERSION(21) 编译；静态库更旧时向下索取，只用到 1.16 之前
     * 早已存在的稳定函数，结构体是追加式扩展。 */
    const OrtApi *api = NULL;
    for (int v = ORT_API_VERSION; v >= 16; v--) {
        api = base->GetApi((uint32_t)v);
        if (api)
            break;
    }
    if (!api) {
        g_ort_state = -1;
        snprintf(g_ort_err, sizeof(g_ort_err),
                 "内置 ONNX Runtime 静态库版本不兼容：GetApi(%d..16) 全部不可用",
                 ORT_API_VERSION);
        return;
    }
    const char *ver = base->GetVersionString();
    snprintf(g_ort_lib, sizeof(g_ort_lib), "%s", "static (linked into ter-music)");
    snprintf(g_ort_version, sizeof(g_ort_version), "%s", ver ? ver : "unknown");
    g_ort = api;
    g_ort_state = 1;
}

#else /* !TER_MUSIC_ONNXRUNTIME_STATIC — 运行期 dlopen，编译期零链接 */

typedef const OrtApiBase *(ORT_API_CALL *OrtGetApiBaseFn)(void);

static pthread_once_t g_ort_once = PTHREAD_ONCE_INIT;
static int g_ort_state = -1;              /* 1=可用，-1=不可用（pthread_once 后只读） */
static void *g_ort_handle = NULL;
static const OrtApi *g_ort = NULL;
static char g_ort_lib[64];
static char g_ort_version[64];
static char g_ort_err[KARAOKE_MODEL_ERR_CAP];

static void ort_load_once(void)
{
    /* 顺序：优先精确 soname（Debian/Deepin 只有 .so.1.21），再按常见约定回退。
     * 都找不到即"功能不可用"，不向用户报错。 */
    static const char *const names[] = {
        "libonnxruntime.so.1.21",
        "libonnxruntime.so.1",
        "libonnxruntime.so",
    };
    char detail[256] = {0};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            const char *e = dlerror();
            if (e) {
                append_str(detail, sizeof(detail), e);
                append_str(detail, sizeof(detail), "; ");
            }
            continue;
        }
        OrtGetApiBaseFn get_base = (OrtGetApiBaseFn)dlsym(h, "OrtGetApiBase");
        if (!get_base) {
            dlclose(h);
            append_str(detail, sizeof(detail), "OrtGetApiBase not found; ");
            continue;
        }
        const OrtApiBase *base = get_base();
        if (!base || !base->GetApi || !base->GetVersionString) {
            dlclose(h);
            append_str(detail, sizeof(detail), "OrtApiBase invalid; ");
            continue;
        }
        /* 头部按 ORT_API_VERSION(21) 编译；运行库旧于头文件时向下索取，
         * 只用到 1.21 之前早已存在的稳定函数，结构体是追加式扩展。 */
        const OrtApi *api = NULL;
        for (int v = ORT_API_VERSION; v >= 16; v--) {
            api = base->GetApi((uint32_t)v);
            if (api)
                break;
        }
        if (!api) {
            dlclose(h);
            append_str(detail, sizeof(detail), "GetApi(21..16) unavailable; ");
            continue;
        }
        const char *ver = base->GetVersionString();
        snprintf(g_ort_lib, sizeof(g_ort_lib), "%s", names[i]);
        snprintf(g_ort_version, sizeof(g_ort_version), "%s", ver ? ver : "unknown");
        g_ort_handle = h;
        g_ort = api;
        g_ort_state = 1;
        return;
    }
    g_ort_state = -1;
    snprintf(g_ort_err, sizeof(g_ort_err),
             "ONNX Runtime 运行库不可用（尝试 libonnxruntime.so.1.21 / .so.1 / .so）：%s",
             detail[0] ? detail : "dlopen 失败且无错误信息");
}

#endif /* TER_MUSIC_ONNXRUNTIME_STATIC */

static void ensure_ort_loaded(void)
{
    pthread_once(&g_ort_once, ort_load_once);
}

int karaoke_model_runtime_available(char *err, size_t err_cap)
{
    ensure_ort_loaded();
    if (g_ort_state == 1)
        return 0;
    if (err && err_cap)
        snprintf(err, err_cap, "%s", g_ort_err);
    return -1;
}

const char *karaoke_model_runtime_library(void)
{
    ensure_ort_loaded();
    return (g_ort_state == 1) ? g_ort_lib : NULL;
}

const char *karaoke_model_runtime_version(void)
{
    ensure_ort_loaded();
    return (g_ort_state == 1) ? g_ort_version : NULL;
}

/* ORT 调用失败 → 统一错误文本（并释放 OrtStatus）。返回 0=无错，-1=有错。 */
static int ort_status_fail(const OrtApi *api, OrtStatus *st, const char *what,
                           char *err, size_t err_cap)
{
    if (!st)
        return 0;
    const char *msg = api->GetErrorMessage ? api->GetErrorMessage(st) : NULL;
    set_err(err, err_cap, "%s：%s", what, msg ? msg : "(no message)");
    api->ReleaseStatus(st);
    return -1;
}

/* ================================================================
 * 4.5 FFT：N = 3 × M（M 为 2 的幂）的混合基 Cooley-Tukey
 *
 * 为什么自己写：见文件头 —— 本机 libavutil 的 tx 实现对这个长度的 rfft/FFT
 * 不可用（RDFT 踩栈、PFA 复杂 FFT 返回置换顺序）。自实现后本模块只依赖
 * libc/libm/pthread/dl，构建与单测都不再需要 libavutil。
 *
 * 分解：N = 3M，取 n = n1 + 3·n2（n1∈[0,3)，n2∈[0,M)），则
 *   X[m·M + k'] = Σ_{n1} e^{−2πi·n1·m/3} · W_N^{n1·k'} · Y_{n1}[k']
 *   其中 Y_{n1}[k'] = FFT_M{x[n1+3·n2]}[k']
 * 逆变换：先对每个 k' 做 3 点逆 DFT 与 W_N 共轭旋转，再 3 个 M 点逆 FFT，
 * 最后整体除以 N（与未归一化正变换配对）。
 *
 * 本模型 N=6144、M=2048；dim_f=2048 恰为 M，故正变换只算 m=0 的组合。
 * ================================================================ */

typedef struct {
    float re, im;
} KmCplx;

typedef struct {
    int n;                /* N = 3×M（表项 n_fft） */
    int m;                /* M：2 的幂 */
    float *cos_n;         /* N/2：cos(2πj/N) */
    float *sin_n;         /* N/2：sin(2πj/N) */
    float *cos_m;         /* M/2：cos(2πj/M) */
    float *sin_m;         /* M/2：sin(2πj/M) */
    int   *bitrev;        /* M */
    KmCplx *sub;          /* 3×M：子序列 → 子 FFT → 子逆 FFT */
    KmCplx *full;         /* N：逆变换用的完整（共轭对称）频谱 */
    KmCplx *wn;           /* 2×M：W_N^{k'} 与 W_N^{2k'}，k'∈[0,M) */
    KmCplx w3[3][3];      /* e^{−2πi·n1·m/3} */
    KmCplx w3_inv[3][3];  /* e^{+2πi·n1·m/3} */
} KmFftPlan;

/* ================================================================
 * 5. 会话
 * ================================================================ */

struct KaraokeModelSession {
    const KaraokeModelSpec *spec;
    char path[KARAOKE_MODEL_PATH_CAP];
    OrtEnv *env;
    OrtSessionOptions *options;
    OrtSession *session;
    OrtMemoryInfo *mem_info;
    float *in_patch;
    float *out_patch;
    size_t patch_elems;
    int threads;

    /* DSP 暂存：第一次 separate() 时分配（validate/open 不付这份内存）。 */
    int dsp_ready;
    KmFftPlan *fft;
    float *frame;    /* n_fft：加窗分析帧 / 逆变换输出 */
    KmCplx *bins;    /* dim_f：一帧的频点 */
    float *window;   /* n_fft 点周期 Hann */
    float *part;     /* 2 × chunk_size（按声道平面存放） */
    float *ola;      /* 2 × (chunk_size + n_fft)（交织） */
    float *wsum;     /* chunk_size + n_fft */
};

size_t karaoke_model_patch_elems(const KaraokeModelSpec *spec)
{
    if (!spec || spec->dim_f <= 0 || spec->dim_t <= 0)
        return 0;
    return (size_t)spec->input_channels * (size_t)spec->dim_f * (size_t)spec->dim_t;
}

size_t karaoke_model_chunk_frames(const KaraokeModelSpec *spec)
{
    if (!spec || spec->hop <= 0 || spec->dim_t <= 1)
        return 0;
    return (size_t)spec->hop * (size_t)(spec->dim_t - 1);
}

/* 线程数：≤0 用默认 min(4, nproc-1)，至少 1；上限 nproc。 */
static int normalize_threads(int threads)
{
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    int max_threads = (nproc > 1) ? (int)(nproc > 64 ? 64 : nproc) : 1;
    if (threads <= 0) {
        int def = KARAOKE_MODEL_THREADS_CAP;
        if (max_threads - 1 < def)
            def = max_threads - 1;
        threads = (def < 1) ? 1 : def;
    }
    if (threads > max_threads)
        threads = max_threads;
    return threads;
}

/* 读取并校验某一路 I/O：名字、float32、形状 [?,4,dim_f,dim_t]。 */
static int check_io_tensor(const OrtApi *api, OrtSession *session, int is_input,
                           const KaraokeModelSpec *spec, char *err, size_t err_cap)
{
    const char *what = is_input ? "输入" : "输出";
    const char *want_name = is_input ? spec->input_name : spec->output_name;
    OrtTypeInfo *type_info = NULL;
    OrtStatus *st = NULL;
    OrtAllocator *alloc = NULL;
    char *name = NULL;
    int rc = -1;

    st = api->GetAllocatorWithDefaultOptions(&alloc);
    if (ort_status_fail(api, st, "取默认分配器失败", err, err_cap))
        return -1;

    if (is_input)
        st = api->SessionGetInputName(session, 0, alloc, &name);
    else
        st = api->SessionGetOutputName(session, 0, alloc, &name);
    if (ort_status_fail(api, st, "读取 ONNX 输入/输出名失败", err, err_cap))
        goto done;
    if (!name || strcmp(name, want_name) != 0) {
        set_err(err, err_cap, "ONNX %s名不符：期望 \"%s\"，实际 \"%s\"",
                what, want_name, name ? name : "(null)");
        goto done;
    }

    if (is_input)
        st = api->SessionGetInputTypeInfo(session, 0, &type_info);
    else
        st = api->SessionGetOutputTypeInfo(session, 0, &type_info);
    if (ort_status_fail(api, st, "读取 ONNX 输入/输出类型失败", err, err_cap))
        goto done;

    const OrtTensorTypeAndShapeInfo *ts = NULL;
    st = api->CastTypeInfoToTensorInfo(type_info, &ts);
    if (ort_status_fail(api, st, "ONNX 类型不是张量", err, err_cap))
        goto done;
    if (!ts) {
        set_err(err, err_cap, "ONNX %s类型不是张量", what);
        goto done;
    }

    ONNXTensorElementDataType elem = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    st = api->GetTensorElementType(ts, &elem);
    if (ort_status_fail(api, st, "读取 ONNX 元素类型失败", err, err_cap))
        goto done;
    if (elem != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        set_err(err, err_cap, "ONNX %s元素类型不是 float32（实际 %d）", what, (int)elem);
        goto done;
    }

    size_t rank = 0;
    st = api->GetDimensionsCount(ts, &rank);
    if (ort_status_fail(api, st, "读取 ONNX 形状失败", err, err_cap))
        goto done;
    if (rank != (size_t)spec->input_rank) {
        set_err(err, err_cap, "ONNX %s秩不符：期望 %d，实际 %zu",
                what, spec->input_rank, rank);
        goto done;
    }
    int64_t dims[8];
    if (rank > sizeof(dims) / sizeof(dims[0])) {
        set_err(err, err_cap, "ONNX %s秩过大（%zu）", what, rank);
        goto done;
    }
    st = api->GetDimensions(ts, dims, rank);
    if (ort_status_fail(api, st, "读取 ONNX 维度失败", err, err_cap))
        goto done;

    /* dims[0] 动态时 ONNX Runtime 报 -1；只接受动态或 1（batch 固定 1 推理）。 */
    int dim0_ok = (dims[0] <= 0) || (dims[0] == 1);
    int shape_ok = dim0_ok && dims[1] == spec->input_channels &&
                   dims[2] == spec->dim_f && dims[3] == spec->dim_t;
    if (!shape_ok) {
        set_err(err, err_cap,
                "ONNX %s形状不符：期望 [%s,%d,%d,%d]，实际 [%lld,%lld,%lld,%lld]",
                what, spec->batch_dynamic ? "?" : "1",
                spec->input_channels, spec->dim_f, spec->dim_t,
                (long long)dims[0], (long long)dims[1],
                (long long)dims[2], (long long)dims[3]);
        goto done;
    }
    rc = 0;

done:
    if (name && alloc) {
        OrtStatus *free_st = api->AllocatorFree(alloc, name);
        if (free_st)
            api->ReleaseStatus(free_st);
    }
    if (type_info)
        api->ReleaseTypeInfo(type_info);
    return rc;
}

static int check_session_io(const OrtApi *api, OrtSession *session,
                            const KaraokeModelSpec *spec, char *err, size_t err_cap)
{
    size_t in_count = 0, out_count = 0;
    OrtStatus *st = api->SessionGetInputCount(session, &in_count);
    if (ort_status_fail(api, st, "读取 ONNX 输入数量失败", err, err_cap))
        return -1;
    st = api->SessionGetOutputCount(session, &out_count);
    if (ort_status_fail(api, st, "读取 ONNX 输出数量失败", err, err_cap))
        return -1;
    if (in_count != 1 || out_count != 1) {
        set_err(err, err_cap, "ONNX 模型应有 1 个输入/1 个输出，实际 %zu/%zu",
                in_count, out_count);
        return -1;
    }
    if (check_io_tensor(api, session, 1, spec, err, err_cap) != 0)
        return -1;
    return check_io_tensor(api, session, 0, spec, err, err_cap);
}

/* ---------- FFT 计划（N = 3×M，M 为 2 的幂） ---------- */

static int fft_plan_init(KmFftPlan **out, int n_fft, char *err, size_t err_cap)
{
    *out = NULL;
    if (n_fft <= 0 || (n_fft % 3) != 0) {
        set_err(err, err_cap, "不支持的 n_fft=%d（本实现要求 N=3×2^k）", n_fft);
        return -1;
    }
    int m = n_fft / 3;
    if (m < 2 || (m & (m - 1)) != 0) {
        set_err(err, err_cap, "不支持的 n_fft=%d（N/3 必须是 2 的幂）", n_fft);
        return -1;
    }
    KmFftPlan *p = calloc(1, sizeof(*p));
    if (!p) {
        set_err(err, err_cap, "内存不足（FFT 计划）");
        return -1;
    }
    p->n = n_fft;
    p->m = m;
    p->cos_n = malloc(sizeof(float) * (size_t)(n_fft / 2));
    p->sin_n = malloc(sizeof(float) * (size_t)(n_fft / 2));
    p->cos_m = malloc(sizeof(float) * (size_t)(m / 2));
    p->sin_m = malloc(sizeof(float) * (size_t)(m / 2));
    p->bitrev = malloc(sizeof(int) * (size_t)m);
    p->sub = malloc(sizeof(KmCplx) * (size_t)m * 3);
    p->full = malloc(sizeof(KmCplx) * (size_t)n_fft);
    p->wn = malloc(sizeof(KmCplx) * (size_t)m * 2);
    if (!p->cos_n || !p->sin_n || !p->cos_m || !p->sin_m || !p->bitrev ||
        !p->sub || !p->full || !p->wn) {
        set_err(err, err_cap, "内存不足（FFT 表）");
        goto fail;
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
    /* 3 点 DFT 系数：w3[n1][mm] = e^{−2πi·n1·mm/3} */
    for (int n1 = 0; n1 < 3; n1++) {
        for (int mm = 0; mm < 3; mm++) {
            double ang = -2.0 * M_PI * (double)(n1 * mm) / 3.0;
            p->w3[n1][mm].re = (float)cos(ang);
            p->w3[n1][mm].im = (float)sin(ang);
            p->w3_inv[n1][mm].re = (float)cos(-ang);
            p->w3_inv[n1][mm].im = (float)sin(-ang);
        }
    }
    /* bit-reversal 表（M 点基 2 FFT） */
    int bits = 0;
    while ((1 << bits) < m)
        bits++;
    for (int i = 0; i < m; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            r |= ((i >> b) & 1) << (bits - 1 - b);
        p->bitrev[i] = r;
    }
    *out = p;
    return 0;
fail:
    free(p->cos_n);
    free(p->sin_n);
    free(p->cos_m);
    free(p->sin_m);
    free(p->bitrev);
    free(p->sub);
    free(p->full);
    free(p->wn);
    free(p);
    return -1;
}

static void fft_plan_free(KmFftPlan *p)
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
    free(p);
}

/* M 点基 2 FFT（就地，未归一化）；inverse 用共轭旋转因子。 */
static void fft_radix2_m(const KmFftPlan *p, KmCplx *a, int inverse)
{
    const int n = p->m;
    for (int i = 0; i < n; i++) {
        int j = p->bitrev[i];
        if (j > i) {
            KmCplx t = a[i];
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
                const KmCplx u = a[base + j];
                const KmCplx v = a[base + j + half];
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

/* 实序列 x[0..N-1] → 前 dim_f 个频点（numpy.rfft 语义）。 */
static void fft_forward(const KmFftPlan *p, const float *x, KmCplx *out_bins,
                        int dim_f)
{
    const int m = p->m;
    const int n = p->n;
    for (int n1 = 0; n1 < 3; n1++) {
        KmCplx *a = p->sub + (size_t)n1 * (size_t)m;
        for (int n2 = 0; n2 < m; n2++) {
            a[n2].re = x[(size_t)n1 + (size_t)3 * (size_t)n2];
            a[n2].im = 0.0f;
        }
        fft_radix2_m(p, a, 0);
    }
    for (int k = 0; k < dim_f && k < n; k++) {
        const int kp = k % m;
        const int mm = k / m;
        float re = 0.0f, im = 0.0f;
        for (int n1 = 0; n1 < 3; n1++) {
            const KmCplx y = p->sub[(size_t)n1 * (size_t)m + (size_t)kp];
            if (n1 == 0) {
                re += y.re;
                im += y.im;
                continue;
            }
            const KmCplx tw = p->wn[(size_t)(n1 - 1) * (size_t)m + (size_t)kp];
            /* W_N^{n1·k} = w3[n1][mm] · W_N^{n1·k'} */
            const KmCplx w3v = p->w3[n1][mm];
            const float tr = w3v.re * tw.re - w3v.im * tw.im;
            const float ti = w3v.re * tw.im + w3v.im * tw.re;
            re += tr * y.re - ti * y.im;
            im += tr * y.im + ti * y.re;
        }
        out_bins[k].re = re;
        out_bins[k].im = im;
    }
}

/* 前 dim_f 个频点（其余按共轭对称/补零）→ 实序列 x[0..N-1]（已除以 N）。 */
static void fft_inverse(KmFftPlan *p, const KmCplx *bins, int dim_f, float *out_x)
{
    const int m = p->m;
    const int n = p->n;
    const int half = n / 2;
    KmCplx *full = p->full;

    for (int k = 0; k <= half; k++) {
        KmCplx v = {0.0f, 0.0f};
        if (k < dim_f)
            v = bins[k];
        if (k == 0 || k == half)
            v.im = 0.0f;   /* 实序列的 DC/Nyquist 只有实部（irfft 语义） */
        full[k] = v;
    }
    for (int k = half + 1; k < n; k++) {
        const KmCplx v = full[n - k];
        full[k].re = v.re;
        full[k].im = -v.im;
    }
    for (int kp = 0; kp < m; kp++) {
        const KmCplx x0 = full[kp];
        const KmCplx x1 = full[m + kp];
        const KmCplx x2 = full[2 * m + kp];
        for (int n1 = 0; n1 < 3; n1++) {
            float zr = 0.0f, zi = 0.0f;
            const KmCplx *w = p->w3_inv[n1];
            zr = w[0].re * x0.re - w[0].im * x0.im;
            zi = w[0].re * x0.im + w[0].im * x0.re;
            zr += w[1].re * x1.re - w[1].im * x1.im;
            zi += w[1].re * x1.im + w[1].im * x1.re;
            zr += w[2].re * x2.re - w[2].im * x2.im;
            zi += w[2].re * x2.im + w[2].im * x2.re;
            if (n1 != 0) {
                const KmCplx tw = p->wn[(size_t)(n1 - 1) * (size_t)m + (size_t)kp];
                /* 乘以 conj(W_N^{n1·k'}) */
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
        fft_radix2_m(p, p->sub + (size_t)n1 * (size_t)m, 1);
    const float inv_n = 1.0f / (float)n;
    for (int n1 = 0; n1 < 3; n1++) {
        const KmCplx *a = p->sub + (size_t)n1 * (size_t)m;
        for (int n2 = 0; n2 < m; n2++)
            out_x[(size_t)n1 + (size_t)3 * (size_t)n2] = a[n2].re * inv_n;
    }
}

static void session_free_dsp(KaraokeModelSession *s)
{
    if (!s)
        return;
    fft_plan_free(s->fft);
    free(s->frame);
    free(s->bins);
    free(s->window);
    free(s->part);
    free(s->ola);
    free(s->wsum);
    s->fft = NULL;
    s->frame = NULL;
    s->bins = NULL;
    s->window = NULL;
    s->part = NULL;
    s->ola = NULL;
    s->wsum = NULL;
    s->dsp_ready = 0;
}

void karaoke_model_session_close(KaraokeModelSession *s)
{
    if (!s)
        return;
    session_free_dsp(s);
    if (g_ort) {
        if (s->session)
            g_ort->ReleaseSession(s->session);
        if (s->mem_info)
            g_ort->ReleaseMemoryInfo(s->mem_info);
        if (s->options)
            g_ort->ReleaseSessionOptions(s->options);
        if (s->env)
            g_ort->ReleaseEnv(s->env);
    }
    free(s->in_patch);
    free(s->out_patch);
    free(s);
}

KaraokeModelSession *karaoke_model_session_open(const KaraokeModelSpec *spec,
                                                const char *model_path,
                                                int threads,
                                                char *err, size_t err_cap)
{
    char rt_err[KARAOKE_MODEL_ERR_CAP];
    if (!spec) {
        set_err(err, err_cap, "模型表项为空");
        return NULL;
    }
    if (!model_path || !*model_path) {
        set_err(err, err_cap, "模型路径为空");
        return NULL;
    }
    if (karaoke_model_runtime_available(rt_err, sizeof(rt_err)) != 0) {
        set_err(err, err_cap, "ONNX Runtime 不可用：%s", rt_err);
        return NULL;
    }
    const OrtApi *api = g_ort;
    KaraokeModelSession *s = calloc(1, sizeof(*s));
    if (!s) {
        set_err(err, err_cap, "内存不足（会话）");
        return NULL;
    }
    s->spec = spec;
    s->threads = normalize_threads(threads);
    snprintf(s->path, sizeof(s->path), "%s", model_path);

    OrtStatus *st = api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "ter-music-karaoke-model",
                                   &s->env);
    if (ort_status_fail(api, st, "创建 ONNX 环境失败", err, err_cap))
        goto fail;
    st = api->CreateSessionOptions(&s->options);
    if (ort_status_fail(api, st, "创建 ONNX 会话选项失败", err, err_cap))
        goto fail;
    st = api->SetIntraOpNumThreads(s->options, s->threads);
    if (ort_status_fail(api, st, "设置推理线程数失败", err, err_cap))
        goto fail;
    st = api->SetInterOpNumThreads(s->options, 1);
    if (ort_status_fail(api, st, "设置算子间线程数失败", err, err_cap))
        goto fail;
    st = api->SetSessionGraphOptimizationLevel(s->options, ORT_ENABLE_ALL);
    if (ort_status_fail(api, st, "设置图优化级别失败", err, err_cap))
        goto fail;
    st = api->SetSessionLogSeverityLevel(s->options, ORT_LOGGING_LEVEL_ERROR);
    if (ort_status_fail(api, st, "设置会话日志级别失败", err, err_cap))
        goto fail;
    /* 峰值内存 1.11GiB → 0.68GiB；代价是推理慢 2–8%（可接受）。 */
    st = api->DisableCpuMemArena(s->options);
    if (ort_status_fail(api, st, "关闭 CPU 内存 arena 失败", err, err_cap))
        goto fail;

    st = api->CreateSession(s->env, model_path, s->options, &s->session);
    if (ort_status_fail(api, st, "创建 ONNX 会话失败（模型损坏或运行库不兼容）",
                        err, err_cap))
        goto fail;
    if (check_session_io(api, s->session, spec, err, err_cap) != 0)
        goto fail;

    st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &s->mem_info);
    if (ort_status_fail(api, st, "创建 CPU 内存信息失败", err, err_cap))
        goto fail;

    s->patch_elems = karaoke_model_patch_elems(spec);
    if (s->patch_elems == 0) {
        set_err(err, err_cap, "模型表项参数非法（patch 为空）");
        goto fail;
    }
    s->in_patch = (float *)xaligned(s->patch_elems * sizeof(float));
    s->out_patch = (float *)xaligned(s->patch_elems * sizeof(float));
    if (!s->in_patch || !s->out_patch) {
        set_err(err, err_cap, "内存不足（模型输入/输出缓冲 %zu 个 float）",
                s->patch_elems);
        goto fail;
    }
    return s;

fail:
    karaoke_model_session_close(s);
    return NULL;
}

const KaraokeModelSpec *karaoke_model_session_spec(const KaraokeModelSession *s)
{
    return s ? s->spec : NULL;
}

int karaoke_model_session_threads(const KaraokeModelSession *s)
{
    return s ? s->threads : 0;
}

/* ---------- 单 patch 推理 ---------- */

static int run_patch(KaraokeModelSession *s, const float *in_patch, float *out_patch,
                     char *err, size_t err_cap)
{
    const KaraokeModelSpec *spec = s->spec;
    const OrtApi *api = g_ort;
    size_t bytes = s->patch_elems * sizeof(float);
    int64_t dims[4];
    dims[0] = 1;
    dims[1] = spec->input_channels;
    dims[2] = spec->dim_f;
    dims[3] = spec->dim_t;

    OrtValue *in_val = NULL;
    OrtValue *out_val = NULL;
    OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
        s->mem_info, (void *)in_patch, bytes, dims, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_val);
    if (ort_status_fail(api, st, "包装 ONNX 输入张量失败", err, err_cap))
        return -1;

    const char *in_names[1] = {spec->input_name};
    const char *out_names[1] = {spec->output_name};
    st = api->Run(s->session, NULL, in_names, (const OrtValue *const *)&in_val, 1,
                  out_names, 1, &out_val);
    api->ReleaseValue(in_val);
    if (ort_status_fail(api, st, "ONNX 推理失败", err, err_cap))
        return -1;

    void *out_data = NULL;
    st = api->GetTensorMutableData(out_val, &out_data);
    if (ort_status_fail(api, st, "读取 ONNX 输出数据失败", err, err_cap)) {
        api->ReleaseValue(out_val);
        return -1;
    }
    if (out_patch)
        memcpy(out_patch, out_data, bytes);
    api->ReleaseValue(out_val);
    return 0;
}

int karaoke_model_infer_patch(KaraokeModelSession *s,
                              const float *in_patch, float *out_patch,
                              char *err, size_t err_cap)
{
    if (!s) {
        set_err(err, err_cap, "会话为空");
        return -1;
    }
    if (!in_patch || !out_patch) {
        set_err(err, err_cap, "输入/输出缓冲为空");
        return -1;
    }
    return run_patch(s, in_patch, out_patch, err, err_cap);
}

/* ================================================================
 * 6. DSP：STFT / iSTFT / 单段分离
 *
 * 逐字复刻 UVR 5.6 MDX-Net（研究报告 §2.3，参考实现
 * .tmp/karaoke-research/bench/dsp_pipeline.py 已用真实模型验证）：
 *   - 分析窗与合成窗都是周期 Hann；
 *   - STFT 前对 chunk 做两端 n_fft/2 的 reflect pad（numpy reflect 语义）；
 *   - 只取前 dim_f 个频点；UVR 固定把第 0/1/2 频点清零后送模型；
 *   - iSTFT 把 dim_f..Nyquist 补零，按 Σw² 归一，裁掉两端 n_fft/2；
 *   - chunk 间用对称 Hann(actual) 交叉淡化并按同一窗归一；
 *   - 结果再去掉全局 trim、截断到原长，×compensate 得人声；
 *   - 伴奏 = mix − compensate × 人声（时域相减）。
 * ================================================================ */

static float period_hann(int n, int n_fft)
{
    return (float)(0.5 - 0.5 * cos(2.0 * M_PI * (double)n / (double)n_fft));
}

/* numpy.hanning(N)：对称 Hann，端点 0（N≤1 时取 1，避免除零；参考实现对
 * N=1 极短末块同样只在交叉淡化里用一次，取 1 与 np 的边界近似等价）。 */
static float sym_hann(size_t i, size_t n)
{
    if (n <= 1)
        return 1.0f;
    return (float)(0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(n - 1)));
}

static int dsp_ensure(KaraokeModelSession *s, char *err, size_t err_cap)
{
    if (s->dsp_ready)
        return 0;
    const KaraokeModelSpec *spec = s->spec;
    const int n_fft = spec->n_fft;
    const int trim = n_fft / 2;
    const size_t chunk = karaoke_model_chunk_frames(spec);
    const size_t cyl = chunk + (size_t)n_fft;   /* iSTFT 输出长度 = (T-1)*hop+n_fft */

    if (n_fft <= 0 || chunk == 0 || trim <= 0 || spec->dim_f <= 0 ||
        spec->dim_f > n_fft) {
        set_err(err, err_cap, "模型表项 FFT 参数非法");
        return -1;
    }
    if (fft_plan_init(&s->fft, n_fft, err, err_cap) != 0)
        return -1;
    s->frame = calloc((size_t)n_fft, sizeof(float));
    s->bins = calloc((size_t)spec->dim_f, sizeof(KmCplx));
    s->window = calloc((size_t)n_fft, sizeof(float));
    s->part = calloc(chunk * 2, sizeof(float));
    s->ola = calloc(cyl * 2, sizeof(float));
    s->wsum = calloc(cyl, sizeof(float));
    if (!s->frame || !s->bins || !s->window || !s->part || !s->ola || !s->wsum) {
        set_err(err, err_cap, "内存不足（DSP 暂存）");
        session_free_dsp(s);
        return -1;
    }
    for (int i = 0; i < n_fft; i++)
        s->window[i] = period_hann(i, n_fft);
    s->dsp_ready = 1;
    return 0;
}

/* reflect pad 索引映射：把「pad 后数组」下标映射到「原始 part」下标。
 * numpy.pad(mode="reflect") 不重复边缘样本：
 *   左 pad k∈[0,trim) → part[trim-k]；右 pad r → part[chunk-2-r]。 */
static inline size_t reflect_index(size_t x, size_t chunk, size_t trim)
{
    if (x < trim)
        return trim - x;
    if (x < trim + chunk)
        return x - trim;
    return chunk - 2 - (x - trim - chunk);
}

/* 对一段 chunk（长度 chunk_size，交织 stereo）做 STFT 填入模型输入 tensor。
 * tensor 布局 [4][dim_f][dim_t]：ch0=L.re, ch1=L.im, ch2=R.re, ch3=R.im。
 * zero_low_bins：UVR 推理前把 0/1/2 频点清零（自检的纯往返不清零）。 */
static void stft_chunk(KaraokeModelSession *s, const float *part, float *patch,
                       int zero_low_bins)
{
    const KaraokeModelSpec *spec = s->spec;
    const int n_fft = spec->n_fft;
    const int hop = spec->hop;
    const int dim_f = spec->dim_f;
    const int dim_t = spec->dim_t;
    const size_t chunk = karaoke_model_chunk_frames(spec);
    const size_t trim = (size_t)n_fft / 2;
    const int first = zero_low_bins ? 3 : 0;

    memset(patch, 0, sizeof(float) * karaoke_model_patch_elems(spec));
    for (int t = 0; t < dim_t; t++) {
        for (int ch = 0; ch < KARAOKE_MODEL_CHANNELS; ch++) {
            for (int j = 0; j < n_fft; j++) {
                size_t idx = reflect_index((size_t)t * (size_t)hop + (size_t)j,
                                           chunk, trim);
                s->frame[j] = part[(size_t)ch * chunk + idx] * s->window[j];
            }
            fft_forward(s->fft, s->frame, s->bins, dim_f);
            const size_t base = ((size_t)ch * 2u) * (size_t)dim_f * (size_t)dim_t;
            for (int f = first; f < dim_f; f++) {
                patch[base + (size_t)f * (size_t)dim_t + (size_t)t] = s->bins[f].re;
                patch[base + (size_t)dim_f * (size_t)dim_t +
                      (size_t)f * (size_t)dim_t + (size_t)t] = s->bins[f].im;
            }
        }
    }
}

/* 模型输出 tensor → 波形：iSTFT（共轭补全 + 3×M 逆变换）+ Σw² 归一。
 * ola/wsum 长度均为 chunk+2*trim = chunk+n_fft。 */
static void istft_chunk(KaraokeModelSession *s, const float *patch,
                        float *ola, float *wsum)
{
    const KaraokeModelSpec *spec = s->spec;
    const int n_fft = spec->n_fft;
    const int hop = spec->hop;
    const int dim_f = spec->dim_f;
    const int dim_t = spec->dim_t;
    const size_t cyl = karaoke_model_chunk_frames(spec) + (size_t)n_fft;

    memset(ola, 0, sizeof(float) * cyl * 2);
    memset(wsum, 0, sizeof(float) * cyl);
    for (int t = 0; t < dim_t; t++) {
        for (int ch = 0; ch < KARAOKE_MODEL_CHANNELS; ch++) {
            const size_t base = ((size_t)ch * 2u) * (size_t)dim_f * (size_t)dim_t;
            for (int f = 0; f < dim_f; f++) {
                s->bins[f].re = patch[base + (size_t)f * (size_t)dim_t + (size_t)t];
                s->bins[f].im = patch[base + (size_t)dim_f * (size_t)dim_t +
                                      (size_t)f * (size_t)dim_t + (size_t)t];
            }
            fft_inverse(s->fft, s->bins, dim_f, s->frame);
            for (int j = 0; j < n_fft; j++) {
                size_t pos = (size_t)t * (size_t)hop + (size_t)j;
                ola[pos * 2 + (size_t)ch] += s->frame[j] * s->window[j];
            }
        }
        for (int j = 0; j < n_fft; j++) {
            size_t pos = (size_t)t * (size_t)hop + (size_t)j;
            float w = s->window[j];
            wsum[pos] += w * w;
        }
    }
    /* Σw² 归一（torch.istft 语义）；无覆盖处保持 0。 */
    for (size_t p = 0; p < cyl; p++) {
        float d = wsum[p];
        if (d > 1e-11f) {
            float inv = 1.0f / d;
            ola[p * 2] *= inv;
            ola[p * 2 + 1] *= inv;
        } else {
            ola[p * 2] = 0.0f;
            ola[p * 2 + 1] = 0.0f;
        }
    }
}

int karaoke_model_stft_selftest(const KaraokeModelSpec *spec, char *err, size_t err_cap)
{
    if (!spec || spec->n_fft <= 0 || spec->hop <= 0 || spec->dim_f <= 0 ||
        spec->dim_t <= 1) {
        set_err(err, err_cap, "模型表项 DSP 参数非法");
        return -1;
    }
    KaraokeModelSession tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.spec = spec;
    if (dsp_ensure(&tmp, err, err_cap) != 0)
        return -1;
    int rc = -1;
    const size_t chunk = karaoke_model_chunk_frames(spec);
    const size_t trim = (size_t)spec->n_fft / 2;
    float *part = (float *)calloc(chunk * 2, sizeof(float));
    float *patch = (float *)calloc(karaoke_model_patch_elems(spec), sizeof(float));
    float *ola = (float *)calloc((chunk + (size_t)spec->n_fft) * 2, sizeof(float));
    float *wsum = (float *)calloc(chunk + (size_t)spec->n_fft, sizeof(float));
    if (!part || !patch || !ola || !wsum) {
        set_err(err, err_cap, "内存不足（DSP 自检）");
        goto done;
    }
    /* 带限测试信号（全部 <14.7kHz，落在 dim_f 内）：双声道不同相位/频率。
     * 不能掺宽带白噪声 —— >14.7kHz 的能量按 MDX 语义会被丢弃，往返必然
     * 有约 1% 的相对误差（实测 1.035e-2），那是模型设计而不是 FFT 的误差。 */
    for (size_t i = 0; i < chunk; i++) {
        double t = (double)i / (double)KARAOKE_MODEL_SAMPLE_RATE;
        double l = 0.30 * sin(2.0 * M_PI * 440.0 * t) +
                   0.20 * sin(2.0 * M_PI * 1200.0 * t + 0.3) +
                   0.10 * sin(2.0 * M_PI * 7000.0 * t);
        double r = 0.30 * sin(2.0 * M_PI * 440.0 * t + 0.7) +
                   0.20 * sin(2.0 * M_PI * 3500.0 * t) +
                   0.10 * sin(2.0 * M_PI * 12000.0 * t + 0.9);
        part[i] = (float)l;                         /* 按声道平面存放 */
        part[chunk + i] = (float)r;
    }
    stft_chunk(&tmp, part, patch, 0);
    istft_chunk(&tmp, patch, ola, wsum);
    /* 只比较内部区域：首/尾 n_fft 个样本会被 reflect pad 的折点影响（折点
     * 在 >14.7kHz 有宽带能量，按 MDX 语义被截掉属预期），那不是 FFT/OLA
     * 数学的误差。内部区域应达到 float32 FFT 量级（实测 ~1e-6）。 */
    double num = 0.0, den = 0.0, peak = 0.0;
    const size_t margin = (size_t)spec->n_fft < chunk / 2 ? (size_t)spec->n_fft
                                                          : chunk / 2;
    for (size_t i = margin; i + margin < chunk; i++) {
        for (int ch = 0; ch < 2; ch++) {
            double got = ola[((trim + i) * 2) + (size_t)ch];
            double want = part[(size_t)ch * chunk + i];
            double d = got - want;
            num += d * d;
            den += want * want;
            if (fabs(d) > peak)
                peak = fabs(d);
        }
    }
    double rms_rel = sqrt(num / (den > 0.0 ? den : 1.0));
    if (rms_rel > 1e-3) {
        set_err(err, err_cap, "STFT/iSTFT 自检失败：相对 RMS 误差 %.3e（阈值 1e-3），峰值 %.3e",
                rms_rel, peak);
        goto done;
    }
    rc = 0;

done:
    free(part);
    free(patch);
    free(ola);
    free(wsum);
    session_free_dsp(&tmp);
    return rc;
}

int karaoke_model_separate(KaraokeModelSession *s,
                           const float *mix, size_t frames, KaraokeStem stem,
                           float *out, char *err, size_t err_cap)
{
    if (!s) {
        set_err(err, err_cap, "会话为空");
        return -1;
    }
    if (!mix || !out) {
        set_err(err, err_cap, "输入/输出缓冲为空");
        return -1;
    }
    if (frames == 0) {
        set_err(err, err_cap, "帧数为 0");
        return -1;
    }
    if (stem != KARAOKE_STEM_VOCALS && stem != KARAOKE_STEM_INSTRUMENTAL) {
        set_err(err, err_cap, "未知的 stem 选择 %d", (int)stem);
        return -1;
    }
    const KaraokeModelSpec *spec = s->spec;
    const int n_fft = spec->n_fft;
    const int trim = n_fft / 2;
    const size_t chunk = karaoke_model_chunk_frames(spec);
    if (chunk == 0 || (size_t)chunk <= (size_t)n_fft) {
        set_err(err, err_cap, "模型表项 chunk/FFT 参数非法");
        return -1;
    }
    const size_t gen = chunk - (size_t)n_fft;
    const size_t step = chunk - (size_t)n_fft;   /* UVR Default overlap */

    if (dsp_ensure(s, err, err_cap) != 0)
        return -1;

    size_t rem = frames % gen;
    size_t pad = gen + (size_t)trim - rem;
    unsigned long long total_ll = (unsigned long long)trim + (unsigned long long)frames +
                                  (unsigned long long)pad;
    if (total_ll > (unsigned long long)SIZE_MAX / (2ull * sizeof(float))) {
        set_err(err, err_cap, "帧数过大（%zu）", frames);
        return -1;
    }
    const size_t total = (size_t)total_ll;

    float *result = (float *)calloc(total * 2, sizeof(float));
    float *divider = (float *)calloc(total, sizeof(float));
    if (!result || !divider) {
        free(result);
        free(divider);
        set_err(err, err_cap, "内存不足（分离结果缓冲：%.1f MiB）",
                (double)(total * 3 * sizeof(float)) / (1024.0 * 1024.0));
        return -1;
    }

    int rc = 0;
    for (size_t start = 0; start < total; start += step) {
        size_t end = start + chunk;
        if (end > total)
            end = total;
        size_t actual = end - start;

        /* part[ch][j] = 补零后的 mixture[start+j]（mixture 前置 trim 个 0）。 */
        for (int ch = 0; ch < KARAOKE_MODEL_CHANNELS; ch++) {
            for (size_t j = 0; j < chunk; j++) {
                long long mp = (long long)start + (long long)j - (long long)trim;
                float v = 0.0f;
                if (mp >= 0 && (unsigned long long)mp < (unsigned long long)frames)
                    v = mix[(size_t)mp * 2 + (size_t)ch];
                s->part[(size_t)ch * chunk + j] = v;
            }
        }
        stft_chunk(s, s->part, s->in_patch, 1);
        if (run_patch(s, s->in_patch, s->out_patch, err, err_cap) != 0) {
            rc = -1;
            break;
        }
        istft_chunk(s, s->out_patch, s->ola, s->wsum);
        for (size_t j = 0; j < actual; j++) {
            float w = sym_hann(j, actual);
            size_t pos = (size_t)trim + j;
            result[(start + j) * 2] += s->ola[pos * 2] * w;
            result[(start + j) * 2 + 1] += s->ola[pos * 2 + 1] * w;
            divider[start + j] += w;
        }
    }

    if (rc == 0) {
        const float comp = spec->compensate;
        for (size_t i = 0; i < frames; i++) {
            const size_t p = (size_t)trim + i;
            float d = divider[p];
            float voc_l = 0.0f, voc_r = 0.0f;
            if (d > 1e-12f) {
                float inv = comp / d;
                voc_l = result[p * 2] * inv;
                voc_r = result[p * 2 + 1] * inv;
            }
            if (stem == KARAOKE_STEM_VOCALS) {
                out[i * 2] = voc_l;
                out[i * 2 + 1] = voc_r;
            } else {
                out[i * 2] = mix[i * 2] - voc_l;
                out[i * 2 + 1] = mix[i * 2 + 1] - voc_r;
            }
        }
    }

    free(result);
    free(divider);
    return rc;
}

/* ================================================================
 * 7. 校验 / 安装
 * ================================================================ */

static int stat_regular_size(const char *path, long long *out_size,
                             char *err, size_t err_cap)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        set_err(err, err_cap, "无法访问模型文件 %s：%s", path, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        set_err(err, err_cap, "不是普通文件：%s", path);
        return -1;
    }
    if (st.st_size < 0) {
        set_err(err, err_cap, "文件大小异常：%s", path);
        return -1;
    }
    *out_size = (long long)st.st_size;
    return 0;
}

int karaoke_model_validate_file(const char *path, const KaraokeModelSpec **out_spec,
                               char *err, size_t err_cap)
{
    if (out_spec)
        *out_spec = NULL;
    if (!path || !*path) {
        set_err(err, err_cap, "模型文件路径为空");
        return -1;
    }
    long long size = 0;
    if (stat_regular_size(path, &size, err, err_cap) != 0)
        return -1;

    const KaraokeModelSpec *spec = NULL;
    for (size_t i = 0; i < KARAOKE_MODEL_COUNT; i++) {
        if (g_models[i].size == size) {
            spec = &g_models[i];
            break;
        }
    }
    if (!spec) {
        set_err(err, err_cap,
                "不支持的模型（大小 %lld 字节，不在受支持表内；受支持项：%s）",
                size, g_models[0].id);
        return -1;
    }

    char hex[65];
    if (sha256_file(path, hex, NULL, err, err_cap) != 0)
        return -1;
    if (!hex64_equal_ci(hex, spec->sha256)) {
        set_err(err, err_cap,
                "不支持的模型（SHA256 不匹配）：实际 %s，期望 %s（%s，%s）",
                hex, spec->sha256, spec->id, spec->url);
        return -1;
    }

    /* SHA256/大小对上了仍要能建会话并核对 I/O 形状，防"伪造同名同大小文件"、
     * 防模型内部结构与表项参数不符。 */
    char sess_err[KARAOKE_MODEL_ERR_CAP];
    KaraokeModelSession *session = karaoke_model_session_open(spec, path, 0,
                                                              sess_err, sizeof(sess_err));
    if (!session) {
        set_err(err, err_cap, "模型可读但无法作为受支持模型加载：%s", sess_err);
        return -1;
    }
    karaoke_model_session_close(session);
    if (out_spec)
        *out_spec = spec;
    return 0;
}

int karaoke_model_installed_path(const KaraokeModelSpec *spec, const char *data_dir,
                                 char *out, size_t cap)
{
    if (!spec || !spec->id || !*spec->id || !data_dir || !*data_dir || !out || cap == 0)
        return -1;
    int n = snprintf(out, cap, "%s/models/%s.onnx", data_dir, spec->id);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

/* ================================================================
 * 8. 内置模型定位（随包分发，config v12）
 *
 * 运行期只认两个位置，顺序固定（见 karaoke_model.h）：
 *   1) <TER_MUSIC_DATA_DIR>/models/<id>.onnx —— 随包安装（首选）
 *   2) <$XDG_DATA_HOME>/ter-music/models/<id>.onnx —— 旧用户手工安装（只读兼容）
 * 只做 stat + SHA256，不建 ONNX 会话：模型校验不能依赖可选的 ONNX Runtime，
 * 否则"库缺失"会被误报成"模型损坏"。
 * ================================================================ */

/* 候选路径是否可读且是普通文件：0=是；1=不存在；-1=存在但不可用（err 已写） */
static int bundled_candidate_state(const char *path, long long *out_size,
                                   char *err, size_t err_cap)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return 1;
        }
        set_err(err, err_cap, "内置模型不可访问：%s（%s）", path, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        set_err(err, err_cap, "内置模型不是普通文件：%s", path);
        return -1;
    }
    if (st.st_size <= 0) {
        set_err(err, err_cap, "内置模型是空文件：%s", path);
        return -1;
    }
    *out_size = (long long)st.st_size;
    return 0;
}

int karaoke_model_locate_bundled(const KaraokeModelSpec *spec,
                                 const char *bundled_data_dir,
                                 const char *legacy_data_dir,
                                 char *out, size_t cap,
                                 char *err, size_t err_cap)
{
    if (out && cap)
        out[0] = '\0';
    if (err && err_cap)
        err[0] = '\0';
    if (!spec || !spec->id || !*spec->id) {
        set_err(err, err_cap, "内置模型表项无效（id 为空）");
        return -1;
    }
    if (!out || cap == 0) {
        set_err(err, err_cap, "内置模型路径缓冲为空");
        return -1;
    }

    const char *dirs[2] = { bundled_data_dir, legacy_data_dir };
    static const char *const dir_labels[2] = {
        "随包数据目录", "旧用户数据目录"
    };
    int tried = 0;
    for (int i = 0; i < 2; i++) {
        const char *dir = dirs[i];
        if (!dir || !dir[0])
            continue;
        if (i == 1 && dirs[0] && strcmp(dirs[0], dir) == 0)
            continue;   /* 两个位置相同：只查一次 */
        tried++;

        char candidate[KARAOKE_MODEL_PATH_CAP];
        if (karaoke_model_installed_path(spec, dir, candidate, sizeof(candidate)) != 0) {
            set_err(err, err_cap, "%s路径过长：%s", dir_labels[i], dir);
            return -1;
        }

        long long size = 0;
        int state = bundled_candidate_state(candidate, &size, err, err_cap);
        if (state == 1)
            continue;   /* 该位置没有：试下一个 */
        if (state < 0)
            return -1;

        /* 文件存在：从这里开始"坏包"必须可见，绝不再静默回退到另一位置。 */
        if (size != spec->size) {
            set_err(err, err_cap,
                    "内置模型校验失败：%s 大小 %lld 字节，期望 %lld 字节（%s）",
                    candidate, size, spec->size, spec->id);
            return -1;
        }
        char hex[65];
        if (sha256_file(candidate, hex, NULL, err, err_cap) != 0)
            return -1;
        if (!hex64_equal_ci(hex, spec->sha256)) {
            set_err(err, err_cap,
                    "内置模型校验失败：%s SHA256 %s，期望 %s（%s，官方 %s）",
                    candidate, hex, spec->sha256, spec->id, spec->url);
            return -1;
        }

        snprintf(out, cap, "%s", candidate);
        return 0;
    }

    /* 两个位置都不存在（或都没给）：明确报"内置模型缺失"。 */
    if (tried == 0) {
        set_err(err, err_cap,
                "内置模型缺失：没有可查的数据目录（编译期数据目录与旧用户数据目录均为空）");
        return -1;
    }
    char checked[2 * KARAOKE_MODEL_PATH_CAP + 128];
    checked[0] = '\0';
    for (int i = 0; i < 2; i++) {
        const char *dir = dirs[i];
        if (!dir || !dir[0])
            continue;
        if (i == 1 && dirs[0] && strcmp(dirs[0], dir) == 0)
            continue;
        if (checked[0])
            append_str(checked, sizeof(checked), "，或 ");
        append_str(checked, sizeof(checked), dir);
        append_str(checked, sizeof(checked), "/models/");
        append_str(checked, sizeof(checked), spec->id);
        append_str(checked, sizeof(checked), ".onnx");
    }
    set_err(err, err_cap,
            "内置模型缺失：未找到 %s（模型应随包安装到 <前缀>/share/ter-music/models/，"
            "运行期不复制到 XDG）", checked);
    return -1;
}

/* 进程内缓存：首次使用时校验一次（30MiB 文件 SHA256 + stat），之后复用结果。
 * 失败也缓存：重复起播不应反复读盘/重复刷错误提示；装好模型后重启生效。 */
static pthread_mutex_t g_bundled_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_bundled_state = 0;    /* 0=未探测 1=可用 -1=不可用 */
static char g_bundled_path_cache[KARAOKE_MODEL_PATH_CAP];
static char g_bundled_err_cache[KARAOKE_MODEL_ERR_CAP];

int karaoke_model_bundled_path(const char *legacy_data_dir,
                               char *out, size_t cap,
                               char *err, size_t err_cap)
{
    if (out && cap)
        out[0] = '\0';
    if (err && err_cap)
        err[0] = '\0';

    pthread_mutex_lock(&g_bundled_lock);
    if (g_bundled_state == 0) {
        const KaraokeModelSpec *spec = karaoke_model_by_id(KARAOKE_MODEL_BUNDLED_ID);
        g_bundled_path_cache[0] = '\0';
        g_bundled_err_cache[0] = '\0';
        if (!spec) {
            /* 表里没有内置 id 属于编译期错误（改表必须同步本文件头常量）。 */
            set_err(g_bundled_err_cache, sizeof(g_bundled_err_cache),
                    "内置模型表缺失：受支持模型表里没有 '%s'",
                    KARAOKE_MODEL_BUNDLED_ID);
            g_bundled_state = -1;
        } else if (karaoke_model_locate_bundled(spec, TER_MUSIC_DATA_DIR,
                                                legacy_data_dir,
                                                g_bundled_path_cache,
                                                sizeof(g_bundled_path_cache),
                                                g_bundled_err_cache,
                                                sizeof(g_bundled_err_cache)) != 0) {
            g_bundled_state = -1;
        } else {
            g_bundled_state = 1;
        }
    }
    int rc = (g_bundled_state == 1) ? 0 : -1;
    if (rc == 0 && out && cap)
        snprintf(out, cap, "%s", g_bundled_path_cache);
    if (rc != 0 && err && err_cap)
        snprintf(err, err_cap, "%s", g_bundled_err_cache);
    pthread_mutex_unlock(&g_bundled_lock);
    return rc;
}

/* 目标文件是否已经是受支持模型的正确副本（大小 + SHA256）。 */
static int installed_copy_valid(const char *path, const KaraokeModelSpec *spec)
{
    long long size = 0;
    char hex[65];
    char scratch[64];
    if (stat_regular_size(path, &size, scratch, sizeof(scratch)) != 0)
        return 0;
    if (size != spec->size)
        return 0;
    if (sha256_file(path, hex, NULL, scratch, sizeof(scratch)) != 0)
        return 0;
    return hex64_equal_ci(hex, spec->sha256);
}

static int mkdir_models(const char *data_dir, char *models_dir, size_t cap,
                        char *err, size_t err_cap)
{
    int n = snprintf(models_dir, cap, "%s/models", data_dir);
    if (n < 0 || (size_t)n >= cap) {
        set_err(err, err_cap, "数据目录路径过长（上限 %zu 字节）", cap);
        return -1;
    }
    if (mkdir(models_dir, 0700) != 0 && errno != EEXIST) {
        set_err(err, err_cap, "无法创建模型目录 %s：%s", models_dir, strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(models_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_err(err, err_cap, "模型目录不可用：%s", models_dir);
        return -1;
    }
    return 0;
}

/* 复制 + 边读边算 SHA256 + fsync + rename + fsync(dir)；失败删除半成品。 */
static int copy_atomic(const char *src, const char *dst, const KaraokeModelSpec *spec,
                       char *err, size_t err_cap)
{
    char dir[KARAOKE_MODEL_PATH_CAP];
    int dn = snprintf(dir, sizeof(dir), "%s", dst);
    if (dn < 0 || (size_t)dn >= sizeof(dir)) {
        set_err(err, err_cap, "目标路径过长");
        return -1;
    }
    char *slash = strrchr(dir, '/');
    const char *base = dst;
    if (slash) {
        *slash = '\0';
        base = slash + 1;
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    char tmp[KARAOKE_MODEL_PATH_CAP + 64];
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
        int tn = snprintf(tmp, sizeof(tmp), "%s/.%s.part.%ld.%d", dir, base,
                          (long)getpid(), attempt);
        if (tn < 0 || (size_t)tn >= sizeof(tmp)) {
            set_err(err, err_cap, "临时文件路径过长");
            return -1;
        }
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) {
            set_err(err, err_cap, "无法创建临时文件 %s：%s", tmp, strerror(errno));
            return -1;
        }
    }
    if (fd < 0) {
        set_err(err, err_cap, "无法创建临时文件（重试 16 次仍冲突）");
        return -1;
    }

    int src_fd = open(src, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) {
        set_err(err, err_cap, "无法打开源模型文件 %s：%s", src, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }

    unsigned char *buf = malloc(256 * 1024);
    if (!buf) {
        set_err(err, err_cap, "内存不足（复制缓冲）");
        close(src_fd);
        close(fd);
        unlink(tmp);
        return -1;
    }
    Sha256Ctx ctx;
    sha256_init(&ctx);
    long long copied = 0;
    int rc = 0;
    for (;;) {
        ssize_t n = read(src_fd, buf, 256 * 1024);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            set_err(err, err_cap, "读取源模型失败：%s", strerror(errno));
            rc = -1;
            break;
        }
        if (n == 0)
            break;
        sha256_update(&ctx, buf, (size_t)n);
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(fd, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                set_err(err, err_cap, "写入模型副本失败：%s", strerror(errno));
                rc = -1;
                break;
            }
            off += w;
        }
        if (rc != 0)
            break;
        copied += n;
    }
    free(buf);
    close(src_fd);

    if (rc == 0) {
        char hex[65];
        sha256_final_hex(&ctx, hex);
        if (copied != spec->size || !hex64_equal_ci(hex, spec->sha256)) {
            set_err(err, err_cap,
                    "复制后校验失败（源文件在校验后被改动？）：大小 %lld/%lld，SHA256 %s",
                    copied, spec->size, hex);
            rc = -1;
        }
    }
    if (rc == 0 && fsync(fd) != 0) {
        set_err(err, err_cap, "fsync 模型副本失败：%s", strerror(errno));
        rc = -1;
    }
    if (close(fd) != 0 && rc == 0) {
        set_err(err, err_cap, "关闭模型副本失败：%s", strerror(errno));
        rc = -1;
    }
    if (rc != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, dst) != 0) {
        set_err(err, err_cap, "安装模型失败（rename）：%s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        (void)fsync(dir_fd);   /* 目录项落盘失败不改变"已安装"结论 */
        close(dir_fd);
    }
    return 0;
}

int karaoke_model_install(const char *src, const char *data_dir,
                          char *out_id, size_t id_cap,
                          char *err, size_t err_cap)
{
    if (out_id && id_cap)
        out_id[0] = '\0';
    if (!data_dir || !*data_dir) {
        set_err(err, err_cap, "数据目录为空（调用方应传 $XDG_DATA_HOME/ter-music）");
        return -1;
    }
    const KaraokeModelSpec *spec = NULL;
    if (karaoke_model_validate_file(src, &spec, err, err_cap) != 0)
        return -1;

    char models_dir[KARAOKE_MODEL_PATH_CAP];
    if (mkdir_models(data_dir, models_dir, sizeof(models_dir), err, err_cap) != 0)
        return -1;

    char dst[KARAOKE_MODEL_PATH_CAP];
    if (karaoke_model_installed_path(spec, data_dir, dst, sizeof(dst)) != 0) {
        set_err(err, err_cap, "安装路径过长（数据目录 %s）", data_dir);
        return -1;
    }

    if (installed_copy_valid(dst, spec)) {
        /* 幂等：已安装正确副本，不做任何覆盖。 */
    } else if (copy_atomic(src, dst, spec, err, err_cap) != 0) {
        return -1;
    }
    if (out_id && id_cap)
        snprintf(out_id, id_cap, "%s", spec->id);
    return 0;
}
