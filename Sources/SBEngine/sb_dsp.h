#ifndef SB_DSP_H
#define SB_DSP_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SB_DSP_MAX_EQ_BANDS 12
/* All frequencies are Hz, gains dB. Types: peak, LP, HP, BP, notch, LS, HS. */
typedef struct { int type; float frequency, q, gain_db; } sb_dsp_eq_band;
typedef struct {
    int voice_boost, mono;
    float noise_reduction; /* 0...1; custom spectral reducer, not Apple voice isolation */
    int normalization_mode; /* 0: -14 with Voice Boost, otherwise unity; 1: off; 2: custom */
    float target_lufs; /* custom target clamped to -20...-6 */
    int deesser; /* 0: off, 1: normal (-6.75 / 10 dB), 2: strong (-9 / 13 dB) */
    size_t eq_count;
    sb_dsp_eq_band eq[SB_DSP_MAX_EQ_BANDS];
} sb_dsp_config;
typedef struct sb_dsp sb_dsp;
/* Supports 8...192 kHz, one or two channels; NULL on invalid bounds/allocation.
   A zero-initialized configuration disables optional effects. */
sb_dsp *sb_dsp_create(double sample_rate, unsigned channels, size_t max_frames);
void sb_dsp_destroy(sb_dsp *dsp);
/* Configure/reset/process must be serialized. Configuration changes crossfade EQ. */
void sb_dsp_configure(sb_dsp *dsp, const sb_dsp_config *config);
/* Pure custom-EQ transfer response, using the processing coefficients/clamps.
   Excludes Voice Boost and all other stages; no allocation or DSP state changes.
   Invalid bands are skipped as in configure. NULL, count > 12, invalid sample
   rate (outside 8...192 kHz) or query frequency (not 0 < f < Nyquist) returns NAN.
   An exact transfer-function zero may return -INFINITY; no display clamp applies. */
double sb_dsp_custom_eq_gain_db(const sb_dsp_config *config, double sample_rate, double frequency);
void sb_dsp_reset(sb_dsp *dsp);
/* Call after reset/configure and before PCM. Seeds accepted loudness history and
   applies the configured normalization gain immediately. Invalid/nonfinite levels
   outside -70...0 are ignored; normalization off always retains unity gain. */
void sb_dsp_seed_loudness(sb_dsp *dsp, float level);
/* Planar channels 1 or 2, finite values sanitized. Rejects n > max_frames,
   invalid pointers or cap < n WITHOUT accepting input (returns 0). Otherwise
   consumes all n, returns <= n. Output may alias input. No allocation here.
   The linked -0.7 dBFS sample limiter always remains active. PCM below its
   ceiling is bit-exact with all optional effects disabled after reset.
   Fixed latency is retained with effects disabled. Leading padding is never
   returned: every produced frame corresponds to accepted source PCM. */
size_t sb_dsp_process(sb_dsp *dsp, const float *const input[2], size_t n,
                      float *const output[2], size_t cap);
/* Drain delayed SOURCE frames only; repeat until zero. No extra synthetic tail.
   After flushing, reset before accepting another stream. cap may be any size. */
size_t sb_dsp_flush(sb_dsp *dsp, float *const output[2], size_t cap);
/* Constant 1024 + ceil(0.010 * sample_rate) source frames. */
size_t sb_dsp_latency(const sb_dsp *dsp);
float sb_dsp_loudness_db(const sb_dsp *dsp); /* -160 until first valid window */
float sb_dsp_smart_threshold(const sb_dsp *dsp); /* processed-domain amplitude */
#ifdef __cplusplus
}
#endif
#endif
