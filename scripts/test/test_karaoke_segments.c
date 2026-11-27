/**
 * @file test_karaoke_segments.c
 * @brief v2 会话级分段缓存单测（/tmp 生命周期、manifest 原子读写、
 *        ready 标记/帧数校验、段布局/段帧数计算、v1/v2 选择）
 *
 * 被测模块：audio/karaoke_cache.c 的 v2 部分（第 7 节）。只解析 FLAC
 * STREAMINFO 头，因此不需要 FFmpeg/真实模型；真实模型与真实曲目由
 * scripts/test/karaoke-segments-e2e.sh 覆盖。
 *
 * 覆盖（对应 T4 验收）：
 *   - 根目录 `/tmp/ter-music-karaoke-<uid>-<pid>/`、0700、首次创建；
 *   - 启动清理同 uid 且 pid 已不存在的陈旧目录；正常退出删除本进程目录；
 *   - 段布局 seg%03d.inst.flac / seg%03d.soft.flac / seg%03d.ready +
 *     manifest.txt，路径全在会话根子树内；
 *   - manifest 字段齐全、原子替换（无 .part 残留）、complete 语义；
 *   - ready 最后写：缺文件 / 截断 / 坏标记 / 段号不符 / 帧数不符都判未就绪；
 *   - 段帧数：非末段 661500、末段为剩余帧；总段数向上取整；
 *   - v1 整曲缓存完整时不会产生 v2 分段（只读兼容、优先）。
 *
 * 依赖策略：无外部依赖；失败即 exit 1。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#include "audio/karaoke_cache.h"
#include "audio/karaoke_mode.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ── 小工具 ─────────────────────────────────────────────────────── */

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

/* 只带 STREAMINFO 头的假 FLAC + 填充（缓存模块只读头与文件大小）。 */
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
    si[1] = 0x00;
    si[2] = 0x10;
    si[3] = 0x00;
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

static int make_track(const char *dir, const char *name, size_t size)
{
    char path[KARAOKE_CACHE_PATH_CAP];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unsigned char *buf = malloc(size);
    if (!buf)
        return -1;
    memset(buf, 0x5a, size);
    int rc = write_file(path, buf, size);
    free(buf);
    return rc;
}

static int count_dir_entries(const char *dir, const char *needle)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (!needle || strstr(e->d_name, needle) != NULL)
            n++;
    }
    closedir(d);
    return n;
}

/* 造一个完整可用的 v1 成对缓存（假 FLAC + meta + commit）。 */
static int prepare_v1_cache(const KaraokeCacheConfig *cfg, const char *track,
                            long long frames, int rate, KaraokeCachePaths *out)
{
    KaraokeCachePaths paths;
    char inst_tmp[KARAOKE_CACHE_PATH_CAP], soft_tmp[KARAOKE_CACHE_PATH_CAP];
    char meta_tmp[KARAOKE_CACHE_PATH_CAP];
    char err[KARAOKE_CACHE_ERR_CAP] = {0};
    if (karaoke_cache_begin(cfg, track, &paths, inst_tmp, sizeof(inst_tmp),
                            soft_tmp, sizeof(soft_tmp), meta_tmp,
                            sizeof(meta_tmp), err, sizeof(err)) != 0) {
        fprintf(stderr, "  begin: %s\n", err);
        return -1;
    }
    if (write_fake_flac(inst_tmp, rate, 2, 16, frames, 8192) != 0 ||
        write_fake_flac(soft_tmp, rate, 2, 16, frames, 8192) != 0) {
        karaoke_cache_abort_temps(inst_tmp, soft_tmp, meta_tmp);
        return -1;
    }
    if (karaoke_cache_write_meta(meta_tmp, cfg, frames, rate, frames, frames,
                                 err, sizeof(err)) != 0) {
        karaoke_cache_abort_temps(inst_tmp, soft_tmp, meta_tmp);
        return -1;
    }
    if (karaoke_cache_commit(cfg, &paths, inst_tmp, soft_tmp, meta_tmp, err,
                             sizeof(err)) != 0) {
        karaoke_cache_abort_temps(inst_tmp, soft_tmp, meta_tmp);
        return -1;
    }
    if (out)
        *out = paths;
    return 0;
}

