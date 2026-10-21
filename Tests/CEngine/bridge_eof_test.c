#include "SBEngine.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

/* Final-input outputs and finish() tails use hardware-playback completion in Swift.
   Prove every nonempty stream produces at least one such output, including awkward
   one-frame input tails, unity bypass and rate changes immediately before EOF. */
static void check(size_t frames, double rate, int smart, int voice, int change_at_end, int all_quiet) {
    SBEngine *e = sb_engine_create(48000, 2, 32768); assert(e);
    SBEngineConfig c = sb_engine_default_config();
    c.rate = (float)rate; c.smart_speed = smart; c.voice_boost = voice;
    c.mono = 1; c.deesser = 1; c.noise_reduction = 0; c.normalization_mode = 1;
    assert(sb_engine_configure(e, &c)); sb_engine_reset(e, 0);
    float left[3840], right[3840], out_left[32768], out_right[32768];
    double source[32768], saved[32768];
    size_t terminal = 0, total = 0;
    for (size_t offset = 0; offset < frames;) {
        size_t n = frames - offset; if (n > 3840) n = 3840;
        int final = offset + n == frames;
        if (final && change_at_end) { c.rate = 1; assert(sb_engine_configure(e, &c)); }
        for (size_t i = 0; i < n; ++i) {
            /* Includes quiet blocks so the invariant covers Smart Speed microsplices. */
            float x = all_quiet || offset / 3840 % 3 == 1 ? 0 : .12f * sinf((float)(offset + i) * .09f);
            left[i] = x; right[i] = x;
        }
        size_t accepted = sb_engine_push(e, left, right, n); assert(accepted == n);
        offset += n;
        for (;;) {
            size_t out = sb_engine_read(e, out_left, out_right, source, saved, 32768);
            if (!out) break;
            total += out; if (final) terminal += out;
        }
    }
    sb_engine_finish(e);
    for (size_t attempt = 0; !sb_engine_is_drained(e); ++attempt) {
        assert(attempt < 100);
        size_t out = sb_engine_read(e, out_left, out_right, source, saved, 32768);
        terminal += out; total += out;
    }
    assert(frames == 0 || (total > 0 && terminal > 0));
    sb_engine_destroy(e);
}
int main(void) {
    const size_t lengths[] = {0,1,7,148,149,3839,3840,3841,4095,4096,4097,7680,7681,11521,30721};
    const double rates[] = {.5,1,1.3,5};
    size_t cases = 0;
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; ++i)
        for (size_t r = 0; r < sizeof rates / sizeof *rates; ++r)
            for (int smart = 0; smart < 2; ++smart)
                for (int voice = 0; voice < 2; ++voice)
                    for (int change = 0; change < 2; ++change) {
                        for (int quiet = 0; quiet < 2; ++quiet) {
                            check(lengths[i], rates[r], smart, voice, change, quiet); ++cases;
                        }
                    }
    printf("Bridge EOF completion gate: %zu source lengths/settings/rate-change cases retain a terminal hardware-playback buffer.\n", cases);
    return 0;
}
