/**
 * @file karaoke_model.h
 * @brief 可选 ONNX 模型消人声 — 受支持模型表、校验/安装、运行时 dlopen 与会话
 *
 * 本模块只负责"模型域"：知道哪些模型是可受支持的、校验/安装模型文件、
 * 运行期可选地加载 ONNX Runtime、按受支持表项跑单段分离。**不含** UI、
 * 配置、缓存与播放接线（那些在别的模块/任务里）。
 *
 * 设计边界（刻意如此，不要放宽）：
 *
 * 1. **编译期不链接 ONNX Runtime**。运行期按
 *    `libonnxruntime.so.1.21` → `libonnxruntime.so.1` → `libonnxruntime.so`
 *    的顺序 `dlopen(RTLD_NOW | RTLD_LOCAL)`，取 `OrtGetApiBase`，再按
 *    `ORT_API_VERSION`(21) 向下尝试 16 取第一张可用 API 表。库缺失/版本不符/
 *    建会话失败都只返回可读错误：不崩溃、不影响主程序，也不影响播放原有
 *    路径（失败一律回退原声）。
 * 2. **只接受受支持模型表内的模型**。MDX-Net 的 n_fft/dim_f/dim_t/hop/
 *    compensate/primary_stem 无法从任意 ONNX 可靠推断，所以"任意 .onnx
 *    一律拒绝"是有意的安全边界（错误里给出期望 SHA256 与官方 URL）。
 * 3. **模型随包分发，但不进 git**（config v12 起）。唯一内置模型固定为
 *    `uvr-mdxnet-3-9662`：打包时由 `TER_MUSIC_BUNDLED_MODEL` 注入并安装到
 *    `<安装前缀>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`，运行期
 *    由 `karaoke_model_bundled_path()` 定位 + 校验大小/SHA256 后直接使用，
 *    **不复制到 XDG**。旧用户曾在 `$XDG_DATA_HOME/ter-music/models/` 手工
 *    安装同一模型，作为只读兼容回退保留。模型许可为 MIT，署名/来源/
 *    SHA256 见 `resources/models/NOTICE.md`（随包安装）与本文件的模型表。
 * 4. 只验证并支持 **44.1kHz 立体声 float32**（模型训练/推理采样率）；其它
 *    采样率由调用方先重采样。
 *
 * 线程与生命周期约束（实现必须遵守，调用方也按此用）：
 *
 * - `karaoke_model_runtime_available()` / `runtime_library()` /
 *   `runtime_version()`：任意线程可调，内部 `pthread_once` 缓存探测结果；
 *   静态构建命中即返回 0（运行库名固定为 `static (linked into ter-music)`）。
 * - `KaraokeModelSession` **非线程安全**：同一会话同一时刻只允许一个线程
 *   调用 `karaoke_model_infer_patch` / `karaoke_model_separate`；不同会话
 *   互不影响。建议全进程只保留一个会话（ORT CPU 会话常驻约 76MB）。
 * - `karaoke_model_validate_file()` / `karaoke_model_install()` 会读 30MiB
 *   级文件做 SHA256 并建一次 ONNX 会话（几十到几百毫秒，见 REPORT 实测
 *   74–121ms），**不得**在 UI 热路径或播放线程里调用；调用方应放到后台任务。
 * - 受支持表项（`KaraokeModelSpec`）是静态只读数据，永不释放；指针可长期持有。
 * - `karaoke_model_separate()` 内部按 `REPORT §2.3` 的 MDX 流水线（STFT →
 *   逐 chunk 推理 → iSTFT → Hann 交叉淡化）处理任意长度；额外堆内存约
 *   16B/采样点（3 分钟曲目约 100MB）加每 chunk 约 4MB FFT/OLA 暂存。
 * - ONNX Runtime（Debian 包）建会话时会向 **stderr** 打印大量
 *   `Schema error: ...` 警告（实测约 116KB/567 行）。本模块不劫持全局 fd；
 *   调用方（例如分离 worker 子进程）必须把 stderr 重定向到 /dev/null 或
 *   日志文件，否则 TUI 会被刷屏、pipe 未排空还会死锁。
 *
 * ONNX Runtime 的两种接入方式（对调用方透明，只有 `runtime_library()` 文案不同）：
 * - **静态链接**（官方 deb/rpm/AppImage：`-DTER_MUSIC_ONNXRUNTIME_STATIC=ON`，
 *   静态库由 `scripts/build/build-onnxruntime-static.sh` 产出）：ORT 编进
 *   ter-music，`ORT_API_VERSION 21` 直接可用，不再依赖系统
 *   `libonnxruntime.so`（Debian 10/11/12 与 EL7–10 根本没有这个包）。
 * - **运行期 dlopen**（默认/源码构建）：编译期零链接，按
 *   `libonnxruntime.so.1.21` → `.so.1` → `.so` 顺序加载；缺库时本功能不可用，
 *   原声播放不受影响，错误原因由 `karaoke_model_runtime_available()` 给出。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#ifndef KARAOKE_MODEL_H
#define KARAOKE_MODEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 常量 ───────────────────────────────────────────────────────── */

