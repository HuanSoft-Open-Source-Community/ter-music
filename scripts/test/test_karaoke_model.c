/**
 * @file test_karaoke_model.c
 * @brief ONNX 模型消人声「模型域」单测：表项、校验/安装、dlopen、会话与单段分离
 *
 * 覆盖：
 *   - 受支持模型表字段完整性 / 按 id、按 SHA256 查询；
 *   - 错误路径：不存在、非普通文件、大小不符、SHA256 被篡改（伪造）都拒绝；
 *   - 原子安装：0600、models/<id>.onnx、幂等、坏副本自愈、失败不留半成品、
 *     不碰目录里的无关文件；
 *   - 运行时：dlopen libonnxruntime、版本与库名可读；
 *   - 会话：线程数、I/O 形状校验（伪造 spec 必须被拒绝）；
 *   - DSP 自检（不需要模型/运行库）：STFT→iSTFT 往返；
 *   - 单 patch 推理与单段分离：长度、有限值、伴奏 = mix − 1.035×人声；
 *   - 记录加载/推理耗时与峰值 RSS。
 *
 * 依赖策略（与 run-unit-tests.sh 的 SKIP 语义一致）：
 *   - 没有 libonnxruntime / 没有模型文件 → 打印 SKIP 并 exit 0（不算失败）；
 *   - 有模型时校验失败、形状不符、输出异常 → FAIL（exit 1）。
 *
 * 模型来源（按顺序找，第一个存在的即用）：
 *   1. $TER_MUSIC_KARAOKE_MODEL 或 $KARAOKE_MODEL_PATH
 *   2. $XDG_DATA_HOME/ter-music/models/uvr-mdxnet-3-9662.onnx
 *      （未设 XDG_DATA_HOME 时用 $HOME/.local/share/ter-music/models/…）
 *   3. 仓库内开发夹具 .tmp/karaoke-research/model/UVR_MDXNET_3_9662.onnx
 *   4. 仓库内开发夹具 .tmp/karaoke-model/UVR_MDXNET_3_9662.onnx
 *   模型文件不随仓库/包分发；前两条是生产路径，后两条只是本机联调便利。
 *
 * 注意：Debian 的 libonnxruntime 建会话时会向 stderr 打约 116KB 的
 * `Schema error: ...` 噪音。本测试在 validate/open 期间临时把 fd2 指到
 * /dev/null，避免污染测试输出（错误仍通过 err 参数返回）。
 */

#include "audio/karaoke_model.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

#include "audio/karaoke_cache.h"
#include "audio/karaoke_mode.h"
#include "audio/karaoke_worker.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define SR 44100

/* ── 小工具 ─────────────────────────────────────────────────────── */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static long peak_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long kb = 0;
    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            (void)sscanf(line + 6, "%ld", &kb);
            break;
        }
    }
    if (f)
        fclose(f);
    return kb;
}

static int saved_stderr = -1;

static void quiet_stderr_begin(void)
{
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0)
        return;
    saved_stderr = dup(STDERR_FILENO);
    (void)dup2(devnull, STDERR_FILENO);
    close(devnull);
}

static void quiet_stderr_end(void)
{
    if (saved_stderr < 0)
        return;
    (void)dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    saved_stderr = -1;
}

static int write_file_bytes(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    const unsigned char *p = (const unsigned char *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    close(fd);
    return 0;
}

/* 复制整个文件（用于伪造/截断模型副本）。 */
static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) {
        close(in);
        return -1;
    }
    unsigned char buf[256 * 1024];
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = -1;
            break;
        }
        if (n == 0)
            break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                rc = -1;
                break;
            }
            off += w;
        }
        if (rc != 0)
            break;
    }
    close(in);
    close(out);
    return rc;
}

static int file_size(const char *path, long long *out)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    *out = (long long)st.st_size;
    return 0;
}

/* 目录里是否残留安装临时文件（安装临时文件形如 ".<id>.onnx.part.<pid>.<n>"）。 */
static int dir_has_no_part_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 1;   /* 目录都不存在 = 没有半成品 */
    struct dirent *e;
    int clean = 1;
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, ".part.") != NULL) {
            clean = 0;
            break;
        }
    }
    closedir(d);
    return clean;
}

/* 找模型：返回静态/静态缓冲路径或 NULL。 */
static const char *find_model(char *buf, size_t cap,
                              const KaraokeModelSpec *spec)
{
    const char *env = getenv("TER_MUSIC_KARAOKE_MODEL");
    if (!env || !*env)
        env = getenv("KARAOKE_MODEL_PATH");
    if (env && *env && access(env, R_OK) == 0)
        return env;

    char candidate[KARAOKE_MODEL_PATH_CAP];
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(candidate, sizeof(candidate), "%s/ter-music/models/%s.onnx",
                 xdg, spec->id);
        if (access(candidate, R_OK) == 0)
            return strcpy(buf, candidate);   /* 便于返回，调用方传缓冲 */
    }
    if (home && *home) {
        snprintf(candidate, sizeof(candidate),
                 "%s/.local/share/ter-music/models/%s.onnx", home, spec->id);
        if (access(candidate, R_OK) == 0)
            return strcpy(buf, candidate);
    }
    snprintf(candidate, sizeof(candidate),
             ".tmp/karaoke-research/model/UVR_MDXNET_3_9662.onnx");
    if (access(candidate, R_OK) == 0) {
        char cwd[KARAOKE_MODEL_PATH_CAP];
        if (getcwd(cwd, sizeof(cwd)) == NULL)
            return NULL;
        snprintf(buf, cap, "%s/%s", cwd, candidate);
        return buf;
    }
    snprintf(candidate, sizeof(candidate),
             ".tmp/karaoke-model/UVR_MDXNET_3_9662.onnx");
    if (access(candidate, R_OK) == 0) {
        char cwd[KARAOKE_MODEL_PATH_CAP];
        if (getcwd(cwd, sizeof(cwd)) == NULL)
            return NULL;
        snprintf(buf, cap, "%s/%s", cwd, candidate);
        return buf;
    }
    return NULL;
}

/* ── 1. 表项 ─────────────────────────────────────────────────────── */

