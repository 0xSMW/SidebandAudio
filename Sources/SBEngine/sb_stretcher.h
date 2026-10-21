#ifndef SB_STRETCHER_H
#define SB_STRETCHER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sb_stretcher sb_stretcher;

/* Portable planar-float WSOLA, with one shared alignment for all channels.
 * sample_rate: finite 8000...384000 Hz; channels: 1 or 2.
 * max_buffer_frames is a fixed input-ring capacity, at least 4 * window_frames,
 * where window_frames = align_up_4(round(1024 * sample_rate / 44100)).
 * No allocation occurs after create. Returns NULL for invalid input/allocation
 * failure. The initial rate is 1.0. One thread must own each instance. */
sb_stretcher *sb_stretcher_create(double sample_rate, unsigned channels,
                                 size_t max_buffer_frames);
void sb_stretcher_destroy(sb_stretcher *stretcher);

/* Discards audio, metadata and EOF state; retains the configured rate. */
void sb_stretcher_reset(sb_stretcher *stretcher);

/* Accepts finite rates in [0.5, 5.0], returning 1 on success, 0 otherwise.
 * Changes apply to the next synthesis hop; already staged output is preserved.
 * Rates cannot change after EOF until reset. */
int sb_stretcher_set_rate(sb_stretcher *stretcher, double rate);

/* Accepts as many frames as fit and returns that number (possibly zero).
 * Caller retries the remainder after pulling output. Audio pointers for the
 * configured channels are required. Nonfinite samples become zero.
 * Optional metadata is in original-media FRAMES and cumulative Smart Speed
 * SAVED FRAMES. Null positions use accepted-input frame indices; null savings
 * hold the last value. Nonfinite/decreasing metadata is repaired monotonically.
 * Metadata follows the nominal tempo clock, never the correlation-selected lag.
 * No further push is accepted once pull(..., finished=1) has latched EOF. */
size_t sb_stretcher_push(sb_stretcher *stretcher,
                         const float *const input[2],
                         const double *source_positions,
                         const double *silence_saved, size_t frames);

/* Returns up to capacity frames into required configured-channel audio outputs.
 * Metadata outputs are optional. finished=1 permanently signals no future input
 * until reset; call repeatedly to drain, even if an earlier pull returned zero.
 * At a constant rate the total output is ceil(accepted_input_frames / rate),
 * within floating-point rounding at an exact integer boundary. EOF padding does
 * not add output duration. A stream that stays at unity is sample-exact and has
 * no WSOLA lookahead. Rate transitions occur at hop boundaries.
 * Latching EOF with capacity=0 and output=NULL is supported. */
size_t sb_stretcher_pull(sb_stretcher *stretcher, float *const output[2],
                         double *source_positions, double *silence_saved,
                         size_t capacity, int finished);

/* Accepted input not yet covered by emitted output's nominal clock, rounded up;
 * excludes retained alignment history. Returns zero after the EOF drain. */
size_t sb_stretcher_buffered_frames(const sb_stretcher *stretcher);
/* Input-ring slots available now; push also retires history when possible. */
size_t sb_stretcher_writable_frames(const sb_stretcher *stretcher);
int sb_stretcher_is_drained(const sb_stretcher *stretcher);

#ifdef __cplusplus
}
#endif
#endif
