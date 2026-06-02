#include "audio/progress/progress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// 全局进度跟踪器实例
static ProgressTracker g_tracker = {
    .total_samples_played = 0,
    .sample_rate = 0,
    .playback_speed = 1.0f,
    .play_start_time_us = 0,
    .pause_accumulated_us = 0,
    .last_pause_time_us = 0,
    .is_playing = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/**
 * 获取当前时间的微秒数
 */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

void progress_tracker_init(int sample_rate) {
    pthread_mutex_lock(&g_tracker.lock);
    
    g_tracker.sample_rate = sample_rate;
    g_tracker.playback_speed = 1.0f;
    g_tracker.total_samples_played = 0;
    g_tracker.play_start_time_us = 0;
    g_tracker.pause_accumulated_us = 0;
    g_tracker.last_pause_time_us = 0;
    g_tracker.is_playing = 0;
    
    pthread_mutex_unlock(&g_tracker.lock);
}

void progress_tracker_start(void) {
    pthread_mutex_lock(&g_tracker.lock);
    
    uint64_t now = get_current_time_us();
    g_tracker.play_start_time_us = now;
    g_tracker.pause_accumulated_us = 0;
    g_tracker.is_playing = 1;
    
    pthread_mutex_unlock(&g_tracker.lock);
}

void progress_tracker_destroy(void) {
    pthread_mutex_destroy(&g_tracker.lock);
}

void progress_tracker_add_samples(int num_samples) {
    pthread_mutex_lock(&g_tracker.lock);

    if (g_tracker.is_playing && num_samples > 0 && g_tracker.playback_speed > 0) {
        // atempo 滤镜输出的样本数已经根据倍速调整
        // 为了正确计算原始音频的播放位置，需要将样本数乘以倍速
        g_tracker.total_samples_played += (int64_t)(num_samples * g_tracker.playback_speed);
    }

    pthread_mutex_unlock(&g_tracker.lock);
}

int progress_tracker_get_position_seconds(void) {
    pthread_mutex_lock(&g_tracker.lock);
    
    int position = 0;
    
    if (g_tracker.sample_rate > 0) {
        position = (int)(g_tracker.total_samples_played / g_tracker.sample_rate);
        
        // BUGFIX 2026.03.26: 只有当 play_start_time_us 非零时才进行 wall-clock 校准
        // 避免在进度跟踪器启动前产生错误的非零位置
        if (g_tracker.is_playing && g_tracker.play_start_time_us > 0) {
            uint64_t current_time = get_current_time_us();
            uint64_t elapsed = current_time - g_tracker.play_start_time_us;
            elapsed -= g_tracker.pause_accumulated_us;
            int wallclock_pos = (int)(elapsed / 1000000);
            
            int diff = abs(position - wallclock_pos);
            // BUGFIX 2026.03.26: 降低校准阈值从 2 秒到 0 秒
            // 任何正向差异都校正（wallclock_pos > position），确保初始化延迟被快速纠正
            if (diff > 0 && wallclock_pos > position) {
                int64_t correction_samples = (int64_t)wallclock_pos * g_tracker.sample_rate;
                if (correction_samples > g_tracker.total_samples_played) {
                    g_tracker.total_samples_played = correction_samples;
                    position = wallclock_pos;
                }
            }
        }
    }
    
    pthread_mutex_unlock(&g_tracker.lock);
    return position;
}

void progress_tracker_seek(int position_seconds) {
    pthread_mutex_lock(&g_tracker.lock);
    
    if (position_seconds < 0) {
        position_seconds = 0;
    }
    
    if (g_tracker.sample_rate > 0) {
        g_tracker.total_samples_played = (int64_t)position_seconds * g_tracker.sample_rate;
        
        if (g_tracker.is_playing) {
            uint64_t current_pos_us = (uint64_t)position_seconds * 1000000;
            uint64_t now = get_current_time_us();
            if (now >= current_pos_us) {
                g_tracker.play_start_time_us = now - current_pos_us;
            } else {
                g_tracker.play_start_time_us = 0;
            }
            g_tracker.pause_accumulated_us = 0;
        }
    }
    
    pthread_mutex_unlock(&g_tracker.lock);
}

void progress_tracker_on_pause(void) {
    pthread_mutex_lock(&g_tracker.lock);
    
    g_tracker.last_pause_time_us = get_current_time_us();
    g_tracker.is_playing = 0;
    
    pthread_mutex_unlock(&g_tracker.lock);
}

void progress_tracker_on_resume(void) {
    pthread_mutex_lock(&g_tracker.lock);
    
    // 检查采样率是否有效，避免除零错误
    if (g_tracker.sample_rate <= 0) {
        pthread_mutex_unlock(&g_tracker.lock);
        return;
    }
    
    // 计算当前样本数对应的时间位置
    int current_pos_seconds = (int)(g_tracker.total_samples_played / g_tracker.sample_rate);
    uint64_t current_pos_us = (uint64_t)current_pos_seconds * 1000000;
    
    // 调整播放开始时间，使得 wall-clock 时间与样本数位置匹配
    // 即：play_start_time_us = 当前时间 - 当前位置时间
    uint64_t now = get_current_time_us();
    if (now >= current_pos_us) {
        g_tracker.play_start_time_us = now - current_pos_us;
    } else {
        // 避免溢出，这种情况通常不会发生
        g_tracker.play_start_time_us = 0;
    }
    
    g_tracker.pause_accumulated_us = 0;
    g_tracker.is_playing = 1;
    
    pthread_mutex_unlock(&g_tracker.lock);
}

void progress_tracker_on_stop(void) {
    pthread_mutex_lock(&g_tracker.lock);
    
    g_tracker.total_samples_played = 0;
    g_tracker.sample_rate = 0;
    g_tracker.play_start_time_us = 0;
    g_tracker.pause_accumulated_us = 0;
    g_tracker.last_pause_time_us = 0;
    g_tracker.is_playing = 0;
    
    pthread_mutex_unlock(&g_tracker.lock);
}

int progress_tracker_get_percent(int total_duration) {
    if (total_duration <= 0) {
        return 0;
    }
    
    int position = progress_tracker_get_position_seconds();
    int percent = (position * 100) / total_duration;
    
    if (percent > 100) {
        percent = 100;
    }
    
    return percent;
}

int progress_tracker_is_ready(void) {
    pthread_mutex_lock(&g_tracker.lock);
    int ready = (g_tracker.sample_rate > 0);
    pthread_mutex_unlock(&g_tracker.lock);
    return ready;
}

void progress_tracker_set_sample_rate(int new_sample_rate) {
    pthread_mutex_lock(&g_tracker.lock);

    if (new_sample_rate <= 0) {
        pthread_mutex_unlock(&g_tracker.lock);
        return;
    }

    // 如果已有样本累加，按旧采样率计算位置并转换为新采样率的样本数
    if (g_tracker.sample_rate > 0 && g_tracker.total_samples_played > 0) {
        int position_seconds = (int)(g_tracker.total_samples_played / g_tracker.sample_rate);
        g_tracker.total_samples_played = (int64_t)position_seconds * new_sample_rate;
    }

    g_tracker.sample_rate = new_sample_rate;

    // 只在已经有样本累加时才重新校准 wall-clock
    // 这避免了在播放开始前（total_samples_played = 0）意外启动计时
    // 场景 1：播放开始时 set_sample_rate 在 start() 之前调用，此时 total_samples_played = 0，不校准
    // 场景 2：播放中动态改变采样率，此时 total_samples_played > 0，需要校准以保持时间同步
    if (g_tracker.is_playing && g_tracker.total_samples_played > 0) {
        int current_pos_seconds = (int)(g_tracker.total_samples_played / g_tracker.sample_rate);
        uint64_t current_pos_us = (uint64_t)current_pos_seconds * 1000000;
        uint64_t now = get_current_time_us();
        if (now >= current_pos_us) {
            g_tracker.play_start_time_us = now - current_pos_us;
        } else {
            g_tracker.play_start_time_us = 0;
        }
        g_tracker.pause_accumulated_us = 0;
    }

    pthread_mutex_unlock(&g_tracker.lock);
}

void progress_tracker_set_speed(float speed) {
    pthread_mutex_lock(&g_tracker.lock);

    if (speed > 0) {
        g_tracker.playback_speed = speed;
    }

    pthread_mutex_unlock(&g_tracker.lock);
}