static void test_registry(void)
{
    size_t count = 0;
    const KaraokeModelSpec *spec = karaoke_model_supported(&count);
    CHECK(spec != NULL, "受支持表为空");
    CHECK(count >= 1, "受支持表至少 1 项");
    if (!spec || count < 1)
        return;

    for (size_t i = 0; i < count; i++) {
        const KaraokeModelSpec *m = &spec[i];
        CHECK(m->id && *m->id, "id 非空");
        CHECK(m->display_name && *m->display_name, "display_name 非空");
        CHECK(m->url && strncmp(m->url, "https://", 8) == 0, "url 是 https 官方直链");
        CHECK(m->sha256 && strlen(m->sha256) == 64, "sha256 是 64 位 hex");
        CHECK(m->size > 0, "size > 0");
        CHECK(m->n_fft > 0 && m->hop > 0, "n_fft/hop > 0");
        CHECK(m->dim_f > 0 && m->dim_t > 1, "dim_f/dim_t 合法");
        CHECK(m->compensate > 0.0f && m->compensate < 4.0f, "compensate 合理");
        CHECK(m->primary_stem == KARAOKE_STEM_VOCALS ||
              m->primary_stem == KARAOKE_STEM_INSTRUMENTAL, "primary_stem 合法");
        CHECK(m->input_name && *m->input_name, "input_name 非空");
        CHECK(m->output_name && *m->output_name, "output_name 非空");
        CHECK(m->input_rank == 4, "输入秩为 4");
        CHECK(m->input_channels == 4, "输入通道维为 4");
        CHECK(m->license && *m->license, "license 非空");
        CHECK(m->credit && *m->credit, "credit 署名非空");
        /* 本实现要求 N = 3×2^k（6144 = 3×2048），DSP 自检会真正验证。 */
        CHECK((m->n_fft % 3) == 0, "n_fft 满足 3×2^k 分解");
        /* chunk 必须大于 n_fft（Default overlap 的 step 为正）。 */
        CHECK(m->hop * (m->dim_t - 1) > m->n_fft, "chunk > n_fft");
    }

    const KaraokeModelSpec *m0 = &spec[0];
    CHECK(karaoke_model_by_id(m0->id) == m0, "按 id 命中");
    CHECK(karaoke_model_by_sha256(m0->sha256) == m0, "按 SHA256 命中");
    char upper[65];
    for (int i = 0; i < 64; i++) {
        char c = m0->sha256[i];
        upper[i] = (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
    }
    upper[64] = '\0';
    CHECK(karaoke_model_by_sha256(upper) == m0, "SHA256 查询大小写不敏感");
    CHECK(karaoke_model_by_id("no-such-model") == NULL, "未知 id 返回 NULL");
    CHECK(karaoke_model_by_sha256("deadbeef") == NULL, "非法 SHA256 返回 NULL");
    CHECK(karaoke_model_patch_elems(m0) ==
              (size_t)4 * (size_t)m0->dim_f * (size_t)m0->dim_t,
          "patch 元素数 = 4×dim_f×dim_t");
    CHECK(karaoke_model_chunk_frames(m0) ==
              (size_t)m0->hop * (size_t)(m0->dim_t - 1),
          "chunk 帧数 = hop×(dim_t−1)");
}

/* ── 2. DSP 自检（不需要模型/运行库） ───────────────────────────── */

static void test_dsp_selftest(const KaraokeModelSpec *spec)
{
    char err[KARAOKE_MODEL_ERR_CAP] = {0};
    double t0 = now_ms();
    int rc = karaoke_model_stft_selftest(spec, err, sizeof(err));
    double ms = now_ms() - t0;
    CHECK(rc == 0, err[0] ? err : "STFT/iSTFT 自检失败");
    printf("  [info] stft selftest: rc=%d %.1fms\n", rc, ms);
}

/* ── 3. 校验错误路径（不需要模型文件） ─────────────────────────── */

static void test_validate_errors(void)
{
    char err[KARAOKE_MODEL_ERR_CAP] = {0};
    const KaraokeModelSpec *spec = NULL;

    CHECK(karaoke_model_validate_file(NULL, &spec, err, sizeof(err)) == -1,
          "NULL 路径必须拒绝");
    CHECK(err[0] != '\0', "NULL 路径给出错误文本");
    CHECK(spec == NULL, "失败时不回填 spec");

    err[0] = '\0';
    CHECK(karaoke_model_validate_file("/tmp/tm-karaoke-model-nonexistent.onnx",
                                      &spec, err, sizeof(err)) == -1,
          "不存在的文件必须拒绝");
    CHECK(strstr(err, "模型文件") != NULL || err[0] != '\0', "不存在文件有错误文本");

    err[0] = '\0';
    CHECK(karaoke_model_validate_file("/tmp", &spec, err, sizeof(err)) == -1,
          "目录必须拒绝");
    CHECK(err[0] != '\0', "目录给错误文本");

    const char *tiny = "/tmp/tm-karaoke-model-tiny.onnx";
    const char payload[] = "not a model";
    CHECK(write_file_bytes(tiny, payload, sizeof(payload)) == 0, "写小文件");
    err[0] = '\0';
    CHECK(karaoke_model_validate_file(tiny, &spec, err, sizeof(err)) == -1,
          "大小不符必须拒绝");
    CHECK(strstr(err, "不受支持") != NULL || strstr(err, "大小") != NULL,
          "大小不符错误文本可读");
    unlink(tiny);
}

/* ── 3b. SHA-256 已知向量（不依赖模型/运行库） ─────────────────── */

static void test_sha256_vectors(void)
{
    char err[KARAOKE_MODEL_ERR_CAP] = {0};
    char hex[65] = {0};
    const char *abc_path = "/tmp/tm-karaoke-sha-abc.bin";
    const char *empty_path = "/tmp/tm-karaoke-sha-empty.bin";

    CHECK(write_file_bytes(abc_path, "abc", 3) == 0, "写 SHA256 向量文件（abc）");
    int rc = karaoke_model_sha256_file(abc_path, hex, err, sizeof(err));
    CHECK(rc == 0, "计算 SHA256(abc)");
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "SHA256(\"abc\") 与 FIPS 180-4 向量一致");
    unlink(abc_path);

    CHECK(write_file_bytes(empty_path, "", 0) == 0, "写 SHA256 向量文件（空）");
    hex[0] = '\0';
    err[0] = '\0';
    rc = karaoke_model_sha256_file(empty_path, hex, err, sizeof(err));
    CHECK(rc == 0, "计算 SHA256(空文件)");
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "SHA256(空) 与 FIPS 180-4 向量一致");
    unlink(empty_path);

    CHECK(karaoke_model_sha256_file("/tmp", hex, err, sizeof(err)) == -1,
          "SHA256 对目录失败而非崩溃");
}

