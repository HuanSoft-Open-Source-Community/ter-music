/**
 * @file equalizer.h
 * @brief 10-band ISO graphic equalizer — biquad IIR filter cascade
 *
 * Processes S32 interleaved PCM in-place using cascaded peaking EQ
 * biquad filters. When EQ is disabled the process function is a no-op.
 *
 * @author ter-music team
 * @date 2026-06-05
 */

#ifndef EQUALIZER_H
#define EQUALIZER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────────────────────────────────────────────── */

#define EQ_BAND_COUNT     10
#define EQ_GAIN_MIN      -12
#define EQ_GAIN_MAX       12
#define EQ_PREAMP_MIN    -12
#define EQ_PREAMP_MAX     12
#define EQ_PRESET_COUNT   10

/* ISO standard centre frequencies for a 10-band graphic equaliser (Hz) */
extern const int eq_band_frequencies[EQ_BAND_COUNT];

/* ── Lifecycle ──────────────────────────────────────────────────── */

/** Initialise EQ module (zero state, disabled). Called at startup. */
void eq_init(void);

/**
 * Process (or bypass) one chunk of interleaved S32 PCM samples in place.
 * Must be called from the playback thread only.
 *
 * @param samples      Pointer to interleaved S32 sample buffer (modified in place)
 * @param frame_count  Number of frames (sample pairs/groups) in this chunk
 * @param channels     Number of channels (1=mono, 2=stereo)
 * @param sample_rate  Sample rate in Hz (used for coefficient recalculation)
 */
void eq_process(int32_t *samples, int frame_count, int channels, int sample_rate);

/* ── Runtime control ────────────────────────────────────────────── */

/** Enable or disable the equaliser (0 = off, non-zero = on). */
void eq_set_enabled(int enabled);

/** Returns non-zero if the equaliser is currently enabled. */
int  eq_is_enabled(void);

/** Set pre-amp gain in dB (clamped to EQ_PREAMP_MIN … EQ_PREAMP_MAX). */
void eq_set_preamp(int gain_db);

/** Get current pre-amp gain in dB. */
int  eq_get_preamp(void);

/** Set one band's gain in dB (clamped to EQ_GAIN_MIN … EQ_GAIN_MAX). */
void eq_set_band_gain(int band, int gain_db);

/** Get one band's current gain in dB. */
int  eq_get_band_gain(int band);

/**
 * Bulk-set all band gains (e.g. after loading config).
 * @param gains  Array of EQ_BAND_COUNT gain values in dB.
 */
void eq_set_all_gains(const int gains[EQ_BAND_COUNT]);

/**
 * Apply a built-in EQ preset by index (0 … EQ_PRESET_COUNT-1).
 * This enables the EQ and loads the preset band gains.
 * The caller is responsible for saving config afterwards.
 *
 * @param idx  Preset index; clamped to valid range.
 */
void eq_apply_preset(int idx);

#ifdef __cplusplus
}
#endif

#endif /* EQUALIZER_H */
