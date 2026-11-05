#ifndef DYN_PULSE_H
#define DYN_PULSE_H

#include <stdint.h>
#include <stddef.h>

/* ---- opaque type forward declarations ---- */
typedef struct pa_mainloop pa_mainloop;
typedef struct pa_mainloop_api pa_mainloop_api;
typedef struct pa_context pa_context;
typedef struct pa_stream pa_stream;
typedef struct pa_operation pa_operation;

/* ---- enum constants (from pulseaudio public ABI) ---- */
/* pa_context_state */
#define PA_CONTEXT_UNCONNECTED   0
#define PA_CONTEXT_CONNECTING    1
#define PA_CONTEXT_AUTHORIZING   2
#define PA_CONTEXT_SETTING_NAME  3
#define PA_CONTEXT_READY         4
#define PA_CONTEXT_FAILED        5
#define PA_CONTEXT_TERMINATED    6

/* pa_stream_state */
#define PA_STREAM_UNCONNECTED   0
#define PA_STREAM_CREATING      1
#define PA_STREAM_READY         2
#define PA_STREAM_FAILED        3
#define PA_STREAM_TERMINATED    4

/* pa_operation_state */
#define PA_OPERATION_RUNNING    0
#define PA_OPERATION_DONE       1
#define PA_OPERATION_CANCELLED  2

/* pa_sample_format */
#define PA_SAMPLE_U8            0
#define PA_SAMPLE_S16LE         3
#define PA_SAMPLE_S16BE         4
#define PA_SAMPLE_FLOAT32LE     5
#define PA_SAMPLE_FLOAT32BE     6
#define PA_SAMPLE_S32LE         7

/* pa_stream_flags */
#define PA_STREAM_NOFLAGS          0x0000U
#define PA_STREAM_START_CORKED     0x0001U
#define PA_STREAM_ADJUST_LATENCY   0x2000U

/* pa_context_flags */
#define PA_CONTEXT_NOFLAGS         0U

/* pa_seek_mode */
#define PA_SEEK_RELATIVE           0

/* misc */
#define PA_CHANNELS_MAX            32U
#define PA_INVALID_INDEX           ((uint32_t)-1)

/* ---- struct definitions (ABI-compatible with libpulse) ---- */
typedef struct {
    int32_t  format;        /* pa_sample_format_t = int */
    uint32_t rate;
    uint8_t  channels;
    /* 3 bytes padding */
} pa_sample_spec;

typedef struct {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
} pa_buffer_attr;

typedef uint32_t pa_volume_t;

typedef struct {
    uint8_t     channels;
    /* 3 bytes padding */
    pa_volume_t values[PA_CHANNELS_MAX];
} pa_cvolume;

/* ---- function pointer table ---- */
struct pulseaudio_funcs {
    /* mainloop */
    pa_mainloop *    (*mainloop_new)(void);
    pa_mainloop_api *(*mainloop_get_api)(pa_mainloop *);
    void             (*mainloop_free)(pa_mainloop *);
    int              (*mainloop_iterate)(pa_mainloop *, int, int *);

    /* context */
    pa_context * (*context_new)(pa_mainloop_api *, const char *);
    int          (*context_connect)(pa_context *, const char *, unsigned, void *);
    int          (*context_get_state)(pa_context *);
    void         (*context_disconnect)(pa_context *);
    void         (*context_unref)(pa_context *);
    pa_operation *(*context_set_sink_input_volume)(pa_context *, uint32_t,
                                                    const pa_cvolume *, void *, void *);

    /* stream */
    pa_stream *  (*stream_new)(pa_context *, const char *,
                                const pa_sample_spec *, void *);
    int          (*stream_connect_playback)(pa_stream *, const char *,
                                             const pa_buffer_attr *, unsigned,
                                             const pa_cvolume *, pa_stream *);
    int          (*stream_get_state)(pa_stream *);
    uint32_t     (*stream_get_index)(pa_stream *);
    void         (*stream_disconnect)(pa_stream *);
    void         (*stream_unref)(pa_stream *);
    size_t       (*stream_writable_size)(pa_stream *);
    int          (*stream_write)(pa_stream *, const void *, size_t,
                                  void *, int64_t, int);
    pa_operation *(*stream_flush)(pa_stream *, void *, void *);
    int          (*stream_is_corked)(pa_stream *);
    pa_operation *(*stream_cork)(pa_stream *, int, void *, void *);

    /* utility */
    size_t (*usec_to_bytes)(uint64_t, const pa_sample_spec *);

    /* operation */
    int  (*operation_get_state)(pa_operation *);
    void (*operation_unref)(pa_operation *);

    /* volume */
    pa_cvolume *(*cvolume_set)(pa_cvolume *, unsigned, pa_volume_t);
    pa_volume_t (*sw_volume_from_linear)(double);

    /* state */
    int   loaded;
    void *handle;
};

/* ---- public API ---- */
extern struct pulseaudio_funcs P;

int  pulse_load(void);
void pulse_unload(void);

#endif /* DYN_PULSE_H */
