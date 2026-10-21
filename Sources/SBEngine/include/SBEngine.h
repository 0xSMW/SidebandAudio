#ifndef SB_ENGINE_H
#define SB_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SB_ENGINE_MAX_EQ_BANDS 12
/* Original C DSP. Serial calls on a decode worker; never share an instance concurrently. */
typedef struct SBEngine SBEngine;
typedef struct { int type; float frequency, q, gain_db; } SBEngineEQBand;
typedef struct {
    float rate; /* 0.5...5; independent tempo, preserved pitch */
    int smart_speed, voice_boost, mono;
    float noise_reduction; /* 0...1; custom stationary-noise reducer */
    int normalization_mode; /* 0: follow Voice Boost; 1: off; 2: custom target */
    float target_lufs;
    int deesser; /* 0: off; 1: normal; 2: strong */
    size_t eq_count;
    SBEngineEQBand eq[SB_ENGINE_MAX_EQ_BANDS]; /* peak, LP, HP, BP, notch, LS, HS */
} SBEngineConfig;
/* Clock runs are caller-owned POD values: copying a run buffer preserves value semantics.
   Construction/query/pruning allocate nothing. The builder needs at most `frames` run slots. */
typedef struct {
    int64_t output, length;
    double source, source_step, saved, saved_step;
} SBTimelineRun;
size_t sb_timeline_build_runs(const double *source, const double *saved, size_t frames,
                              int64_t output_start, SBTimelineRun *runs, size_t capacity);
size_t sb_timeline_prune_count(const SBTimelineRun *runs, size_t count, int64_t last_played);
double sb_timeline_original(const SBTimelineRun *runs, size_t count, int64_t frame, int64_t base_original);
double sb_timeline_saved(const SBTimelineRun *runs, size_t count, int64_t frame, double saved_before);
/* Serial caller-owned timeline. Storage grows only for retained history plus playback ahead;
   append reuses its allocation and rejects allocation failure without changing the clock. */
typedef struct SBTimeline SBTimeline;
SBTimeline *sb_timeline_create(void);
SBTimeline *sb_timeline_clone(const SBTimeline *);
void sb_timeline_destroy(SBTimeline *);
int sb_timeline_append(SBTimeline *, const double *source, const double *saved, size_t frames);
void sb_timeline_reset(SBTimeline *, int64_t original_frame);
int64_t sb_timeline_played(SBTimeline *, int64_t output_frame);
int64_t sb_timeline_output_end(const SBTimeline *);
int64_t sb_timeline_last_played(const SBTimeline *);
int64_t sb_timeline_base_original(const SBTimeline *);
double sb_timeline_saved_before(const SBTimeline *);
const SBTimelineRun *sb_timeline_runs(const SBTimeline *, size_t *count);
double sb_timeline_original_at(const SBTimeline *, int64_t frame);
double sb_timeline_saved_at(const SBTimeline *, int64_t frame);
SBEngineConfig sb_engine_default_config(void);
void sb_engine_config_set_band(SBEngineConfig *, size_t index, int type, float frequency, float q, float gain_db);
/* Pure custom-band response only, sharing the audio path's coefficients/clamps.
   No engine instance, allocation or processing-state mutation. Invalid bands are
   skipped as in processing; NULL/count > 12, sample rate outside 8...192 kHz or
   nonfinite/out-of-range query (require 0 < frequency < Nyquist) returns NAN.
   Exact transfer zeros may return -INFINITY; display clipping belongs to the UI. */
double sb_engine_custom_eq_gain_db(const SBEngineConfig *config, double sample_rate, double frequency);
SBEngine *sb_engine_create(double sample_rate, unsigned channels, size_t max_input_frames);
void sb_engine_destroy(SBEngine *);
int sb_engine_configure(SBEngine *, const SBEngineConfig *);
void sb_engine_reset(SBEngine *, int64_t original_frame);
/* Optional warm start, after reset/configure and before input. Invalid seeds are ignored. */
void sb_engine_seed_loudness(SBEngine *, float loudness_lufs);
/* Returns accepted input frames; zero means drain output before retrying. Never discards unaccepted input. */
size_t sb_engine_push(SBEngine *, const float *left, const float *right, size_t frames);
/* No further input until reset. Read repeatedly to drain all source frames, including delayed tails. */
void sb_engine_finish(SBEngine *);
size_t sb_engine_read(SBEngine *, float *left, float *right, double *source_frames,
                      double *silence_saved_frames, size_t capacity);
int sb_engine_is_drained(const SBEngine *);
float sb_engine_loudness(const SBEngine *); /* -160 until valid */
size_t sb_engine_latency(const SBEngine *); /* DSP latency in source frames; WSOLA also buffers grains */
#ifdef __cplusplus
}
#endif
#endif
