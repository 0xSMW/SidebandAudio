# SBEngine

Original C11 podcast processing with a portable scalar implementation. Apple platforms
use Accelerate for double-precision FFTs and WSOLA correlation; other platforms use the
C standard library and `libm`. Define `SB_ENGINE_PORTABLE` to select scalar arithmetic
on Apple platforms. No Audio Units, platform pitch correction, or system noise-isolation DSP.
Sideband supplies codec decoding, network transport and audio-device output separately.

## Signal path

`spectral noise reduction → average mono → custom EQ → Voice Boost EQ → loudness normalization → Voice Boost compression → de-esser → linked sample limiter → Smart Speed → WSOLA tempo`

- **Tempo:** 0.5–5×, pitch-preserving WSOLA at every non-unity rate. Window length is
  `alignUp4(round(1024 × sampleRate / 44100))`, 50% overlap, shared stereo lag, Hann
  crossfades and nominal analysis-hop advancement. Accelerated correlation rechecks
  numerically close candidates using the original ordered double sum, preserving lag
  decisions and first-best ties. Joint-channel correlation preserves
  antiphase stereo. Exact unity uses untouched PCM after upstream processing.
- **Smart Speed:** post-mastering peak detection across both channels, in stable 4096-frame
  blocks. Each accepted 148-frame quiet window becomes a 46-frame linear splice, removing
  102 frames. Detection uses the full window, including audio ahead of the splice. These
  are fixed sample counts, as recovered from Overcast, rather than millisecond windows.
- **Voice Boost EQ:** 80 Hz high-pass/Q .5; 1635 Hz/−3 dB/Q .8; 9000 Hz/−4 dB/Q 1.4;
  16 kHz low-pass/Q .5. Custom bands precede these filters.
- **Normalization:** Sideband has an independent on/off control and −20…−6 LUFS target,
  defaulting to enabled at −14 LUFS. The K-weighted meter and bounded adaptive gain operate before compression.
  This is a rolling speech loudness estimate, not a certified integrated-LUFS meter.
  The C API retains legacy mode 0 (follow Voice Boost) for compatibility; Sideband uses
  only mode 1 (off) or mode 2 (independent target).
- **Compression:** Voice Boost enables −18 dB threshold, 2.25:1 ratio, 2 dB soft knee,
  +4 dB makeup, 20 ms attack and 50 ms release.
- **De-esser:** independently off/normal/strong. Linked bright/reference energy detector,
  adaptive high shelf around 3–9 kHz, 20 ms attack/10 ms release. Normal uses −6.75 dB
  relative threshold and 10 dB maximum reduction; strong uses −9 dB and 13 dB.
- **Limiter:** always active, linked channels, −0.7 dBFS sample ceiling, 10 ms lookahead
  and release. No oversampled true-peak guarantee.
- **Noise reduction:** adjustable 1024-point, 256-hop spectral reducer with shared stereo
  gains and 50 ms warmup. Intended for stationary background noise; it can attenuate
  voice harmonics. Mono, noise reduction, normalization, EQ and de-essing work with
  Voice Boost disabled. When noise reduction is dry, analysis and estimator state keep
  advancing; synthesis uses the analytical unity-window reconstruction rather than an
  inverse FFT, retaining pending overlap history for an immediate effect transition.

This recreates the researched pipeline with original DSP. Overcast uses Apple's `ipto`
stretcher at ordinary speeds and `vois` for noise reduction. SBEngine replaces both;
it is not a bit-identical or established perceptual match. EOF extension and joint-channel
alignment are deliberate robustness differences. Device listening and battery/route-change
measurements remain necessary before claiming audio parity.

## C API

Public header: `Sources/SBEngine/include/SBEngine.h`. Swift Package product: `SBEngine`.
`sb_engine_custom_eq_gain_db` is a pure, allocation-free preview query for custom bands.
It shares playback's coefficients and clamps, accepts the processing sample rate, and
excludes Voice Boost and dynamics. Sideband samples it on a logarithmic frequency axis.
The facade supports 8–192 kHz, one or two planar float channels. Instances have one serial
owner; create/destroy may allocate, while configure/reset/push/read/finish do not allocate.
The caller may use arbitrary input chunk sizes. The creation capacity sizes internal
buffering, rather than limiting the length of a push.

1. Create, obtain `sb_engine_default_config()`, then configure. Use
   `sb_engine_config_set_band` to populate custom filters.
2. Push PCM, retrying any unaccepted suffix after reading output. Push returns the number
   of **accepted** frames. Do not discard the remainder.
