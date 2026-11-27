/**
 * @file karaoke_cache.h
 * @brief 可选 ONNX 模型消人声 — 伴奏缓存（成对 soft/full + 原子写 + LRU）
 *
 * 目录布局（模型-only 三档）：
 *   $XDG_CACHE_HOME/ter-music/karaoke/<model_id>/<track_hash>/
 *       inst.flac        纯伴奏（full 档播放）
 *       soft.flac        0.4×原声 + 0.6×伴奏（soft 档播放）
 *       meta.txt         键值元数据（源帧数/采样率/模型/overlap/soft 系数）
 *       .*.part.<pid>.*  写入中的临时文件（commit 成功后 rename）
 *
 * 一次模型分离产出 inst.flac + soft.flac 两份，由 worker 从同一遍推理的
 * 内存波形直接生成（soft 不二次推理）。本模块只负责：
 *   - 由 曲目路径 + 大小 + mtime + model_id/model_sha256 + 格式版本 +
 *     overlap + soft 系数 计算 track_hash（SHA256，见 karaoke_cache_paths）；
 *   - 命中查询（校验成对文件头/帧数/元数据一致，损坏则整目录清理）；
 *   - 原子提交（临时文件 fsync + rename 成对文件 + fsync 目录）；
 *   - LRU 上限 2GB（全局跨 model_id，按目录 mtime 淘汰，跳过正在写入/最近使用）；
 *   - 启动清理残留下的 `.part.*` 半成品。
 *
 * 不做 FFmpeg 解码：FLAC 由 STREAMINFO 头校验（采样率/声道/位深/总帧数），
 * 静音/损坏再由 worker 编码后自检（见 karaoke_worker.h）。这样本模块可被
 * 纯 C 单测直接覆盖，不引入播放/解码依赖。
 *
 * 线程约定：本模块无全局可变状态（配置由调用方以 KaraokeCacheConfig 传入），
 * 可在 worker 线程与播放线程并发调用；同一曲目同一模型的并发提交由
 * worker 的单 job 队列保证不会发生。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#ifndef KARAOKE_CACHE_H
#define KARAOKE_CACHE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KARAOKE_CACHE_FORMAT_VERSION 1
/* stem 派生算法版本：纳入 track_hash 材料。2 = voc=ring×compensate、
 * inst=mixture−voc（修复"inst 其实是人声 stem"的 T2 阻断问题）；版本变更
 * 使旧算法写出的 inst/soft 缓存永不命中（v1/v2 同一规则）。 */
#define KARAOKE_CACHE_STEM_VERSION   2
#define KARAOKE_CACHE_OVERLAP        0.25f  /* 固定 overlap（用户拍板） */
#define KARAOKE_CACHE_SOFT_INST_MIX  0.60f  /* soft = 0.60×伴奏 + 0.40×原声 */
#define KARAOKE_CACHE_SOFT_ORIG_MIX  0.40f
#define KARAOKE_CACHE_LIMIT_MB       2048   /* 全局 LRU 上限 */
#define KARAOKE_CACHE_PATH_CAP       4096
#define KARAOKE_CACHE_ERR_CAP        512
#define KARAOKE_CACHE_HASH_LEN       65

/* 播放源种类（与 KARAOKE_MODE_* 一一对应：OFF→ORIGINAL、SOFT→SOFT、FULL→FULL）。 */
typedef enum {
    KARAOKE_SOURCE_ORIGINAL = 0,
    KARAOKE_SOURCE_SOFT     = 1,
    KARAOKE_SOURCE_FULL     = 2
} KaraokeSourceKind;

/** 缓存配置（调用方填，通常由 audio.c 从 g_app_config 构造）。 */
typedef struct {
    const char *root;          /* ".../ter-music/karaoke"（app_cache_dir(1) + "/karaoke"） */
    const char *model_id;      /* 受支持模型 id，如 "uvr-mdxnet-3-9662" */
    const char *model_sha256;  /* 模型整文件 SHA256（64 位小写 hex） */
    float overlap;             /* 固定 KARAOKE_CACHE_OVERLAP */
    float soft_inst_mix;       /* 固定 KARAOKE_CACHE_SOFT_INST_MIX */
    int   limit_mb;            /* LRU 上限；≤0 用 KARAOKE_CACHE_LIMIT_MB */
} KaraokeCacheConfig;