/* ── 4. 运行时 ───────────────────────────────────────────────────── */

static int test_runtime(void)
{
    char err[KARAOKE_MODEL_ERR_CAP] = {0};
    int rc = karaoke_model_runtime_available(err, sizeof(err));
    const char *lib = karaoke_model_runtime_library();
    const char *ver = karaoke_model_runtime_version();
    if (rc != 0) {
        printf("  [info] ONNX Runtime 不可用：%s\n", err);
        return 0;
    }
    CHECK(lib && *lib, "可用时给出运行库名");
    CHECK(ver && *ver, "可用时给出运行库版本");
    printf("  [info] runtime: lib=%s version=%s\n", lib ? lib : "?", ver ? ver : "?");
    return 1;
}

/* ── 5. 模型校验（真实文件） ────────────────────────────────────── */

static const KaraokeModelSpec *test_validate_real(const char *model_path,
                                                  long *out_ms)
{
    char err[KARAOKE_MODEL_ERR_CAP] = {0};
    const KaraokeModelSpec *spec = NULL;
    quiet_stderr_begin();
    double t0 = now_ms();
    int rc = karaoke_model_validate_file(model_path, &spec, err, sizeof(err));
    quiet_stderr_end();
    double ms = now_ms() - t0;
    if (out_ms)
        *out_ms = (long)ms;
    CHECK(rc == 0, err[0] ? err : "受支持模型应校验通过");
    CHECK(spec != NULL, "校验通过回填表项");
    if (spec) {
        CHECK(karaoke_model_by_id(spec->id) == spec, "回填表项来自静态表");
        CHECK(access(model_path, R_OK) == 0, "模型文件可读");
    }
    return spec;
}

static void test_validate_forged(const char *model_path,
                                 const KaraokeModelSpec *spec)
{
    char err[KARAOKE_MODEL_ERR_CAP] = {0};
    char forged[KARAOKE_MODEL_PATH_CAP];
    snprintf(forged, sizeof(forged), "/tmp/tm-karaoke-model-forged-%ld.onnx",
             (long)getpid());
    if (copy_file(model_path, forged) != 0) {
        CHECK(0, "复制伪造样本失败");
        return;
    }
    /* 改最后一个字节，大小不变、SHA256 必变。 */
    int fd = open(forged, O_RDWR);
    CHECK(fd >= 0, "打开伪造样本");
    if (fd >= 0) {
        unsigned char b = 0;
        CHECK(pread(fd, &b, 1, spec->size - 1) == 1, "读末字节");
        b ^= 0x5a;
        CHECK(pwrite(fd, &b, 1, spec->size - 1) == 1, "改写末字节");
        close(fd);
    }
    const KaraokeModelSpec *hit = NULL;
    int rc = karaoke_model_validate_file(forged, &hit, err, sizeof(err));
    CHECK(rc == -1, "同大小但内容被篡改的模型必须拒绝");
    CHECK(hit == NULL, "篡改模型不回填 spec");
    CHECK(strstr(err, "SHA256") != NULL, "篡改错误提到 SHA256");
    unlink(forged);

    /* 截断到一半：大小不符 → 拒绝。 */
    char truncated[KARAOKE_MODEL_PATH_CAP];
    snprintf(truncated, sizeof(truncated), "/tmp/tm-karaoke-model-trunc-%ld.onnx",
             (long)getpid());
    if (copy_file(model_path, truncated) == 0) {
        CHECK(truncate(truncated, (off_t)(spec->size / 2)) == 0, "截断副本");
        err[0] = '\0';
        rc = karaoke_model_validate_file(truncated, &hit, err, sizeof(err));
        CHECK(rc == -1, "截断模型必须拒绝");
        unlink(truncated);
    }
}

/* ── 6. 安装（原子/幂等/自愈/失败清理） ─────────────────────────── */

