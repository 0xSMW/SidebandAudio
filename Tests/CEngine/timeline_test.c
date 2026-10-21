#include "SBEngine.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static size_t legacy_runs(const double *source, const double *saved, size_t n, int64_t output, SBTimelineRun *runs) {
    size_t count = 0, start = 0;
    while (start < n) {
        double source_step = start + 1 < n ? source[start + 1] - source[start] : 0;
        double saved_step = start + 1 < n ? saved[start + 1] - saved[start] : 0;
        size_t end = start + 1;
        while (end + 1 < n && fabs(source[end + 1] - source[end] - source_step) < .00001 &&
               fabs(saved[end + 1] - saved[end] - saved_step) < .00001) ++end;
        size_t length = end - start + (end == n - 1);
        if (!length) length = 1;
        runs[count++] = (SBTimelineRun){output + (int64_t)start, (int64_t)length, source[start],
            length > 1 ? source_step : 0, saved[start], length > 1 ? saved_step : 0};
        start += length;
    }
    return count;
}
static uint32_t random_state = 4789;
static uint32_t next_random(void) { random_state = random_state * 1664525u + 1013904223u; return random_state; }
static void construction_matches_swift_reference(void) {
    double source[257], saved[257];
    SBTimelineRun actual[257], expected[257];
    for (size_t trial = 0; trial < 1000; ++trial) {
        size_t n = 1 + next_random() % 257;
        double s = 400000, v = 0, slope = 1.3;
        for (size_t i = 0; i < n; ++i) {
            if (next_random() % 31 == 0) { s += 777; v += 777; }
            if (next_random() % 17 == 0) slope = (double)(5 + next_random() % 45) / 10;
            source[i] = s; saved[i] = v; s += slope;
        }
        size_t expected_count = legacy_runs(source, saved, n, 900, expected);
        size_t actual_count = sb_timeline_build_runs(source, saved, n, 900, actual, n);
        assert(expected_count == actual_count);
        for (size_t i = 0; i < actual_count; ++i) {
            assert(actual[i].output == expected[i].output && actual[i].length == expected[i].length);
            assert(actual[i].source == expected[i].source && actual[i].source_step == expected[i].source_step);
            assert(actual[i].saved == expected[i].saved && actual[i].saved_step == expected[i].saved_step);
        }
    }
    assert(sb_timeline_build_runs(source, saved, 257, 0, actual, 256) == 0);
    assert(sb_timeline_build_runs(source, saved, 2, INT64_MAX, actual, 257) == 0);
}
static void boundaries_clone_reset_and_pruning(void) {
    SBTimeline *t = sb_timeline_create(); assert(t);
    assert(sb_timeline_original_at(t, 0) == 0);
    double source[] = {100, 101.3, 102.6, 500, 501.3, 502.6, 700};
    double saved[] = {0, 0, 0, 396.1, 396.1, 396.1, 592.2};
    assert(sb_timeline_append(t, source, saved, 7));
    for (int64_t i = 0; i < 7; ++i) {
        assert(fabs(sb_timeline_original_at(t, i) - source[i]) < 1e-9);
        assert(fabs(sb_timeline_saved_at(t, i) - saved[i]) < 1e-9);
    }
    assert(sb_timeline_original_at(t, INT64_MIN) == source[0]);
    assert(fabs(sb_timeline_original_at(t, INT64_MAX) - (source[6] + source[6] - source[5])) < 1e-9);
    assert(sb_timeline_saved_at(t, INT64_MAX) == saved[6]);
    SBTimeline *copy = sb_timeline_clone(t); assert(copy);
    assert(sb_timeline_played(t, -3) == 0);
    assert(sb_timeline_played(t, 4) == 4);
    sb_timeline_reset(t, 900000);
    assert(sb_timeline_original_at(t, 0) == 900000);
    assert(sb_timeline_saved_at(t, 0) == saved[4]);
    assert(sb_timeline_output_end(copy) == 7 && sb_timeline_original_at(copy, 0) == 100);
    for (size_t j = 0; j < 600; ++j) {
        double s[128], v[128];
        for (size_t i = 0; i < 128; ++i) { s[i] = 900000 + j * 128 + i; v[i] = j * 128; }
        (void)sb_timeline_played(t, sb_timeline_output_end(t));
        assert(sb_timeline_append(t, s, v, 128));
    }
    size_t count = 0; (void)sb_timeline_runs(t, &count);
    assert(count < 150 && sb_timeline_output_end(t) == 76800);
    (void)sb_timeline_played(t, 76000);
    double heard = sb_timeline_saved_at(t, 76000);
    sb_timeline_reset(t, 42);
    assert(sb_timeline_saved_at(t, 0) == heard);
    (void)sb_timeline_runs(t, &count); assert(count == 0);
    sb_timeline_destroy(copy); sb_timeline_destroy(t);
}
int main(void) {
    construction_matches_swift_reference();
    boundaries_clone_reset_and_pruning();
    puts("Timeline C gates passed: reference parity, discontinuities, EOF, bounded history, clone, resets.");
    return 0;
}
