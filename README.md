# Sideband Audio

Sideband Audio is a Swift package for processing spoken-word audio on iOS 17 and macOS 14. It provides pitch-preserving speed from 0.5x to 5x, silence shortening, voice EQ and compression, noise reduction, loudness normalization, de-essing and up to 12 custom EQ bands. A C11 engine does the processing and tags every output frame with its position in the original file, so a player can show file time and heard silence savings at any speed.

<img src="Documentation/Images/Demo.jpg" alt="Sideband Audio demo" width="360">

## APIs

There are three library products, and each one depends on the one listed above it. Use the C engine directly, the Swift processor for PCM you decode yourself, or the local file player for AVFoundation playback.

- `SBEngine` (C11, libc and libm, Accelerate on Apple platforms)
  - Engine lifecycle: `sb_engine_create`, `sb_engine_configure`, `sb_engine_reset`, `sb_engine_seed_loudness`, `sb_engine_push`, `sb_engine_read`, `sb_engine_finish`, `sb_engine_is_drained`, `sb_engine_destroy`
  - Measurements: `sb_engine_loudness`, `sb_engine_latency`
  - Configuration: `SBEngineConfig`, `SBEngineEQBand`, `sb_engine_default_config`, `sb_engine_config_set_band`, `sb_engine_custom_eq_gain_db`
  - Source clock: `SBTimeline`, `SBTimelineRun` and the `sb_timeline_*` functions
- `SidebandAudio` (Swift, Foundation)
  - Settings: `AudioConfiguration`, `EQBand`, `EQBand.Kind`, `DeEsser`
  - Processing: `AudioProcessor`, `ProcessedAudio`, `ProcessedAudioView`, `AudioProcessingError`
  - Clock: `AudioTimeline`
- `SidebandAudioPlayer` (Swift, AVFoundation, Observation)
  - `LocalFilePlayer`, `PlaybackMode`, `PlaybackState`, `PlayerFailure`

## Implementation guide

### Installation

