#ifndef DYN_PIPEWIRE_H
#define DYN_PIPEWIRE_H

#include <stdint.h>
#include <stddef.h>

/*
 * PipeWire / SPA 的最小 ABI 兼容层。
 *
 * 本工程通过 dlopen("libpipewire-0.3.so.0") 在运行时加载 PipeWire，
 * 构建时不依赖 pipewire/spa 头文件。因此这里手写的常量、结构体布局和
 * 函数签名必须与 PipeWire 1.x（本机验证版本 1.6.4）完全一致。
 *
 * 任何改动都必须用 scripts/test/pipewire-abi-check.sh 对照系统头文件
 * 做逐项校验，避免再次出现 pod 解析失败、回调错位、spa_hook 越界等问题。
 */

/* ---- opaque type forward declarations ---- */
struct pw_main_loop;
struct pw_loop;
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_properties;
struct pw_buffer;
struct pw_stream_events;
struct pw_stream_control;
struct spa_pod;
struct spa_pod_builder;
struct spa_command;
struct spa_audio_info_raw;

/* ---- SPA 基础类型编号（spa/utils/type.h） ----
 * SPA_TYPE_START = 0，因此 SPA_TYPE_None 从 1 开始。 */
#define SPA_TYPE_None        1
#define SPA_TYPE_Bool        2
#define SPA_TYPE_Id          3
#define SPA_TYPE_Int         4
#define SPA_TYPE_Long        5
#define SPA_TYPE_Float       6
#define SPA_TYPE_Double      7
#define SPA_TYPE_String      8
#define SPA_TYPE_Bytes       9
#define SPA_TYPE_Rectangle   10
#define SPA_TYPE_Fraction    11
#define SPA_TYPE_Bitmap      12
#define SPA_TYPE_Array       13
#define SPA_TYPE_Struct      14
#define SPA_TYPE_Object      15
#define SPA_TYPE_Sequence    16
#define SPA_TYPE_Pointer     17
#define SPA_TYPE_Fd          18
#define SPA_TYPE_Choice      19
#define SPA_TYPE_Pod         20

/* ---- SPA object 类型（spa/utils/type.h，SPA_TYPE_OBJECT_START = 0x40000） ---- */
#define SPA_TYPE_OBJECT_Format          0x40003
#define SPA_TYPE_OBJECT_ParamBuffers    0x40004
#define SPA_TYPE_OBJECT_ParamMeta       0x40005
#define SPA_TYPE_OBJECT_ParamIO         0x40006
#define SPA_TYPE_OBJECT_ParamProfile    0x40007
#define SPA_TYPE_OBJECT_ParamPortConfig 0x40008

/* ---- SPA param 类型（spa/param/param.h） ---- */
#define SPA_PARAM_Invalid          0
#define SPA_PARAM_PropInfo         1
#define SPA_PARAM_Props            2
#define SPA_PARAM_EnumFormat       3
#define SPA_PARAM_Format           4
#define SPA_PARAM_Buffers          5
#define SPA_PARAM_Meta             6
#define SPA_PARAM_IO               7
#define SPA_PARAM_EnumProfile      8
#define SPA_PARAM_Profile          9
#define SPA_PARAM_EnumPortConfig   10
#define SPA_PARAM_PortConfig       11

/* ---- SPA format 属性键（spa/param/format.h） ---- */
#define SPA_FORMAT_mediaType        0x1
#define SPA_FORMAT_mediaSubtype     0x2
#define SPA_FORMAT_AUDIO_format     0x10001
#define SPA_FORMAT_AUDIO_rate       0x10003
#define SPA_FORMAT_AUDIO_channels   0x10004

/* ---- SPA media type / subtype（spa/param/format.h） ---- */
#define SPA_MEDIA_TYPE_audio        1
#define SPA_MEDIA_SUBTYPE_raw       1

/* ---- SPA audio format（spa/param/audio/raw.h） ---- */
#define SPA_AUDIO_FORMAT_UNKNOWN    0
#define SPA_AUDIO_FORMAT_ENCODED    1
#define SPA_AUDIO_FORMAT_S32_LE     0x10b
#define SPA_AUDIO_FORMAT_S32        SPA_AUDIO_FORMAT_S32_LE

/* ---- SPA 控制属性（spa/param/props.h） ---- */
#define SPA_PROP_volume             0x10003