/** 一个曲目缓存目录的路径集合。 */
typedef struct {
    char dir[KARAOKE_CACHE_PATH_CAP];       /* .../<model_id>/<track_hash> */
    char hash[KARAOKE_CACHE_HASH_LEN];      /* track_hash（小写 hex） */
    char inst_path[KARAOKE_CACHE_PATH_CAP]; /* dir/inst.flac */
    char soft_path[KARAOKE_CACHE_PATH_CAP]; /* dir/soft.flac */
    char meta_path[KARAOKE_CACHE_PATH_CAP]; /* dir/meta.txt */
} KaraokeCachePaths;

/**
 * 默认缓存根目录：$XDG_CACHE_HOME/ter-music/karaoke（回退 ~/.cache/ter-music/karaoke）。
 * 生产调用方通常用 app_cache_dir(1) 拼，本函数供测试/工具复用。
 * @return 0 成功；-1 失败（无 HOME/XDG 或过长）。
 */
int karaoke_cache_default_root(char *out, size_t cap);

/**
 * 计算某曲目在某模型下的缓存路径（不做任何文件系统写操作）。
 *
 * track_hash = SHA256(厂商前缀 + model_id + model_sha256 + realpath +
 * 大小 + mtime(ns) + overlap + soft 系数 + FLAC 格式版本)。
 *
 * @return 0 成功；-1 失败（路径不存在/realpath 失败/过长；err 写原因）。
 */
int karaoke_cache_paths(const KaraokeCacheConfig *cfg, const char *track_path,
                        KaraokeCachePaths *out, char *err, size_t err_cap);

/**
 * 准备写入：创建缓存目录（0700），返回三个临时文件路径（同目录，0600，
 * 形如 .inst.flac.part.<pid>.<n>）。调用方写完、fsync 后交给 commit。
 *
 * @return 0 成功；-1 失败。
 */
int karaoke_cache_begin(const KaraokeCacheConfig *cfg, const char *track_path,
                        KaraokeCachePaths *out,
                        char *inst_tmp, size_t inst_cap,
                        char *soft_tmp, size_t soft_cap,
                        char *meta_tmp, size_t meta_cap,
                        char *err, size_t err_cap);

/**
 * 写 meta.txt（键值行）。worker 编码完两份 FLAC 后调用，随后 commit 校验。
 * 字段：format/model_id/model_sha256/overlap/soft_inst/source_frames/
 * source_rate/inst_frames/soft_frames。
 */
int karaoke_cache_write_meta(const char *meta_tmp, const KaraokeCacheConfig *cfg,
                             long long source_frames, int source_rate,
                             long long inst_frames, long long soft_frames,
                             char *err, size_t err_cap);

/**
 * 原子提交成对缓存：校验两份 FLAC（STREAMINFO）与 meta 的一致性 →
 * fsync → rename(inst) → rename(soft) → rename(meta 最后) → fsync(dir) →
 * LRU 淘汰。任何校验失败都不改名（调用方负责 abort 临时文件）。
 *
 * @return 0 成功；-1 失败（err 写原因）。
 */
int karaoke_cache_commit(const KaraokeCacheConfig *cfg,
                         const KaraokeCachePaths *paths,
                         const char *inst_tmp, const char *soft_tmp,
                         const char *meta_tmp,
                         char *err, size_t err_cap);

/** 删除三个临时文件（NULL/空串安全）。 */
void karaoke_cache_abort_temps(const char *inst_tmp, const char *soft_tmp,
                               const char *meta_tmp);

/**
 * 查询某播放档位的缓存是否命中：
 *   mode=KARAOKE_MODE_OFF → 0（默认播放原声；不查缓存）
 *   mode=SOFT/FULL      → 成对缓存完整且校验通过时返回 1 并填 out_path；
 *                          否则返回 0；若发现残留/损坏目录会清理（只删
 *                          cfg->root 子树内的路径）。
 * 命中时 touch 文件与目录 mtime（LRU "最近使用"）。
 */
int karaoke_cache_resolve(const KaraokeCacheConfig *cfg, const char *track_path,
                          int mode, char *out_path, size_t out_cap,
                          char *err, size_t err_cap);

/** 成对缓存是否完整有效（不 touch、不清理）。1=完整，0=不完整。 */
int karaoke_cache_complete(const KaraokeCacheConfig *cfg, const char *track_path);

/** 读取 FLAC STREAMINFO：采样率/声道/位深/总帧数。0=成功，-1=失败。 */
int karaoke_cache_flac_info(const char *path, int *sample_rate, int *channels,
                            int *bits_per_sample, long long *frames,
                            char *err, size_t err_cap);

