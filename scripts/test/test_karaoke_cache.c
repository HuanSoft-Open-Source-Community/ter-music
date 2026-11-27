/**
 * @file test_karaoke_cache.c
 * @brief 伴奏缓存单测：路径/键、成对原子提交、命中校验、损坏拒绝、LRU、启动清理
 *
 * 不依赖 FFmpeg/模型/ONNX Runtime：FLAC 只写 STREAMINFO 头 + 填充字节，
 * 缓存模块也只解析 STREAMINFO；元数据在 meta.txt。真实模型端到端
 * （一次分离产出两份缓存、soft 系数、性能）在 test_karaoke_model.c 里，
 * 需要模型时运行。
 *
 * 覆盖：
 *   - 缓存键 = 曲目路径+大小+mtime_ns + model_id/model_sha256 + overlap +
 *     soft 系数 + 格式版本；改 mtime / 换模型 / 改 overlap 自动失效。
 *   - 路径布局 <root>/<model_id>/<track_hash>/{inst,soft}.flac + meta.txt。
 *   - begin → 成对提交 → resolve 命中（soft/full 分别返回对应文件）。
 *   - 半成品/缺文件/头损坏/帧数不符/静音（压缩比异常）拒绝并清理。
 *   - 命中 touch 更新 mtime；LRU 按目录 mtime 淘汰到上限内；usage 统计。
 *   - 启动清理删除过期的 .part.* 半成品。
 */

#include "audio/karaoke_cache.h"
#include "audio/karaoke_mode.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static const char *const MODEL_ID = "uvr-mdxnet-3-9662";
static const char *const MODEL_SHA =
    "e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc";

static KaraokeCacheConfig make_cfg(const char *root)
{
    KaraokeCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = root;
    cfg.model_id = MODEL_ID;
    cfg.model_sha256 = MODEL_SHA;
    cfg.overlap = KARAOKE_CACHE_OVERLAP;
    cfg.soft_inst_mix = KARAOKE_CACHE_SOFT_INST_MIX;
    cfg.limit_mb = 0;   /* 默认 2048 */
    return cfg;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static void be24(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 16);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)v;
}

/* 写一个"只有 STREAMINFO 头 + 填充"的假 FLAC：缓存模块只读头与文件大小。 */
static int write_fake_flac(const char *path, int sample_rate, int channels,
                           int bits, long long frames, size_t audio_bytes)
{
    unsigned char buf[4 + 4 + 34];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "fLaC", 4);
    buf[4] = 0x80;   /* last metadata block, type 0 = STREAMINFO */
    be24(buf + 5, 34);
    unsigned char *si = buf + 8;
    si[0] = 0x10;
    si[1] = 0x00;   /* min block size 4096 */
    si[2] = 0x10;
    si[3] = 0x00;   /* max block size 4096 */
    si[10] = (unsigned char)(sample_rate >> 12);
    si[11] = (unsigned char)((sample_rate >> 4) & 0xff);
    si[12] = (unsigned char)(((sample_rate & 0x0f) << 4) |
                             (((channels - 1) & 0x07) << 1) |
                             (((bits - 1) >> 4) & 0x01));
    si[13] = (unsigned char)((((bits - 1) & 0x0f) << 4) |
                             ((frames >> 32) & 0x0f));
    si[14] = (unsigned char)((frames >> 24) & 0xff);
    si[15] = (unsigned char)((frames >> 16) & 0xff);
    si[16] = (unsigned char)((frames >> 8) & 0xff);
    si[17] = (unsigned char)(frames & 0xff);

    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(buf, 1, sizeof(buf), f) == sizeof(buf);
    unsigned char zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    size_t left = audio_bytes;
    while (ok && left > 0) {
        size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
        ok = fwrite(zeros, 1, n, f) == n;
        left -= n;
    }
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

/* 造一个曲目文件（内容随便），返回路径。 */
static int make_track(const char *dir, const char *name, long long size)
{
    char path[KARAOKE_CACHE_PATH_CAP];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unsigned char *buf = malloc((size_t)size);
    if (!buf)
        return -1;
    memset(buf, 0x5a, (size_t)size);
    int rc = write_file(path, buf, (size_t)size);
    free(buf);
    return rc;
}