/** 模型要求的采样率（Hz）：只支持 44.1kHz，其它先由调用方重采样。 */
#define KARAOKE_MODEL_SAMPLE_RATE 44100

/** 模型要求的声道数：立体声交织。 */
#define KARAOKE_MODEL_CHANNELS 2

/** 默认推理线程上限：本机 Ryzen 5 4500U 实测 4 线程最优，6 线程反而更慢。 */
#define KARAOKE_MODEL_THREADS_CAP 4

/** 模型 id（也是安装文件名与缓存目录名）缓冲建议大小。 */
#define KARAOKE_MODEL_ID_CAP 96

/** 路径缓冲建议大小。 */
#define KARAOKE_MODEL_PATH_CAP 4096

/** 错误文本缓冲建议大小（调用方给 err 缓冲时建议 ≥ 此值）。 */
#define KARAOKE_MODEL_ERR_CAP 512

/** 分离目标 stem。 */
typedef enum {
    KARAOKE_STEM_VOCALS = 0,        /**< 主 stem = 人声（模型直出） */
    KARAOKE_STEM_INSTRUMENTAL = 1   /**< 伴奏 = mix − compensate × 人声 */
} KaraokeStem;

/* ── 受支持模型表 ───────────────────────────────────────────────── */

/**
 * 一个受支持模型的全部元数据与推理参数。
 *
 * 数值来源是 UVR（Ultimate Vocal Remover GUI）官方 model_data.json 与其
 * 5.6 的 MDX-Net 推理公式（见团队研究 `.tmp/karaoke-research/REPORT.md`）；
 * SHA256/大小为官方 release 资产实测值。新增模型只扩展表，不改调用方。
 */
typedef struct {
    const char *id;            /**< 应用内 id，也是安装文件名与缓存目录名 */
    const char *display_name;  /**< 显示名（UVR model_name_mapper） */
    const char *url;           /**< 官方下载直链（设置页展示） */
    const char *sha256;        /**< 整文件 SHA256（小写 hex，64 字符） */
    const char *uvr_hash;      /**< UVR 自己的 model hash（文件末 10MiB 的 MD5） */
    long long   size;          /**< 文件字节数 */
    int   n_fft;               /**< STFT 窗长/FFT 点数（6144） */
    int   hop;                 /**< STFT 跳距（1024） */
    int   dim_f;               /**< 模型频点数（2048，即 0–14.7kHz） */
    int   dim_t;               /**< 模型帧数（256） */
    float compensate;          /**< 伴奏时域相减的人声系数（1.035） */
    int   primary_stem;        /**< 模型直出的 stem：KARAOKE_STEM_VOCALS */
    const char *input_name;    /**< ONNX 输入名（"input"） */
    const char *output_name;   /**< ONNX 输出名（"output"） */
    int   input_rank;          /**< 输入张量秩（4 = [batch,4,dim_f,dim_t]） */
    int   input_channels;      /**< 输入第二维（4 = [L.re,L.im,R.re,R.im]） */
    int   batch_dynamic;       /**< 非 0：batch 维动态（ONNX 里是 -1/未定） */
    const char *license;       /**< 许可说明（"MIT …"） */
    const char *credit;        /**< 署名文案（UVR 要求，UI 展示） */
} KaraokeModelSpec;

/* ── 内置模型（随包分发，config v12） ────────────────────────────── */

/**
 * 内置模型的固定应用内 id（= 包内文件名去掉 .onnx，也是缓存目录名）。
 * 自 config v12 起应用只使用这一个模型：没有"功能开关/模型选择"配置键，
 * 控制栏按钮与设置页三档始终可见。
 */
#define KARAOKE_MODEL_BUNDLED_ID "uvr-mdxnet-3-9662"

