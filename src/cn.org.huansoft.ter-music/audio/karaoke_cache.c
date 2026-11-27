/**
 * @file karaoke_cache.c
 * @brief 可选 ONNX 模型消人声 — 伴奏缓存（成对 soft/full + 原子写 + LRU）
 *
 * 本文件不依赖 FFmpeg/配置/UI：所有路径、模型信息与上限由
 * KaraokeCacheConfig 传入；FLAC 只解析 STREAMINFO 头；元数据在
 * `<track_hash>/meta.txt`（与两份 FLAC 一起原子提交）。
 *
 * 原子性顺序（commit）：校验临时文件 → fsync → rename(inst) →
 * rename(soft) → rename(meta 最后) → fsync(dir)。resolve 要求三件齐全且
 * 元数据/FAC 头互相一致，因此任何时刻崩溃都不会被误判为完整缓存；
 * 残留目录在下一次 resolve/commit 时被清理。
 *
 * 安全边界：删除操作只发生在 cfg->root 子树内（目录名由我们构造）；
 * 不跟随 root 外的符号链接目录（rename/删除前做前缀检查）。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#include "audio/karaoke_cache.h"

#include "audio/karaoke_mode.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ================================================================
 * 1. SHA-256（自包含实现，只用于缓存键；不需要防侧信道）
 *
 * 为什么不复用 audio/karaoke_model.c 的哈希：那个 API 只接受**文件**，
 * 缓存键是对内存字符串（路径/大小/mtime/参数）做哈希。为 200 字节的
 * 材料写一个临时文件再哈希既不原子也没必要，故在此保留一份小实现。
 * ================================================================ */

typedef struct {
    uint32_t state[8];
    uint64_t total_bits;
    unsigned char block[64];
    size_t block_len;
} CacheSha256;

static const uint32_t g_sha_k[64] = {
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

static void sha256_block(CacheSha256 *ctx, const unsigned char *p)
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
        uint32_t t1 = h + s1 + ch + g_sha_k[i] + w[i];
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

static void sha256_init(CacheSha256 *ctx)
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

static void sha256_update(CacheSha256 *ctx, const void *data, size_t len)
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

static void sha256_final_hex(CacheSha256 *ctx, char out_hex[KARAOKE_CACHE_HASH_LEN])
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

/* ================================================================
 * 2. 小工具
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

static int cfg_valid(const KaraokeCacheConfig *cfg)
{
    return cfg && cfg->root && cfg->root[0] && cfg->model_id && cfg->model_id[0] &&
           cfg->model_sha256 && strlen(cfg->model_sha256) == 64;
}

static int cfg_limit_mb(const KaraokeCacheConfig *cfg)
{
    return (cfg && cfg->limit_mb > 0) ? cfg->limit_mb : KARAOKE_CACHE_LIMIT_MB;
}

/* 路径必须在 cfg->root 子树内（防止删除/rename 越界）。 */
static int path_within_root(const KaraokeCacheConfig *cfg, const char *path)
{
    if (!cfg || !cfg->root || !path)
        return 0;
    size_t n = strlen(cfg->root);
    if (strncmp(path, cfg->root, n) != 0)
        return 0;
    return path[n] == '/' || path[n] == '\0';
}

/* mkdir -p（只用于我们构造的 root/<model>/<hash>；逐级 0700）。 */
static int mkdir_p_0700(const char *path, char *err, size_t err_cap)
{
    char tmp[KARAOKE_CACHE_PATH_CAP];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        set_err(err, err_cap, "缓存路径过长");
        return -1;
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
            set_err(err, err_cap, "创建缓存目录 %s 失败：%s", tmp, strerror(errno));
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        set_err(err, err_cap, "创建缓存目录 %s 失败：%s", tmp, strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_err(err, err_cap, "缓存目录不可用：%s", tmp);
        return -1;
    }
    return 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_regular(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* 目录里是否有 worker 正在写的 `.part.` 半成品（F2：绝不删在写目录）。 */
static int dir_has_part_files(const char *dir);

static int dir_has_part_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, ".part.") != NULL) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* 递归删除（仅调用方已确认 path 在 root 子树内）。
 * 用 lstat：符号链接一律只 unlink 自身，绝不跟随（防越界删除）。 */
static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (!d) {
        rmdir(path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[KARAOKE_CACHE_PATH_CAP];
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >=
            (int)sizeof(child))
            continue;
        remove_tree(child);
    }
    closedir(d);
    rmdir(path);
}

static int fsync_path(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

/* ================================================================
 * 3. 路径与键
 * ================================================================ */

int karaoke_cache_default_root(char *out, size_t cap)
{
    if (!out || cap == 0)
        return -1;
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg && xdg[0])
        n = snprintf(out, cap, "%s/ter-music/karaoke", xdg);
    else if (home && home[0])
        n = snprintf(out, cap, "%s/.cache/ter-music/karaoke", home);
    else
        return -1;
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int join_path(char *out, size_t cap, const char *dir, const char *name,
                     char *err, size_t err_cap)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= cap) {
        set_err(err, err_cap, "缓存路径过长");
        return -1;
    }
    return 0;
}

int karaoke_cache_paths(const KaraokeCacheConfig *cfg, const char *track_path,
                        KaraokeCachePaths *out, char *err, size_t err_cap)
{
    if (!cfg_valid(cfg) || !track_path || !track_path[0] || !out) {
        set_err(err, err_cap, "缓存配置/曲目路径非法");
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char real[PATH_MAX];
    if (!realpath(track_path, real)) {
        set_err(err, err_cap, "无法解析曲目真实路径 %s：%s", track_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode)) {
        set_err(err, err_cap, "曲目不是普通文件：%s", real);
        return -1;
    }

    char material[KARAOKE_CACHE_PATH_CAP + 256];
    int n = snprintf(material, sizeof(material),
                     "ter-music-karaoke-v%d\n"
                     "stem=%d\n"
                     "%s\n"
                     "%s\n"
                     "%s\n"
                     "%lld\n"
                     "%lld.%09ld\n"
                     "overlap=%.6f\n"
                     "soft_inst=%.6f\n"
                     "flac=16bit,2ch\n",
                     KARAOKE_CACHE_FORMAT_VERSION, KARAOKE_CACHE_STEM_VERSION,
                     cfg->model_id, cfg->model_sha256, real,
                     (long long)st.st_size,
                     (long long)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec,
                     (double)(cfg->overlap > 0.0f ? cfg->overlap
                                                  : KARAOKE_CACHE_OVERLAP),
                     (double)(cfg->soft_inst_mix > 0.0f
                                  ? cfg->soft_inst_mix
                                  : KARAOKE_CACHE_SOFT_INST_MIX));
    if (n < 0 || (size_t)n >= sizeof(material)) {
        set_err(err, err_cap, "缓存键材料过长");
        return -1;
    }
    CacheSha256 ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, material, (size_t)n);
    sha256_final_hex(&ctx, out->hash);

    int m = snprintf(out->dir, sizeof(out->dir), "%s/%s/%s", cfg->root,
                     cfg->model_id, out->hash);
    if (m < 0 || (size_t)m >= sizeof(out->dir)) {
        set_err(err, err_cap, "缓存目录路径过长");
        return -1;
    }
    if (!path_within_root(cfg, out->dir)) {
        set_err(err, err_cap, "缓存目录越界");
        return -1;
    }
    if (join_path(out->inst_path, sizeof(out->inst_path), out->dir, "inst.flac",
                  err, err_cap) != 0 ||
        join_path(out->soft_path, sizeof(out->soft_path), out->dir, "soft.flac",
                  err, err_cap) != 0 ||
        join_path(out->meta_path, sizeof(out->meta_path), out->dir, "meta.txt",
                  err, err_cap) != 0)
        return -1;
    return 0;
}