static void test_install(const char *model_path, const KaraokeModelSpec *spec)
{
    char err[KARAOKE_MODEL_ERR_CAP] = {0};
    char tmpl[] = "/tmp/tm-karaoke-install-XXXXXX";
    char *data_dir = mkdtemp(tmpl);
    CHECK(data_dir != NULL, "创建临时 data_dir");
    if (!data_dir)
        return;

    char installed[KARAOKE_MODEL_PATH_CAP];
    CHECK(karaoke_model_installed_path(spec, data_dir, installed,
                                       sizeof(installed)) == 0,
          "计算安装路径");
    CHECK(strstr(installed, spec->id) != NULL, "安装路径含模型 id");
    CHECK(karaoke_model_installed_path(NULL, data_dir, installed,
                                       sizeof(installed)) == -1,
          "NULL 表项计算路径失败");
    CHECK(karaoke_model_installed_path(spec, NULL, installed,
                                       sizeof(installed)) == -1,
          "NULL data_dir 计算路径失败");
    char tiny_path[4] = {0};
    CHECK(karaoke_model_installed_path(spec, data_dir, tiny_path,
                                       sizeof(tiny_path)) == -1,
          "缓冲过小计算路径失败");

    char out_id[KARAOKE_MODEL_ID_CAP] = {0};
    CHECK(karaoke_model_install(model_path, NULL, out_id, sizeof(out_id),
                                err, sizeof(err)) == -1,
          "NULL data_dir 安装失败");
    err[0] = '\0';
    CHECK(karaoke_model_install(model_path, data_dir, out_id, sizeof(out_id),
                                err, sizeof(err)) == 0,
          err[0] ? err : "安装受支持模型");
    CHECK(strcmp(out_id, spec->id) == 0, "安装回填 id");

    long long size = 0;
    struct stat st;
    CHECK(file_size(installed, &size) == 0, "安装后的文件存在");
    CHECK(size == spec->size, "安装后大小与表项一致");
    CHECK(stat(installed, &st) == 0 && (st.st_mode & 0777) == 0600,
          "安装文件权限 0600");

    /* 安装副本本身也要能通过完整校验（大小+SHA256+会话 I/O）。 */
    const KaraokeModelSpec *installed_spec = NULL;
    quiet_stderr_begin();
    int rc = karaoke_model_validate_file(installed, &installed_spec,
                                         err, sizeof(err));
    quiet_stderr_end();
    CHECK(rc == 0, "安装副本校验通过");
    CHECK(installed_spec == spec, "安装副本命中同一表项");

    /* 幂等：再装一次仍是 0，且不改写已有正确副本（inode 不变）。 */
    struct stat st_before;
    CHECK(stat(installed, &st_before) == 0, "stat 安装副本");
    err[0] = '\0';
    CHECK(karaoke_model_install(model_path, data_dir, out_id, sizeof(out_id),
                                err, sizeof(err)) == 0,
          "重复安装幂等成功");
    struct stat st_after;
    CHECK(stat(installed, &st_after) == 0, "stat 安装副本（复装后）");
    CHECK(st_before.st_ino == st_after.st_ino, "幂等安装不重写正确副本");

    /* 目录里的无关文件必须原样保留。 */
    char foreign[KARAOKE_MODEL_PATH_CAP];
    snprintf(foreign, sizeof(foreign), "%s/models/other-model.onnx", data_dir);
    const char foreign_data[] = "keep me";
    CHECK(write_file_bytes(foreign, foreign_data, sizeof(foreign_data)) == 0,
          "写无关文件");
    err[0] = '\0';
    CHECK(karaoke_model_install(model_path, data_dir, out_id, sizeof(out_id),
                                err, sizeof(err)) == 0, "再次安装成功");
    char rd[32] = {0};
    int fd = open(foreign, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, rd, sizeof(rd) - 1);
        close(fd);
        CHECK(n == (ssize_t)sizeof(foreign_data) &&
                  memcmp(rd, foreign_data, sizeof(foreign_data)) == 0,
              "安装不碰无关文件");
    } else {
        CHECK(0, "无关文件仍存在");
    }

    /* 坏副本自愈：把目标截断后再安装，应重新写成正确副本。 */
    CHECK(truncate(installed, 128) == 0, "把安装副本截断成坏副本");
    err[0] = '\0';
    CHECK(karaoke_model_install(model_path, data_dir, out_id, sizeof(out_id),
                                err, sizeof(err)) == 0,
          "坏副本自动修复安装");
    size = 0;
    CHECK(file_size(installed, &size) == 0 && size == spec->size,
          "自愈后大小正确");

    /* 失败路径：源文件非法（截断）→ 拒绝且不留下目标/临时文件。 */
    char tmpl2[] = "/tmp/tm-karaoke-install-fail-XXXXXX";
    char *data_dir2 = mkdtemp(tmpl2);
    CHECK(data_dir2 != NULL, "创建第二个 data_dir");
    char badsrc[KARAOKE_MODEL_PATH_CAP];
    snprintf(badsrc, sizeof(badsrc), "/tmp/tm-karaoke-badsrc-%ld.onnx",
             (long)getpid());
    if (copy_file(model_path, badsrc) == 0) {
        CHECK(truncate(badsrc, 1024) == 0, "伪造截断源文件");
        char bad_dst[KARAOKE_MODEL_PATH_CAP];
        CHECK(karaoke_model_installed_path(spec, data_dir2, bad_dst,
                                           sizeof(bad_dst)) == 0,
              "计算第二个安装路径");
        err[0] = '\0';
        CHECK(karaoke_model_install(badsrc, data_dir2, out_id, sizeof(out_id),
                                    err, sizeof(err)) == -1,
              "非法源文件安装失败");
        CHECK(access(bad_dst, F_OK) != 0, "失败时不留下目标文件");
        unlink(badsrc);
    }
    CHECK(dir_has_no_part_files(data_dir2), "失败时不留下 .part 临时文件");

    /* 复核自愈后的最终副本。 */
    installed_spec = NULL;
    quiet_stderr_begin();
    rc = karaoke_model_validate_file(installed, &installed_spec, err, sizeof(err));
    quiet_stderr_end();
    CHECK(rc == 0 && installed_spec == spec, "最终安装副本有效");

    char cmd[KARAOKE_MODEL_PATH_CAP + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", data_dir, data_dir2);
    (void)system(cmd);
}

/* ── 7. 会话 / 推理 / 分离 ──────────────────────────────────────── */