The package requires Swift tools 6.2. Repository: [0xSMW/SidebandAudio](https://github.com/0xSMW/SidebandAudio).

In Xcode, choose File > Add Package Dependencies and enter `https://github.com/0xSMW/SidebandAudio.git`. Select the `main` branch. To work on a local checkout, use Add Local and select its folder.

In `Package.swift`:

```swift
dependencies: [
    .package(url: "https://github.com/0xSMW/SidebandAudio.git", branch: "main"),
],
targets: [
    .target(name: "MyApp", dependencies: [
        .product(name: "SidebandAudio", package: "SidebandAudio"),
        .product(name: "SidebandAudioPlayer", package: "SidebandAudio"), // optional
    ]),
]
```

Depend on `SBEngine` alone for the C API.

### Settings

`AudioConfiguration` is a `Codable`, `Equatable`, `Sendable` value. Every product reads the same settings.

```swift
import SidebandAudio

var settings = AudioConfiguration.speech
settings.rate = 1.8
settings.noiseReduction = 0.3
settings.normalizationTargetLUFS = -16
settings.deEsser = .normal
settings.equalizer = [
    EQBand(kind: .highPass, frequency: 90, q: 0.707),
    EQBand(kind: .peak, frequency: 3_000, q: 1.2, gainDB: 2.5),
]

let accepted = try settings.validated()
let gainAt3k = try accepted.equalizerGainDB(at: 3_000, sampleRate: 44_100)
```

| Property | Values | `AudioConfiguration()` | `.speech` |
|---|---|---|---|
| `rate` | `0.5...5`, pitch preserved | `1` | `1` |
| `shortenSilence` | `Bool` | `false` | `true` |
| `voiceEnhancement` | `Bool`, voice EQ and compression | `false` | `true` |
| `mono` | `Bool`, averages both channels | `false` | `false` |
| `noiseReduction` | `0...1` | `0` | `0` |
| `normalizationEnabled` | `Bool` | `false` | `true` |
| `normalizationTargetLUFS` | `-20...-6` | `-14` | `-14` |
| `deEsser` | `DeEsser.off`, `.normal`, `.strong` | `.off` | `.off` |
| `equalizer` | Up to 12 `EQBand` values with unique `id`s | `[]` | `[]` |

`EQBand(id:kind:frequency:q:gainDB:)` defaults to a `.peak` band at 1,000 Hz, Q 0.707 and 0 dB. `EQBand.Kind` has seven cases: `.peak`, `.lowPass`, `.highPass`, `.bandPass`, `.notch`, `.lowShelf` and `.highShelf`. Valid ranges are `frequency` 20...20,000 Hz, `q` 0.3...8 and `gainDB` -12...12. `EQBand` is `Identifiable` by its `UUID`, which suits a SwiftUI `ForEach`.

`validated()` returns the configuration unchanged or throws `AudioProcessingError.invalidConfiguration(field)`, where `field` names the bad value (`"rate"`, `"equalizer Q"`, `"duplicate equalizer id"` and so on). It rejects non-finite and out-of-range values. It never clamps them.

`equalizerGainDB(at:sampleRate:)` returns the custom EQ response in dB for drawing a curve. It uses the same coefficients and clamps as processing and excludes Voice Boost and dynamics. It validates the configuration first, throws `.invalidFormat` unless the sample rate is 8...192 kHz and `0 < frequency < sampleRate / 2`, and can return `-infinity` at an exact transfer zero, such as a notch center. Clipping the chart is the caller's job.

Processing runs in this order: noise reduction, mono, custom EQ, Voice Boost EQ, normalization, Voice Boost compression, de-esser, limiter, silence shortening, tempo. `Documentation/Engine.md` lists the filter, compressor and de-esser parameters. Limits to plan for:

- The limiter is always on, including in the neutral `AudioConfiguration()`, with a -0.7 dBFS sample-peak ceiling and no oversampled true-peak guarantee.
- Normalization uses a rolling K-weighted loudness estimate, not a certified integrated-LUFS meter.
- Noise reduction targets stationary background noise and can attenuate voice harmonics.
- The measurements in `Documentation/Engine.md` come from synthetic test signals. Battery use and audio quality on devices have not been measured, and neither has a perceptual comparison with other players.

### Playing local files

`LocalFilePlayer` decodes a local file with `AVAudioFile`, processes it on a private serial queue and plays it through `AVAudioEngine`. It is `@MainActor` and `@Observable`. Keep a strong reference for as long as audio should play.

```swift
import SwiftUI
import AVFoundation
import SidebandAudio
import SidebandAudioPlayer

struct PlayerView: View {
    let fileURL: URL  // a local file URL
    @State private var player = LocalFilePlayer(configuration: .speech)
    @State private var message: String?

    var body: some View {
        VStack(spacing: 12) {
            Text(String(format: "%.1f / %.1f s, saved %.1f s", player.position, player.duration, player.silenceSaved))
            Button(player.isPlaying ? "Pause" : "Play") {
                if player.isPlaying { player.pause() } else { player.play() }
            }
            Button("1.5x") {
                var settings = player.configuration
                settings.rate = 1.5
                do { try player.configure(settings) } catch { message = error.localizedDescription }
            }
            Picker("Mode", selection: Binding(get: { player.mode }, set: { player.setMode($0) })) {
                ForEach(PlaybackMode.allCases, id: \.self) { Text($0.rawValue.capitalized) }
            }
            if case .failed(let failure) = player.state { Text(failure.localizedDescription) }
            if let message { Text(message) }
        }
        .task {
            do {
                #if os(iOS)
                let session = AVAudioSession.sharedInstance()
                try session.setCategory(.playback, mode: .spokenAudio, policy: .longFormAudio)
                try session.setActive(true)
                #endif
                try await player.load(fileURL: fileURL)
            } catch {
                message = error.localizedDescription
            }
        }
    }
}
```

Methods:

- `init(configuration: AudioConfiguration = .speech)` stores the settings without validating them. `load` reports an invalid initial configuration.
- `load(fileURL:) async throws` replaces the current source. It throws `PlayerFailure.invalidFileURL` for non-file URLs, `AudioProcessingError.invalidConfiguration` for invalid stored settings, `PlayerFailure.open` or `.processing` for unreadable or unsupported audio, and `CancellationError` when a newer `load` or `stop()` supersedes it. Settings and mode changes made while the file opens apply before output starts. The file must be 8...192 kHz with at least one frame. Files with more than two channels are downmixed to stereo with `AVAudioConverter`.
- `configure(_:) throws` validates synchronously. On success `configuration` updates immediately. On failure it throws `AudioProcessingError` and keeps the current settings. A rate-only change keeps the PCM already queued, about 200 ms, so the new rate is heard after that audio plays. Any other change fades out briefly and regenerates from the audible source position.
- `setMode(_:)` switches between `PlaybackMode.processed` and `.original`. Original plays the decoded PCM at 1x and never creates or calls the C processor or timeline. Settings stay editable in Original and apply when you return to Processed. The mode persists across loads.
- `seek(to:)` takes original-media seconds, clamped to `0...duration`. Seeking from `.ended` moves to `.paused`.
- `play()` starts or resumes. From `.ended` it restarts at 0.
- `pause()` pauses output.
- `stop()` unloads the source, releases output resources, cancels an in-flight load and returns to `.idle` with zeroed counters.
- `recoverAfterMediaServicesReset() async throws` reloads the current file and restores position, heard savings and play intent. Call it after your app reactivates its audio session. It does nothing when no file is loaded.

None of `play()`, `pause()`, `seek(to:)`, `setMode(_:)` and `stop()` throws. A decode, DSP or output failure during playback sets `state` to `.failed(PlayerFailure)`. That state is terminal for the loaded file: `play()` and `pause()` are ignored, and `load` recovers. The player rebuilds its audio graph on `AVAudioEngineConfigurationChange` and continues from the audible position.

Observable state, refreshed about every 250 ms during playback:

| Property | Meaning |
|---|---|
| `configuration` | Last accepted `AudioConfiguration` |
| `state` | `PlaybackState`: `.idle`, `.loading`, `.ready`, `.playing`, `.paused`, `.ended`, `.failed(PlayerFailure)` |
| `isPlaying` | `state == .playing` |
| `mode` | Current `PlaybackMode` |
| `fileURL` | Loaded file, or `nil` |
| `position`, `duration` | Original-media seconds: file frames divided by `sampleRate`, regardless of rate or removed silence |
| `silenceSaved` | Seconds of silence shortening the listener has heard. It excludes time saved by `rate`, carries across seeks and mode changes, and resets on `load` and `stop` |
| `sampleRate` | File sample rate in Hz, or 0 before load |
| `measuredLoudness` | Engine loudness estimate in LUFS, or `nil` until valid. The player uses it to seed normalization after seeks |

`PlayerFailure` cases are `.invalidFileURL`, `.open(String)`, `.processing(AudioProcessingError)` and `.output(String)`. Both error enums conform to `LocalizedError`.

### Audio session obligations

The SDK never configures or activates `AVAudioSession`. On iOS, set a playback category and activate the session before `load`, as in the example above. Your app also handles:

- Interruptions. Pause on `.began`. On `.ended` with `.shouldResume`, reactivate the session and call `play()` only if audio was playing before the interruption and the user has not paused since.
- Route changes. Pause on `.oldDeviceUnavailable`, for example when headphones are unplugged.
- Media services reset. Reactivate the session, then `try await player.recoverAfterMediaServicesReset()`.

Document access, security-scoped URLs, Now Playing and remote commands belong to the app. `Examples/SidebandAudioDemo/DemoModel.swift` demonstrates document access and audio-session events; add system media controls in your host app.

### Processing PCM you decode

`AudioProcessor` processes planar Float32 PCM, mono or stereo, at 8...192 kHz.

```swift
import SidebandAudio

let processor = try AudioProcessor(sampleRate: 44_100, channelCount: 2, configuration: .speech)

for chunk in decodedChunks {                // [[Float]], one array per channel
    let output = try processor.process(chunk)
    write(output.channels)                  // your output sink
}
let tail = try processor.finish()           // buffered audio left at end of input
write(tail.channels)
```

The borrowed form avoids copies. Output storage is allocated at construction and reused:

```swift
try processor.process(left: leftPointer, right: rightPointer, frames: count) { view in
    copyToOutput(view.left, view.right, view.frameCount)
}
try processor.finish { view in
    copyToOutput(view.left, view.right, view.frameCount)
}
```

Members:

- `init(sampleRate:channelCount:configuration:) throws` throws `.invalidFormat` for unsupported rates or channel counts, `.invalidConfiguration` and `.allocationFailed`. The default configuration is the neutral `AudioConfiguration()`. `sampleRate` and `channelCount` are read-only.
- `configure(_:) throws` validates and applies new settings. `configuration` holds the last accepted value.
- `process(_:) throws -> ProcessedAudio` needs one array per channel, all the same length. `process(left:right:frames:consume:) throws` takes raw pointers. `right` may be `nil` for mono. Both forms reject non-finite samples before accepting any input. They accept every frame, push and drain internally, and can deliver output over several callbacks or none at all.
- `finish() throws -> ProcessedAudio` and `finish(consume:) throws` end input and drain all remaining output. Call one of them at end of input or the last audio stays buffered. Calling finish again after a complete drain does nothing. If a callback threw, a later call resumes the drain.
- `isFinished` becomes true once the drain completes.
- `reset(at:seedLoudness:) throws` starts a new stream at an original-media frame and keeps the settings. `seedLoudness` is an optional LUFS value that lets normalization start from a known level. After `finish` starts, `process` and `configure` throw `.finished` until you reset. To seek, call `try processor.reset(at: frame, seedLoudness: processor.loudness)`.
- `loudness` is the engine's estimate in LUFS, or `nil` until valid.
- `latencyFrames` is DSP latency in source frames. Fixed blocks and the tempo stretcher buffer more on top of it. Latency delays when output is available. It does not shift timestamps.

Output types:

- `ProcessedAudio` is an owned `Sendable` copy with `channels: [[Float]]`, `sourceFrames: [Double]`, `silenceSavedFrames: [Double]` and `frameCount`. Its memberwise `init` lets you build chunks yourself.
- `ProcessedAudioView` holds `UnsafeBufferPointer`s with the same fields: `left`, `right`, `sourceFrames`, `silenceSavedFrames` and `frameCount`. The pointers are valid only until the callback returns. For mono input `right` aliases `left`.
- `sourceFrames[i]` is the original-media frame of output frame `i`. This mapping accounts for tempo. `silenceSavedFrames[i]` is cumulative silence removed up to that frame and excludes tempo. Map playback through these values or `AudioTimeline`. Never multiply elapsed time by `rate`.

Contracts:

- `AudioProcessor` is not `Sendable`. Use it from one serial owner. Reentrant calls throw `.invalidInput`, and concurrent use is unsupported. It is not designed or verified for use inside a real-time render callback. Process on a decode queue and schedule the result, as `LocalFilePlayer` does.
- If a callback throws, input already accepted and output already delivered stay consumed. Reset before retrying input.
- `.stalled` means the engine stopped making progress.

`AudioProcessingError` cases: `.invalidFormat`, `.invalidConfiguration(String)`, `.invalidInput`, `.finished`, `.stalled`, `.allocationFailed`.

### Mapping played output to file time

`AudioTimeline` stores the clock metadata from processor output and answers what file position and heard savings correspond to an output frame. Output frames count from the last `reset(at:)`.

```swift
let timeline = try AudioTimeline()
timeline.reset(at: startFrame)

try processor.process(left: leftPointer, right: rightPointer, frames: count) { view in
    try timeline.append(view)              // or append(_ chunk: ProcessedAudio)
    schedule(view)
}

// When your output reports how many output frames have played since the reset:
timeline.played(playedFrames)
let positionSeconds = timeline.original(at: playedFrames) / processor.sampleRate
let savedSeconds = timeline.silenceSaved(at: playedFrames) / processor.sampleRate
```

- `init() throws` throws `.allocationFailed`.
- `append(_: ProcessedAudio)` and `append(_: ProcessedAudioView)` throw `.invalidInput` unless channel counts match, source and saved values are finite and neither decreases. They throw `.allocationFailed` without changing the clock.
- `played(_:)` records playback progress, clamped to `0...outputEnd`, so queued silence that has not been heard never counts as saved. It returns the clamped frame. Call it as playback advances, because the timeline discards history more than 8,192 frames behind the last played frame.
- `original(at:)` returns the original-media frame and `silenceSaved(at:)` the cumulative heard savings, including `savedBefore`.
- `reset(at:)` moves savings heard up to `lastPlayed` into `savedBefore`, drops queued output, and sets `baseOriginal`. Negative frames clamp to 0.
- `outputEnd` is the number of output frames appended since the reset. `lastPlayed` is the last value given to `played`. `baseOriginal` is the reset frame. `savedBefore` is savings carried over from before the reset.

`AudioTimeline` is not `Sendable`. Use it from the same serial owner as the processor.

### C engine

`SBEngine` is the same engine through a C interface. Exact signatures are in [`Sources/SBEngine/include/SBEngine.h`](Sources/SBEngine/include/SBEngine.h). Instances have one serial owner. Create and destroy may allocate. Configure, reset, push, read and finish do not.

```c
#include <SBEngine.h>

enum { CAP = 4096 };
static float out_l[CAP], out_r[CAP];
static double src[CAP], saved[CAP];

static size_t drain(SBEngine *e) {
    size_t total = 0, n;
    while ((n = sb_engine_read(e, out_l, out_r, src, saved, CAP)) > 0) {
        consume(out_l, out_r, src, saved, n);  /* src: original frames; saved: cumulative silence */
        total += n;
    }
    return total;
}

int process_file(void) {
    SBEngine *e = sb_engine_create(44100, 2, CAP);  /* NULL: bad format or allocation failure */
    if (!e) return 0;

    SBEngineConfig c = sb_engine_default_config();
    c.rate = 1.5f;
    c.normalization_mode = 2;  /* custom target */
    c.target_lufs = -16;
    sb_engine_config_set_band(&c, 0, 5 /* low shelf */, 120, 0.7f, -3);
    if (!sb_engine_configure(e, &c)) { sb_engine_destroy(e); return 0; }

    sb_engine_reset(e, 0);            /* original-media start frame */
    sb_engine_seed_loudness(e, -18);  /* optional, before the first push */

    const float *l, *r;
    size_t frames;
    while (decode(&l, &r, &frames)) {
        size_t done = 0;
        while (done < frames) {
            size_t accepted = sb_engine_push(e, l + done, r + done, frames - done);
            done += accepted;  /* retry the unaccepted suffix after reading */
            if (drain(e) == 0 && accepted == 0) { sb_engine_destroy(e); return 0; }  /* stalled */
        }
    }

    sb_engine_finish(e);
    while (!sb_engine_is_drained(e)) {
        if (drain(e) == 0 && !sb_engine_is_drained(e)) {
            sb_engine_destroy(e); return 0;  /* stalled: never report success */
        }
    }

    float lufs = sb_engine_loudness(e);       /* -160 until valid */
    size_t latency = sb_engine_latency(e);    /* DSP latency in source frames */
    (void)lufs; (void)latency;
    sb_engine_destroy(e);
    return 1;
}
```

Contracts:

- `sb_engine_create` accepts 8...192 kHz and 1 or 2 channels. `max_input_frames` sizes internal buffering. It does not limit the length of a push.
- `sb_engine_default_config()` returns rate 1 with Smart Speed and Voice Boost on and `normalization_mode` 0, which follows Voice Boost. This differs from the Swift neutral `AudioConfiguration()`. The Swift layer uses only mode 1 (off) and mode 2 (custom target).
- `sb_engine_config_set_band` writes band `index` and raises `eq_count` to include it. Band types are 0 peak, 1 low-pass, 2 high-pass, 3 band-pass, 4 notch, 5 low shelf, 6 high shelf. The Swift `EQBand.Kind` raw values match.
- `sb_engine_configure` returns 0 for a NULL argument, a rate outside 0.5...5 or more than 12 bands. The Swift layer applies the full validation from `validated()`.
- `sb_engine_push` returns the accepted frame count. Zero means read output before retrying. The engine never discards input it has not accepted. For mono, `right` may be NULL.
- `sb_engine_read` returns PCM plus per-frame source and saved-frame metadata, as in the Swift view.
- After `sb_engine_finish`, keep reading until `sb_engine_is_drained` returns nonzero. Push is refused until `sb_engine_reset`. Reset before a new stream, a seek, or configuring after end of input. Reset keeps settings and clears the stream's saved-frame counters.
- `sb_engine_custom_eq_gain_db(config, sample_rate, frequency)` is the C form of `equalizerGainDB`. It returns `NAN` for invalid arguments. It needs no engine instance and allocates nothing.

Timeline functions, which `AudioTimeline` wraps:

| Group | Functions | Notes |
|---|---|---|
| Instance | `sb_timeline_create`, `sb_timeline_clone`, `sb_timeline_destroy` | Clone copies retained runs |
| Updates | `sb_timeline_append`, `sb_timeline_reset`, `sb_timeline_played` | Append returns 0 on failure and leaves the clock unchanged. C `played` does not clamp to `output_end` |
| Queries | `sb_timeline_original_at`, `sb_timeline_saved_at`, `sb_timeline_output_end`, `sb_timeline_last_played`, `sb_timeline_base_original`, `sb_timeline_saved_before`, `sb_timeline_runs` | `runs` returns the retained `SBTimelineRun` array and its count |
| Stateless runs | `sb_timeline_build_runs`, `sb_timeline_prune_count`, `sb_timeline_original`, `sb_timeline_saved` | Operate on a caller-owned `SBTimelineRun` buffer. The builder needs at most `frames` slots and allocates nothing |

`Documentation/Engine.md` describes each stage, the latency formula, test measurements and a command-line WAV renderer in `Tools/render.c`.

### Demo app

Open `Examples/SidebandAudioDemo/SidebandAudioDemo.xcodeproj` and run the `SidebandAudioDemo` scheme on an iOS 17 or later simulator or device. Select your signing team for a device build. The screen exposes every `AudioConfiguration` setting in place, the Processed and Original modes, and an importer for local files. Imported files are copied into the app's caches directory before loading. The demo makes no network requests.

The bundled sample, `MoonChallenge.wav`, is 13.480544 seconds: an 11.480544-second archival recording of John F. Kennedy's May 25, 1961 statement about landing on the Moon, with one second of digital silence added before and after. The added silence makes the silence-shortening comparison repeatable despite noise in the recording. Original mode plays the file at 1x without the engine. `Examples/SidebandAudioDemo/Resources/ATTRIBUTION.md` records the source and preparation.

```sh
sh Scripts/build-demo.sh [destination]
```

The script increments the build number in `.build/demo-build-number` and builds the Release configuration without signing. The default destination is `generic/platform=iOS Simulator`.

### Tests

Run from the package root:

```sh
sh Tests/CEngine/run.sh               # nine C engine tests under address and undefined-behavior sanitizers
SB_ENGINE_PORTABLE=1 sh Tests/CEngine/run.sh # exercise the scalar implementation on Apple platforms too
swift test --no-parallel              # Swift processor and player tests
python3 Scripts/check-independence.py # fails on app or network dependencies in Sources/ and Package.swift
```

### License and provenance

The code is MIT licensed; see `LICENSE`. The demo recording is a separate public-domain United States government work, and its status is recorded in `Examples/SidebandAudioDemo/Resources/ATTRIBUTION.md`. The code license does not apply to it. [`PROVENANCE.md`](PROVENANCE.md) describes where the source came from.