int karaoke_cache_begin(const KaraokeCacheConfig *cfg, const char *track_path,
                        KaraokeCachePaths *out,
                        char *inst_tmp, size_t inst_cap,
                        char *soft_tmp, size_t soft_cap,
                        char *meta_tmp, size_t meta_cap,
                        char *err, size_t err_cap)
{
    if (!inst_tmp || !soft_tmp || !meta_tmp) {
        set_err(err, err_cap, "临时文件缓冲为空");
        return -1;
    }
    if (karaoke_cache_paths(cfg, track_path, out, err, err_cap) != 0)
        return -1;

    char model_dir[KARAOKE_CACHE_PATH_CAP];
    int n = snprintf(model_dir, sizeof(model_dir), "%s/%s", cfg->root, cfg->model_id);
    if (n < 0 || (size_t)n >= sizeof(model_dir)) {
        set_err(err, err_cap, "模型缓存目录路径过长");
        return -1;
    }
    if (mkdir_p_0700(cfg->root, err, err_cap) != 0 ||
        mkdir_p_0700(model_dir, err, err_cap) != 0 ||
        mkdir_p_0700(out->dir, err, err_cap) != 0)
        return -1;

    /* 同一进程可能重复 begin（上一 job 已取消/失败）：先清掉同名旧临时文件。 */
    static unsigned g_tmp_seq = 0;
    unsigned seq = ++g_tmp_seq;
    int t1 = snprintf(inst_tmp, inst_cap, "%s/.inst.flac.part.%ld.%u",
                      out->dir, (long)getpid(), seq);
    int t2 = snprintf(soft_tmp, soft_cap, "%s/.soft.flac.part.%ld.%u",
                      out->dir, (long)getpid(), seq);
    int t3 = snprintf(meta_tmp, meta_cap, "%s/.meta.txt.part.%ld.%u",
                      out->dir, (long)getpid(), seq);
    if (t1 < 0 || (size_t)t1 >= inst_cap || t2 < 0 || (size_t)t2 >= soft_cap ||
        t3 < 0 || (size_t)t3 >= meta_cap) {
        set_err(err, err_cap, "临时文件路径过长");
        return -1;
    }
    unlink(inst_tmp);
    unlink(soft_tmp);
    unlink(meta_tmp);
    return 0;
}

/* ================================================================
 * 4. FLAC STREAMINFO 解析
 * ================================================================ */

static uint32_t be24(const unsigned char *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

int karaoke_cache_flac_info(const char *path, int *sample_rate, int *channels,
                            int *bits_per_sample, long long *frames,
                            char *err, size_t err_cap)
{
    if (!path || !path[0]) {
        set_err(err, err_cap, "FLAC 路径为空");
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, err_cap, "无法打开 FLAC %s：%s", path, strerror(errno));
        return -1;
    }
    unsigned char hdr[4];
    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "fLaC", 4) != 0) {
        fclose(f);
        set_err(err, err_cap, "不是 FLAC 文件（缺 fLaC 魔数）：%s", path);
        return -1;
    }
    int found = 0;
    for (int i = 0; i < 128 && !found; i++) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, f) != 4) {
            set_err(err, err_cap, "FLAC 元数据块截断：%s", path);
            fclose(f);
            return -1;
        }
        int last = (bh[0] & 0x80) != 0;
        int type = bh[0] & 0x7f;
        uint32_t len = be24(bh + 1);
        if (type == 0 && len == 34) {
            unsigned char si[34];
            if (fread(si, 1, 34, f) != 34) {
                set_err(err, err_cap, "FLAC STREAMINFO 截断：%s", path);
                fclose(f);
                return -1;
            }
            int sr = ((int)si[10] << 12) | ((int)si[11] << 4) | (si[12] >> 4);
            int ch = (int)((si[12] >> 1) & 0x07) + 1;
            int bps = (int)((((si[12] & 0x01) << 4) | (si[13] >> 4))) + 1;
            long long total = ((long long)(si[13] & 0x0f) << 32) |
                              ((long long)si[14] << 24) |
                              ((long long)si[15] << 16) |
                              ((long long)si[16] << 8) | (long long)si[17];
            if (sample_rate)
                *sample_rate = sr;
            if (channels)
                *channels = ch;
            if (bits_per_sample)
                *bits_per_sample = bps;
            if (frames)
                *frames = total;
            found = 1;
            break;
        }
        if (fseek(f, (long)len, SEEK_CUR) != 0) {
            set_err(err, err_cap, "FLAC 元数据块越界：%s", path);
            fclose(f);
            return -1;
        }
        if (last)
            break;
    }
    fclose(f);
    if (!found) {
        set_err(err, err_cap, "FLAC 缺少 STREAMINFO：%s", path);
        return -1;
    }
    return 0;
}

/* ================================================================
 * 5. meta.txt 读写与成对校验
 * ================================================================ */

typedef struct {
    int format;
    char model_id[128];
    char model_sha256[65];
    double overlap;
    double soft_inst;
    long long source_frames;
    int source_rate;
    long long inst_frames;
    long long soft_frames;
} CacheMeta;

static int meta_read(const char *path, CacheMeta *m, char *err, size_t err_cap)
{
    memset(m, 0, sizeof(*m));
    FILE *f = fopen(path, "r");
    if (!f) {
        set_err(err, err_cap, "无法读取缓存元数据 %s：%s", path, strerror(errno));
        return -1;
    }
    char line[512];
    int seen = 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        size_t vl = strlen(val);
        while (vl && (val[vl - 1] == '\n' || val[vl - 1] == '\r'))
            val[--vl] = '\0';
        if (strcmp(key, "format") == 0) {
            m->format = atoi(val);
            seen++;
        } else if (strcmp(key, "model_id") == 0) {
            snprintf(m->model_id, sizeof(m->model_id), "%s", val);
            seen++;
        } else if (strcmp(key, "model_sha256") == 0) {
            snprintf(m->model_sha256, sizeof(m->model_sha256), "%s", val);
            seen++;
        } else if (strcmp(key, "overlap") == 0) {
            m->overlap = atof(val);
            seen++;
        } else if (strcmp(key, "soft_inst") == 0) {
            m->soft_inst = atof(val);
            seen++;
        } else if (strcmp(key, "source_frames") == 0) {
            m->source_frames = atoll(val);
            seen++;
        } else if (strcmp(key, "source_rate") == 0) {
            m->source_rate = atoi(val);
            seen++;
        } else if (strcmp(key, "inst_frames") == 0) {
            m->inst_frames = atoll(val);
            seen++;
        } else if (strcmp(key, "soft_frames") == 0) {
            m->soft_frames = atoll(val);
            seen++;
        }
    }
    fclose(f);
    if (seen < 9) {
        set_err(err, err_cap, "缓存元数据字段不全：%s", path);
        return -1;
    }
    return 0;
}