/* 提交一份合法缓存（两份假 FLAC + meta），返回 0/-1。 */
static int prepare_cache(const KaraokeCacheConfig *cfg, const char *track_path,
                         long long frames, int rate, long long audio_bytes,
                         KaraokeCachePaths *paths_out)
{
    KaraokeCachePaths paths;
    char inst_tmp[KARAOKE_CACHE_PATH_CAP], soft_tmp[KARAOKE_CACHE_PATH_CAP];
    char meta_tmp[KARAOKE_CACHE_PATH_CAP];
    char err[KARAOKE_CACHE_ERR_CAP] = {0};
    if (karaoke_cache_begin(cfg, track_path, &paths, inst_tmp, sizeof(inst_tmp),
                            soft_tmp, sizeof(soft_tmp), meta_tmp, sizeof(meta_tmp),
                            err, sizeof(err)) != 0) {
        fprintf(stderr, "  begin: %s\n", err);
        return -1;
    }
    if (write_fake_flac(inst_tmp, rate, 2, 16, frames, (size_t)audio_bytes) != 0 ||
        write_fake_flac(soft_tmp, rate, 2, 16, frames, (size_t)audio_bytes) != 0) {
        karaoke_cache_abort_temps(inst_tmp, soft_tmp, meta_tmp);
        return -1;
    }
    if (karaoke_cache_write_meta(meta_tmp, cfg, frames, rate, frames, frames,
                                 err, sizeof(err)) != 0) {
        fprintf(stderr, "  meta: %s\n", err);
        karaoke_cache_abort_temps(inst_tmp, soft_tmp, meta_tmp);
        return -1;
    }
    if (karaoke_cache_commit(cfg, &paths, inst_tmp, soft_tmp, meta_tmp,
                             err, sizeof(err)) != 0) {
        fprintf(stderr, "  commit: %s\n", err);
        karaoke_cache_abort_temps(inst_tmp, soft_tmp, meta_tmp);
        return -1;
    }
    if (paths_out)
        *paths_out = paths;
    return 0;
}

/* ── 1. 键与路径 ─────────────────────────────────────────────────── */

