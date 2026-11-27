/**
 * @file karaoke_worker.h
 * @brief 可选 ONNX 模型消人声 — 后台分离 worker（单线程 + 单 job）
 *
 * 职责：在低优先级后台线程里把一首曲目解码成 44.1kHz 立体声 → 逐 chunk 跑
 * ONNX 模型（固定 overlap 0.25）→ 同一遍推理结果里同时生成
 *   inst（纯伴奏）与 soft（0.4×原声 + 0.6×inst）
 * → 重采样回原曲采样率 → 两份 16bit FLAC → 原子提交成对缓存。
 *
 * 规则（用户拍板）：
 *   - 同一曲目同一时刻只允许一个 job；换曲/关闭/退出取消当前 job；
 *   - >30 分钟曲目拒绝（KARAOKE_WORKER_MAX_SECONDS）；
 *   - 首版不预取下一曲；
 *   - 单声道/模型或运行时缺失/解码/推理/编码失败 → FAILED，不生成坏缓存；
 *   - 播放线程只查询状态，不做推理；worker 自己开模型会话（CPU EP、
 *     DisableCpuMemArena，见 karaoke_model.h），并把建会话期间 ORT 的
 *     ~116KB stderr 噪音定向到 /dev/null，避免污染 TUI。
 *
 * 线程/生命周期：
 *   - karaoke_worker_init() 在音频引擎初始化后调用一次（清状态、清理过期
 *     半成品由调用方用 karaoke_cache_cleanup_stale 完成）；
 *   - karaoke_worker_request() 可任意线程调用；重复请求同一曲目（缓存已完成/
 *     正在跑同一 job）直接返回 0；
 *   - karaoke_worker_cancel() 请求取消当前 job（在解码包/推理 chunk/编码帧边界
 *     生效；一次 chunk 推理约 1–2s，取消延迟以此为上限）；
 *   - karaoke_worker_shutdown() 取消并 join；200ms 内未退出则 detach，保证
 *     进程退出不被 worker 阻塞（残留 .part 由下次启动清理）。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#ifndef KARAOKE_WORKER_H
#define KARAOKE_WORKER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KARAOKE_WORKER_ERR_CAP       512
#define KARAOKE_WORKER_PATH_CAP      4096
#define KARAOKE_WORKER_MODEL_ID_CAP  128
#define KARAOKE_WORKER_MAX_SECONDS   (30 * 60)   /* >30min 拒绝分离 */

/** job 产出的缓存格式（2026-11-25 起生产恒为 v2 分段；v1 仅保留给旧版
 *  单测 test_karaoke_model.c 的成对缓存兼容夹具，生产不再生成）。
 *
 *  R2：V2=0 是唯一的“零初始化安全默认”——任何新调用点 memset 后忘记赋值
 *  只会走 v2（只写 /tmp 会话缓存，符合产品契约），不会静默写 XDG v1。
 *  v1 写路径必须显式赋 KARAOKE_JOB_FORMAT_LEGACY_V1；未指定（UNSPECIFIED）
 *  或未知值由 karaoke_worker_request() 拒绝。 */
#define KARAOKE_JOB_FORMAT_UNSPECIFIED (-1)
#define KARAOKE_JOB_FORMAT_SEGMENTS_V2 0
#define KARAOKE_JOB_FORMAT_LEGACY_V1  1

/** progress 阶段（供状态栏显示"准备中/顺序/回填"）。 */
typedef enum {
    KARAOKE_PHASE_NONE = 0,
    KARAOKE_PHASE_FORWARD = 1,   /**< 顺序向前产出段 */
    KARAOKE_PHASE_BACKFILL = 2,  /**< 曲尾后回填此前缺失段 */
    KARAOKE_PHASE_COMPLETE = 3   /**< 整曲段集完整 */
} KaraokeJobPhase;

/** job 状态（供状态栏/设置页/测试查询）。 */
typedef enum {
    KARAOKE_JOB_IDLE = 0,      /**< 无 job（首次播放/已取消） */
    KARAOKE_JOB_RUNNING = 1,   /**< 正在分离；percent 有效 */
    KARAOKE_JOB_READY = 2,     /**< 最近一次 job 成功，成对缓存已提交 */
    KARAOKE_JOB_FAILED = 3,    /**< 最近一次 job 失败（error 有效） */
    KARAOKE_JOB_CANCELLED = 4  /**< 最近一次 job 被取消（换曲/关闭/退出） */
} KaraokeJobState;