static void test_session_and_inference(const char *model_path,
                                       const KaraokeModelSpec *spec)
{
    char err[KARAOKE_MODEL_ERR_CAP] = {0};

    /* 伪造 spec（dim_f 不符）→ 会话必须因 I/O 形状不符拒绝。 */
    KaraokeModelSpec fake = *spec;
    fake.dim_f = spec->dim_f + 1;
    quiet_stderr_begin();
    KaraokeModelSession *bad = karaoke_model_session_open(&fake, model_path, 1,
                                                          err, sizeof(err));
    quiet_stderr_end();
    CHECK(bad == NULL, "与表项形状不符的模型必须拒绝建会话");
    CHECK(strstr(err, "形状") != NULL, "形状不符错误文本可读");

    err[0] = '\0';
    quiet_stderr_begin();
    double t0 = now_ms();
    KaraokeModelSession *session = karaoke_model_session_open(spec, model_path, 0,
                                                              err, sizeof(err));
    double open_ms = now_ms() - t0;
    quiet_stderr_end();
    CHECK(session != NULL, err[0] ? err : "打开会话");
    if (!session)
        return;
    int threads = karaoke_model_session_threads(session);
    CHECK(threads >= 1 && threads <= KARAOKE_MODEL_THREADS_CAP, "默认线程数在 1..4");
    printf("  [info] session: open=%.1fms threads=%d rss_hwm=%ldKB\n",
           open_ms, threads, peak_rss_kb());

    /* 单 patch 推理：输出必须全为有限值。 */
    size_t patch_elems = karaoke_model_patch_elems(spec);
    float *in_patch = (float *)calloc(patch_elems, sizeof(float));
    float *out_patch = (float *)malloc(patch_elems * sizeof(float));
    CHECK(in_patch && out_patch, "分配 patch 缓冲");
    if (in_patch && out_patch) {
        for (size_t i = 0; i < patch_elems; i++)
            in_patch[i] = 0.01f * (float)((int)(i % 7) - 3);
        t0 = now_ms();
        int rc = karaoke_model_infer_patch(session, in_patch, out_patch,
                                           err, sizeof(err));
        double ms = now_ms() - t0;
        CHECK(rc == 0, err[0] ? err : "单 patch 推理");
        size_t bad_vals = 0;
        for (size_t i = 0; i < patch_elems; i++)
            if (!isfinite(out_patch[i]))
                bad_vals++;
        CHECK(bad_vals == 0, "单 patch 输出全部有限");
        printf("  [info] infer_patch: %.1fms elems=%zu\n", ms, patch_elems);
    }
    free(in_patch);
    free(out_patch);

    /* 单段分离：6 秒（> chunk 254976 样本，走 3 个 chunk，覆盖交叉淡化）。 */
    size_t frames = 6u * (size_t)SR;   /* 264600 */
    size_t n = frames * 2;
    float *mix = (float *)malloc(n * sizeof(float));
    /* 前后各留一个哨兵，检测写出越界。 */
    float *voc_buf = (float *)malloc((n + 2) * sizeof(float));
    float *inst_buf = (float *)malloc((n + 2) * sizeof(float));
    CHECK(mix && voc_buf && inst_buf, "分配分离缓冲");
    if (!mix || !voc_buf || !inst_buf) {
        free(mix);
        free(voc_buf);
        free(inst_buf);
        karaoke_model_session_close(session);
        return;
    }
    for (size_t i = 0; i < frames; i++) {
        double t = (double)i / (double)SR;
        mix[i * 2] = (float)(0.20 * sin(2.0 * M_PI * 440.0 * t) +
                             0.10 * sin(2.0 * M_PI * 3300.0 * t));
        mix[i * 2 + 1] = (float)(0.20 * sin(2.0 * M_PI * 440.0 * t + 0.4) +
                                 0.10 * sin(2.0 * M_PI * 1500.0 * t));
    }
    voc_buf[0] = inst_buf[0] = 12345.0f;
    voc_buf[n + 1] = inst_buf[n + 1] = -12345.0f;
    float *voc = voc_buf + 1;
    float *inst = inst_buf + 1;

    t0 = now_ms();
    int rc_voc = karaoke_model_separate(session, mix, frames, KARAOKE_STEM_VOCALS,
                                        voc, err, sizeof(err));
    double voc_ms = now_ms() - t0;
    CHECK(rc_voc == 0, err[0] ? err : "分离人声");
    err[0] = '\0';
    t0 = now_ms();
    int rc_inst = karaoke_model_separate(session, mix, frames,
                                         KARAOKE_STEM_INSTRUMENTAL, inst,
                                         err, sizeof(err));
    double inst_ms = now_ms() - t0;
    CHECK(rc_inst == 0, err[0] ? err : "分离伴奏");
    CHECK(voc_buf[0] == 12345.0f && inst_buf[0] == 12345.0f, "前哨兵未被写坏");
    CHECK(voc_buf[n + 1] == -12345.0f && inst_buf[n + 1] == -12345.0f,
          "后哨兵未被写坏（输出长度恰为 frames×2）");

    if (rc_voc == 0 && rc_inst == 0) {
        size_t bad_vals = 0;
        double mix_rms = 0.0, diff_rms = 0.0;
        double vocals_energy = 0.0;
        for (size_t i = 0; i < n; i++) {
            if (!isfinite(voc[i]) || !isfinite(inst[i]))
                bad_vals++;
            mix_rms += (double)mix[i] * mix[i];
            double want = (double)mix[i] - (double)spec->compensate * voc[i];
            double d = (double)inst[i] - want;
            diff_rms += d * d;
            vocals_energy += (double)voc[i] * voc[i];
        }
        mix_rms = sqrt(mix_rms / (double)n);
        diff_rms = sqrt(diff_rms / (double)n);
        CHECK(bad_vals == 0, "分离输出全部有限（无 NaN/Inf）");
        CHECK(mix_rms > 1e-3, "测试混音非静音");
        /* 伴奏 = mix − compensate×人声；两次推理的 ORT 归约顺序可能不同，
         * 允许 1% 混音 RMS 的数值容差。 */
        CHECK(diff_rms <= 0.01 * mix_rms + 1e-6,
              "伴奏 = mix − 1.035×人声（容差 1% RMS）");
        printf("  [info] separate: voc=%.1fms inst=%.1fms frames=%zu 3 chunks; "
               "vocals_rms=%.5f mix_rms=%.4f formula_diff_rms=%.2e rss_hwm=%ldKB\n",
               voc_ms, inst_ms, frames, sqrt(vocals_energy / (double)n), mix_rms,
               diff_rms, peak_rss_kb());
    }

    free(mix);
    free(voc_buf);
    free(inst_buf);
    karaoke_model_session_close(session);
}

/* ── main ───────────────────────────────────────────────────────── */

/* 定义在文件后部（依赖真实模型 + 3 分钟测试曲）。 */
static void test_cache_worker_e2e(const char *model_path,
                                  const KaraokeModelSpec *spec);