/* 写一对合法段 FLAC + ready 标记。 */
static int write_segment(const KaraokeSegmentPaths *p, int seg, long long frames,
                         int ready)
{
    char inst[KARAOKE_CACHE_PATH_CAP], soft[KARAOKE_CACHE_PATH_CAP];
    if (karaoke_cache_segment_file(p, seg, 0, inst, sizeof(inst)) != 0 ||
        karaoke_cache_segment_file(p, seg, 1, soft, sizeof(soft)) != 0)
        return -1;
    if (write_fake_flac(inst, KARAOKE_SEGMENT_SAMPLE_RATE,
                        KARAOKE_SEGMENT_CHANNELS, 16, frames, 4096) != 0 ||
        write_fake_flac(soft, KARAOKE_SEGMENT_SAMPLE_RATE,
                        KARAOKE_SEGMENT_CHANNELS, 16, frames, 4096) != 0)
        return -1;
    if (!ready)
        return 0;
    char err[KARAOKE_CACHE_ERR_CAP] = {0};
    return karaoke_cache_segment_mark_ready(p, seg, frames, err, sizeof(err));
}

static long long dead_pid(void)
{
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    if (pid < 0)
        return -1;
    (void)waitpid(pid, NULL, 0);
    return (long long)pid;
}

/* ── 用例 ───────────────────────────────────────────────────────── */

static void test_paths_and_layout(const char *root_dir, const char *track)
{
    KaraokeCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = root_dir;
    cfg.model_id = "uvr-mdxnet-3-9662";
    cfg.model_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    cfg.overlap = KARAOKE_CACHE_OVERLAP;
    cfg.soft_inst_mix = KARAOKE_CACHE_SOFT_INST_MIX;

    KaraokeSegmentPaths p;
    char err[KARAOKE_CACHE_ERR_CAP] = {0};
    CHECK(karaoke_cache_segment_paths(&cfg, track, &p, err, sizeof(err)) == 0,
          "v2 路径计算成功");
    CHECK(strlen(p.hash) == 64, "track_hash 为 64 位 hex");
    CHECK(strncmp(p.dir, root_dir, strlen(root_dir)) == 0,
          "段目录在会话根子树内");
    char expect[KARAOKE_CACHE_PATH_CAP];
    snprintf(expect, sizeof(expect), "%s/%s/%s", root_dir, cfg.model_id, p.hash);
    CHECK(strcmp(p.dir, expect) == 0, "dir = root/<model_id>/<track_hash>");
    snprintf(expect, sizeof(expect), "%s/manifest.txt", p.dir);
    CHECK(strcmp(p.manifest_path, expect) == 0, "manifest = dir/manifest.txt");

    char f[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_segment_file(&p, 0, 0, f, sizeof(f)) == 0 &&
              strcmp(f, (snprintf(expect, sizeof(expect), "%s/seg000.inst.flac",
                                  p.dir), expect)) == 0,
          "段文件名 seg000.inst.flac");
    CHECK(karaoke_cache_segment_file(&p, 12, 1, f, sizeof(f)) == 0 &&
              strcmp(f, (snprintf(expect, sizeof(expect), "%s/seg012.soft.flac",
                                  p.dir), expect)) == 0,
          "段文件名 seg012.soft.flac");
    CHECK(karaoke_cache_segment_marker(&p, 7, f, sizeof(f)) == 0 &&
              strcmp(f, (snprintf(expect, sizeof(expect), "%s/seg007.ready",
                                  p.dir), expect)) == 0,
          "ready 标记 seg007.ready");

    /* v2 键材料包含 44.1k/15s/模型信息：换模型目录不同。 */
    KaraokeCacheConfig cfg2 = cfg;
    cfg2.model_id = "other-model";
    KaraokeSegmentPaths p2;
    CHECK(karaoke_cache_segment_paths(&cfg2, track, &p2, NULL, 0) == 0 &&
              strcmp(p2.dir, p.dir) != 0,
          "不同模型 id → 不同段目录");
}