static void test_paths_and_keys(const char *root, const char *track_dir)
{
    KaraokeCacheConfig cfg = make_cfg(root);
    char track[KARAOKE_CACHE_PATH_CAP];
    snprintf(track, sizeof(track), "%s/a.flac", track_dir);
    CHECK(make_track(track_dir, "a.flac", 1024) == 0, "造曲目文件");

    KaraokeCachePaths p1, p2;
    char err[KARAOKE_CACHE_ERR_CAP] = {0};
    CHECK(karaoke_cache_paths(&cfg, track, &p1, err, sizeof(err)) == 0,
          err[0] ? err : "计算路径");
    CHECK(strlen(p1.hash) == 64, "track_hash 是 64 位 hex");
    CHECK(strstr(p1.dir, MODEL_ID) != NULL, "目录含 model_id");
    CHECK(strstr(p1.dir, p1.hash) != NULL, "目录含 track_hash");
    CHECK(strstr(p1.dir, root) == p1.dir, "目录在 root 子树内");
    CHECK(strcmp(p1.inst_path, p1.dir) != 0, "inst 路径非空");
    char check[KARAOKE_CACHE_PATH_CAP];
    snprintf(check, sizeof(check), "%s/inst.flac", p1.dir);
    CHECK(strcmp(p1.inst_path, check) == 0, "inst 路径 = dir/inst.flac");
    snprintf(check, sizeof(check), "%s/soft.flac", p1.dir);
    CHECK(strcmp(p1.soft_path, check) == 0, "soft 路径 = dir/soft.flac");
    snprintf(check, sizeof(check), "%s/meta.txt", p1.dir);
    CHECK(strcmp(p1.meta_path, check) == 0, "meta 路径 = dir/meta.txt");

    /* 同一路径/大小/mtime 多次计算稳定 */
    CHECK(karaoke_cache_paths(&cfg, track, &p2, NULL, 0) == 0 &&
              strcmp(p1.hash, p2.hash) == 0,
          "同一曲目键稳定");

    /* mtime 变化 → 键变化 */
    struct timespec times[2];
    times[0].tv_sec = 1700000000;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    CHECK(utimensat(AT_FDCWD, track, times, 0) == 0, "设置曲目 mtime");
    CHECK(karaoke_cache_paths(&cfg, track, &p2, NULL, 0) == 0 &&
              strcmp(p1.hash, p2.hash) != 0,
          "mtime 变化后键变化（自动失效）");

    /* 大小变化 → 键变化 */
    CHECK(make_track(track_dir, "a.flac", 2048) == 0, "改曲目大小");
    CHECK(karaoke_cache_paths(&cfg, track, &p2, NULL, 0) == 0 &&
              strcmp(p1.hash, p2.hash) != 0,
          "大小变化后键变化");

    /* 模型/参数变化 → 键变化 */
    KaraokeCacheConfig cfg2 = cfg;
    cfg2.model_id = "other-model";
    CHECK(karaoke_cache_paths(&cfg2, track, &p2, NULL, 0) == 0 &&
              strcmp(p1.hash, p2.hash) != 0,
          "换模型 id 后键变化");
    cfg2 = cfg;
    cfg2.model_sha256 =
        "0000000000000000000000000000000000000000000000000000000000000000";
    CHECK(karaoke_cache_paths(&cfg2, track, &p2, NULL, 0) == 0 &&
              strcmp(p1.hash, p2.hash) != 0,
          "换模型 SHA256 后键变化");
    cfg2 = cfg;
    cfg2.overlap = 0.5f;
    CHECK(karaoke_cache_paths(&cfg2, track, &p2, NULL, 0) == 0 &&
              strcmp(p1.hash, p2.hash) != 0,
          "改 overlap 后键变化");
    cfg2 = cfg;
    cfg2.soft_inst_mix = 0.5f;
    CHECK(karaoke_cache_paths(&cfg2, track, &p2, NULL, 0) == 0 &&
              strcmp(p1.hash, p2.hash) != 0,
          "改 soft 系数后键变化");

    /* 不存在的路径 / 目录 → 失败 */
    CHECK(karaoke_cache_paths(&cfg, "/tmp/tm-karaoke-cache-no-such-file", &p2,
                              NULL, 0) == -1,
          "不存在的曲目拒绝算键");
    CHECK(karaoke_cache_paths(&cfg, track_dir, &p2, NULL, 0) == -1,
          "目录拒绝算键");
}

/* ── 2. 提交/命中/损坏 ──────────────────────────────────────────── */

