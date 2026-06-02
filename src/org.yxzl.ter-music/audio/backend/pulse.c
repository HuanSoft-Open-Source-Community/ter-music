/**
 * @file pulse.c
 * @brief PulseAudio backend — runtime dlopen loading and stream connection
 *
 * Provides the dlopen/dlsym wrapper for libpulse and the connection
 * establishment logic used by audio.c via the function pointer table P.
 *
 * @author ter-music team
 */

#include "audio/audio_internal.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  PulseAudio Opaque Pointer Definitions                             */
/*  Matches the forward declarations in audio/backend/pulse.h         */
/* ================================================================== */

// These opaque pointers are defined in pulse.c and extern-declared in
// audio_internal.h so that audio.c dispatch functions can use them.
pa_mainloop *pa_ml = NULL;
pa_context  *pa_ctx = NULL;
pa_stream   *pa_s   = NULL;
pa_sample_spec pa_ss;
int pa_connected = 0;
int pulse_loaded = 0;

/* ================================================================== */
/*  dlopen / dlsym 宏                                                 */
/* ================================================================== */

#define PULSE_LOAD(name) do { \
    *(void **)(&P.name) = dlsym(P.handle, "pa_" #name); \
    if (!P.name) { \
        fprintf(stderr, "dlsym(pa_" #name ") failed: %s\n", dlerror()); \
        dlclose(P.handle); \
        P.handle = NULL; \
        return -1; \
    } \
} while(0)

/* ================================================================== */
/*  Load / Unload                                                     */
/* ================================================================== */

int pulse_load(void) {
    if (P.loaded) return 0;
    P.handle = dlopen(PULSE_SONAME, RTLD_LAZY | RTLD_LOCAL);
    if (!P.handle) return -1;

    PULSE_LOAD(mainloop_new);
    PULSE_LOAD(mainloop_get_api);
    PULSE_LOAD(mainloop_free);
    PULSE_LOAD(mainloop_iterate);

    PULSE_LOAD(context_new);
    PULSE_LOAD(context_connect);
    PULSE_LOAD(context_get_state);
    PULSE_LOAD(context_disconnect);
    PULSE_LOAD(context_unref);
    PULSE_LOAD(context_set_sink_input_volume);

    PULSE_LOAD(stream_new);
    PULSE_LOAD(stream_connect_playback);
    PULSE_LOAD(stream_get_state);
    PULSE_LOAD(stream_get_index);
    PULSE_LOAD(stream_disconnect);
    PULSE_LOAD(stream_unref);
    PULSE_LOAD(stream_writable_size);
    PULSE_LOAD(stream_write);
    PULSE_LOAD(stream_flush);
    PULSE_LOAD(stream_is_corked);
    PULSE_LOAD(stream_cork);

    PULSE_LOAD(usec_to_bytes);

    PULSE_LOAD(operation_get_state);
    PULSE_LOAD(operation_unref);

    PULSE_LOAD(cvolume_set);
    PULSE_LOAD(sw_volume_from_linear);

    P.loaded = 1;
    return 0;
}

void pulse_unload(void) {
    if (P.handle) {
        dlclose(P.handle);
        P.handle = NULL;
    }
    P.loaded = 0;
}

/* ================================================================== */
/*  Connection                                                        */
/* ================================================================== */

int pulse_ensure_connected(void) {
    if (pa_connected && pa_ml && pa_ctx) return 0;  /* already connected */
    if (!pulse_loaded) return -1;

    if (!pa_ml) {
        pa_ml = P.mainloop_new();
        if (!pa_ml) return -1;
    }
    if (!pa_ctx) {
        pa_ctx = P.context_new(P.mainloop_get_api(pa_ml), APP_NAME);
        if (!pa_ctx) { P.mainloop_free(pa_ml); pa_ml = NULL; return -1; }
        P.context_connect(pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
    }
    /* Wait for connection ready */
    int timeout = 500;
    while (timeout > 0) {
        int state = P.context_get_state(pa_ctx);
        if (state == PA_CONTEXT_READY) { pa_connected = 1; return 0; }
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) break;
        P.mainloop_iterate(pa_ml, 1, NULL);
        timeout--;
    }
    /* Connection failed, clean up */
    if (pa_ctx) { P.context_disconnect(pa_ctx); P.context_unref(pa_ctx); pa_ctx = NULL; }
    if (pa_ml) { P.mainloop_free(pa_ml); pa_ml = NULL; }
    pa_connected = 0;
    return -1;
}