static void test_frame_math(void)
{
    int total = -1;
    CHECK(karaoke_cache_segment_count(0, &total) == 0 && total == 1,
          "0 帧按 1 段处理");
    CHECK(karaoke_cache_segment_count(661500, &total) == 0 && total == 1,
          "恰 15s → 1 段");
    CHECK(karaoke_cache_segment_count(661501, &total) == 0 && total == 2,
          "15s+1 帧 → 2 段");
    CHECK(karaoke_cache_segment_count(3 * 661500, &total) == 0 && total == 3,
          "3×15s → 3 段");

    KaraokeSegmentManifest m;
    memset(&m, 0, sizeof(m));
    m.complete = 1;
    m.total_segments = 3;
    m.source_frames = 2 * 661500 + 12345;
    long long frames = -1;
    CHECK(karaoke_cache_segment_expected(&m, 0, &frames) == 0 &&
              frames == 661500,
          "非末段 = 661500 帧");
    CHECK(karaoke_cache_segment_expected(&m, 1, &frames) == 0 &&
              frames == 661500,
          "非末段 2 = 661500 帧");
    CHECK(karaoke_cache_segment_expected(&m, 2, &frames) == 0 &&
              frames == 12345,
          "末段 = 剩余帧");
    CHECK(karaoke_cache_segment_expected(&m, 3, &frames) != 0,
          "越界段返回未知");
    m.complete = 0;
    CHECK(karaoke_cache_segment_expected(&m, 2, &frames) == 0 && frames == 12345,
          "complete=0 但 total/source 已知时给期望（EOF 后末段短帧可见）");
    m.total_segments = 0;
    m.source_frames = 0;
    CHECK(karaoke_cache_segment_expected(&m, 0, &frames) != 0,
          "total/source 未知时不给期望帧数");
}