/** 内置模型的包内文件名（安装到 `<数据目录>/models/<该文件名>`）。 */
#define KARAOKE_MODEL_BUNDLED_FILE "uvr-mdxnet-3-9662.onnx"

/**
 * 取受支持模型表（静态只读）。
 * @param count 可空；返回表项数量。
 * @return 表首项；表永远非空。
 */
const KaraokeModelSpec *karaoke_model_supported(size_t *count);

/** 按应用内 id 查表；未知 id 返回 NULL。 */
const KaraokeModelSpec *karaoke_model_by_id(const char *id);

/** 按整文件 SHA256（大小写不敏感）查表；未知返回 NULL。 */
const KaraokeModelSpec *karaoke_model_by_sha256(const char *sha256_hex);

/* ── ONNX Runtime（dlopen，可选） ───────────────────────────────── */

/**
 * 探测 ONNX Runtime 是否可用（静态链接时直接校验内置 API 表；动态构建时
 * dlopen + API 表版本校验；都不建会话）。结果内部缓存；可被任意线程调用。
 *
 * @param err     可空；失败时写入可读原因（含尝试过的库名/dlerror）。
 * @param err_cap err 容量。
 * @return 0 = 可用；-1 = 不可用（不影响主程序，调用方应把本功能标记为不可用）。
 */
int karaoke_model_runtime_available(char *err, size_t err_cap);

/** 运行库名：动态构建如 "libonnxruntime.so.1.21"，静态构建为
 *  "static (linked into ter-music)"；不可用/未加载返回 NULL。 */
const char *karaoke_model_runtime_library(void);

/** 运行库版本字符串（如 "1.21.0"）；不可用/未加载返回 NULL。 */
const char *karaoke_model_runtime_version(void);

/* ── 校验 / 安装 ────────────────────────────────────────────────── */

/**
 * 计算文件 SHA256（小写 hex，65 字节含结尾 NUL）。供校验/安装/诊断复用。
 * @return 0 成功；-1 失败（err 写原因）。
 */
int karaoke_model_sha256_file(const char *path, char out_hex[65],
                              char *err, size_t err_cap);

/**
 * 在给定数据目录里定位**内置模型**并校验大小 + 整文件 SHA256：
 *
 *   1) `<bundled_data_dir>/models/<id>.onnx` —— 随包安装位置（首选）；
 *   2) `<legacy_data_dir>/models/<id>.onnx` —— 旧用户手工安装位置
 *      （`$XDG_DATA_HOME/ter-music`，只读兼容，绝不写入）。
 *
 * 只做 stat + SHA256，**不建 ONNX 会话**（不需要 ONNX Runtime，几十毫秒级）；
 * 因此"库缺失"与"模型缺失/损坏"是两条可区分的失败路径。任一位置的文件
 * 存在但大小/SHA256 不符时立即报"内置模型校验失败"（不静默改用另一位置，
 * 坏包必须可见）；两个位置都不存在时报"内置模型缺失"。
 *
 * @param spec              受支持表项（不能为 NULL；生产传内置 id 的表项）。
 * @param bundled_data_dir  随包安装的数据目录（可为 NULL/空=跳过该位置）。
 * @param legacy_data_dir   旧用户数据目录（可为 NULL/空=跳过该位置）。
 * @param out               成功时写入选中的绝对路径（NUL 结尾）。
 * @param err               失败时写入用户可读原因（含"内置模型缺失/校验失败"）。
 * @return 0 = 通过；-1 = 不可用。
 */
int karaoke_model_locate_bundled(const KaraokeModelSpec *spec,
                                 const char *bundled_data_dir,
                                 const char *legacy_data_dir,
                                 char *out, size_t cap,
                                 char *err, size_t err_cap);

/**
 * 生产入口：用编译期 `TER_MUSIC_DATA_DIR` 与调用方给的旧用户数据目录
 * （通常是 `app_data_dir(0)` = `$XDG_DATA_HOME/ter-music`）定位并校验内置
 * 模型。结果（成功或失败）在**进程内缓存**：首次使用时校验一次，之后不再
 * 重复读 30MiB 文件，也不重复报错。
 *
 * @param legacy_data_dir 旧用户数据目录（可为 NULL）。
 * @param out             成功时写入选中的绝对路径。
 * @param err             失败时写入用户可读原因。
 * @return 0 = 可用；-1 = 不可用（调用方应回退原声并提示）。
 */
