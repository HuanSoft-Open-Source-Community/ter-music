#ifndef DYN_ALSA_H
#define DYN_ALSA_H

/* ---- opaque type forward declarations ---- */
typedef struct _snd_pcm snd_pcm_t;

/* ---- types ---- */
typedef long          snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

/* ---- enum constants (from ALSA public ABI, /usr/include/alsa/pcm.h) ---- */
#define SND_PCM_STREAM_PLAYBACK       0

#define SND_PCM_ACCESS_MMAP_INTERLEAVED     0
#define SND_PCM_ACCESS_MMAP_NONINTERLEAVED  1
#define SND_PCM_ACCESS_MMAP_COMPLEX         2
#define SND_PCM_ACCESS_RW_INTERLEAVED       3

#define SND_PCM_FORMAT_S8      0
#define SND_PCM_FORMAT_U8      1
#define SND_PCM_FORMAT_S16_LE  2
#define SND_PCM_FORMAT_S16_BE  3
#define SND_PCM_FORMAT_U16_LE  4
#define SND_PCM_FORMAT_U16_BE  5
#define SND_PCM_FORMAT_S24_LE  6
#define SND_PCM_FORMAT_S24_BE  7
#define SND_PCM_FORMAT_U24_LE  8
#define SND_PCM_FORMAT_U24_BE  9
#define SND_PCM_FORMAT_S32_LE  10
#define SND_PCM_FORMAT_S32_BE  11

/* ---- function pointer table ---- */
struct alsa_funcs {
    int                (*pcm_open)(snd_pcm_t **, const char *, int, int);
    int                (*pcm_set_params)(snd_pcm_t *, int, int, unsigned,
                                          unsigned, int, unsigned);
    snd_pcm_sframes_t  (*pcm_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
    int                (*pcm_wait)(snd_pcm_t *, int);
    int                (*pcm_prepare)(snd_pcm_t *);
    int                (*pcm_drop)(snd_pcm_t *);
    int                (*pcm_close)(snd_pcm_t *);
    int                (*pcm_pause)(snd_pcm_t *, int);
    const char *       (*strerror)(int);

    int   loaded;
    void *handle;
};

/* ---- public API ---- */
extern struct alsa_funcs A;

int  alsa_load(void);
void alsa_unload(void);

#endif /* DYN_ALSA_H */
