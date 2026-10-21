import Foundation
import Testing
@preconcurrency import AVFoundation
import SidebandAudio
@testable import SidebandAudioPlayer

@Suite(.serialized)
@MainActor
struct LocalFilePlayerTests {
    @Test func remoteURLIsRejectedBeforeChangingTheSession() async throws {
        let player = LocalFilePlayer()
        do {
            try await player.load(fileURL: URL(string: "https://invalid.example/audio.caf")!)
            Issue.record("Remote URL was accepted")
        } catch {
            #expect(error as? PlayerFailure == .invalidFileURL)
        }
        #expect(player.state == .idle)
        #expect(player.fileURL == nil)
        #expect(player.sampleRate == 0)
    }

    @Test func invalidConfigurationDoesNotCommit() throws {
        let player = LocalFilePlayer(configuration: .init())
        var invalid = player.configuration
        invalid.rate = .nan
        #expect(throws: AudioProcessingError.self) { try player.configure(invalid) }
        #expect(player.configuration == AudioConfiguration())
        #expect(player.state == .idle)
    }

    @Test func localLoadIsPreparedWithoutPlayingAndStopUnloads() async throws {
        let fixture = try Fixture(seconds: 0.08)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer(configuration: .init())
        try await player.load(fileURL: fixture.url)
        #expect(player.state == .ready)
        #expect(!player.isPlaying)
        #expect(player.fileURL == fixture.url)
        #expect(player.duration == 0.08)
        #expect(player.sampleRate == 48_000)
        // Preparing a short file reaches decode EOF; it must not emit ended while paused.
        try await Task.sleep(for: .milliseconds(350))
        #expect(player.state == .ready)
        #expect(player.position == 0)
        #expect(player.silenceSaved == 0)
        #expect(player.effectiveRate == 1)
        player.stop()
        #expect(player.state == .idle)
        #expect(player.fileURL == nil)
        #expect(player.duration == 0)
        #expect(player.position == 0)
        try await Task.sleep(for: .milliseconds(300))
        #expect(player.state == .idle)
    }

    @Test func seekBoundsAndModeChangesRetainPausedPosition() async throws {
        let fixture = try Fixture(seconds: 1)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer(configuration: .init())
        try await player.load(fileURL: fixture.url)
        player.pause()
        player.seek(to: 0.4)
        player.setMode(.original)
        var config = player.configuration
        config.rate = 2.5
        config.shortenSilence = true
        try player.configure(config)
        try await Task.sleep(for: .milliseconds(350))
        #expect(player.mode == .original)
        #expect(player.configuration == config)
        #expect(player.effectiveRate == 1)
        #expect(player.state == .paused)
        #expect(abs(player.position - 0.4) < 0.001)
        #expect(player.silenceSaved == 0)
        player.setMode(.processed)
        try await Task.sleep(for: .milliseconds(350))
        #expect(player.state == .paused)
        #expect(abs(player.position - 0.4) < 0.001)
        #expect(player.effectiveRate == 2.5)
        player.seek(to: .infinity)
        #expect(player.position == 0)
        player.seek(to: -10)
        #expect(player.position == 0)
        player.seek(to: 99)
        #expect(player.position == player.duration)
        try await Task.sleep(for: .milliseconds(350))
        #expect(player.position == player.duration)
        #expect(player.state == .paused)
        player.stop()
    }

    @Test func modeRequestSurvivesSeekInTheSameActorTurn() async throws {
        let fixture = try Fixture(seconds: 1)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer(configuration: .init())
        try await player.load(fileURL: fixture.url)
        player.pause()
        player.setMode(.original)
        player.seek(to: 0.4)
        let original = await player.diagnostics()
        #expect(original?.mode == .original)
        #expect(original?.hasProcessor == false)
        #expect(original?.playingIntent == false)
        #expect(original?.configurationPending == false)
        player.setMode(.processed)
        player.seek(to: 0.6)
        let processed = await player.diagnostics()
        #expect(processed?.mode == .processed)
        #expect(processed?.hasProcessor == true)
        #expect(processed?.playingIntent == false)
        #expect(processed?.configurationPending == false)
        #expect(player.state == .paused)
        player.stop()
    }