int main(void)
{
    /* 纯 TTY 下工作目录可能是仓库根；不依赖它：模型候选路径都显式判断。 */
    size_t count = 0;
    const KaraokeModelSpec *spec = karaoke_model_supported(&count);

    test_registry();
    if (spec && count > 0)
        test_dsp_selftest(spec);
    test_validate_errors();
    test_sha256_vectors();

    int runtime_ok = test_runtime();
    if (!runtime_ok) {
        printf("SKIP test_karaoke_model（ONNX Runtime 不可用：未找到 "
               "libonnxruntime.so.1.21/.so.1/.so）\n");
        if (failures > 0) {
            fprintf(stderr, "%d failure(s)\n", failures);
            return 1;
        }
        return 0;
    }

    char model_path[KARAOKE_MODEL_PATH_CAP] = {0};
    const char *model = find_model(model_path, sizeof(model_path), spec);
    if (!model) {
        printf("SKIP test_karaoke_model（未找到受支持模型 %s；可用 "
               "TER_MUSIC_KARAOKE_MODEL=<path> 指定）\n", spec->id);
        if (failures > 0) {
            fprintf(stderr, "%d failure(s)\n", failures);
            return 1;
        }
        return 0;
    }
    printf("  [info] model: %s\n", model);

    long validate_ms = 0;
    const KaraokeModelSpec *validated = test_validate_real(model, &validate_ms);
    if (validated) {
        printf("  [info] validate: %.0fms (size=%lld sha256=%.12s…)\n",
               (double)validate_ms, validated->size,
               validated->sha256 ? validated->sha256 : "?");
        test_validate_forged(model, validated);
        test_install(model, validated);
        test_session_and_inference(model, validated);
        test_cache_worker_e2e(model, validated);
    }

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS test_karaoke_model\n");
    return 0;
}

/* ================================================================
 * 8. 缓存 + worker 端到端（真实模型；无模型时整段跳过）
 *
 * 覆盖验收要求：
 *   - 一次模型分离同时产出 inst.flac + soft.flac 成对缓存；
 *   - 只触发一次推理：第二次 request/切档/重播不再起 job；
 *   - soft = 0.4×原声 + 0.6×inst 的关系（解码后逐样本比较）；
 *   - 非静音、时长匹配、人声（居中分量）能量相对原曲下降（给 dB）；
 *   - 取消：job 取消后不生成坏缓存、状态为 CANCELLED；
 *   - 记录分离耗时、两份缓存大小、峰值 RSS。
 * ================================================================ */