int karaoke_cache_write_meta(const char *meta_tmp, const KaraokeCacheConfig *cfg,
                             long long source_frames, int source_rate,
                             long long inst_frames, long long soft_frames,
                             char *err, size_t err_cap)
{
    if (!meta_tmp || !cfg_valid(cfg)) {
        set_err(err, err_cap, "缓存元数据参数非法");
        return -1;
    }
    FILE *f = fopen(meta_tmp, "w");
    if (!f) {
        set_err(err, err_cap, "无法写缓存元数据 %s：%s", meta_tmp, strerror(errno));
        return -1;
    }
    int rc = fprintf(f,
                     "format=%d\n"
                     "model_id=%s\n"
                     "model_sha256=%s\n"
                     "overlap=%.6f\n"
                     "soft_inst=%.6f\n"
                     "source_frames=%lld\n"
                     "source_rate=%d\n"
                     "inst_frames=%lld\n"
                     "soft_frames=%lld\n",
                     KARAOKE_CACHE_FORMAT_VERSION, cfg->model_id,
                     cfg->model_sha256,
                     (double)(cfg->overlap > 0.0f ? cfg->overlap
                                                  : KARAOKE_CACHE_OVERLAP),
                     (double)(cfg->soft_inst_mix > 0.0f
                                  ? cfg->soft_inst_mix
                                  : KARAOKE_CACHE_SOFT_INST_MIX),
                     source_frames, source_rate, inst_frames, soft_frames);
    int flush_rc = fflush(f);
    int close_rc = fclose(f);
    if (rc < 0 || flush_rc != 0 || close_rc != 0) {
        set_err(err, err_cap, "写缓存元数据失败：%s", strerror(errno));
        unlink(meta_tmp);
        return -1;
    }
    return 0;
}

/* 静音拒绝：16bit 立体声 FLAC 的压缩比上限取 25:1（原声 4 字节/帧）。 */
static long long min_nonsilent_bytes(long long frames)
{
    long long raw = frames * 2 * 2;
    long long min = raw / 25;
    return min > 2048 ? min : 2048;
}

static int check_one_flac(const char *path, const CacheMeta *m, int is_soft,
                          char *err, size_t err_cap)
{
    int sr = 0, ch = 0, bps = 0;
    long long frames = 0;
    if (karaoke_cache_flac_info(path, &sr, &ch, &bps, &frames, err, err_cap) != 0)
        return -1;
    if (sr != m->source_rate) {
        set_err(err, err_cap, "缓存采样率不符：%s 实际 %d，元数据 %d", path, sr,
                m->source_rate);
        return -1;
    }
    if (ch != 2) {
        set_err(err, err_cap, "缓存声道数不符：%s 实际 %d（要求 2）", path, ch);
        return -1;
    }
    if (bps != 16) {
        set_err(err, err_cap, "缓存位深不符：%s 实际 %d（要求 16）", path, bps);
        return -1;
    }
    long long expect = is_soft ? m->soft_frames : m->inst_frames;
    if (expect <= 0 || frames != expect) {
        set_err(err, err_cap, "缓存帧数与元数据不符：%s 实际 %lld，元数据 %lld",
                path, frames, expect);
        return -1;
    }
    if (frames != m->source_frames) {
        set_err(err, err_cap, "缓存时长与原曲不符：%s 帧数 %lld，原曲 %lld", path,
                frames, m->source_frames);
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        set_err(err, err_cap, "缓存文件不可读：%s", path);
        return -1;
    }
    if (st.st_size < min_nonsilent_bytes(frames)) {
        set_err(err, err_cap, "缓存疑似静音/损坏（%lld 字节 < 下限）：%s",
                (long long)st.st_size, path);
        return -1;
    }
    return 0;
}

/* 校验三件套（路径任意：正式文件或临时文件）。 */
static int validate_triplet(const KaraokeCacheConfig *cfg, const char *inst,
                            const char *soft, const char *meta,
                            char *err, size_t err_cap)
{
    CacheMeta mi;
    if (meta_read(meta, &mi, err, err_cap) != 0)
        return -1;
    if (mi.format != KARAOKE_CACHE_FORMAT_VERSION) {
        set_err(err, err_cap, "缓存格式版本不符：%d（要求 %d）", mi.format,
                KARAOKE_CACHE_FORMAT_VERSION);
        return -1;
    }
    if (strcmp(mi.model_id, cfg->model_id) != 0) {
        set_err(err, err_cap, "缓存模型 id 不符：%s", mi.model_id);
        return -1;
    }
    if (strcmp(mi.model_sha256, cfg->model_sha256) != 0) {
        set_err(err, err_cap, "缓存模型 SHA256 不符");
        return -1;
    }
    float want_overlap = cfg->overlap > 0.0f ? cfg->overlap : KARAOKE_CACHE_OVERLAP;
    float want_soft = cfg->soft_inst_mix > 0.0f ? cfg->soft_inst_mix
                                                : KARAOKE_CACHE_SOFT_INST_MIX;
    if (fabs(mi.overlap - (double)want_overlap) > 1e-6) {
        set_err(err, err_cap, "缓存 overlap 不符：%.3f", mi.overlap);
        return -1;
    }
    if (fabs(mi.soft_inst - (double)want_soft) > 1e-6) {
        set_err(err, err_cap, "缓存 soft 系数不符：%.3f", mi.soft_inst);
        return -1;
    }
    if (mi.source_rate < 8000 || mi.source_rate > 384000 || mi.source_frames <= 0) {
        set_err(err, err_cap, "缓存元数据参数非法");
        return -1;
    }
    if (mi.inst_frames != mi.soft_frames || mi.inst_frames != mi.source_frames) {
        set_err(err, err_cap, "缓存两份文件帧数不一致");
        return -1;
    }
    if (check_one_flac(inst, &mi, 0, err, err_cap) != 0)
        return -1;
    return check_one_flac(soft, &mi, 1, err, err_cap);
}