static void test_commit_and_resolve(const char *root, const char *track_dir)
{
    KaraokeCacheConfig cfg = make_cfg(root);
    char track[KARAOKE_CACHE_PATH_CAP];
    snprintf(track, sizeof(track), "%s/b.flac", track_dir);
    CHECK(make_track(track_dir, "b.flac", 4096) == 0, "造曲目 b");

    KaraokeCachePaths paths;
    char err[KARAOKE_CACHE_ERR_CAP] = {0};
    CHECK(karaoke_cache_complete(&cfg, track) == 0, "未提交前 complete=0");
    char out[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_FULL, out, sizeof(out),
                                err, sizeof(err)) == 0,
          "未提交前 resolve=0");

    CHECK(prepare_cache(&cfg, track, 44100, 44100, 20000, &paths) == 0,
          "成对提交缓存");
    CHECK(karaoke_cache_complete(&cfg, track) == 1, "提交后 complete=1");
    CHECK(access(paths.inst_path, F_OK) == 0 && access(paths.soft_path, F_OK) == 0,
          "两份 FLAC 均存在");
    CHECK(access(paths.meta_path, F_OK) == 0, "meta 存在");

    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_FULL, out, sizeof(out),
                                err, sizeof(err)) == 1,
          err[0] ? err : "FULL 命中");
    CHECK(strcmp(out, paths.inst_path) == 0, "FULL 返回 inst.flac");
    out[0] = '\0';
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_SOFT, out, sizeof(out),
                                err, sizeof(err)) == 1,
          "SOFT 命中");
    CHECK(strcmp(out, paths.soft_path) == 0, "SOFT 返回 soft.flac");
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_OFF, out, sizeof(out),
                                err, sizeof(err)) == 0,
          "OFF 不查缓存");

    /* 命中 touch：把 mtime 改旧，resolve 后应变新。 */
    struct timespec old[2];
    old[0].tv_sec = 1600000000;
    old[0].tv_nsec = 0;
    old[1] = old[0];
    CHECK(utimensat(AT_FDCWD, paths.inst_path, old, 0) == 0, "把 inst mtime 改旧");
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_FULL, out, sizeof(out),
                                err, sizeof(err)) == 1,
          "改旧后仍命中");
    struct stat st;
    CHECK(stat(paths.inst_path, &st) == 0 && st.st_mtime > 1600000000,
          "命中 touch 更新 mtime（LRU 最近使用）");

    /* 静音拒绝：文件大小低于压缩比下限（frames*4/25）。 */
    char track2[KARAOKE_CACHE_PATH_CAP];
    snprintf(track2, sizeof(track2), "%s/c.flac", track_dir);
    CHECK(make_track(track_dir, "c.flac", 4096) == 0, "造曲目 c");
    KaraokeCachePaths p2;
    char inst_tmp[KARAOKE_CACHE_PATH_CAP], soft_tmp[KARAOKE_CACHE_PATH_CAP];
    char meta_tmp[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_begin(&cfg, track2, &p2, inst_tmp, sizeof(inst_tmp),
                              soft_tmp, sizeof(soft_tmp), meta_tmp,
                              sizeof(meta_tmp), err, sizeof(err)) == 0,
          "begin c");
    CHECK(write_fake_flac(inst_tmp, 44100, 2, 16, 44100, 100) == 0 &&
              write_fake_flac(soft_tmp, 44100, 2, 16, 44100, 100) == 0,
          "写静音假 FLAC");
    CHECK(karaoke_cache_write_meta(meta_tmp, &cfg, 44100, 44100, 44100, 44100,
                                   err, sizeof(err)) == 0,
          "写 meta c");
    CHECK(karaoke_cache_commit(&cfg, &p2, inst_tmp, soft_tmp, meta_tmp,
                               err, sizeof(err)) == -1,
          "静音缓存拒绝提交");
    CHECK(access(p2.inst_path, F_OK) != 0 && access(p2.soft_path, F_OK) != 0,
          "拒绝时不留正式文件");
    karaoke_cache_abort_temps(inst_tmp, soft_tmp, meta_tmp);

    /* 帧数与 meta 不符 → 拒绝 */
    CHECK(karaoke_cache_begin(&cfg, track2, &p2, inst_tmp, sizeof(inst_tmp),
                              soft_tmp, sizeof(soft_tmp), meta_tmp,
                              sizeof(meta_tmp), err, sizeof(err)) == 0,
          "begin c2");
    CHECK(write_fake_flac(inst_tmp, 44100, 2, 16, 44100, 20000) == 0 &&
              write_fake_flac(soft_tmp, 44100, 2, 16, 44100, 20000) == 0,
          "写帧数不符假 FLAC");
    CHECK(karaoke_cache_write_meta(meta_tmp, &cfg, 48000, 44100, 48000, 48000,
                                   err, sizeof(err)) == 0,
          "写不一致 meta");
    CHECK(karaoke_cache_commit(&cfg, &p2, inst_tmp, soft_tmp, meta_tmp,
                               err, sizeof(err)) == -1,
          "帧数/时长不符拒绝提交");
    karaoke_cache_abort_temps(inst_tmp, soft_tmp, meta_tmp);

    /* 损坏：把已提交的 inst 头改坏 → resolve=0 且目录被清理 */
    int fd = open(paths.inst_path, O_WRONLY);
    CHECK(fd >= 0, "打开 inst 以破坏");
    if (fd >= 0) {
        CHECK(write(fd, "XXXX", 4) == 4, "破坏 FLAC 魔数");
        close(fd);
    }
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_FULL, out, sizeof(out),
                                err, sizeof(err)) == 0,
          "损坏缓存 resolve=0");
    CHECK(access(paths.dir, F_OK) != 0, "损坏目录已清理");
}

/* ── 3. 半成品/缺一份 → 清理 ───────────────────────────────────── */

