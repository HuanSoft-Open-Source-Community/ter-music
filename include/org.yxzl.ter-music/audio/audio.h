#ifndef AUDIO_H
#define AUDIO_H

#include "types.h"

/* ── Extern globals ── */
extern PlayState g_play_state;
extern int g_current_play_index;
extern PlayMode g_play_mode;
extern pthread_t g_play_thread;
extern int g_play_thread_running;
extern char g_default_audio_device[128];
extern int g_active_backend;
extern float g_playback_speed;

/* 播放线程维护（实现位于 audio/audio.c；此前误声明在 ui/ui.h） */
void reap_finished_playback_thread(void);
void process_pending_playback_action(void);
extern int g_audio_sample_rate;
extern int g_audio_bit_rate;
extern int g_audio_bit_depth;
extern char g_audio_codec_name[32];

/* ── Function prototypes ── */
void toggle_playback_speed(void);
void apply_playback_speed_change(void);
void init_ffmpeg();
void init_audio_device();
/* 本进程是否初始化过音频引擎：只有 core 宿主（local TUI / daemon）为 1。
 * 远端前端用它拒绝任何本地起播与本地音频收尾。 */
int audio_engine_initialized(void);
/* 停止并销毁当前音频后端的流（PulseAudio / ALSA / PipeWire）。
 * 进程退出时必须在释放自己的 D-Bus 主总线名之前调用：否则新的前端会在
 * “名字已经消失、旧进程仍在出声”的窗口里另起一个核心，两路播放叠在一起。 */
void audio_backend_shutdown(void);
int audio_backend_is_available(int backend);
void play_audio(int index);
void pause_audio();
void resume_audio();
void stop_audio();
void wait_for_playback_thread_shutdown(void);
void prev_track();
void next_track();
void cycle_play_mode(void);
PlayMode get_play_mode(void);
void set_play_mode(PlayMode mode);
const char *get_play_mode_str(void);
void cleanup();
void seek_audio(double position);
int get_and_clear_initial_seek_position(void);
/* 设置“播放起始跳转位置”（秒）：用于恢复上次播放进度，由播放线程消费后清零 */
void audio_set_initial_seek_position(int seconds);
int get_volume_percent(void);
void set_volume_percent(int volume);
void adjust_volume(int delta);
void persist_playback_session_state(void);

/* 当前可听播放位置 / 总时长（秒）。无播放时返回 0。
 * 位置已扣除后端尚未消费的缓冲（PipeWire 的环可提前约 3 秒），因此
 * 断点恢复/MPRIS/进度条都以用户实际听到的位置为准。
 * 以访问器形式暴露，避免非 TUI 模块（info/cli）为读取两个全局量
 * 而包含 ui/ui.h（该头文件引入 ncurses）。 */
int audio_get_position_seconds(void);
int audio_get_duration_seconds(void);

/* 当前可听播放位置（毫秒）。语义与秒版一致（同样扣除后端未消费缓冲），
 * 供歌词逐字高亮这类需要亚秒精度的读取方使用；无播放时返回 0。 */
int64_t audio_get_position_ms(void);

/* 正在出声的曲目路径（g_play_mutex 下的拷贝）。
 * @return 1 = 有当前曲目且 out 已填充；0 = 无。 */
int audio_current_track_path(char *out, size_t out_size);

/* 队列被编辑/重排后调用：若正在出声的路径仍在 bq 中，把
 * g_current_play_index 同步到它的新位置；若已不在队列，置 -1。
 * 前端 metadata 仍用当前路径/pinned 条目上报，不依赖这个下标。 */
void audio_sync_queue_position(void);

#endif