int karaoke_cache_commit(const KaraokeCacheConfig *cfg,
                         const KaraokeCachePaths *paths,
                         const char *inst_tmp, const char *soft_tmp,
                         const char *meta_tmp,
                         char *err, size_t err_cap)
{
    if (!cfg_valid(cfg) || !paths || !inst_tmp || !soft_tmp || !meta_tmp) {
        set_err(err, err_cap, "缓存提交参数非法");
        return -1;
    }
    if (!path_within_root(cfg, paths->dir)) {
        set_err(err, err_cap, "缓存提交路径越界");
        return -1;
    }
    if (validate_triplet(cfg, inst_tmp, soft_tmp, meta_tmp, err, err_cap) != 0)
        return -1;
    if (fsync_path(inst_tmp) != 0 || fsync_path(soft_tmp) != 0 ||
        fsync_path(meta_tmp) != 0) {
        set_err(err, err_cap, "缓存 fsync 失败：%s", strerror(errno));
        return -1;
    }
    /* meta 最后 rename：三件不全时 resolve 一定判为不完整。 */
    if (rename(inst_tmp, paths->inst_path) != 0) {
        set_err(err, err_cap, "缓存 rename(inst) 失败：%s", strerror(errno));
        return -1;
    }
    if (rename(soft_tmp, paths->soft_path) != 0) {
        set_err(err, err_cap, "缓存 rename(soft) 失败：%s", strerror(errno));
        unlink(paths->inst_path);
        return -1;
    }
    if (rename(meta_tmp, paths->meta_path) != 0) {
        set_err(err, err_cap, "缓存 rename(meta) 失败：%s", strerror(errno));
        unlink(paths->inst_path);
        unlink(paths->soft_path);
        return -1;
    }
    if (fsync_dir(paths->dir) != 0) {
        /* 目录 fsync 失败不回滚（数据已落盘）；记入 err 供诊断。 */
        set_err(err, err_cap, "缓存目录 fsync 失败：%s", strerror(errno));
    }
    (void)karaoke_cache_evict(cfg, paths->dir, NULL, 0);
    return 0;
}

void karaoke_cache_abort_temps(const char *inst_tmp, const char *soft_tmp,
                               const char *meta_tmp)
{
    if (inst_tmp && inst_tmp[0])
        unlink(inst_tmp);
    if (soft_tmp && soft_tmp[0])
        unlink(soft_tmp);
    if (meta_tmp && meta_tmp[0])
        unlink(meta_tmp);
}

int karaoke_cache_remove(const KaraokeCacheConfig *cfg, const char *track_path)
{
    if (!cfg_valid(cfg) || !track_path)
        return -1;
    KaraokeCachePaths paths;
    if (karaoke_cache_paths(cfg, track_path, &paths, NULL, 0) != 0)
        return -1;
    if (!path_within_root(cfg, paths.dir))
        return -1;
    /* F2：同目录若还有 worker 正在写的 .part 文件，删除会打断 commit；
     * 这里只报告（-1），让损坏文件留到下一次空闲时清理。 */
    if (is_dir(paths.dir) && dir_has_part_files(paths.dir))
        return -1;
    if (is_dir(paths.dir))
        remove_tree(paths.dir);
    return 0;
}

int karaoke_cache_complete(const KaraokeCacheConfig *cfg, const char *track_path)
{
    if (!cfg_valid(cfg) || !track_path)
        return 0;
    KaraokeCachePaths paths;
    if (karaoke_cache_paths(cfg, track_path, &paths, NULL, 0) != 0)
        return 0;
    if (!is_regular(paths.inst_path) || !is_regular(paths.soft_path) ||
        !is_regular(paths.meta_path))
        return 0;
    return validate_triplet(cfg, paths.inst_path, paths.soft_path,
                            paths.meta_path, NULL, 0) == 0;
}

int karaoke_cache_resolve(const KaraokeCacheConfig *cfg, const char *track_path,
                          int mode, char *out_path, size_t out_cap,
                          char *err, size_t err_cap)
{
    if (!cfg_valid(cfg) || !track_path || !track_path[0]) {
        set_err(err, err_cap, "缓存查询参数非法");
        return 0;
    }
    if (mode == KARAOKE_MODE_OFF || mode < KARAOKE_MODE_OFF ||
        mode >= KARAOKE_MODE_COUNT)
        return 0;   /* 原声不需要缓存 */

    KaraokeCachePaths paths;
    if (karaoke_cache_paths(cfg, track_path, &paths, NULL, 0) != 0)
        return 0;

    int hit = 0;
    if (is_regular(paths.inst_path) && is_regular(paths.soft_path) &&
        is_regular(paths.meta_path)) {
        char why[KARAOKE_CACHE_ERR_CAP] = {0};
        if (validate_triplet(cfg, paths.inst_path, paths.soft_path,
                             paths.meta_path, why, sizeof(why)) == 0) {
            hit = 1;
        } else if (err && err_cap) {
            snprintf(err, err_cap, "%s", why);
        }
    }
    if (!hit) {
        /* 残留/损坏：整目录清掉（只删 root 子树内的自家路径）。F2：worker
         * 正在写（存在 .part 半成品）时绝不删除，留给后续清理。 */
        if (path_within_root(cfg, paths.dir) && is_dir(paths.dir) &&
            !dir_has_part_files(paths.dir))
            remove_tree(paths.dir);
        return 0;
    }
    /* LRU：touch 两份文件与目录（不重写内容）。 */
    (void)utimensat(AT_FDCWD, paths.inst_path, NULL, 0);
    (void)utimensat(AT_FDCWD, paths.soft_path, NULL, 0);
    (void)utimensat(AT_FDCWD, paths.meta_path, NULL, 0);
    (void)utimensat(AT_FDCWD, paths.dir, NULL, 0);

    const char *src = (mode == KARAOKE_MODE_SOFT) ? paths.soft_path : paths.inst_path;
    if (out_path && out_cap) {
        int n = snprintf(out_path, out_cap, "%s", src);
        if (n < 0 || (size_t)n >= out_cap) {
            set_err(err, err_cap, "缓存路径缓冲过小");
            return 0;
        }
    }
    return 1;
}

/* ================================================================
 * 6. LRU / 占用统计 / 启动清理
 * ================================================================ */

typedef struct {
    char path[KARAOKE_CACHE_PATH_CAP];
    long long bytes;
    time_t mtime;
    int busy;   /* 含 .part. 或最近修改：不参与淘汰 */
} CacheDirEntry;

static long long dir_bytes(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    long long total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[KARAOKE_CACHE_PATH_CAP];
        if (snprintf(child, sizeof(child), "%s/%s", dir, e->d_name) >=
            (int)sizeof(child))
            continue;
        struct stat st;
        if (stat(child, &st) == 0 && S_ISREG(st.st_mode))
            total += st.st_size;
    }
    closedir(d);
    return total;
}

static int dir_has_part(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, ".part.") != NULL) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* 收集 root/<model>/<hash>/ 目录（两层）。 */
static int collect_dirs(const KaraokeCacheConfig *cfg, CacheDirEntry **out,
                        int *count)
{
    *out = NULL;
    *count = 0;
    DIR *root = opendir(cfg->root);
    if (!root)
        return 0;
    int cap = 0;
    struct dirent *m;
    time_t now = time(NULL);
    while ((m = readdir(root)) != NULL) {
        if (m->d_name[0] == '.')
            continue;
        char model_dir[KARAOKE_CACHE_PATH_CAP];
        if (snprintf(model_dir, sizeof(model_dir), "%s/%s", cfg->root,
                     m->d_name) >= (int)sizeof(model_dir))
            continue;
        if (!is_dir(model_dir))
            continue;
        DIR *md = opendir(model_dir);
        if (!md)
            continue;
        struct dirent *t;
        while ((t = readdir(md)) != NULL) {
            if (t->d_name[0] == '.')
                continue;
            char dir[KARAOKE_CACHE_PATH_CAP];
            if (snprintf(dir, sizeof(dir), "%s/%s", model_dir, t->d_name) >=
                (int)sizeof(dir))
                continue;
            struct stat st;
            if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            if (*count == cap) {
                int ncap = cap ? cap * 2 : 16;
                CacheDirEntry *n = realloc(*out, (size_t)ncap * sizeof(**out));
                if (!n)
                    break;
                *out = n;
                cap = ncap;
            }
            CacheDirEntry *e = &(*out)[(*count)++];
            snprintf(e->path, sizeof(e->path), "%s", dir);
            e->bytes = dir_bytes(dir);
            e->mtime = st.st_mtime;
            e->busy = dir_has_part(dir) || (now - st.st_mtime < 120);
        }
        closedir(md);
    }
    closedir(root);
    return 0;
}