    @Test func rapidReturnToOriginalCancelsPendingTransitionAndFade() async throws {
        let fixture = try Fixture(seconds: 1)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer(configuration: .init())
        player.setMode(.original)
        try await player.load(fileURL: fixture.url)
        player.pause()
        player.setMode(.processed)
        player.setMode(.original)
        var effective = await player.diagnostics()
        for _ in 0..<50 {
            if effective?.mode == .original && effective?.configurationPending == false { break }
            try await Task.sleep(for: .milliseconds(10))
            effective = await player.diagnostics()
        }
        #expect(effective?.mode == .original)
        #expect(effective?.hasProcessor == false)
        #expect(effective?.playingIntent == false)
        #expect(effective?.configurationPending == false)
        #expect((effective?.fadeGain ?? 0) > 0)
        #expect(player.state == .paused)
        player.stop()
    }

    @Test func seekSupersedingEOFReassertsCurrentPlayIntentWithoutStartingOutput() async throws {
        let fixture = try Fixture(seconds: 0.1)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer(configuration: .init())
        try await player.load(fileURL: fixture.url)
        player.pause()
        // This entire sequence is synchronous on MainActor. The first play encounters EOF and
        // returns before engine.start(); its EOF event is stale after the final seek.
        player.seek(to: player.duration)
        player.play()
        player.seek(to: player.duration)
        for _ in 0..<50 {
            if player.state == .ended { break }
            try await Task.sleep(for: .milliseconds(10))
        }
        #expect(player.state == .ended)
        #expect(player.position == player.duration)
        #expect(!player.isPlaying)
        let effective = await player.diagnostics()
        #expect(effective?.playingIntent == false)
        player.stop()
    }

    @Test func pauseAfterSupersedingEOFCannotResumeTheWorker() async throws {
        let fixture = try Fixture(seconds: 0.1)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer(configuration: .init())
        try await player.load(fileURL: fixture.url)
        player.seek(to: player.duration)
        player.play()
        player.seek(to: player.duration)
        player.pause()
        try await Task.sleep(for: .milliseconds(100))
        #expect(player.state == .paused)
        #expect(!player.isPlaying)
        let effective = await player.diagnostics()
        #expect(effective?.playingIntent == false)
        player.stop()
    }