static void test_manifest(const KaraokeSegmentPaths *p)
{
    KaraokeSegmentManifest m;
    memset(&m, 0, sizeof(m));
    m.format = KARAOKE_SEGMENT_FORMAT_VERSION;
    snprintf(m.model_id, sizeof(m.model_id), "uvr-mdxnet-3-9662");
    snprintf(m.model_sha256, sizeof(m.model_sha256),
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    m.overlap = 0.25;
    m.soft_inst = 0.60;
    m.sample_rate = 44100;
    m.segment_seconds = 15;
    m.total_segments = 0;
    m.source_frames = 0;
    m.complete = 0;
    CHECK(karaoke_cache_segment_manifest_write(p, &m, NULL, 0) == 0,
          "写未完成 manifest");
    CHECK(count_dir_entries(p->dir, ".part.") == 0,
          "manifest 原子写后无 .part 残留");

    KaraokeSegmentManifest r;
    memset(&r, 0, sizeof(r));
    CHECK(karaoke_cache_segment_manifest_read(p->manifest_path, &r, NULL, 0) == 0,
          "读回 manifest");
    CHECK(r.format == KARAOKE_SEGMENT_FORMAT_VERSION && r.sample_rate == 44100 &&
              r.segment_seconds == 15 && r.complete == 0,
          "manifest 关键字段回读一致");

    m.total_segments = 2;
    m.source_frames = 2 * 661500;
    m.complete = 1;
    CHECK(karaoke_cache_segment_manifest_write(p, &m, NULL, 0) == 0,
          "原子更新为完整 manifest");
    memset(&r, 0, sizeof(r));
    CHECK(karaoke_cache_segment_manifest_read(p->manifest_path, &r, NULL, 0) == 0 &&
              r.complete == 1 && r.total_segments == 2 &&
              r.source_frames == 2 * 661500,
          "完整 manifest 回读一致");

    /* 截断/缺字段的 manifest 必须被拒绝。 */
    char bad[KARAOKE_CACHE_PATH_CAP];
    snprintf(bad, sizeof(bad), "%s/bad.manifest", p->dir);
    CHECK(write_file(bad, "format=2\nmodel_id=x\n", 20) == 0,
          "写坏 manifest");
    KaraokeSegmentManifest rb;
    CHECK(karaoke_cache_segment_manifest_read(bad, &rb, NULL, 0) != 0,
          "字段不全的 manifest 被拒绝");
    unlink(bad);
}

static void test_ready_validation(const KaraokeSegmentPaths *p,
                                  KaraokeSegmentManifest *m)
{
    long long frames = 0;
    CHECK(karaoke_cache_segment_ready(p, m, 3, &frames, NULL, 0) == 0,
          "缺段文件 → 未就绪");

    /* 非末段满段：未完成 manifest 也接受（首段起播要在 job 完成前可见）。 */
    m->complete = 0;
    m->total_segments = 0;
    m->source_frames = 0;
    CHECK(write_segment(p, 3, KARAOKE_SEGMENT_FRAMES, 0) == 0,
          "写段 3 的成对 FLAC（无 ready）");
    CHECK(karaoke_cache_segment_ready(p, m, 3, &frames, NULL, 0) == 0,
          "缺 ready 标记 → 未就绪");

    CHECK(write_segment(p, 3, KARAOKE_SEGMENT_FRAMES, 1) == 0,
          "写段 3 ready 标记");
    CHECK(karaoke_cache_segment_ready(p, m, 3, &frames, NULL, 0) == 1 &&
              frames == KARAOKE_SEGMENT_FRAMES,
          "成对 FLAC + ready → 就绪");

    /* 截断检测 */
    char soft[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_segment_file(p, 3, 1, soft, sizeof(soft)) == 0,
          "取 soft 段路径");
    CHECK(truncate(soft, 100) == 0, "截断 soft 段文件");
    CHECK(karaoke_cache_segment_ready(p, m, 3, NULL, NULL, 0) == 0,
          "截断的段文件 → 未就绪");
    CHECK(write_segment(p, 3, KARAOKE_SEGMENT_FRAMES, 1) == 0,
          "恢复段 3 文件");

    /* 坏标记 / 段号不符 */
    char marker[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_segment_marker(p, 4, marker, sizeof(marker)) == 0,
          "取段 4 标记路径");
    CHECK(write_segment(p, 4, KARAOKE_SEGMENT_FRAMES, 0) == 0,
          "写段 4 成对 FLAC");
    CHECK(write_file(marker, "garbage\n", 8) == 0, "写坏 ready 标记");
    CHECK(karaoke_cache_segment_ready(p, m, 4, NULL, NULL, 0) == 0,
          "坏标记 → 未就绪");
    char content[64];
    snprintf(content, sizeof(content), "seg=5\nframes=%d\n",
             (int)KARAOKE_SEGMENT_FRAMES);
    CHECK(write_file(marker, content, strlen(content)) == 0,
          "写段号不符的 ready 标记");
    CHECK(karaoke_cache_segment_ready(p, m, 4, NULL, NULL, 0) == 0,
          "段号不符 → 未就绪");

    /* 帧数不符：标记 661500，FLAC 里只声明 1000 帧 */
    char inst[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_segment_file(p, 4, 0, inst, sizeof(inst)) == 0 &&
              karaoke_cache_segment_file(p, 4, 1, soft, sizeof(soft)) == 0,
          "取段 4 文件路径");
    CHECK(write_fake_flac(inst, 44100, 2, 16, 1000, 4096) == 0 &&
              write_fake_flac(soft, 44100, 2, 16, 1000, 4096) == 0,
          "写帧数不符的段文件");
    snprintf(content, sizeof(content), "seg=4\nframes=%d\n",
             (int)KARAOKE_SEGMENT_FRAMES);
    CHECK(write_file(marker, content, strlen(content)) == 0,
          "写帧数 661500 的 ready 标记");
    CHECK(karaoke_cache_segment_ready(p, m, 4, NULL, NULL, 0) == 0,
          "FLAC 帧数与标记不符 → 未就绪");

    /* 完整 manifest 后，末段短帧必须与清单一致。 */
    m->complete = 1;
    m->total_segments = 5;
    m->source_frames = 4 * KARAOKE_SEGMENT_FRAMES + 12345;
    CHECK(write_segment(p, 4, KARAOKE_SEGMENT_FRAMES, 1) == 0,
          "段 4 恢复为满段");
    CHECK(karaoke_cache_segment_ready(p, m, 4, NULL, NULL, 0) == 0,
          "完整清单下末段帧数不符 → 未就绪");
    CHECK(write_segment(p, 4, 12345, 1) == 0, "末段写 12345 帧");
    CHECK(karaoke_cache_segment_ready(p, m, 4, NULL, NULL, 0) == 1,
          "末段短帧与清单一致 → 就绪");
}

static void test_resolve_and_complete(const KaraokeCacheConfig *cfg,
                                      const char *track,
                                      const KaraokeSegmentPaths *p)
{
    KaraokeSegmentManifest m;
    memset(&m, 0, sizeof(m));
    m.format = KARAOKE_SEGMENT_FORMAT_VERSION;
    snprintf(m.model_id, sizeof(m.model_id), "%s", cfg->model_id);
    snprintf(m.model_sha256, sizeof(m.model_sha256), "%s", cfg->model_sha256);
    m.overlap = cfg->overlap;
    m.soft_inst = cfg->soft_inst_mix;
    m.sample_rate = 44100;
    m.segment_seconds = 15;
    m.total_segments = 2;
    m.source_frames = 2 * KARAOKE_SEGMENT_FRAMES;
    m.complete = 1;
    CHECK(karaoke_cache_segment_manifest_write(p, &m, NULL, 0) == 0,
          "写完整 2 段 manifest");
    CHECK(write_segment(p, 0, KARAOKE_SEGMENT_FRAMES, 1) == 0 &&
              write_segment(p, 1, KARAOKE_SEGMENT_FRAMES, 1) == 0,
          "写两段合法段文件");
    CHECK(karaoke_cache_segment_ready_for_mode(cfg, track, 0,
                                               KARAOKE_MODE_SOFT) == 1,
          "SOFT 档段 0 可用");
    CHECK(karaoke_cache_segment_ready_for_mode(cfg, track, 0,
                                               KARAOKE_MODE_FULL) == 1,
          "FULL 档段 0 可用");
    CHECK(karaoke_cache_segment_ready_for_mode(cfg, track, 0,
                                               KARAOKE_MODE_OFF) == 0,
          "OFF 档不查段缓存");
    char resolved[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_segment_resolve(cfg, track, KARAOKE_MODE_SOFT, 1,
                                        resolved, sizeof(resolved)) == 1 &&
              strstr(resolved, "seg001.soft.flac") != NULL,
          "SOFT 档解析到 seg001.soft.flac");
    CHECK(karaoke_cache_segment_resolve(cfg, track, KARAOKE_MODE_FULL, 1,
                                        resolved, sizeof(resolved)) == 1 &&
              strstr(resolved, "seg001.inst.flac") != NULL,
          "FULL 档解析到 seg001.inst.flac");
    CHECK(karaoke_cache_segment_complete(cfg, track) == 1,
          "两段齐全 → v2 完整");
    CHECK(karaoke_cache_segment_ready_for_mode(cfg, track, 2,
                                               KARAOKE_MODE_SOFT) == 0,
          "越界段不可用");
    CHECK(karaoke_cache_segment_complete(cfg, track) == 1,
          "重复查询仍完整");
}

static void test_v1_priority_no_v2(const char *root_dir, const char *track)
{
    /* v1 整曲缓存（XDG 语义）与 v2 会话根分离：v1 完整时不应产生 v2 分段。 */
    char v1_root[KARAOKE_CACHE_PATH_CAP];
    snprintf(v1_root, sizeof(v1_root), "%s/v1-cache", root_dir);
    KaraokeCacheConfig v1;
    memset(&v1, 0, sizeof(v1));
    v1.root = v1_root;
    v1.model_id = "uvr-mdxnet-3-9662";
    v1.model_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    v1.overlap = KARAOKE_CACHE_OVERLAP;
    v1.soft_inst_mix = KARAOKE_CACHE_SOFT_INST_MIX;
    v1.limit_mb = KARAOKE_CACHE_LIMIT_MB;
    CHECK(prepare_v1_cache(&v1, track, 44100, 44100, NULL) == 0,
          "准备完整 v1 成对缓存");
    CHECK(karaoke_cache_complete(&v1, track) == 1, "v1 成对缓存完整");
    CHECK(karaoke_cache_segment_lookup(&v1, track, NULL, NULL) == 0,
          "v1 完整不产生 v2 分段 manifest");
    CHECK(karaoke_cache_segment_complete(&v1, track) == 0,
          "v1 完整时 v2 不完整（选择顺序由播放侧保证 v1 优先）");

    /* 损坏的 v1 缓存仍按现有逻辑清理（安全边界内），但不会误删在写目录。 */
    KaraokeCachePaths v1_paths;
    CHECK(karaoke_cache_paths(&v1, track, &v1_paths, NULL, 0) == 0,
          "取 v1 路径");
    char part[KARAOKE_CACHE_PATH_CAP];
    snprintf(part, sizeof(part), "%s/.inst.flac.part.999", v1_paths.dir);
    CHECK(write_file(part, "x", 1) == 0, "放一个在写 .part 半成品");
    char resolved[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_resolve(&v1, track, KARAOKE_MODE_FULL, resolved,
                                sizeof(resolved), NULL, 0) == 1,
          "v1 仍命中（.part 不影响完整校验）");
    /* 制造损坏但保留 .part：resolve 不应删除整个目录（F2）。 */
    char soft_path[KARAOKE_CACHE_PATH_CAP];
    snprintf(soft_path, sizeof(soft_path), "%s/soft.flac", v1_paths.dir);
    CHECK(truncate(soft_path, 100) == 0, "截断 v1 soft.flac");
    CHECK(karaoke_cache_resolve(&v1, track, KARAOKE_MODE_FULL, resolved,
                                sizeof(resolved), NULL, 0) == 0,
          "损坏的 v1 缓存不再命中");
    CHECK(access(v1_paths.dir, F_OK) == 0,
          "目录含在写 .part 时不被清理（F2）");
    unlink(part);
    (void)karaoke_cache_resolve(&v1, track, KARAOKE_MODE_FULL, resolved,
                                sizeof(resolved), NULL, 0);
    CHECK(access(v1_paths.dir, F_OK) != 0,
          "去掉 .part 后损坏目录按现有逻辑清理");
}

static void test_session_lifecycle(void)
{
    char root[KARAOKE_CACHE_PATH_CAP];
    CHECK(karaoke_cache_session_root(root, sizeof(root)) == 0,
          "会话根可计算");
    char expect[KARAOKE_CACHE_PATH_CAP];
    snprintf(expect, sizeof(expect), "/tmp/ter-music-karaoke-%ld-%ld",
             (long)getuid(), (long)getpid());
    CHECK(strcmp(root, expect) == 0, "会话根 = /tmp/ter-music-karaoke-<uid>-<pid>");

    char err[KARAOKE_CACHE_ERR_CAP] = {0};
    CHECK(karaoke_cache_session_ensure(root, sizeof(root), err, sizeof(err)) == 0,
          "首次创建会话根");
    struct stat st;
    CHECK(stat(root, &st) == 0 && S_ISDIR(st.st_mode), "会话根是目录");
    CHECK((st.st_mode & 0777) == 0700, "会话根权限 0700");

    /* 同 uid、pid 已不存在的陈旧目录 → 启动清理删除 */
    long long dpid = dead_pid();
    CHECK(dpid > 0, "取得已退出进程的 pid");
    char stale[KARAOKE_CACHE_PATH_CAP];
    snprintf(stale, sizeof(stale), "/tmp/ter-music-karaoke-%ld-%lld",
             (long)getuid(), dpid);
    CHECK(mkdir(stale, 0700) == 0 || errno == EEXIST, "造陈旧会话目录");
    char child[KARAOKE_CACHE_PATH_CAP];
    snprintf(child, sizeof(child), "%s/leftover", stale);
    (void)write_file(child, "x", 1);
    karaoke_cache_session_cleanup_stale();
    CHECK(access(stale, F_OK) != 0, "pid 不存在的陈旧目录被清理");
    CHECK(access(root, F_OK) == 0, "清理不误删本进程目录");

    /* 当前进程目录：退出清理 */
    snprintf(child, sizeof(child), "%s/selfdata", root);
    CHECK(write_file(child, "x", 1) == 0, "本进程目录写文件");
    karaoke_cache_session_remove_self();
    CHECK(access(root, F_OK) != 0, "退出清理删除本进程目录");
}

int main(void)
{
    char tmpl[] = "/tmp/tm-karaoke-seg-XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root) {
        fprintf(stderr, "FAIL 无法创建临时目录: %s\n", strerror(errno));
        return 1;
    }
    char track[KARAOKE_CACHE_PATH_CAP];
    snprintf(track, sizeof(track), "%s/track.flac", root);
    if (make_track(root, "track.flac", 65536) != 0) {
        fprintf(stderr, "FAIL 无法创建测试曲目\n");
        return 1;
    }

    char seg_root[KARAOKE_CACHE_PATH_CAP];
    snprintf(seg_root, sizeof(seg_root), "%s/session", root);
    KaraokeCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = seg_root;
    cfg.model_id = "uvr-mdxnet-3-9662";
    cfg.model_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    cfg.overlap = KARAOKE_CACHE_OVERLAP;
    cfg.soft_inst_mix = KARAOKE_CACHE_SOFT_INST_MIX;

    KaraokeSegmentPaths paths;
    char err[KARAOKE_CACHE_ERR_CAP] = {0};
    CHECK(karaoke_cache_segment_paths(&cfg, track, &paths, err, sizeof(err)) == 0,
          "主用例 v2 路径");
    CHECK(karaoke_cache_segment_mkdir(&paths, err, sizeof(err)) == 0,
          "创建段目录");

    test_paths_and_layout(seg_root, track);
    test_frame_math();
    test_manifest(&paths);
    KaraokeSegmentManifest m;
    memset(&m, 0, sizeof(m));
    m.format = KARAOKE_SEGMENT_FORMAT_VERSION;
    snprintf(m.model_id, sizeof(m.model_id), "%s", cfg.model_id);
    snprintf(m.model_sha256, sizeof(m.model_sha256), "%s", cfg.model_sha256);
    m.overlap = cfg.overlap;
    m.soft_inst = cfg.soft_inst_mix;
    m.sample_rate = 44100;
    m.segment_seconds = 15;
    m.complete = 0;
    test_ready_validation(&paths, &m);
    test_resolve_and_complete(&cfg, track, &paths);
    test_v1_priority_no_v2(root, track);
    test_session_lifecycle();

    /* 清理临时目录 */
    char cmd[KARAOKE_CACHE_PATH_CAP + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf -- '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "warn: 清理临时目录失败\n");

    if (failures == 0) {
        printf("test_karaoke_segments: PASS\n");
        return 0;
    }
    printf("test_karaoke_segments: %d FAIL\n", failures);
    return 1;
}