static int cmp_dir_mtime(const void *a, const void *b)
{
    const CacheDirEntry *x = (const CacheDirEntry *)a;
    const CacheDirEntry *y = (const CacheDirEntry *)b;
    if (x->mtime < y->mtime)
        return -1;
    if (x->mtime > y->mtime)
        return 1;
    return strcmp(x->path, y->path);
}

static long long total_bytes(const CacheDirEntry *dirs, int count)
{
    long long total = 0;
    for (int i = 0; i < count; i++)
        total += dirs[i].bytes;
    return total;
}

int karaoke_cache_evict(const KaraokeCacheConfig *cfg, const char *exclude_dir,
                        char *err, size_t err_cap)
{
    if (!cfg_valid(cfg))
        return -1;
    CacheDirEntry *dirs = NULL;
    int count = 0;
    collect_dirs(cfg, &dirs, &count);
    if (!dirs)
        return 0;
    long long limit = (long long)cfg_limit_mb(cfg) * 1024 * 1024;
    long long total = total_bytes(dirs, count);
    if (total > limit) {
        qsort(dirs, (size_t)count, sizeof(*dirs), cmp_dir_mtime);
        for (int i = 0; i < count && total > limit; i++) {
            if (dirs[i].busy)
                continue;
            if (exclude_dir && exclude_dir[0] && strcmp(dirs[i].path, exclude_dir) == 0)
                continue;
            if (!path_within_root(cfg, dirs[i].path))
                continue;
            remove_tree(dirs[i].path);
            total -= dirs[i].bytes;
        }
    }
    free(dirs);
    (void)err;
    (void)err_cap;
    return 0;
}

long long karaoke_cache_usage_mb(const KaraokeCacheConfig *cfg, int *track_count)
{
    if (track_count)
        *track_count = 0;
    if (!cfg_valid(cfg))
        return -1;
    CacheDirEntry *dirs = NULL;
    int count = 0;
    collect_dirs(cfg, &dirs, &count);
    long long total = total_bytes(dirs, count);
    int tracks = 0;
    for (int i = 0; i < count; i++) {
        char inst[KARAOKE_CACHE_PATH_CAP];
        if (snprintf(inst, sizeof(inst), "%s/inst.flac", dirs[i].path) <
                (int)sizeof(inst) &&
            is_regular(inst))
            tracks++;
    }
    free(dirs);
    if (track_count)
        *track_count = tracks;
    return (total + (1024 * 1024 - 1)) / (1024 * 1024);
}

void karaoke_cache_cleanup_stale(const KaraokeCacheConfig *cfg)
{
    if (!cfg_valid(cfg))
        return;
    CacheDirEntry *dirs = NULL;
    int count = 0;
    collect_dirs(cfg, &dirs, &count);
    time_t now = time(NULL);
    for (int i = 0; i < count; i++) {
        DIR *d = opendir(dirs[i].path);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strstr(e->d_name, ".part.") == NULL)
                continue;
            char child[KARAOKE_CACHE_PATH_CAP];
            if (snprintf(child, sizeof(child), "%s/%s", dirs[i].path,
                         e->d_name) >= (int)sizeof(child))
                continue;
            struct stat st;
            if (stat(child, &st) == 0 && S_ISREG(st.st_mode) &&
                now - st.st_mtime > 600)
                unlink(child);
        }
        closedir(d);
    }
    free(dirs);
}

/* ================================================================
 * 7. v2 会话级分段缓存（D2/D6）
 *
 * 只写 /tmp/ter-music-karaoke-<uid>-<pid>/；XDG 下的 v1 整曲缓存保持
 * 只读兼容（本文件的 1–6 节），播放侧优先 v1。
 * ================================================================ */

#include <signal.h>

#define SESSION_PREFIX "ter-music-karaoke-"
#define SEGMENT_ROOT_MIN_BYTES 1024   /* 段文件下限：检测截断/空文件（静音段也远大于此） */

static int lstat_is_regular(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int lstat_is_dir_nosymlink(const char *path, uid_t *owner)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return 0;
    if (owner)
        *owner = st.st_uid;
    return 1;
}

int karaoke_cache_session_root(char *out, size_t cap)
{
    if (!out || cap == 0)
        return -1;
    int n = snprintf(out, cap, "/tmp/" SESSION_PREFIX "%ld-%ld",
                     (long)getuid(), (long)getpid());
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int karaoke_cache_session_ensure(char *out, size_t cap, char *err, size_t err_cap)
{
    static int s_created = 0;   /* 本进程是否已创建（pid 复用残留只在首建时清理） */
    char root[KARAOKE_CACHE_PATH_CAP];
    if (karaoke_cache_session_root(root, sizeof(root)) != 0) {
        set_err(err, err_cap, "会话缓存根路径过长");
        return -1;
    }
    struct stat st;
    if (lstat(root, &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            unlink(root);   /* 绝不跟随符号链接 */
        } else if (S_ISDIR(st.st_mode)) {
            if (!s_created) {
                /* 同 pid 复用：旧进程崩溃残留，内容不可信，先清空 */
                remove_tree(root);
            }
        } else {
            unlink(root);
        }
    }
    if (mkdir(root, 0700) != 0 && errno != EEXIST) {
        set_err(err, err_cap, "创建会话缓存目录 %s 失败：%s", root, strerror(errno));
        return -1;
    }
    uid_t owner = 0;
    if (!lstat_is_dir_nosymlink(root, &owner) || owner != getuid()) {
        set_err(err, err_cap, "会话缓存目录不可用（类型/属主不符）：%s", root);
        return -1;
    }
    (void)chmod(root, 0700);
    s_created = 1;
    if (out && cap) {
        int n = snprintf(out, cap, "%s", root);
        if (n < 0 || (size_t)n >= cap) {
            set_err(err, err_cap, "会话缓存根路径缓冲过小");
            return -1;
        }
    }
    return 0;
}

void karaoke_cache_session_cleanup_stale(void)
{
    DIR *d = opendir("/tmp");
    if (!d)
        return;
    const size_t plen = strlen(SESSION_PREFIX);
    const long me_uid = (long)getuid();
    const long me_pid = (long)getpid();
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, SESSION_PREFIX, plen) != 0)
            continue;
        long uid = -1, pid = -1;
        if (sscanf(e->d_name + plen, "%ld-%ld", &uid, &pid) != 2)
            continue;
        if (uid != me_uid || pid == me_pid || pid <= 0)
            continue;
        char path[KARAOKE_CACHE_PATH_CAP];
        int n = snprintf(path, sizeof(path), "/tmp/%s", e->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        uid_t owner = 0;
        if (!lstat_is_dir_nosymlink(path, &owner) || owner != getuid())
            continue;
        errno = 0;
        int alive = kill((pid_t)pid, 0);
        int kerr = errno;
        if (alive == 0 || kerr == EPERM)
            continue;             /* 进程仍在 */
        if (kerr != ESRCH)
            continue;             /* 无法判定：保守不删 */
        remove_tree(path);
    }
    closedir(d);
}