int karaoke_model_bundled_path(const char *legacy_data_dir,
                               char *out, size_t cap,
                               char *err, size_t err_cap);

/**
 * 校验一个候选模型文件是否为受支持模型：
 *   1) 存在、可读、是普通文件；
 *   2) 大小与表项精确相等；
 *   3) 整文件 SHA256 命中表项；
 *   4) 能建 ONNX 会话，且输入/输出名、元素类型(float32)、形状
 *      ([?,4,dim_f,dim_t]) 与表项一致。
 * 任何一条不满足都拒绝（错误里尽量给出期望值/实际值）。
 *
 * 会做 30MiB 级 I/O 与一次建会话，**不要**在 UI/播放线程调用。
 *
 * 注意：生产路径**不再调用**本函数（内置模型在运行期只做大小 + SHA256 校验，
 * 见 `karaoke_model_bundled_path()`：建 ONNX 会话依赖可选运行库，模型校验
 * 不应把它当前置条件）。本函数保留给单元测试与排障工具做完整校验。
 *
 * @param out_spec 可空；成功时回填命中的表项（静态指针）。
 * @return 0 = 通过；-1 = 拒绝（err 写原因）。
 */
int karaoke_model_validate_file(const char *path, const KaraokeModelSpec **out_spec,
                               char *err, size_t err_cap);

/**
 * 计算受支持模型在给定数据目录下的安装路径：
 * `<data_dir>/models/<model_id>.onnx`。
 *
 * @param data_dir 调用方给的 ter-music 数据目录（例如
 *                 `$XDG_DATA_HOME/ter-music` 或 `$HOME/.local/share/ter-music`）。
 * @param out     输出缓冲；成功时总是 NUL 结尾。
 * @return 0 成功；-1 参数非法/路径过长（err 写原因，可空）。
 */
int karaoke_model_installed_path(const KaraokeModelSpec *spec, const char *data_dir,
                                 char *out, size_t cap);

/**
 * 校验源文件后**原子安装**到 `<data_dir>/models/<model_id>.onnx`：
 *
 * @note 自 config v12 起模型随包分发，生产路径**不再调用**本函数（也不再
 *       有"选模型/安装"界面）。保留给单元测试与"旧用户手工安装"的排障工具。
 *
 *   - 复制到同目录的 `.<id>.onnx.part.<pid>`（0600，O_EXCL）；
 *   - 复制过程中重新计算 SHA256，与表项比对（防校验后源文件被替换）；
 *   - `fsync` 文件后 `rename` 到目标、再 `fsync` 目录；
 *   - 任一步失败都删除半成品，绝不留下 `.part`；
 *   - 目标已存在且内容正确（大小+SHA256）时跳过复制（幂等）；
 *   - 目标已存在但内容不符时原子替换；
 *   - 目标路径只由 `<model_id>` 决定：安装不同 id 不会覆盖彼此。
 *
 * @param src      用户选择的源文件（任意路径，通常是下载目录）。
 * @param data_dir 调用方给的 ter-music 数据目录。
 * @param out_id   可空；成功时写入模型 id（与 `karaoke_model_supported()` 一致）。
 * @param id_cap   out_id 容量（建议 KARAOKE_MODEL_ID_CAP）。
 * @return 0 成功；-1 失败（err 写原因；失败时不留下目标/临时文件）。
 */
int karaoke_model_install(const char *src, const char *data_dir,
                          char *out_id, size_t id_cap,
                          char *err, size_t err_cap);

/* ── ONNX 会话与推理 ────────────────────────────────────────────── */

/** 不透明的 ONNX 会话（非线程安全，见文件头约定）。 */
typedef struct KaraokeModelSession KaraokeModelSession;

/**
 * 打开一个受支持模型的推理会话。
 *
 * 实现要点（与研究报告对齐，改动需要重新做性能/正确性验证）：
 *   - 只用 CPU ExecutionProvider；**绝不**启用 DNNL EP（本机直接段错误）；
 *   - `DisableCpuMemArena`（峰值内存 1.11GiB → 0.68GiB）；
 *   - `IntraOpNumThreads = threads`（默认 `min(4, nproc-1)`，最少 1）、
 *     `InterOpNumThreads = 1`、图优化 `ORT_ENABLE_ALL`。
 *
 * 调用方应先用 `karaoke_model_validate_file()` 校验；本函数仍会再核对
 * 一次输入/输出名与形状，不符返回错误。
 *
 * @param spec       表项（不能为 NULL）。
 * @param model_path 已校验/已安装的 .onnx 路径。
 * @param threads    推理线程数；≤0 用默认值，超过核数会被钳制。
 * @param err        可空；失败原因。
 * @return 会话指针；失败 NULL（err 写原因）。
 */
