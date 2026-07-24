#ifndef AUDIO_INTERNAL_H
#define AUDIO_INTERNAL_H

#include "types.h"
#include "audio/audio.h"
#include <pthread.h>

/* ================================================================
 * 内部状态：在 audio.c 和各后端 / 拆分模块之间共享
 * ================================================================ */

/* ---- 通用互斥锁 / 条件变量（定义在 audio.c） ---- */
extern pthread_mutex_t audio_mutex;
extern pthread_mutex_t g_volume_mutex;
extern pthread_cond_t  g_play_control_cond;

/* ---- 通用音量/输出状态（定义在 audio.c） ---- */
extern int       g_volume_percent;
extern int       g_pending_volume_sync;
extern int       g_output_sample_rate;
extern int       g_output_channels;

/* ---- Speed state（定义在 audio.c） ---- */
extern float g_speed_ratios[];
extern int   g_speed_index;
extern int   g_speed_count;

/* ---- 播放状态（定义在 audio.c） ---- */
extern PlayState g_play_state;
extern int       g_current_play_index;
extern PlayMode  g_play_mode;
extern PlayQueue g_play_queue;
extern pthread_t g_play_thread;
extern int       g_play_thread_running;
extern pthread_mutex_t g_play_mutex;
extern pthread_mutex_t g_seek_mutex;
extern int g_play_thread_active;
extern int g_play_thread_finished;
extern int g_pending_playback_index;
extern int g_seek_request;
extern int g_seek_position;
extern int g_initial_seek_position;
extern int g_cue_offset;
extern int g_current_position;
extern int g_total_duration;
extern int g_audio_sample_rate;
extern int g_audio_bit_rate;
extern int g_audio_bit_depth;
extern char g_audio_codec_name[32];
extern char g_cached_audio_path[];
extern char g_cached_lyrics_path[];

/* ---- PulseAudio 后端状态（定义在 audio/backend/pulse.c） ---- */
#include "audio/backend/pulse.h"
#define PULSE_SONAME "libpulse.so.0"

extern int  pulse_loaded;
extern pa_mainloop  *pa_ml;
extern pa_context   *pa_ctx;
extern pa_stream    *pa_s;
extern pa_sample_spec  pa_ss;
extern int  pa_connected;

int  pulse_load(void);
void pulse_unload(void);
int  pulse_ensure_connected(void);

/* ---- ALSA 后端状态（定义在 audio/backend/alsa.c） ---- */
#include "audio/backend/alsa.h"
#define ALSA_SONAME "libasound.so.2"

extern int    alsa_loaded;
extern snd_pcm_t *alsa_pcm;
extern int    alsa_ready;

int  alsa_load(void);
void alsa_unload(void);

/* ---- PipeWire 后端状态（定义在 audio/backend/pipewire.c） ---- */
#include "audio/backend/pipewire.h"
#define PIPEWIRE_SONAME "libpipewire-0.3.so.0"

extern int    pipewire_loaded;
extern struct pw_thread_loop *pw_loop;
extern struct pw_main_loop  *pw_mainloop;
extern struct pw_context    *pw_ctx;
extern struct pw_core       *pw_core;
extern struct pw_stream     *pw_s;
extern int    pw_stream_ready;
extern int    pw_stream_connecting;
extern int    pw_channels;
extern int    pw_sample_rate;
extern int    pw_write_underrun;
extern volatile int pw_stream_destroying;
extern pthread_mutex_t pw_state_mutex;

int  pipewire_load(void);
void pipewire_unload(void);
int  build_audio_format_pod(void *dst, int sample_rate, int channels);
int  pw_probe_backend(void);
int  pw_prepare_stream(int sample_rate, int channels);
void pw_cleanup_stream_locked(void);
void pw_cleanup_stream(void);
void pw_flush_stream(void);
int  pw_write_samples(const int32_t *samples, int frame_count);
void pw_pause_stream(void);
void pw_resume_stream(void);
void pw_sync_volume(int volume);

/* ---- Segment buffer（用于分段播放） ---- */
#include "audio/segment_buffer.h"
extern PreloadData g_preload_data;

/* ---- FFmpeg type forward declarations（用于 atempo / codec 接口） ---- */
struct AVCodecContext;
struct AVFrame;

/* ---- 通用辅助函数（定义在 audio.c） ---- */
int  get_configured_latency_ms(void);
void apply_volume_to_samples(int32_t *samples, int sample_count);

/* ---- Atempo filter（定义在 atempo.c） ---- */
int  init_atempo_filter(const struct AVCodecContext *codec_ctx, float speed);
void cleanup_atempo_filter(void);
int  atempo_send_frame(struct AVFrame *frame);
int  atempo_receive_frame(struct AVFrame *frame);
int  atempo_flush(void);
int  atempo_is_active(void);
float atempo_get_speed(void);
int  atempo_get_input_sample_rate(void);

/* ---- Equaliser（定义在 equalizer.c） ---- */
#include "audio/equalizer.h"
void init_equalizer_from_config(void);

/* ---- Audio backend stream ops（定义在 backend_ops.c） ---- */
int  audio_backend_prepare_stream(int sample_rate, int channels);
void audio_backend_cleanup_stream(void);
void audio_backend_flush_stream(void);
int  audio_backend_write_samples(const int32_t *samples, int frame_count);
void audio_backend_pause_stream(void);
void audio_backend_resume_stream(void);
void audio_backend_sync_volume(int force);

/* ---- Playback thread（定义在 playback_thread.c） ---- */
void *play_audio_thread(void *arg);

/* ---- Codec helper（定义在 playback_thread.c） ---- */
int codec_channel_count(const struct AVCodecContext *codec_ctx);

#endif /* AUDIO_INTERNAL_H */