/** 一次分离请求（由 audio.c 从配置构造；worker 复制字符串，不持有指针）。 */
typedef struct {
    char track_path[KARAOKE_WORKER_PATH_CAP];
    char model_id[KARAOKE_WORKER_MODEL_ID_CAP];
    char model_path[KARAOKE_WORKER_PATH_CAP];   /* $XDG_DATA_HOME/ter-music/models/<id>.onnx */
    char model_sha256[65];
    char cache_root[KARAOKE_WORKER_PATH_CAP];   /* v1 根：$XDG_CACHE_HOME/ter-music/karaoke
                                                 * （只读兼容；v2 job 忽略该字段，写
                                                 *  /tmp 会话根） */
    int format;                                 /* KARAOKE_JOB_FORMAT_*：0=v2 分段
                                                 * （生产/零初始化默认），1=v1
                                                 * （旧版兼容夹具显式指定） */
    float overlap;                              /* 固定 0.25 */
    float soft_inst_mix;                        /* 固定 0.60 */
    /* 可选：当前正在播放的曲目（预取下一曲时由 audio.c 填）。v2 会话缓存
     * 预算淘汰时保护该曲目与本次 job 曲目的段集，绝不删除正在播放的伴奏。 */
    char protect_track[KARAOKE_WORKER_PATH_CAP];
} KaraokeWorkerJob;

/** 状态快照（值拷贝，线程安全）。 */
typedef struct {
    int state;                     /* KaraokeJobState */
    int percent;                   /* 0..100（RUNNING/READY 时有效） */
    long long chunks_done;
    long long chunks_total;        /* 估算值（按曲目时长），可能为 0 */
    int segments_done;             /* v2：已就绪段数（含上次 job 留下的） */
    int segments_total;            /* v2：总段数；EOF 前为 0（未知） */
    int first_segment_ready;       /* v2：段 0 是否已就绪（首段起播判定） */
    int phase;                     /* KaraokeJobPhase */
    int completed_jobs;            /* 成功完成次数（测试"只触发一次"） */
    int cancelled_jobs;
    int failed_jobs;
    char track_path[KARAOKE_WORKER_PATH_CAP];
    char error[KARAOKE_WORKER_ERR_CAP];
} KaraokeWorkerStatus;

/** 状态变化回调（在 worker 线程调用；message 可能为空串）。
 * 由 audio.c 注册 → 状态栏提示；测试不注册，从而不依赖 core/i18n。 */
typedef void (*KaraokeWorkerStatusCb)(int state, const char *message);
void karaoke_worker_set_status_callback(KaraokeWorkerStatusCb cb);

/** 初始化模块（幂等；不会起线程）。 */
void karaoke_worker_init(void);

/**
 * 请求分离一首曲目。
 * @return 0 = 接收/已在跑同一曲目/缓存已完成（无需新 job）；
 *         -1 = 参数非法或模块已 shutdown。
 */
int karaoke_worker_request(const KaraokeWorkerJob *job);

/** 请求取消当前 job（换曲/关闭功能/退出前调用）。 */
void karaoke_worker_cancel(void);

/** 取消并停止 worker 线程；不阻塞（200ms 后 detach）。 */
void karaoke_worker_shutdown(void);

/** 当前是否有 job 在跑。 */
int karaoke_worker_busy(void);

/** 取状态快照；返回 0 成功。 */
int karaoke_worker_status(KaraokeWorkerStatus *out);

/** 当前 job 的曲目路径（无 job 时为空串）；返回 1 表示有路径。 */
int karaoke_worker_current_track(char *out, size_t cap);

/**
 * seek 优先提示（D4）：播放线程 seek 到未处理段时调用。
 * 只对**当前正在跑同一曲目**的 v2 job 生效：worker 会在下一个 chunk 边界
 * 放弃当前段的半成品（不写 ready 标记），从目标段前一个模型 chunk 预热后
 * 优先产出目标段，再顺序到曲尾，最后回填此前缺失段。
 *
 * @return 0 = 已受理/无需处理（job 不在跑同一曲目也返回 0）；
 *         -1 = 参数非法。
 */
int karaoke_worker_hint_segment(const char *track_path, int segment);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_WORKER_H */