void karaoke_cache_session_remove_self(void)
{
    char root[KARAOKE_CACHE_PATH_CAP];
    if (karaoke_cache_session_root(root, sizeof(root)) != 0)
        return;
    uid_t owner = 0;
    if (!lstat_is_dir_nosymlink(root, &owner) || owner != getuid())
        return;
    remove_tree(root);
}

/* v2 键材料：与 v1 区分格式版本，并显式绑定 44.1k/15s 分段坐标。 */
int karaoke_cache_segment_paths(const KaraokeCacheConfig *cfg, const char *track_path,
                                KaraokeSegmentPaths *out, char *err, size_t err_cap)
{
    if (!cfg_valid(cfg) || !track_path || !track_path[0] || !out) {
        set_err(err, err_cap, "分段缓存配置/曲目路径非法");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (strlen(cfg->model_id) > 64 || strchr(cfg->model_id, '/') != NULL) {
        set_err(err, err_cap, "模型 id 非法");
        return -1;
    }
    char real[PATH_MAX];
    if (!realpath(track_path, real)) {
        set_err(err, err_cap, "无法解析曲目真实路径 %s：%s", track_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode)) {
        set_err(err, err_cap, "曲目不是普通文件：%s", real);
        return -1;
    }
    char material[KARAOKE_CACHE_PATH_CAP + 256];
    int n = snprintf(material, sizeof(material),
                     "ter-music-karaoke-seg\n"
                     "format=%d\n"
                     "stem=%d\n"
                     "model_id=%s\n"
                     "model_sha256=%s\n"
                     "%s\n"
                     "%lld\n"
                     "%lld.%09ld\n"
                     "overlap=%.6f\n"
                     "soft_inst=%.6f\n"
                     "sample_rate=%d\n"
                     "segment_seconds=%d\n"
                     "flac=16bit,2ch\n",
                     KARAOKE_SEGMENT_FORMAT_VERSION, KARAOKE_CACHE_STEM_VERSION,
                     cfg->model_id, cfg->model_sha256, real,
                     (long long)st.st_size,
                     (long long)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec,
                     (double)(cfg->overlap > 0.0f ? cfg->overlap
                                                  : KARAOKE_CACHE_OVERLAP),
                     (double)(cfg->soft_inst_mix > 0.0f
                                  ? cfg->soft_inst_mix
                                  : KARAOKE_CACHE_SOFT_INST_MIX),
                     KARAOKE_SEGMENT_SAMPLE_RATE, KARAOKE_SEGMENT_SECONDS);
    if (n < 0 || (size_t)n >= sizeof(material)) {
        set_err(err, err_cap, "分段缓存键材料过长");
        return -1;
    }
    CacheSha256 ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, material, (size_t)n);
    sha256_final_hex(&ctx, out->hash);

    int r = snprintf(out->root, sizeof(out->root), "%s", cfg->root);
    int m = snprintf(out->dir, sizeof(out->dir), "%s/%s/%s", cfg->root,
                     cfg->model_id, out->hash);
    int q = snprintf(out->manifest_path, sizeof(out->manifest_path), "%s/%s",
                     out->dir, "manifest.txt");
    if (r < 0 || (size_t)r >= sizeof(out->root) || m < 0 ||
        (size_t)m >= sizeof(out->dir) || q < 0 ||
        (size_t)q >= sizeof(out->manifest_path) ||
        !path_within_root(cfg, out->dir)) {
        set_err(err, err_cap, "分段缓存目录路径过长/越界");
        return -1;
    }
    return 0;
}

int karaoke_cache_segment_mkdir(const KaraokeSegmentPaths *paths,
                                char *err, size_t err_cap)
{
    if (!paths || !paths->dir[0]) {
        set_err(err, err_cap, "分段缓存目录为空");
        return -1;
    }
    char model_dir[KARAOKE_CACHE_PATH_CAP];
    const char *slash = strrchr(paths->dir, '/');
    if (!slash || slash == paths->dir) {
        set_err(err, err_cap, "分段缓存目录非法");
        return -1;
    }
    size_t n = (size_t)(slash - paths->dir);
    if (n >= sizeof(model_dir)) {
        set_err(err, err_cap, "分段缓存目录过长");
        return -1;
    }
    memcpy(model_dir, paths->dir, n);
    model_dir[n] = '\0';
    if (mkdir_p_0700(paths->root, err, err_cap) != 0 ||
        mkdir_p_0700(model_dir, err, err_cap) != 0 ||
        mkdir_p_0700(paths->dir, err, err_cap) != 0)
        return -1;
    uid_t owner = 0;
    if (!lstat_is_dir_nosymlink(paths->dir, &owner) || owner != getuid()) {
        set_err(err, err_cap, "分段缓存目录不可用：%s", paths->dir);
        return -1;
    }
    return 0;
}