    @Test func cancelledLoadCannotInstallItsWorker() async throws {
        let fixture = try Fixture(seconds: 1)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer()
        let load = Task { @MainActor in try await player.load(fileURL: fixture.url) }
        load.cancel()
        do { try await load.value; Issue.record("Cancelled load succeeded") }
        catch { #expect(error is CancellationError) }
        #expect(player.state == .idle)
        #expect(player.fileURL == nil)
    }

    @Test func lastOverlappingLoadOwnsTheSource() async throws {
        let first = try Fixture(seconds: 0.2)
        let second = try Fixture(seconds: 0.5)
        defer { first.cleanUp(); second.cleanUp() }
        let player = LocalFilePlayer(configuration: .init())
        let earlier = Task { @MainActor in try await player.load(fileURL: first.url) }
        await Task.yield()
        try await player.load(fileURL: second.url)
        _ = try? await earlier.value
        try await Task.sleep(for: .milliseconds(300))
        #expect(player.fileURL == second.url)
        #expect(player.duration == 0.5)
        #expect(player.state == .ready)
        player.stop()
    }

    @Test func stopDuringLoadCannotBeReplacedByStaleReady() async throws {
        let fixture = try Fixture(seconds: 1)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer(configuration: .init())
        let load = Task { @MainActor in try await player.load(fileURL: fixture.url) }
        while player.state == .idle { await Task.yield() }
        player.stop()
        _ = try? await load.value
        try await Task.sleep(for: .milliseconds(300))
        #expect(player.state == .idle)
        #expect(player.fileURL == nil)
        #expect(player.position == 0)
    }

    @Test func releasingPreparedPlayerDoesNotRetainIt() async throws {
        let fixture = try Fixture(seconds: 0.1)
        defer { fixture.cleanUp() }
        weak var reference: LocalFilePlayer?
        do {
            let player = LocalFilePlayer(configuration: .init())
            reference = player
            try await player.load(fileURL: fixture.url)
            player.pause()
        }
        try await Task.sleep(for: .milliseconds(350))
        #expect(reference == nil)
    }

    @Test func recoveryPreservesOriginalPositionAndPreferences() async throws {
        let fixture = try Fixture(seconds: 1)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer(configuration: .init(rate: 1.5))
        player.setMode(.original)
        try await player.load(fileURL: fixture.url)
        player.pause()
        player.seek(to: 0.6)
        try await player.recoverAfterMediaServicesReset()
        try await Task.sleep(for: .milliseconds(350))
        #expect(player.state == .paused)
        #expect(player.mode == .original)
        #expect(player.configuration.rate == 1.5)
        #expect(abs(player.position - 0.6) < 0.001)
        #expect(player.silenceSaved == 0)
        #expect(player.effectiveRate == 1)
        player.stop()
    }

    @Test func surroundCenterChannelCanBePrepared() async throws {
        let fixture = try Fixture(seconds: 0.1, surround: true)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer(configuration: .init())
        player.setMode(.original)
        try await player.load(fileURL: fixture.url)
        #expect(player.state == .ready)
        #expect(player.duration == 0.1)
        #expect(player.silenceSaved == 0)
        player.stop()
    }

    @Test func downmixPreservesCenterSpeech() throws {
        let fixture = try Fixture(seconds: 0.1, surround: true)
        defer { fixture.cleanUp() }
        let file = try AVAudioFile(forReading: fixture.url, commonFormat: .pcmFormatFloat32, interleaved: false)
        let input = AVAudioPCMBuffer(pcmFormat: file.processingFormat, frameCapacity: 4_800)!
        try file.read(into: input)
        let stereo = AVAudioFormat(standardFormatWithSampleRate: 48_000,
                                  channelLayout: AVAudioChannelLayout(layoutTag: kAudioChannelLayoutTag_Stereo)!)
        let downmixer = try FileDownmixer(source: file.processingFormat, destination: stereo, capacity: 4_800)
        let output = try downmixer.convert(input)
        #expect(output.frameLength == input.frameLength)
        for channel in 0..<2 {
            let samples = UnsafeBufferPointer(start: output.floatChannelData![channel], count: Int(output.frameLength))
            #expect(samples.contains { abs($0) > 0.01 })
        }
    }

    @Test func malformedAndEmptyFilesFailWithTypedOpenErrors() async throws {
        let fixture = try Fixture(seconds: 0)
        defer { fixture.cleanUp() }
        let player = LocalFilePlayer()
        do { try await player.load(fileURL: fixture.url); Issue.record("Empty audio was accepted") }
        catch { if case .open = error as? PlayerFailure {} else { Issue.record("Expected open failure: \(error)") } }
        if case .failed = player.state {} else { Issue.record("Missing failure state") }
        player.stop()
        try Data("not audio".utf8).write(to: fixture.url)
        do { try await player.load(fileURL: fixture.url); Issue.record("Malformed audio was accepted") }
        catch { if case .open = error as? PlayerFailure {} else { Issue.record("Expected open failure: \(error)") } }
        player.stop()
    }

    @Test func unheardRateFollowsPreferencesThroughSeekRecoveryFailureAndReload() async throws {
        let fixture = try Fixture(seconds: 1)
        let empty = try Fixture(seconds: 0)
        defer { fixture.cleanUp(); empty.cleanUp() }
        let player = LocalFilePlayer(configuration: .init(rate: 1.5))
        #expect(player.effectiveRate == 1.5)
        try await player.load(fileURL: fixture.url)
        var configuration = player.configuration
        configuration.rate = 2
        try player.configure(configuration)
        player.pause()
        player.seek(to: 0.7)
        try await Task.sleep(for: .milliseconds(300))
        #expect(player.effectiveRate == 2)
        try await player.recoverAfterMediaServicesReset()
        #expect(player.effectiveRate == 2)
        #expect(abs(player.position - 0.7) < 0.001)
        do { try await player.load(fileURL: empty.url); Issue.record("Empty audio was accepted") }
        catch { #expect(player.effectiveRate == 2) }
        try await player.load(fileURL: fixture.url)
        #expect(player.effectiveRate == 2)
        #expect(player.position == 0)
        player.stop()
        #expect(player.effectiveRate == 2)
        player.setMode(.original)
        #expect(player.effectiveRate == 1)
    }
}

/// Deterministic heard-clock probes exercise metadata without requiring audio hardware.
@Suite struct EffectivePlaybackRateTests {
    @Test func rateChangesUseHeardMetadataAndAgeOutOfTheRecentWindow() {
        var window = EffectivePlaybackRateWindow()
        window.reset(sampleRate: 100, sourceFrame: 0)
        #expect(window.rate == nil)
        window.record(outputFrame: 100, sourceFrame: 100) // One heard second at 1x.
        window.record(outputFrame: 200, sourceFrame: 300) // One heard second at 2x.
        #expect(window.rate == 1.5)
        window.record(outputFrame: 250, sourceFrame: 400)
        #expect(window.rate == 1.6)
        for frame in stride(from: 300, through: 500, by: 50) {
            window.record(outputFrame: Int64(frame), sourceFrame: Double(frame * 2 - 100))
        }
        #expect(window.rate == 2)
    }

    @Test func pausedClockAndQueuedSilenceCannotIncreaseRate() throws {
        let timeline = try AudioTimeline()
        let chunk = ProcessedAudio(channels: [[Float](repeating: 0, count: 300)],
            sourceFrames: (0..<300).map { Double($0 < 150 ? $0 : $0 + 300) },
            silenceSavedFrames: (0..<300).map { $0 < 150 ? 0 : 300 })
        try timeline.append(chunk)
        var window = EffectivePlaybackRateWindow()
        window.reset(sampleRate: 100, sourceFrame: 0)
        window.record(outputFrame: 100, sourceFrame: timeline.original(at: 100))
        #expect(window.rate == 1)
        // Repeated snapshots while paused cannot consume the queued cut at output frame 150.
        for _ in 0..<20 { window.record(outputFrame: 100, sourceFrame: timeline.original(at: 100)) }
        #expect(window.rate == 1)
        window.record(outputFrame: 200, sourceFrame: timeline.original(at: 200))
        #expect(window.rate == 2.5)
    }

    @Test func discreteCutIsSpreadOverTheRecentWindowAndThenExpires() {
        var window = EffectivePlaybackRateWindow()
        window.reset(sampleRate: 1_000, sourceFrame: 0)
        for frame in stride(from: 100, through: 2_500, by: 100) {
            window.record(outputFrame: Int64(frame), sourceFrame: Double(frame))
        }
        #expect(window.rate == 1)
        window.record(outputFrame: 2_600, sourceFrame: 5_600) // Three source seconds skipped.
        #expect(window.rate == 2.2)
        // One more heard frame cannot turn the cut into an instantaneous thousands-x spike.
        window.record(outputFrame: 2_601, sourceFrame: 5_601)
        #expect(abs((window.rate ?? 0) - 2.2) < 0.001)
        for frame in stride(from: 2_700, through: 5_200, by: 100) {
            window.record(outputFrame: Int64(frame), sourceFrame: Double(frame + 3_000))
        }
        #expect(window.rate == 1)
    }

    @Test func seekAndGenerationResetsExcludeSourceJumpsAndPreviousRates() {
        var window = EffectivePlaybackRateWindow()
        window.reset(sampleRate: 100, sourceFrame: 0)
        window.record(outputFrame: 100, sourceFrame: 300)
        #expect(window.rate == 3)
        // A seek/rebuild starts a new output generation at the actual source position.
        window.reset(sampleRate: 100, sourceFrame: 90_000)
        #expect(window.rate == nil)
        window.record(outputFrame: 100, sourceFrame: 90_150)
        #expect(window.rate == 1.5)
        window.reset(sampleRate: 100, sourceFrame: 200)
        window.record(outputFrame: 100, sourceFrame: 300)
        #expect(window.rate == 1)
    }

    @Test func realProcessedMetadataCombinesSpeedAndHeardSilenceSavings() throws {
        let sampleRate = 24_000.0
        let tone = (0..<24_000).map { Float(0.2 * sin(2 * .pi * 240 * Double($0) / sampleRate)) }
        let input = tone + [Float](repeating: 0, count: 48_000) + tone
        let processor = try AudioProcessor(sampleRate: sampleRate, channelCount: 1,
            configuration: .init(rate: 1.5, shortenSilence: true))
        let timeline = try AudioTimeline()
        try timeline.append(processor.process([input]))
        try timeline.append(processor.finish())
        var window = EffectivePlaybackRateWindow()
        window.reset(sampleRate: sampleRate, sourceFrame: 0)
        let beforeCut: Int64 = 4_000
        window.record(outputFrame: beforeCut, sourceFrame: timeline.original(at: beforeCut))
        #expect(abs((window.rate ?? 0) - 1.5) < 0.01)
        #expect(timeline.silenceSaved(at: beforeCut) == 0)
        let heard = timeline.outputEnd
        window.record(outputFrame: heard, sourceFrame: timeline.original(at: heard))
        #expect(timeline.silenceSaved(at: heard) > 20_000)
        #expect((window.rate ?? 0) > 1.5)
        #expect(abs((window.rate ?? 0) - timeline.original(at: heard) / Double(heard)) < 0.001)
    }
}

/// Owns only its uniquely named deterministic test directory. Tests never start audio output.
private struct Fixture {
    let directory: URL
    let url: URL
    init(seconds: Double, surround: Bool = false) throws {
        directory = FileManager.default.temporaryDirectory.appendingPathComponent("SidebandAudioPlayerTests-\(UUID().uuidString)", isDirectory: true)
        url = directory.appendingPathComponent("fixture.caf")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        let layout = AVAudioChannelLayout(layoutTag: surround ? kAudioChannelLayoutTag_MPEG_5_1_A : kAudioChannelLayoutTag_Mono)!
        let format = AVAudioFormat(standardFormatWithSampleRate: 48_000, channelLayout: layout)
        let file = try AVAudioFile(forWriting: url, settings: format.settings, commonFormat: .pcmFormatFloat32, interleaved: false)
        let frames = AVAudioFrameCount(seconds * 48_000)
        if frames > 0 {
            let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: frames)!
            buffer.frameLength = frames
            let channels = buffer.floatChannelData!
            for channel in 0..<Int(format.channelCount) {
                for frame in 0..<Int(frames) {
                    channels[channel][frame] = (surround && channel != 2) ? 0 : Float(sin(Double(frame) * 0.03) * 0.2)
                }
            }
            try file.write(from: buffer)
        }
    }
    func cleanUp() {
        let manager = FileManager.default
        guard directory.isFileURL, directory.path.hasPrefix("/"),
              directory.deletingLastPathComponent().standardizedFileURL == manager.temporaryDirectory.standardizedFileURL,
              directory.lastPathComponent.hasPrefix("SidebandAudioPlayerTests-"),
              manager.fileExists(atPath: directory.path) else { return }
        try? manager.removeItem(at: directory)
    }
}