/**
 * LRU 淘汰：把 cfg->root 下所有 <model_id>/<track_hash>/ 目录的总字节数
 * 压到 limit_mb 以下，按目录 mtime 最旧优先；跳过 exclude_dir（当前曲目）、
 * 含 `.part.` 的目录（正在写入）与 2 分钟内刚用过的目录。
 * @return 0 成功（含无需淘汰）；-1 参数错误。
 */
int karaoke_cache_evict(const KaraokeCacheConfig *cfg, const char *exclude_dir,
                        char *err, size_t err_cap);

/** 当前缓存占用（MB，向上取整）与曲目目录数；失败返回 -1。 */
long long karaoke_cache_usage_mb(const KaraokeCacheConfig *cfg, int *track_count);

/** 启动清理：删除 cfg->root 下超过 10 分钟的 `.part.*` 半成品。 */
void karaoke_cache_cleanup_stale(const KaraokeCacheConfig *cfg);

/** 删除某曲目在某模型下的缓存目录（只删 cfg->root 子树内路径）。0=成功或不存在。 */
int karaoke_cache_remove(const KaraokeCacheConfig *cfg, const char *track_path);

/* ================================================================
 * v2 会话级分段缓存（D2/D6，2026-11-25 用户拍板）
 *
 * 目录布局（根目录在 /tmp，按进程隔离，退出即删）：
 *   /tmp/ter-music-karaoke-<uid>-<pid>/          0700，首次使用时创建
 *       <model_id>/<track_hash>/
 *           seg%03d.inst.flac     纯伴奏段（44100Hz/2ch/16bit）
 *           seg%03d.soft.flac     0.4×原声 + 0.6×伴奏段
 *           seg%03d.ready         段就绪标记（临时文件 fsync + rename，最后写）
 *           manifest.txt          段清单（原子更新）
 *
 * 段长固定 15s = 661500 帧（44100Hz）；段号与曲目内容时间轴严格对应：
 * 段 k 覆盖内容帧 [k×661500, min((k+1)×661500, source_frames))。段边界就是
 * worker 的 OLA 完成边界，因此按段拼接与整曲输出逐帧一致，不重复不丢失。
 *
 * 生命周期：
 *   - 只有 worker（v2 job）写；XDG 下 v1 缓存保持只读兼容且优先；
 *   - 正常退出：worker shutdown **之后**调 karaoke_cache_session_remove_self()；
 *   - 崩溃残留：下次启动（任意子命令）调
 *     karaoke_cache_session_cleanup_stale()，删除同 uid 且 pid 已不存在的目录；
 *   - 安全：根目录 0700；删除只发生在 /tmp/ter-music-karaoke-<uid>-<pid> 子树内，
 *     拒绝符号链接与越界路径。
 *
 * 线程约定：路径/校验函数无全局可变状态，可在播放线程与 worker 线程并发调用；
 * ready 标记最后写 + manifest 原子替换保证"可见即可用"。
 * ================================================================ */

#define KARAOKE_SEGMENT_FORMAT_VERSION 2
#define KARAOKE_SEGMENT_SECONDS        15
#define KARAOKE_SEGMENT_SAMPLE_RATE    44100
#define KARAOKE_SEGMENT_CHANNELS       2
#define KARAOKE_SEGMENT_FRAMES         (KARAOKE_SEGMENT_SAMPLE_RATE * KARAOKE_SEGMENT_SECONDS)
#define KARAOKE_SEGMENT_MAX_COUNT      8192
#define KARAOKE_SEGMENT_MARKER_CAP     128

/** v2 manifest（键值文本，原子替换）。 */
typedef struct {
    int format;                          /**< 恒为 KARAOKE_SEGMENT_FORMAT_VERSION */
    char model_id[128];
    char model_sha256[65];
    double overlap;
    double soft_inst;
    int sample_rate;                     /**< 44100 */
    int segment_seconds;                 /**< 15 */
    int total_segments;                  /**< 完整时有效；未完成可为 0 */
    long long source_frames;             /**< 模型输出（trim 后）总帧数（44100 域） */
    int complete;                        /**< 1 = 全部段已就绪 */
} KaraokeSegmentManifest;

/** 一个曲目的 v2 分段目录。 */
typedef struct {
    char root[KARAOKE_CACHE_PATH_CAP];      /**< /tmp/ter-music-karaoke-<uid>-<pid> */
    char dir[KARAOKE_CACHE_PATH_CAP];       /**< root/<model_id>/<track_hash> */
    char hash[KARAOKE_CACHE_HASH_LEN];      /**< track_hash（小写 hex） */
    char manifest_path[KARAOKE_CACHE_PATH_CAP];
} KaraokeSegmentPaths;

