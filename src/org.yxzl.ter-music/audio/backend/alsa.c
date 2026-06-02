/**
 * @file alsa.c
 * @brief ALSA backend — runtime dlopen loading only
 *
 * Provides the dlopen/dlsym wrapper for libasound.  Actual PCM
 * read/write is done through the function-pointer table A,
 * used directly in audio.c dispatch functions.
 *
 * @author ter-music team
 */

#include "audio/audio_internal.h"
#include <dlfcn.h>
#include <stdio.h>

/* ================================================================== */
/*  ALSA Opaque Pointer Definitions                                   */
/* ================================================================== */

snd_pcm_t *alsa_pcm = NULL;
int alsa_ready = 0;
int alsa_loaded = 0;

/* ================================================================== */
/*  dlopen / dlsym 宏                                                 */
/* ================================================================== */

#define ALSA_LOAD(name) do { \
    *(void **)(&A.name) = dlsym(A.handle, "snd_" #name); \
    if (!A.name) { \
        fprintf(stderr, "dlsym(snd_" #name ") failed: %s\n", dlerror()); \
        dlclose(A.handle); \
        A.handle = NULL; \
        return -1; \
    } \
} while(0)

/* ================================================================== */
/*  Load / Unload                                                     */
/* ================================================================== */

int alsa_load(void) {
    if (A.loaded) return 0;
    A.handle = dlopen(ALSA_SONAME, RTLD_LAZY | RTLD_LOCAL);
    if (!A.handle) return -1;

    ALSA_LOAD(pcm_open);
    ALSA_LOAD(pcm_set_params);
    ALSA_LOAD(pcm_writei);
    ALSA_LOAD(pcm_wait);
    ALSA_LOAD(pcm_prepare);
    ALSA_LOAD(pcm_drop);
    ALSA_LOAD(pcm_close);
    ALSA_LOAD(pcm_pause);

    A.loaded = 1;
    return 0;
}

void alsa_unload(void) {
    if (A.handle) {
        dlclose(A.handle);
        A.handle = NULL;
    }
    A.loaded = 0;
}