static void test_partial_pair(const char *root, const char *track_dir)
{
    KaraokeCacheConfig cfg = make_cfg(root);
    char track[KARAOKE_CACHE_PATH_CAP];
    snprintf(track, sizeof(track), "%s/d.flac", track_dir);
    CHECK(make_track(track_dir, "d.flac", 8192) == 0, "造曲目 d");
    KaraokeCachePaths paths;
    CHECK(prepare_cache(&cfg, track, 44100, 44100, 20000, &paths) == 0,
          "提交 d 缓存");
    CHECK(unlink(paths.soft_path) == 0, "删掉 soft 造成半成品");
    char out[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_FULL, out, sizeof(out),
                                NULL, 0) == 0,
          "缺一份 → resolve=0");
    CHECK(access(paths.dir, F_OK) != 0, "半成品目录已清理");
}

/* ── 4. LRU / usage / 启动清理 ─────────────────────────────────── */

static void test_lru_and_cleanup(const char *root, const char *track_dir)
{
    KaraokeCacheConfig cfg = make_cfg(root);
    KaraokeCachePaths paths[3];
    char tracks[3][KARAOKE_CACHE_PATH_CAP];
    for (int i = 0; i < 3; i++) {
        snprintf(tracks[i], sizeof(tracks[i]), "%s/lru%d.flac", track_dir, i);
        char name[32];
        snprintf(name, sizeof(name), "lru%d.flac", i);
        CHECK(make_track(track_dir, name, 4096) == 0, "造 LRU 曲目");
        /* 两份假 FLAC 各 300KB（> frames*4/25=16000 下限）。 */
        CHECK(prepare_cache(&cfg, tracks[i], 100000, 44100, 300000, &paths[i]) == 0,
              "提交 LRU 缓存");
    }
    int tracks_used = 0;
    long long used_before = karaoke_cache_usage_mb(&cfg, &tracks_used);
    CHECK(used_before >= 1 && tracks_used == 3, "usage 统计 3 个曲目");

    /* 把目录 mtime 改旧，模拟跨天使用；limit 压到 1MB 触发淘汰。 */
    struct timespec old[2];
    old[0].tv_sec = 1600000000;
    old[0].tv_nsec = 0;
    old[1] = old[0];
    for (int i = 0; i < 3; i++)
        CHECK(utimensat(AT_FDCWD, paths[i].dir, old, 0) == 0, "目录 mtime 改旧");
    /* 再加一个更新的目录作为"当前曲目"保护对象 */
    KaraokeCacheConfig cfg_small = cfg;
    cfg_small.limit_mb = 1;
    char err[KARAOKE_CACHE_ERR_CAP] = {0};
    CHECK(karaoke_cache_evict(&cfg_small, paths[0].dir, err, sizeof(err)) == 0,
          "LRU 淘汰执行");
    CHECK(access(paths[0].dir, F_OK) == 0, "exclude 目录不被淘汰");
    int remain = 0;
    for (int i = 0; i < 3; i++)
        if (access(paths[i].dir, F_OK) == 0)
            remain++;
    CHECK(remain == 1, "LRU 淘汰到上限内（仅保留 1 个）");
    long long used_after = karaoke_cache_usage_mb(&cfg, NULL);
    CHECK(used_after <= 1, "淘汰后占用 ≤ 1MB");

    /* 启动清理：过期 .part.* 删除；新鲜 .part.* 保留 */
    char part_old[KARAOKE_CACHE_PATH_CAP], part_new[KARAOKE_CACHE_PATH_CAP];
    snprintf(part_old, sizeof(part_old), "%s/.inst.flac.part.999.1", paths[0].dir);
    snprintf(part_new, sizeof(part_new), "%s/.soft.flac.part.999.2", paths[0].dir);
    CHECK(write_file(part_old, "x", 1) == 0 && write_file(part_new, "y", 1) == 0,
          "造 .part 文件");
    CHECK(utimensat(AT_FDCWD, part_old, old, 0) == 0, "旧 .part mtime 改旧");
    karaoke_cache_cleanup_stale(&cfg);
    CHECK(access(part_old, F_OK) != 0, "过期 .part 被清理");
    CHECK(access(part_new, F_OK) == 0, "新鲜 .part 保留");

    /* remove：按曲目删目录（F2：目录含在写 .part 时拒绝；清掉后成功） */
    CHECK(karaoke_cache_remove(&cfg, tracks[0]) != 0, "在写 .part 时 remove 拒绝");
    CHECK(access(paths[0].dir, F_OK) == 0, "拒绝后目录仍在");
    unlink(part_new);
    CHECK(karaoke_cache_remove(&cfg, tracks[0]) == 0, "按曲目删除缓存");
    CHECK(access(paths[0].dir, F_OK) != 0, "删除后目录不存在");
}