/** 本进程的会话根目录（纯计算，不创建）。0=成功，-1=失败。 */
int karaoke_cache_session_root(char *out, size_t cap);

/** 创建/确保本进程会话根（0700）；若同名残留（pid 复用）先清空。 */
int karaoke_cache_session_ensure(char *out, size_t cap, char *err, size_t err_cap);

/** 启动清理：删除同 uid 且 pid 已不存在的 /tmp/ter-music-karaoke-* 残留目录。 */
void karaoke_cache_session_cleanup_stale(void);

/** 退出清理：递归删除本进程会话根（只删严格匹配本 uid/pid 的目录）。 */
void karaoke_cache_session_remove_self(void);

/** 计算 v2 分段目录（cfg->root 必须是会话根；不做文件系统写操作）。 */
int karaoke_cache_segment_paths(const KaraokeCacheConfig *cfg, const char *track_path,
                                KaraokeSegmentPaths *out, char *err, size_t err_cap);

/** 创建 <root>/<model_id>/<track_hash>（逐级 0700；已存在则仅校验类型/属主）。 */
int karaoke_cache_segment_mkdir(const KaraokeSegmentPaths *paths,
                                char *err, size_t err_cap);

/** 段文件路径：is_soft=0 → seg%03d.inst.flac；1 → seg%03d.soft.flac。 */
int karaoke_cache_segment_file(const KaraokeSegmentPaths *paths, int seg, int is_soft,
                               char *out, size_t cap);

/** 段 ready 标记路径：seg%03d.ready。 */
int karaoke_cache_segment_marker(const KaraokeSegmentPaths *paths, int seg,
                                 char *out, size_t cap);

/** 总帧数 → 段数（向上取整；frames<=0 → 1）。 */
int karaoke_cache_segment_count(long long frames, int *out);

/** 段应含帧数：完整 manifest 时用 total/source 校验；未完整返回 -1（未知）。 */
int karaoke_cache_segment_expected(const KaraokeSegmentManifest *m, int seg,
                                   long long *out_frames);

/** 读/写 manifest（write 为原子替换：同目录临时文件 → fsync → rename → fsync 目录）。 */
int karaoke_cache_segment_manifest_read(const char *path, KaraokeSegmentManifest *out,
                                        char *err, size_t err_cap);
int karaoke_cache_segment_manifest_write(const KaraokeSegmentPaths *paths,
                                         const KaraokeSegmentManifest *m,
                                         char *err, size_t err_cap);

/**
 * 原子写段就绪标记（最后一步）：写 `<marker>.tmp.<pid>.<seq>` → fsync →
 * rename 到 seg%03d.ready → fsync 目录。调用前必须已 finish+fsync 两份 FLAC。
 */
int karaoke_cache_segment_mark_ready(const KaraokeSegmentPaths *paths, int seg,
                                     long long frames, char *err, size_t err_cap);

/**
 * 校验某段是否就绪：ready 标记可解析且 seg 匹配 → 两份 FLAC 都是普通文件
 * （拒符号链接）→ STREAMINFO = 44100/2ch/16bit → 帧数 = 标记值且与 manifest
 * 期望一致（manifest 完整时）→ 文件大小 > 1024 字节（检测截断/空文件；
 * v2 不做静音压缩比判断——静音段也允许短帧/低能量）。1=就绪，0=未就绪。
 */
int karaoke_cache_segment_ready(const KaraokeSegmentPaths *paths,
                                const KaraokeSegmentManifest *m, int seg,
                                long long *out_frames, char *err, size_t err_cap);

/** 查询曲目的 v2 状态：1=有 manifest 可读（回填 out_paths 与 out_manifest）；
 *  0=没有。 */
int karaoke_cache_segment_lookup(const KaraokeCacheConfig *cfg, const char *track_path,
                                 KaraokeSegmentPaths *out_paths,
                                 KaraokeSegmentManifest *out_manifest);

/** 某段成对文件是否可用（播放源切换判定用）：1=可用，0=不可用。 */
int karaoke_cache_segment_ready_for_mode(const KaraokeCacheConfig *cfg,
                                         const char *track_path, int seg,
                                         int mode);

/** 解析某段某档位的播放文件路径：1=命中并填 out_path；0=未命中。 */
int karaoke_cache_segment_resolve(const KaraokeCacheConfig *cfg, const char *track_path,
                                  int mode, int seg, char *out_path, size_t out_cap);

/** v2 曲目是否完整（manifest complete=1 且全部段 ready）。1=完整。 */
int karaoke_cache_segment_complete(const KaraokeCacheConfig *cfg, const char *track_path);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_CACHE_H */