int karaoke_cache_segment_file(const KaraokeSegmentPaths *paths, int seg, int is_soft,
                               char *out, size_t cap)
{
    if (!paths || !out || seg < 0)
        return -1;
    int n = snprintf(out, cap, "%s/seg%03d.%s.flac", paths->dir, seg,
                     is_soft ? "soft" : "inst");
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int karaoke_cache_segment_marker(const KaraokeSegmentPaths *paths, int seg,
                                 char *out, size_t cap)
{
    if (!paths || !out || seg < 0)
        return -1;
    int n = snprintf(out, cap, "%s/seg%03d.ready", paths->dir, seg);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int karaoke_cache_segment_count(long long frames, int *out)
{
    if (!out)
        return -1;
    if (frames <= 0) {
        *out = 1;
        return 0;
    }
    long long n = (frames + KARAOKE_SEGMENT_FRAMES - 1) / KARAOKE_SEGMENT_FRAMES;
    if (n > KARAOKE_SEGMENT_MAX_COUNT)
        n = KARAOKE_SEGMENT_MAX_COUNT;
    *out = (int)n;
    return 0;
}

int karaoke_cache_segment_expected(const KaraokeSegmentManifest *m, int seg,
                                   long long *out_frames)
{
    if (!m || m->total_segments <= 0 || m->source_frames <= 0)
        return -1;   /* complete=0 但 total/source 已知时也给出期望（EOF 后回填/
                        last 段短帧校验用）；两者未知才算"未知"。 */
    if (seg < 0 || seg >= m->total_segments)
        return -1;
    long long start = (long long)seg * KARAOKE_SEGMENT_FRAMES;
    long long frames = (seg == m->total_segments - 1)
                           ? (m->source_frames - start)
                           : (long long)KARAOKE_SEGMENT_FRAMES;
    if (frames <= 0)
        return -1;
    if (out_frames)
        *out_frames = frames;
    return 0;
}

int karaoke_cache_segment_manifest_read(const char *path, KaraokeSegmentManifest *out,
                                        char *err, size_t err_cap)
{
    if (!path || !path[0] || !out) {
        set_err(err, err_cap, "分段清单路径为空");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "r");
    if (!f) {
        set_err(err, err_cap, "无法读取分段清单 %s：%s", path, strerror(errno));
        return -1;
    }
    char line[512];
    int seen = 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        size_t vl = strlen(val);
        while (vl && (val[vl - 1] == '\n' || val[vl - 1] == '\r'))
            val[--vl] = '\0';
        if (strcmp(key, "format") == 0) {
            out->format = atoi(val);
            seen++;
        } else if (strcmp(key, "model_id") == 0) {
            snprintf(out->model_id, sizeof(out->model_id), "%s", val);
            seen++;
        } else if (strcmp(key, "model_sha256") == 0) {
            snprintf(out->model_sha256, sizeof(out->model_sha256), "%s", val);
            seen++;
        } else if (strcmp(key, "overlap") == 0) {
            out->overlap = atof(val);
            seen++;
        } else if (strcmp(key, "soft_inst") == 0) {
            out->soft_inst = atof(val);
            seen++;
        } else if (strcmp(key, "sample_rate") == 0) {
            out->sample_rate = atoi(val);
            seen++;
        } else if (strcmp(key, "segment_seconds") == 0) {
            out->segment_seconds = atoi(val);
            seen++;
        } else if (strcmp(key, "total_segments") == 0) {
            out->total_segments = atoi(val);
            seen++;
        } else if (strcmp(key, "source_frames") == 0) {
            out->source_frames = atoll(val);
            seen++;
        } else if (strcmp(key, "complete") == 0) {
            out->complete = atoi(val) ? 1 : 0;
            seen++;
        }
    }
    fclose(f);
    if (seen < 10) {
        set_err(err, err_cap, "分段清单字段不全（%d/10）：%s", seen, path);
        return -1;
    }
    return 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

int karaoke_cache_segment_manifest_write(const KaraokeSegmentPaths *paths,
                                         const KaraokeSegmentManifest *m,
                                         char *err, size_t err_cap)
{
    if (!paths || !paths->dir[0] || !paths->manifest_path[0] || !m) {
        set_err(err, err_cap, "分段清单参数非法");
        return -1;
    }
    char tmp[KARAOKE_CACHE_PATH_CAP];
    static unsigned g_seq = 0;
    unsigned seq = __atomic_add_fetch(&g_seq, 1, __ATOMIC_RELAXED);
    int n = snprintf(tmp, sizeof(tmp), "%s/.manifest.txt.part.%ld.%u",
                     paths->dir, (long)getpid(), seq);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        set_err(err, err_cap, "分段清单临时路径过长");
        return -1;
    }
    char body[1024];
    int blen = snprintf(body, sizeof(body),
                        "format=%d\n"
                        "model_id=%s\n"
                        "model_sha256=%s\n"
                        "overlap=%.6f\n"
                        "soft_inst=%.6f\n"
                        "sample_rate=%d\n"
                        "segment_seconds=%d\n"
                        "total_segments=%d\n"
                        "source_frames=%lld\n"
                        "complete=%d\n",
                        KARAOKE_SEGMENT_FORMAT_VERSION, m->model_id,
                        m->model_sha256, m->overlap, m->soft_inst,
                        m->sample_rate, m->segment_seconds, m->total_segments,
                        m->source_frames, m->complete ? 1 : 0);
    if (blen < 0 || (size_t)blen >= sizeof(body)) {
        set_err(err, err_cap, "分段清单内容过长");
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_err(err, err_cap, "创建分段清单失败 %s：%s", tmp, strerror(errno));
        return -1;
    }
    int rc = write_all(fd, body, (size_t)blen);
    if (rc == 0)
        rc = fsync(fd);
    int save = errno;
    close(fd);
    if (rc != 0) {
        set_err(err, err_cap, "写分段清单失败：%s", strerror(save));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, paths->manifest_path) != 0) {
        set_err(err, err_cap, "分段清单 rename 失败：%s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    (void)fsync_dir(paths->dir);
    return 0;
}

int karaoke_cache_segment_mark_ready(const KaraokeSegmentPaths *paths, int seg,
                                     long long frames, char *err, size_t err_cap)
{
    if (!paths || !paths->dir[0] || seg < 0 || frames <= 0 ||
        frames > KARAOKE_SEGMENT_FRAMES) {
        set_err(err, err_cap, "段就绪标记参数非法");
        return -1;
    }
    char marker[KARAOKE_CACHE_PATH_CAP];
    if (karaoke_cache_segment_marker(paths, seg, marker, sizeof(marker)) != 0) {
        set_err(err, err_cap, "段标记路径过长");
        return -1;
    }
    char tmp[KARAOKE_CACHE_PATH_CAP];
    static unsigned g_seq = 0;
    unsigned seq = __atomic_add_fetch(&g_seq, 1, __ATOMIC_RELAXED);
    int n = snprintf(tmp, sizeof(tmp), "%s/.seg%03d.ready.part.%ld.%u", paths->dir,
                     seg, (long)getpid(), seq);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        set_err(err, err_cap, "段标记临时路径过长");
        return -1;
    }
    char body[KARAOKE_SEGMENT_MARKER_CAP];
    int blen = snprintf(body, sizeof(body), "seg=%d\nframes=%lld\n", seg, frames);
    if (blen < 0 || (size_t)blen >= sizeof(body)) {
        set_err(err, err_cap, "段标记内容过长");
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_err(err, err_cap, "创建段标记失败 %s：%s", tmp, strerror(errno));
        return -1;
    }
    int rc = write_all(fd, body, (size_t)blen);
    if (rc == 0)
        rc = fsync(fd);
    int save = errno;
    close(fd);
    if (rc != 0) {
        set_err(err, err_cap, "写段标记失败：%s", strerror(save));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, marker) != 0) {
        set_err(err, err_cap, "段标记 rename 失败：%s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    (void)fsync_dir(paths->dir);
    return 0;
}

static int read_marker(const char *path, int *seg, long long *frames,
                       char *err, size_t err_cap)
{
    if (!lstat_is_regular(path)) {
        set_err(err, err_cap, "段就绪标记缺失/非普通文件：%s", path);
        return -1;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        set_err(err, err_cap, "无法读取段标记 %s：%s", path, strerror(errno));
        return -1;
    }
    char line[KARAOKE_SEGMENT_MARKER_CAP];
    int got_seg = 0, got_frames = 0;
    int seg_val = -1;
    long long frames_val = -1;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *val = eq + 1;
        size_t vl = strlen(val);
        while (vl && (val[vl - 1] == '\n' || val[vl - 1] == '\r'))
            val[--vl] = '\0';
        if (strcmp(line, "seg") == 0) {
            seg_val = atoi(val);
            got_seg = 1;
        } else if (strcmp(line, "frames") == 0) {
            frames_val = atoll(val);
            got_frames = 1;
        }
    }
    fclose(f);
    if (!got_seg || !got_frames || seg_val < 0 || frames_val <= 0) {
        set_err(err, err_cap, "段就绪标记内容非法：%s", path);
        return -1;
    }
    if (seg) *seg = seg_val;
    if (frames) *frames = frames_val;
    return 0;
}

int karaoke_cache_segment_ready(const KaraokeSegmentPaths *paths,
                                const KaraokeSegmentManifest *m, int seg,
                                long long *out_frames, char *err, size_t err_cap)
{
    if (!paths || !m || seg < 0) {
        set_err(err, err_cap, "段校验参数非法");
        return 0;
    }
    char marker[KARAOKE_CACHE_PATH_CAP];
    if (karaoke_cache_segment_marker(paths, seg, marker, sizeof(marker)) != 0)
        return 0;
    int mark_seg = -1;
    long long frames = 0;
    if (read_marker(marker, &mark_seg, &frames, err, err_cap) != 0)
        return 0;
    if (mark_seg != seg) {
        set_err(err, err_cap, "段标记段号不符：%d != %d", mark_seg, seg);
        return 0;
    }
    if (frames > KARAOKE_SEGMENT_FRAMES) {
        set_err(err, err_cap, "段帧数超过上限：%lld", frames);
        return 0;
    }
    long long expected = -1;
    if (karaoke_cache_segment_expected(m, seg, &expected) == 0) {
        if (frames != expected) {
            set_err(err, err_cap, "段帧数与清单不符：%lld != %lld", frames, expected);
            return 0;
        }
    } else if (frames != KARAOKE_SEGMENT_FRAMES) {
        /* 清单未完整时只接受"非末段满段"：末段短帧必须在 complete=1 之后
         * 才可见（worker 的写入顺序保证这一点）。 */
        set_err(err, err_cap, "段未完整时帧数不是满段：%lld", frames);
        return 0;
    }
    const char *suffix[2] = {"inst", "soft"};
    for (int i = 0; i < 2; i++) {
        char path[KARAOKE_CACHE_PATH_CAP];
        if (karaoke_cache_segment_file(paths, seg, i, path, sizeof(path)) != 0) {
            set_err(err, err_cap, "段文件路径过长");
            return 0;
        }
        if (!lstat_is_regular(path)) {
            set_err(err, err_cap, "段文件缺失/非普通文件：%s", path);
            return 0;
        }
        struct stat st;
        if (lstat(path, &st) != 0 || st.st_size < SEGMENT_ROOT_MIN_BYTES) {
            set_err(err, err_cap, "段文件疑似截断：%s", path);
            return 0;
        }
        int sr = 0, ch = 0, bps = 0;
        long long flac_frames = 0;
        if (karaoke_cache_flac_info(path, &sr, &ch, &bps, &flac_frames, err,
                                    err_cap) != 0)
            return 0;
        if (sr != KARAOKE_SEGMENT_SAMPLE_RATE || ch != KARAOKE_SEGMENT_CHANNELS ||
            bps != 16) {
            set_err(err, err_cap, "段格式不符（%d/%d/%d）：%s", sr, ch, bps,
                    path);
            return 0;
        }
        if (flac_frames != frames) {
            set_err(err, err_cap, "段 FLAC 帧数与标记不符：%lld != %lld（%s）",
                    flac_frames, frames, suffix[i]);
            return 0;
        }
    }
    if (out_frames)
        *out_frames = frames;
    return 1;
}

static int segment_manifest_matches(const KaraokeCacheConfig *cfg,
                                    const KaraokeSegmentManifest *m)
{
    if (!cfg || !m)
        return 0;
    if (m->format != KARAOKE_SEGMENT_FORMAT_VERSION)
        return 0;
    if (strcmp(m->model_id, cfg->model_id) != 0 ||
        strcmp(m->model_sha256, cfg->model_sha256) != 0)
        return 0;
    float want_overlap = cfg->overlap > 0.0f ? cfg->overlap : KARAOKE_CACHE_OVERLAP;
    float want_soft = cfg->soft_inst_mix > 0.0f ? cfg->soft_inst_mix
                                                : KARAOKE_CACHE_SOFT_INST_MIX;
    if (fabs(m->overlap - (double)want_overlap) > 1e-6 ||
        fabs(m->soft_inst - (double)want_soft) > 1e-6)
        return 0;
    if (m->sample_rate != KARAOKE_SEGMENT_SAMPLE_RATE ||
        m->segment_seconds != KARAOKE_SEGMENT_SECONDS)
        return 0;
    if (m->complete && (m->total_segments <= 0 || m->source_frames <= 0))
        return 0;
    return 1;
}

int karaoke_cache_segment_lookup(const KaraokeCacheConfig *cfg, const char *track_path,
                                 KaraokeSegmentPaths *out_paths,
                                 KaraokeSegmentManifest *out_manifest)
{
    KaraokeSegmentPaths paths;
    if (karaoke_cache_segment_paths(cfg, track_path, &paths, NULL, 0) != 0)
        return 0;
    KaraokeSegmentManifest m;
    if (karaoke_cache_segment_manifest_read(paths.manifest_path, &m, NULL, 0) != 0)
        return 0;
    if (!segment_manifest_matches(cfg, &m))
        return 0;
    if (out_paths)
        *out_paths = paths;
    if (out_manifest)
        *out_manifest = m;
    return 1;
}

int karaoke_cache_segment_resolve(const KaraokeCacheConfig *cfg, const char *track_path,
                                  int mode, int seg, char *out_path, size_t out_cap)
{
    if (mode == KARAOKE_MODE_OFF || mode < KARAOKE_MODE_OFF ||
        mode >= KARAOKE_MODE_COUNT || seg < 0)
        return 0;
    KaraokeSegmentPaths paths;
    KaraokeSegmentManifest m;
    if (karaoke_cache_segment_lookup(cfg, track_path, &paths, &m) != 1)
        return 0;
    if (karaoke_cache_segment_ready(&paths, &m, seg, NULL, NULL, 0) != 1)
        return 0;
    if (out_path && out_cap) {
        int n = snprintf(out_path, out_cap, "%s/seg%03d.%s.flac", paths.dir, seg,
                         (mode == KARAOKE_MODE_SOFT) ? "soft" : "inst");
        if (n < 0 || (size_t)n >= out_cap)
            return 0;
    }
    return 1;
}

int karaoke_cache_segment_ready_for_mode(const KaraokeCacheConfig *cfg,
                                         const char *track_path, int seg,
                                         int mode)
{
    return karaoke_cache_segment_resolve(cfg, track_path, mode, seg, NULL, 0);
}

int karaoke_cache_segment_complete(const KaraokeCacheConfig *cfg, const char *track_path)
{
    KaraokeSegmentPaths paths;
    KaraokeSegmentManifest m;
    if (karaoke_cache_segment_lookup(cfg, track_path, &paths, &m) != 1)
        return 0;
    if (!m.complete || m.total_segments <= 0 || m.source_frames <= 0)
        return 0;
    int want = 0;
    if (karaoke_cache_segment_count(m.source_frames, &want) != 0 ||
        want != m.total_segments)
        return 0;
    for (int seg = 0; seg < m.total_segments; seg++) {
        if (karaoke_cache_segment_ready(&paths, &m, seg, NULL, NULL, 0) != 1)
            return 0;
    }
    return 1;
}