/* ---- PipeWire stream flags（pipewire/stream.h） ---- */
#define PW_STREAM_FLAG_NONE             0
#define PW_STREAM_FLAG_AUTOCONNECT      (1 << 0)
#define PW_STREAM_FLAG_INACTIVE         (1 << 1)
#define PW_STREAM_FLAG_MAP_BUFFERS      (1 << 2)
#define PW_STREAM_FLAG_DRIVER           (1 << 3)
#define PW_STREAM_FLAG_RT_PROCESS       (1 << 4)
#define PW_STREAM_FLAG_NO_CONVERT       (1 << 5)
#define PW_STREAM_FLAG_EXCLUSIVE        (1 << 6)
#define PW_STREAM_FLAG_DONT_RECONNECT   (1 << 7)
#define PW_STREAM_FLAG_ALLOC_BUFFERS    (1 << 8)
#define PW_STREAM_FLAG_TRIGGER          (1 << 9)
#define PW_STREAM_FLAG_ASYNC            (1 << 10)

/* ---- PipeWire 方向 / 任意目标节点 ---- */
#define PW_DIRECTION_OUTPUT  1
#define PW_ID_ANY            0xffffffffu

/* ---- PipeWire stream state ---- */
enum pw_stream_state {
    PW_STREAM_STATE_ERROR = -1,
    PW_STREAM_STATE_UNCONNECTED = 0,
    PW_STREAM_STATE_CONNECTING = 1,
    PW_STREAM_STATE_PAUSED = 2,
    PW_STREAM_STATE_STREAMING = 3,
};

/* ---- PipeWire properties 键 ---- */
#define PW_KEY_NODE_NAME        "node.name"
#define PW_KEY_NODE_NICK        "node.nick"
#define PW_KEY_NODE_RATE        "node.rate"
#define PW_KEY_NODE_LATENCY     "node.latency"
#define PW_KEY_MEDIA_TYPE       "media.type"
#define PW_KEY_MEDIA_CATEGORY   "media.category"
#define PW_KEY_MEDIA_ROLE       "media.role"
#define PW_KEY_APP_NAME         "application.name"
#define PW_KEY_APP_ID           "application.id"
#define PW_KEY_APP_ICON         "application.icon"

/* ================================================================== */
/*  SPA ABI 结构体                                                     */
/* ================================================================== */

/* spa/utils/list.h：两个指针，共 16 字节 */
struct spa_list {
    struct spa_list *prev;
    struct spa_list *next;
};

/* spa/utils/hook.h：funcs 在 16、data 在 24、removed 在 32、priv 在 40，
 * 总尺寸 48 字节。 */
struct spa_callbacks {
    const void *funcs;
    void *data;
};

struct spa_hook {
    struct spa_list link;
    struct spa_callbacks cb;
    void (*removed)(struct spa_hook *hook);
    void *priv;
};

/* spa/buffer/buffer.h：四字段，各 4 字节 */
struct spa_chunk {
    uint32_t offset;
    uint32_t size;
    int32_t  stride;
    int32_t  flags;
};

/* spa/buffer/buffer.h：type/flags/fd/mapoffset/maxsize/data/chunk，
 * 共 40 字节（fd 是 int64_t，8 字节对齐）。 */
struct spa_data {
    uint32_t type;
    uint32_t flags;
    int64_t  fd;
    uint32_t mapoffset;
    uint32_t maxsize;
    void    *data;
    struct spa_chunk *chunk;
};

/* spa/buffer/buffer.h：metas/datas 是真实指针字段，共 24 字节 */
struct spa_meta {
    uint32_t type;
    uint32_t size;
    void    *data;
};

struct spa_buffer {
    uint32_t n_metas;
    uint32_t n_datas;
    struct spa_meta *metas;
    struct spa_data *datas;
};

/* pipewire/stream.h：buffer/user_data/size/requested/time，共 40 字节 */
struct pw_buffer {
    struct spa_buffer *buffer;
    void     *user_data;
    uint64_t  size;
    uint64_t  requested;
    uint64_t  time;
};

/* ---- PipeWire stream events（1.x 布局，version 2） ----
 * 字段顺序不可调换：旧 0.3 顺序会把 process 解释成 control_info，
 * 导致 process 回调永远不触发。 */