/* 生成一段 3 分钟测试曲：居中 440Hz（模拟人声）+ 两侧反相的 1300Hz（模拟伴奏）。 */
static int encode_test_flac(const char *path, int seconds, int rate)
{
    AVFormatContext *oc = NULL;
    if (avformat_alloc_output_context2(&oc, NULL, "flac", path) < 0 || !oc)
        return -1;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!codec) { avformat_free_context(oc); return -1; }
    AVCodecContext *cc = avcodec_alloc_context3(codec);
    if (!cc) { avformat_free_context(oc); return -1; }
    cc->sample_rate = rate;
    cc->sample_fmt = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&cc->ch_layout, 2);
    cc->time_base = (AVRational){1, rate};
    if (avcodec_open2(cc, codec, NULL) < 0) return -1;
    AVStream *st = avformat_new_stream(oc, NULL);
    avcodec_parameters_from_context(st->codecpar, cc);
    st->time_base = cc->time_base;
    if (avio_open(&oc->pb, path, AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(oc, NULL) < 0)
        return -1;
    int fs = cc->frame_size > 0 ? cc->frame_size : 4608;
    AVFrame *fr = av_frame_alloc();
    fr->format = AV_SAMPLE_FMT_S16;
    fr->sample_rate = rate;
    av_channel_layout_default(&fr->ch_layout, 2);
    fr->nb_samples = fs;
    if (av_frame_get_buffer(fr, 0) < 0) return -1;
    AVPacket *pkt = av_packet_alloc();
    long long total = (long long)seconds * rate, done = 0;
    int rc = 0;
    while (done < total && rc == 0) {
        int n = (int)((total - done) < fs ? (total - done) : fs);
        if (av_frame_make_writable(fr) < 0) { rc = -1; break; }
        fr->nb_samples = n;
        fr->pts = done;
        int16_t *p = (int16_t *)fr->data[0];
        for (int i = 0; i < n; i++) {
            double t = (double)(done + i) / (double)rate;
            double center = 0.25 * sin(2.0 * M_PI * 440.0 * t);
            double side = 0.15 * sin(2.0 * M_PI * 1300.0 * t);
            p[i * 2] = (int16_t)((center + side) * 32767.0);
            p[i * 2 + 1] = (int16_t)((center - side) * 32767.0);
        }
        if (avcodec_send_frame(cc, fr) < 0) { rc = -1; break; }
        while (avcodec_receive_packet(cc, pkt) == 0) {
            av_packet_rescale_ts(pkt, cc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }
        done += n;
    }
    if (rc == 0) {
        avcodec_send_frame(cc, NULL);
        while (avcodec_receive_packet(cc, pkt) == 0) {
            av_packet_rescale_ts(pkt, cc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }
        if (av_write_trailer(oc) < 0) rc = -1;
    }
    av_packet_free(&pkt);
    av_frame_free(&fr);
    avcodec_free_context(&cc);
    if (oc->pb) avio_closep(&oc->pb);
    avformat_free_context(oc);
    return rc;
}

/* 解码整段 FLAC 到 float 交织立体声（测试用；3 分钟约 63MB）。 */
static int decode_flac_float(const char *path, int *out_rate, long long *out_frames,
                             float **out_pcm)
{
    *out_pcm = NULL;
    *out_frames = 0;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) return -1;
    int idx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { idx = (int)i; break; }
    if (idx < 0) return -1;
    AVCodecParameters *par = fmt->streams[idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *cc = avcodec_alloc_context3(codec);
    if (!cc || avcodec_parameters_to_context(cc, par) < 0 || avcodec_open2(cc, codec, NULL) < 0)
        return -1;
    int channels = cc->ch_layout.nb_channels;
    int rate = cc->sample_rate;
    SwrContext *swr = swr_alloc();
    AVChannelLayout in_layout, out_layout;
    av_channel_layout_copy(&in_layout, &cc->ch_layout);
    av_channel_layout_default(&out_layout, 2);
    av_opt_set_chlayout(swr, "in_chlayout", &in_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &out_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", rate, 0);
    av_opt_set_int(swr, "out_sample_rate", rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", cc->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    if (swr_init(swr) < 0) return -1;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    long long cap = (long long)rate * 400, frames = 0;
    float *pcm = malloc((size_t)cap * 2 * sizeof(float));
    int rc = 0;
    if (!pcm) rc = -1;
    while (rc == 0) {
        int r = av_read_frame(fmt, pkt);
        if (r == AVERROR_EOF) break;
        if (r < 0) { rc = -1; break; }
        if (pkt->stream_index != idx) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(cc, pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(cc, fr) == 0) {
            int out_max = (int)av_rescale_rnd(
                swr_get_delay(swr, rate) + fr->nb_samples, rate, rate, AV_ROUND_UP);
            if (frames + out_max > cap) {
                cap = (frames + out_max) * 2;
                float *n = realloc(pcm, (size_t)cap * 2 * sizeof(float));
                if (!n) { rc = -1; break; }
                pcm = n;
            }
            uint8_t *planes[1] = {(uint8_t *)(pcm + frames * 2)};
            int produced = swr_convert(swr, planes, out_max,
                                       (const uint8_t **)fr->extended_data,
                                       fr->nb_samples);
            if (produced < 0) { rc = -1; break; }
            frames += produced;
            av_frame_unref(fr);
        }
    }
    for (;;) {
        int out_max = 4096;
        if (frames + out_max > cap) {
            cap = (frames + out_max) * 2;
            float *n = realloc(pcm, (size_t)cap * 2 * sizeof(float));
            if (!n) { rc = -1; break; }
            pcm = n;
        }
        uint8_t *planes[1] = {(uint8_t *)(pcm + frames * 2)};
        int produced = swr_convert(swr, planes, out_max, NULL, 0);
        if (produced <= 0) break;
        frames += produced;
    }
    av_packet_free(&pkt);
    av_frame_free(&fr);
    swr_free(&swr);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    if (rc != 0) { free(pcm); return -1; }
    (void)channels;
    *out_rate = rate;
    *out_frames = frames;
    *out_pcm = pcm;
    return 0;
}

static double rms_center_side(const float *pcm, long long frames, int side)
{
    double sum = 0.0;
    if (frames <= 0) return 0.0;
    for (long long i = 0; i < frames; i++) {
        double m = side ? (pcm[i * 2] - pcm[i * 2 + 1]) * 0.5
                        : (pcm[i * 2] + pcm[i * 2 + 1]) * 0.5;
        sum += m * m;
    }
    return sqrt(sum / (double)frames);
}

static int probe_duration_seconds(const char *path)
{
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
        return -1;
    int seconds = -1;
    if (avformat_find_stream_info(fmt, NULL) >= 0) {
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                AVStream *st = fmt->streams[i];
                if (st->duration > 0 && st->time_base.den > 0)
                    seconds = (int)av_rescale_q(st->duration, st->time_base,
                                                (AVRational){1, 1});
                break;
            }
        }
        if (seconds < 0 && fmt->duration > 0)
            seconds = (int)(fmt->duration / AV_TIME_BASE);
    }
    avformat_close_input(&fmt);
    return seconds;
}

static void test_cache_worker_e2e(const char *model_path,
                                  const KaraokeModelSpec *spec)
{
    const int kSeconds = 180;   /* 验收要求：≥1 首 3 分钟曲目 */
    char tmpl[] = "/tmp/tm-karaoke-e2e-XXXXXX";
    char *root = mkdtemp(tmpl);
    CHECK(root != NULL, "创建 worker 测试目录");
    if (!root)
        return;
    char track[KARAOKE_WORKER_PATH_CAP];
    char cache_root[KARAOKE_WORKER_PATH_CAP];
    snprintf(cache_root, sizeof(cache_root), "%s/cache/karaoke", root);
    /* 真实曲目（可选）：TERMUSIC_KARAOKE_TEST_TRACK=<path>。
     * 合成音（纯正弦）在 MDX 模型下"居中分量"未必被当成 vocal，因此只有在
     * 真实曲目上才断言人声能量下降；合成音仍验证成对缓存/soft 公式/时长。 */
    const char *real_track = getenv("TERMUSIC_KARAOKE_TEST_TRACK");
    int use_real = (real_track && real_track[0] && access(real_track, R_OK) == 0 &&
                    probe_duration_seconds(real_track) >= 170);
    double t0 = now_ms();
    if (use_real) {
        snprintf(track, sizeof(track), "%s", real_track);
        printf("  [info] cache e2e track: real '%s' (%ds)\n", track,
               probe_duration_seconds(track));
    } else {
        snprintf(track, sizeof(track), "%s/track.flac", root);
        int enc = encode_test_flac(track, kSeconds, 44100);
        CHECK(enc == 0, "生成 3 分钟测试曲（FLAC）");
        if (enc != 0) {
            char cmd[KARAOKE_WORKER_PATH_CAP];
            snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
            (void)system(cmd);
            return;
        }
        printf("  [info] cache e2e track: synthetic 180s (set "
               "TERMUSIC_KARAOKE_TEST_TRACK for real-song vocal check)\n");
    }

    KaraokeCacheConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = cache_root;
    cfg.model_id = spec->id;
    cfg.model_sha256 = spec->sha256;
    cfg.overlap = KARAOKE_CACHE_OVERLAP;
    cfg.soft_inst_mix = KARAOKE_CACHE_SOFT_INST_MIX;
    cfg.limit_mb = KARAOKE_CACHE_LIMIT_MB;

    karaoke_worker_init();
    KaraokeWorkerJob job;
    memset(&job, 0, sizeof(job));
    snprintf(job.track_path, sizeof(job.track_path), "%s", track);
    snprintf(job.model_id, sizeof(job.model_id), "%s", spec->id);
    snprintf(job.model_path, sizeof(job.model_path), "%s", model_path);
    snprintf(job.model_sha256, sizeof(job.model_sha256), "%s", spec->sha256);
    snprintf(job.cache_root, sizeof(job.cache_root), "%s", cache_root);
    /* R2：v1 写路径必须显式指定；零初始化默认现在是 v2（生产契约）。 */
    job.format = KARAOKE_JOB_FORMAT_LEGACY_V1;
    job.overlap = KARAOKE_CACHE_OVERLAP;
    job.soft_inst_mix = KARAOKE_CACHE_SOFT_INST_MIX;

    KaraokeWorkerStatus st;
    memset(&st, 0, sizeof(st));
    if (!use_real) {
        /* 先测取消：起 job 后 2s 取消，应进入 CANCELLED 且不留缓存。 */
        CHECK(karaoke_worker_request(&job) == 0, "取消测试：接受 job");
        usleep(2000000);
        karaoke_worker_cancel();
        for (int i = 0; i < 120; i++) {
            usleep(250000);
            karaoke_worker_status(&st);
            if (st.state == KARAOKE_JOB_CANCELLED ||
                st.state == KARAOKE_JOB_FAILED || st.state == KARAOKE_JOB_READY)
                break;
        }
        CHECK(st.state == KARAOKE_JOB_CANCELLED, "取消后状态为 CANCELLED（不崩溃）");
        CHECK(karaoke_cache_complete(&cfg, track) == 0, "取消后不生成缓存");
    }

    /* 正式分离：一次 job 产出成对缓存。 */
    t0 = now_ms();
    CHECK(karaoke_worker_request(&job) == 0, "重新接受 job");
    memset(&st, 0, sizeof(st));
    for (int i = 0; i < 1200; i++) {
        usleep(250000);
        karaoke_worker_status(&st);
        if (st.state == KARAOKE_JOB_READY || st.state == KARAOKE_JOB_FAILED ||
            st.state == KARAOKE_JOB_CANCELLED)
            break;
    }
    double sep_ms = now_ms() - t0;
    CHECK(st.state == KARAOKE_JOB_READY, st.error[0] ? st.error : "3 分钟曲目分离完成");
    if (st.state != KARAOKE_JOB_READY) {
        char cmd[KARAOKE_WORKER_PATH_CAP];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
        (void)system(cmd);
        return;
    }
    CHECK(st.completed_jobs == 1, "只完成一次分离 job");

    char full_path[KARAOKE_CACHE_PATH_CAP] = {0};
    char soft_path[KARAOKE_CACHE_PATH_CAP] = {0};
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_FULL, full_path,
                                sizeof(full_path), NULL, 0) == 1,
          "FULL 档缓存命中");
    CHECK(karaoke_cache_resolve(&cfg, track, KARAOKE_MODE_SOFT, soft_path,
                                sizeof(soft_path), NULL, 0) == 1,
          "SOFT 档缓存命中");
    CHECK(karaoke_cache_complete(&cfg, track) == 1, "成对缓存完整");
    struct stat sst;
    long long full_bytes = (stat(full_path, &sst) == 0) ? (long long)sst.st_size : 0;
    long long soft_bytes = (stat(soft_path, &sst) == 0) ? (long long)sst.st_size : 0;
    CHECK(full_bytes > 0 && soft_bytes > 0, "两份缓存非空");

    /* 第二次 request：缓存已完成 → 不再起 job。 */
    int rc2 = karaoke_worker_request(&job);
    usleep(300000);
    karaoke_worker_status(&st);
    CHECK(rc2 == 0, "第二次 request 返回 0（无需重复分离）");
    CHECK(st.completed_jobs == 1, "第二次 request 未触发第二次推理");

    /* 解码三份音频做质量校验。 */
    int rate_o = 0, rate_i = 0, rate_s = 0;
    long long frames_o = 0, frames_i = 0, frames_s = 0;
    float *orig = NULL, *inst = NULL, *soft = NULL;
    int dec_o = decode_flac_float(track, &rate_o, &frames_o, &orig);
    int dec_i = decode_flac_float(full_path, &rate_i, &frames_i, &inst);
    int dec_s = decode_flac_float(soft_path, &rate_s, &frames_s, &soft);
    CHECK(dec_o == 0 && dec_i == 0 && dec_s == 0, "解码原曲/inst/soft");
    if (dec_o == 0 && dec_i == 0 && dec_s == 0) {
        CHECK(rate_i == rate_o && rate_s == rate_o, "缓存采样率与原曲一致");
        long long frame_diff = frames_i > frames_o ? frames_i - frames_o
                                                   : frames_o - frames_i;
        CHECK(frame_diff <= 1024, "缓存时长与原曲误差 ≤1 个写批（1024 帧）");
        long long n = frames_o < frames_i ? frames_o : frames_i;
        if (frames_s < n) n = frames_s;
        double rms_i = rms_center_side(inst, n, 0);
        double rms_s = rms_center_side(soft, n, 0);
        double rms_o = rms_center_side(orig, n, 0);
        double side_i = rms_center_side(inst, n, 1);
        CHECK(rms_i > 1e-5 && rms_s > 1e-5, "inst/soft 缓存非静音");
        double db = 20.0 * log10((rms_i > 1e-12 ? rms_i : 1e-12) /
                                 (rms_o > 1e-12 ? rms_o : 1e-12));
        printf("  [info] cache: inst=%lldB soft=%lldB sep=%.1fs rss_hwm=%ldKB\n",
               full_bytes, soft_bytes, sep_ms / 1000.0, peak_rss_kb());
        printf("  [info] center(vocal-like) RMS: orig=%.5f inst=%.5f (%.1f dB) "
               "soft=%.5f side_inst=%.5f\n",
               rms_o, rms_i, db, rms_s, side_i);
        /* 真实曲目上断言居中（人声）分量相对原曲下降；合成纯正弦不作为
         * 判据（模型对纯音的分类不代表真实音乐），只打印数值。 */
        if (use_real)
            CHECK(db < -0.5, "居中人声分量相对原曲下降（>0.5dB）");
        else
            printf("  [info] synthetic track: center reduction %.1f dB "
                   "(not asserted; set TERMUSIC_KARAOKE_TEST_TRACK)\n", db);

        /* soft = 0.4×原声 + 0.6×inst（解码后逐样本比较，容差 s16 量化级）。 */
        double num = 0.0, den = 0.0;
        for (long long i = 0; i < n; i++) {
            for (int ch = 0; ch < 2; ch++) {
                double want = 0.40 * orig[i * 2 + ch] + 0.60 * inst[i * 2 + ch];
                double d = soft[i * 2 + ch] - want;
                num += d * d;
                den += want * want;
            }
        }
        double rel = sqrt(num / (den > 0.0 ? den : 1.0));
        CHECK(rel < 1e-3, "soft = 0.4×原声 + 0.6×inst（相对误差 <1e-3）");
        printf("  [info] soft formula rel_err=%.2e\n", rel);
    }
    free(orig);
    free(inst);
    free(soft);

    karaoke_worker_shutdown();
    char cmd[KARAOKE_WORKER_PATH_CAP];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    (void)system(cmd);
}
