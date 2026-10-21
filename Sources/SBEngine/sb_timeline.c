#include "include/SBEngine.h"
#include <math.h>
#include <limits.h>

size_t sb_timeline_build_runs(const double *source, const double *saved, size_t n,
                              int64_t output, SBTimelineRun *runs, size_t capacity) {
    if (!n || !source || !saved || !runs || capacity < n ||
        n > INT64_MAX || output < 0 || output > INT64_MAX - (int64_t)n) return 0;
    size_t count = 0, start = 0;
    while (start < n) {
        double source_step = start + 1 < n ? source[start + 1] - source[start] : 0;
        double saved_step = start + 1 < n ? saved[start + 1] - saved[start] : 0;
        size_t end = start + 1;
        while (end + 1 < n && fabs((source[end + 1] - source[end]) - source_step) < .00001 &&
               fabs((saved[end + 1] - saved[end]) - saved_step) < .00001) ++end;
        /* A one-frame jump must never be interpolated into the preceding run. */
        size_t length = end - start + (end == n - 1 ? 1 : 0);
        if (!length) length = 1;
        runs[count++] = (SBTimelineRun){ output + (int64_t)start, (int64_t)length,
            source[start], length > 1 ? source_step : 0,
            saved[start], length > 1 ? saved_step : 0 };
        start += length;
    }
    return count;
}
size_t sb_timeline_prune_count(const SBTimelineRun *runs, size_t count, int64_t played) {
    int64_t cutoff = played > 8192 ? played - 8192 : 0;
    if (!runs || !count) return 0;
    /* Find the first run whose inclusive end still touches the retained history. */
    size_t low = 0, high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (runs[mid].output + runs[mid].length < cutoff) low = mid + 1;
        else high = mid;
    }
    /* Match the wrapper's behavior if the supplied played position is beyond all runs. */
    return low < count ? low : 0;
}
static const SBTimelineRun *run_at(const SBTimelineRun *runs, size_t count, int64_t frame) {
    if (!runs || !count) return NULL;
    size_t low = 0, high = count - 1;
    while (low < high) {
        size_t mid = low + (high - low + 1) / 2;
        if (runs[mid].output <= frame) low = mid;
        else high = mid - 1;
    }
    return runs + low;
}
static double delta_at(const SBTimelineRun *run, int64_t frame, int64_t limit) {
    if (frame <= run->output) return 0;
    /* Unsigned subtraction avoids overflow for arbitrary query positions. */
    uint64_t delta = (uint64_t)frame - (uint64_t)run->output;
    return delta < (uint64_t)limit ? (double)delta : (double)limit;
}
double sb_timeline_original(const SBTimelineRun *runs, size_t count, int64_t frame, int64_t base) {
    const SBTimelineRun *run = run_at(runs, count, frame);
    return run ? run->source + delta_at(run, frame, run->length) * run->source_step : (double)base;
}
double sb_timeline_saved(const SBTimelineRun *runs, size_t count, int64_t frame, double before) {
    const SBTimelineRun *run = run_at(runs, count, frame);
    if (!run) return before;
    double saved = run->saved + delta_at(run, frame, run->length > 0 ? run->length - 1 : 0) * run->saved_step;
    return before + fmax(0, saved);
}

#include <stdlib.h>
#include <string.h>
struct SBTimeline {
    SBTimelineRun *runs;
    size_t count, capacity;
    int64_t output_end, last_played, base_original;
    double saved_before;
};
SBTimeline *sb_timeline_create(void) { return calloc(1, sizeof(SBTimeline)); }
void sb_timeline_destroy(SBTimeline *t) { if (t) { free(t->runs); free(t); } }
SBTimeline *sb_timeline_clone(const SBTimeline *t) {
    if (!t) return NULL;
    SBTimeline *copy = sb_timeline_create();
    if (!copy) return NULL;
    *copy = *t;
    copy->runs = NULL;
    copy->capacity = t->count;
    if (t->count) {
        copy->runs = malloc(t->count * sizeof(*t->runs));
        if (!copy->runs) { free(copy); return NULL; }
        memcpy(copy->runs, t->runs, t->count * sizeof(*t->runs));
    }
    return copy;
}
int sb_timeline_append(SBTimeline *t, const double *source, const double *saved, size_t n) {
    if (!t) return 0;
    if (!n) return 1;
    if (!source || !saved || n > INT64_MAX || t->output_end > INT64_MAX - (int64_t)n ||
        n > SIZE_MAX / sizeof(*t->runs) - t->count) return 0;
    /* Worst case is one run per frame. Reserve uninitialized POD memory once, never
       initialize or copy an audio-sized Swift array just to compress its clock. */
    size_t required = t->count + n;
    if (required > t->capacity) {
        size_t capacity = required;
        SBTimelineRun *runs = realloc(t->runs, capacity * sizeof(*runs));
        if (!runs) return 0;
        t->runs = runs;
        t->capacity = capacity;
    }
    size_t added = sb_timeline_build_runs(source, saved, n, t->output_end, t->runs + t->count, n);
    if (!added) return 0;
    t->count += added;
    t->output_end += (int64_t)n;
    size_t prune = sb_timeline_prune_count(t->runs, t->count, t->last_played);
    if (prune) {
        t->count -= prune;
        memmove(t->runs, t->runs + prune, t->count * sizeof(*t->runs));
    }
    return 1;
}
void sb_timeline_reset(SBTimeline *t, int64_t frame) {
    if (!t) return;
    t->saved_before = sb_timeline_saved_at(t, t->last_played);
    t->count = 0;
    t->output_end = t->last_played = 0;
    t->base_original = frame;
}
int64_t sb_timeline_played(SBTimeline *t, int64_t frame) {
    if (!t) return 0;
    return t->last_played = frame > 0 ? frame : 0;
}
int64_t sb_timeline_output_end(const SBTimeline *t) { return t ? t->output_end : 0; }
int64_t sb_timeline_last_played(const SBTimeline *t) { return t ? t->last_played : 0; }
int64_t sb_timeline_base_original(const SBTimeline *t) { return t ? t->base_original : 0; }
double sb_timeline_saved_before(const SBTimeline *t) { return t ? t->saved_before : 0; }
const SBTimelineRun *sb_timeline_runs(const SBTimeline *t, size_t *count) {
    if (count) *count = t ? t->count : 0;
    return t ? t->runs : NULL;
}
double sb_timeline_original_at(const SBTimeline *t, int64_t frame) {
    return t ? sb_timeline_original(t->runs, t->count, frame, t->base_original) : 0;
}
double sb_timeline_saved_at(const SBTimeline *t, int64_t frame) {
    return t ? sb_timeline_saved(t->runs, t->count, frame, t->saved_before) : 0;
}