struct pw_stream_events {
    uint32_t version;
    void (*destroy)(void *data);
    void (*state_changed)(void *data, enum pw_stream_state old,
                          enum pw_stream_state state, const char *error);
    void (*control_info)(void *data, uint32_t id,
                         const struct pw_stream_control *control);
    void (*io_changed)(void *data, uint32_t id, void *area, uint32_t size);
    void (*param_changed)(void *data, uint32_t id, const struct spa_pod *param);
    void (*add_buffer)(void *data, struct pw_buffer *buffer);
    void (*remove_buffer)(void *data, struct pw_buffer *buffer);
    void (*process)(void *data);
    void (*drained)(void *data);
    void (*command)(void *data, const struct spa_command *command);
    void (*trigger_done)(void *data);
};

/* ---- function pointer table ---- */
struct pipewire_funcs {
    /* init */
    void    (*init)(int *, char ***);
    void    (*deinit)(void);

    /* main loop */
    struct pw_main_loop *   (*main_loop_new)(const struct pw_properties *);
    void                    (*main_loop_destroy)(struct pw_main_loop *);
    void                    (*main_loop_run)(struct pw_main_loop *);
    void                    (*main_loop_quit)(struct pw_main_loop *);

    /* thread loop */
    struct pw_thread_loop * (*thread_loop_new)(const char *, const struct pw_properties *);
    void                    (*thread_loop_destroy)(struct pw_thread_loop *);
    int                     (*thread_loop_start)(struct pw_thread_loop *);
    void                    (*thread_loop_stop)(struct pw_thread_loop *);
    void                    (*thread_loop_lock)(struct pw_thread_loop *);
    void                    (*thread_loop_unlock)(struct pw_thread_loop *);
    void                    (*thread_loop_signal)(struct pw_thread_loop *, int);
    struct pw_loop *        (*thread_loop_get_loop)(struct pw_thread_loop *);

    /* context */
    struct pw_context *     (*context_new)(struct pw_loop *,
                              struct pw_properties *, size_t);
    void                    (*context_destroy)(struct pw_context *);
    struct pw_core *        (*context_connect)(struct pw_context *,
                              struct pw_properties *, size_t);

    /* core */
    int  (*core_disconnect)(struct pw_core *);

    /* properties */
    struct pw_properties *  (*properties_new)(const char *, ...);
    void                    (*properties_free)(struct pw_properties *);
    int                     (*properties_set)(struct pw_properties *,
                              const char *, const char *);
    const char *            (*properties_get)(struct pw_properties *,
                              const char *);

    /* stream */
    struct pw_stream *      (*stream_new)(struct pw_core *, const char *,
                              struct pw_properties *);
    struct pw_stream *      (*stream_new_simple)(struct pw_loop *,
                              const char *, struct pw_properties *);
    void                    (*stream_destroy)(struct pw_stream *);
    int                     (*stream_connect)(struct pw_stream *,
                              int, uint32_t, uint32_t,
                              const struct spa_pod **, uint32_t);
    int                     (*stream_disconnect)(struct pw_stream *);
    struct pw_buffer *      (*stream_dequeue_buffer)(struct pw_stream *);
    int                     (*stream_queue_buffer)(struct pw_stream *,
                              struct pw_buffer *);
    int                     (*stream_set_active)(struct pw_stream *, int);
    int                     (*stream_set_control)(struct pw_stream *,
                              uint32_t, uint32_t, float *, ...);
    int                     (*stream_flush)(struct pw_stream *, int);
    enum pw_stream_state    (*stream_get_state)(struct pw_stream *,
                              const char **);
    void                    (*stream_add_listener)(struct pw_stream *,
                              struct spa_hook *,
                              const struct pw_stream_events *, void *);
    int                     (*stream_set_error)(struct pw_stream *,
                              int, const char *, ...);
    int                     (*stream_update_params)(struct pw_stream *,
                              const struct spa_pod **, uint32_t);

    /* state */
    int   loaded;
    void *handle;
};

/* ---- public API ---- */
extern struct pipewire_funcs PW;

int  pipewire_load(void);
void pipewire_unload(void);

/* ---- SPA pod builder helper（不依赖 libspa） ---- */
/* 把 SPA_TYPE_OBJECT_Format / SPA_PARAM_EnumFormat 对象写入 dst（最多 256 字节），
 * 返回实际写入的字节数。 */
int build_audio_format_pod(void *dst, int sample_rate, int channels);

#endif /* DYN_PIPEWIRE_H */