KaraokeModelSession *karaoke_model_session_open(const KaraokeModelSpec *spec,
                                                const char *model_path,
                                                int threads,
                                                char *err, size_t err_cap);

/** 关闭并释放会话（NULL 安全）。 */
void karaoke_model_session_close(KaraokeModelSession *session);

/** 会话对应的表项（只读）；session 为 NULL 返回 NULL。 */
const KaraokeModelSpec *karaoke_model_session_spec(const KaraokeModelSession *session);

/** 会话实际使用的推理线程数；session 为 NULL 返回 0。 */
int karaoke_model_session_threads(const KaraokeModelSession *session);

/** 单次模型输入的 float 元素数：4 × dim_f × dim_t（1 个 patch）。 */
size_t karaoke_model_patch_elems(const KaraokeModelSpec *spec);

/** 一个分离 chunk 的样本帧数：hop × (dim_t − 1)（44.1kHz 下 261120 ≈ 5.92s）。 */
size_t karaoke_model_chunk_frames(const KaraokeModelSpec *spec);

/**
 * 单 patch 推理（最底层接口）。
 *
 * 输入/输出布局均为 `[4][dim_f][dim_t]`（float32，batch 固定 1）：
 *   ch0 = L.re, ch1 = L.im, ch2 = R.re, ch3 = R.im（频点主序）。
 * 由调用方自己做 STFT/iSTFT 时才用这个接口。
 *
 * @param in_patch  `karaoke_model_patch_elems()` 个 float。
 * @param out_patch 同长度输出缓冲。
 * @return 0 成功；-1 失败（err 写原因）。
 */
int karaoke_model_infer_patch(KaraokeModelSession *session,
                              const float *in_patch, float *out_patch,
                              char *err, size_t err_cap);

/**
 * DSP 自检：不需要模型文件、也不需要 ONNX Runtime，仅用 FFT/OLA 对一段
 * 带限立体声信号做 STFT → iSTFT 往返，检查相对 RMS 误差 < 1e-3。
 * 供单元测试与现场排障使用（验证本项目自带 FFT/OLA 与 Σw² 归一化数学）。
 *
 * @return 0 通过；-1 失败/参数非法（err 写原因与实际误差）。
 */
int karaoke_model_stft_selftest(const KaraokeModelSpec *spec, char *err, size_t err_cap);

/**
 * 单段分离：对一段 44.1kHz 立体声浮点混音跑完整 MDX 流水线，输出指定 stem。
 *
 * 数学（逐字复刻 UVR 5.6，见 REPORT §2.3；n 为表项参数）：
 *   chunk_size = hop×(dim_t−1)；trim = n_fft/2；gen = chunk_size−2×trim；
 *   step = chunk_size−n_fft（Default overlap）；
 *   pad = gen+trim−(frames mod gen)；
 *   分析用周期 Hann、合成用同一周期 Hann、OLA 按 Σw² 归一；
 *   chunk 间用对称 Hann(`actual`) 交叉淡化并按同一窗归一；
 *   结果去全局 trim、截断到 frames 后 ×compensate 得人声；
 *   伴奏 = mix − compensate×人声（时域相减，UVR 语义）。
 *
 * @param mix    交织立体声 float32（长度 frames×2，L,R,L,R…）。
 * @param frames 每声道采样帧数（44.1kHz）。
 * @param stem   KARAOKE_STEM_VOCALS 或 KARAOKE_STEM_INSTRUMENTAL。
 * @param out    交织立体声输出（长度 frames×2）；可与 mix 不同缓冲。
 * @return 0 成功（输出长度与输入逐样本一致）；-1 失败（err 写原因）。
 *
 * 说明：内部会分配 O(frames) 的结果/归一化缓冲；对整首 3 分钟曲目约 100MB，
 * 若调用方要处理超长音频，建议自行分段调用并保证段边界有重叠。
 */
int karaoke_model_separate(KaraokeModelSession *session,
                           const float *mix, size_t frames, KaraokeStem stem,
                           float *out, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_MODEL_H */