/* F2 回归（t8 审计）：worker 正在写（存在 .part 半成品）时，任何"清理不完整
 * 目录"的路径都不得删除该目录与在写文件；去掉 .part 后损坏清理照旧。 */
static void test_remove_skips_inflight(const char *cache_root, const char *track_dir)
{
    KaraokeCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = cache_root;
    cfg.model_id = "uvr-mdxnet-3-9662";
    cfg.model_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    cfg.overlap = KARAOKE_CACHE_OVERLAP;
    cfg.soft_inst_mix = KARAOKE_CACHE_SOFT_INST_MIX;
    cfg.limit_mb = KARAOKE_CACHE_LIMIT_MB;

    char track[KARAOKE_CACHE_PATH_CAP];
    snprintf(track, sizeof(track), "%s/inflight.flac", track_dir);
    CHECK(make_track(track_dir, "inflight.flac", 4096) == 0, "造在写曲目");

    KaraokeCachePaths paths;
    char inst_tmp[KARAOKE_CACHE_PATH_CAP], soft_tmp[KARAOKE_CACHE_PATH_CAP];
    char meta_tmp[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_begin(&cfg, track, &paths, inst_tmp, sizeof(inst_tmp),
                              soft_tmp, sizeof(soft_tmp), meta_tmp,
                              sizeof(meta_tmp), NULL, 0) == 0,
          "begin 在写缓存");
    CHECK(write_file(inst_tmp, "inflight", 8) == 0 &&
              write_file(soft_tmp, "inflight", 8) == 0,
          "写 .part 半成品");

    /* 不完整缓存 + 在写 .part：resolve 不得删目录/半成品。 */
    char resolved[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_FULL, resolved,
                                sizeof(resolved), NULL, 0) == 0,
          "在写中的不完整缓存不命中");
    CHECK(access(paths.dir, F_OK) == 0, "在写目录未被 resolve 删除");
    CHECK(access(inst_tmp, F_OK) == 0 && access(soft_tmp, F_OK) == 0,
          "在写 .part 未被 resolve 删除");

    /* remove 同样必须跳过在写目录。 */
    CHECK(karaoke_cache_remove(&cfg, track) != 0, "remove 拒绝删除在写目录");
    CHECK(access(paths.dir, F_OK) == 0, "在写目录未被 remove 删除");
    CHECK(access(inst_tmp, F_OK) == 0 && access(soft_tmp, F_OK) == 0,
          "在写 .part 未被 remove 删除");

    /* 模拟 worker 取消：清掉 .part 后，损坏目录按现有逻辑清理。 */
    karaoke_cache_abort_temps(inst_tmp, soft_tmp, meta_tmp);
    CHECK(access(inst_tmp, F_OK) != 0, ".part 已清理");
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_FULL, resolved,
                                sizeof(resolved), NULL, 0) == 0,
          "损坏缓存不命中");
    CHECK(access(paths.dir, F_OK) != 0, "无在写文件时损坏目录被清理");
}

int main(void)
{
    char root[] = "/tmp/tm-karaoke-cache-XXXXXX";
    char track_dir[] = "/tmp/tm-karaoke-track-XXXXXX";
    char *root_dir = mkdtemp(root);
    char *tdir = mkdtemp(track_dir);
    CHECK(root_dir != NULL && tdir != NULL, "创建临时目录");
    if (!root_dir || !tdir) {
        fprintf(stderr, "FAIL: 无法创建临时目录\n");
        return 1;
    }
    char cache_root[KARAOKE_CACHE_PATH_CAP];
    snprintf(cache_root, sizeof(cache_root), "%s/karaoke", root_dir);

    test_paths_and_keys(cache_root, tdir);
    test_commit_and_resolve(cache_root, tdir);
    test_partial_pair(cache_root, tdir);
    test_lru_and_cleanup(cache_root, tdir);
    test_remove_skips_inflight(cache_root, tdir);

    char cmd[KARAOKE_CACHE_PATH_CAP * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", root_dir, tdir);
    (void)system(cmd);

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS test_karaoke_cache\n");
    return 0;
}