3. Read until zero. Every returned sample has a corresponding original-media frame and
   cumulative Smart Speed saved-frame value. Metadata excludes tempo savings; interpolate
   the source clock directly rather than multiplying playback time by speed.
4. At input EOF call finish, then keep reading until `sb_engine_is_drained` is true.
   Buffered source frames are retained; no synthetic leading padding or extra tail duration
   is returned. Constant-rate output length without Smart Speed is `ceil(inputFrames/rate)`.
5. Before another stream, seek, or configuration after EOF, reset at the original-media
   starting frame. Reset retains settings and clears the stream's saved-frame counters.
   Optionally call `sb_engine_seed_loudness` with a known speech level before the first
   push. Sideband reuses its cached/live measurement across seeks and speed changes,
   avoiding a fresh unity-gain period while the 400 ms meter fills. Off remains unity.

DSP latency is `1024 + ceil(sampleRate × .010)` input frames; fixed blocks and WSOLA
also buffer ahead. This delays availability of output, not its logical timestamp. The
Swift adapter accumulates valid returned PCM on a decode queue, then the player schedules
it directly to the output mixer. It never subtracts that buffering latency from timestamps.

## SDK integration

The Swift `SidebandAudio` product wraps configuration, PCM buffers and timing metadata.
`SidebandAudioPlayer` optionally decodes completed local files through AVAudioFile and
outputs PCM through AVAudioPlayerNode. Original comparison playback bypasses the C
processor entirely. The host owns document access, AVAudioSession and system controls.
There is no network or download implementation in this repository.

## Validation

Run `sh Tests/CEngine/run.sh` from the repository root for the standalone
nine address/undefined-behavior sanitizer gates (including correlation decisions, mono
recurrences, C clock runs and audible EOF callbacks). Apple standalone executables link Accelerate. Run
`swift test --no-parallel` for the independent Swift facade and local-file player regression suite.

Verified on October 18, 2026 with deterministic offline PCM:

| Measurement | Result |
|---|---|
| 440 Hz at 0.5–5×, 44.1/48 kHz | 439.979–440.069 Hz |
| Antiphase stereo at non-unity speed | 439.978 Hz; L = −R preserved |
| Constant-rate duration | `ceil(N / rate)` frames |
| Unity below limiter ceiling, all effects off | Bit-exact PCM |
| Tiny EOF, chunk boundaries, reset/backpressure | Passed; delayed first/last samples retained |
| Smart Speed on 8192 quiet frames | 2684 output frames; 5508 saved source frames |
| EQ requested +6 dB | +6.000 dB measured |
| Normalization fixture, compression off | −30.79 LUFS +16.79 dB gain = −14.00 LUFS |
| Strong de-esser fixture | 8 kHz −6.68 dB; 800 Hz −0.00 dB |
| Limiter fixture | Peak 0.922571 (−0.7 dBFS) |
| Stationary-noise fixture | Residual −4.74 dB; harmonic −2.39 dB |

Final integration verification: 448 Swift tests across 90 suites passed. Signed Sideband Release 1.0 build 95 passed deep/strict signature verification; the app was not installed.

These are synthetic test measurements, not an audition or an Overcast null comparison.

The 30.880907-second local synthesized-speech fixture rendered to 15.440454 seconds
at 2× without Smart Speed, with zero silence savings. At 2× with Smart Speed and Voice
Boost it rendered to 13.162222 seconds, reporting 4.556463 seconds of removed source
silence and zero clipped samples. This verifies a complete speech render through the
public C API, including metadata and EOF, rather than only isolated filter responses.

## Offline WAV rendering

Build a PCM16 WAV comparison tool without an app (Apple acceleration shown):

```sh
clang -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
  -I Sources/SBEngine/include \
  Tools/render.c \
  Sources/SBEngine/sb_engine.c \
  Sources/SBEngine/sb_dsp.c \
  Sources/SBEngine/sb_stretcher.c \
  -lm -framework Accelerate -o /tmp/sbengine-render
/tmp/sbengine-render input.wav new-output.wav 2 1 1 0
```

For a portable build, omit `-framework Accelerate` and define `-DSB_ENGINE_PORTABLE`
on Apple hosts.

Arguments are input path, new output path, rate, Smart Speed (0/1), Voice Boost (0/1)
and optional noise strength (0…1). Existing output paths are refused. The tool streams
PCM16LE mono/stereo WAVs up to 100 MiB with fixed buffers and prints duration, peak,
clipped-sample and source/silence-clock measurements to stderr.
