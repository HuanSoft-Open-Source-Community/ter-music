#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdint.h>
#include <pthread.h>

/**
 * 进度跟踪器结构体
 * 基于样本数累加的精确进度跟踪
 */
typedef struct {
    // === 核心状态 ===
    int64_t total_samples_played;      // 累计已播放的样本帧数
    int sample_rate;                    // 当前音频采样率（如 44100）
    float playback_speed;               // 倍速播放因子（如 1.0, 1.5, 2.0）
    
    // === Wall-clock 时间跟踪（用于校准） ===
    uint64_t play_start_time_us;        // 播放开始时的微秒时间戳
    uint64_t pause_accumulated_us;      // 暂停累计的微秒
    uint64_t last_pause_time_us;        // 上次暂停的时间点
    int is_playing;                     // 是否正在播放
    
    // === 线程同步 ===
    pthread_mutex_t lock;               // 保护上述状态
} ProgressTracker;

/**
 * 初始化进度跟踪器
 * @param sample_rate 音频采样率（如 44100）
 */
void progress_tracker_init(int sample_rate);

/**
 * 销毁进度跟踪器，释放资源
 */
void progress_tracker_destroy(void);

/**
 * 累加已播放的样本数
 * @param num_samples 样本数量
 */
void progress_tracker_add_samples(int num_samples);

/**
 * 获取当前播放位置（秒）
 * @return 当前位置（秒）
 */
int progress_tracker_get_position_seconds(void);

/**
 * 获取当前播放位置（毫秒）
 *
 * 与秒版共用同一份样本计数与 wall-clock 校准，只是不整除到秒：
 * 歌词逐字高亮需要亚秒精度（整秒跳变不足以驱动 100–300ms 粒度的字符切换）。
 * @return 当前位置（毫秒，>=0）
 */
int64_t progress_tracker_get_position_ms(void);

/**
 * 跳转到指定位置
 * @param position_seconds 目标位置（秒）
 */
void progress_tracker_seek(int position_seconds);

/**
 * 暂停时的回调
 */
void progress_tracker_on_pause(void);

/**
 * 恢复播放时的回调
 */
void progress_tracker_on_resume(void);

/**
 * 停止播放时重置跟踪器
 */
void progress_tracker_on_stop(void);

/**
 * 获取进度百分比（0-100）
 * @param total_duration 总时长（秒）
 * @return 进度百分比
 */
int progress_tracker_get_percent(int total_duration);

/**
 * 检查进度跟踪器是否已就绪
 * @return 1 表示已就绪，0 表示未就绪
 */
int progress_tracker_is_ready(void);

/**
 * 更新采样率（动态切换歌曲时使用）
 * @param new_sample_rate 新的采样率
 */
void progress_tracker_set_sample_rate(int new_sample_rate);

/**
 * 设置倍速播放因子
 * @param speed 倍速值（如 1.0, 1.5, 2.0）
 */
void progress_tracker_set_speed(float speed);

/**
 * 开始播放，启动wall-clock计时
 * 在音频设备完全准备好后调用
 */
void progress_tracker_start(void);

#endif // PROGRESS_H
