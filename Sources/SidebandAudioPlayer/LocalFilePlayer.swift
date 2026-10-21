import Foundation
import Observation
@preconcurrency import AVFoundation
import SidebandAudio

public enum PlaybackMode: String, Codable, CaseIterable, Sendable {
    case original, processed
}

public enum PlayerFailure: Error, Equatable, Sendable, LocalizedError {
    case invalidFileURL
    case open(String)
    case processing(AudioProcessingError)
    case output(String)

    public var errorDescription: String? {
        switch self {
        case .invalidFileURL: "A local file URL is required."
        case .open(let message): "Cannot open audio: \(message)"
        case .processing(let error): error.localizedDescription
        case .output(let message): "Audio output failed: \(message)"
        }
    }
}

public enum PlaybackState: Equatable, Sendable {
    case idle, loading, ready, playing, paused, ended
    case failed(PlayerFailure)
}

/// Optional AVFoundation output for local files. The host owns AVAudioSession, interruptions,
/// route policy and Now Playing. Position and duration always use original-media seconds.
@MainActor @Observable
public final class LocalFilePlayer {
    public private(set) var configuration: AudioConfiguration
    public private(set) var state: PlaybackState = .idle
    public private(set) var position: TimeInterval = 0
    public private(set) var duration: TimeInterval = 0
    public private(set) var silenceSaved: TimeInterval = 0
    public private(set) var sampleRate: Double = 0
    public private(set) var measuredLoudness: Double?
    public private(set) var fileURL: URL?
    public private(set) var mode: PlaybackMode = .processed
    public var isPlaying: Bool { state == .playing }

    @ObservationIgnored private var worker: FileWorker?
    @ObservationIgnored private var progressTask: Task<Void, Never>?
    @ObservationIgnored private var generation: UInt64 = 0
    @ObservationIgnored private var wantsPlaying = false
    @ObservationIgnored private var commandRevision: UInt64 = 0

    /// Invalid initial configurations are reported on load; configure(_:) validates synchronously.
    public init(configuration: AudioConfiguration = .speech) {
        self.configuration = configuration
    }

    deinit {
        progressTask?.cancel()
        worker?.shutdown()
    }

    /// Replaces the source. Cancellation or a newer load prevents the old result being installed.
    public func load(fileURL: URL) async throws {
        guard fileURL.isFileURL else { throw PlayerFailure.invalidFileURL }
        let accepted = try configuration.validated()
        generation &+= 1
        let current = generation
        commandRevision &+= 1
        let revision = commandRevision
        progressTask?.cancel()
        progressTask = nil
        worker?.shutdown()
        worker = nil
        wantsPlaying = false
        self.fileURL = fileURL
        position = 0; duration = 0; silenceSaved = 0; sampleRate = 0; measuredLoudness = nil
        state = .loading
        do {
            try Task.checkCancellation()
            let newWorker = try await FileWorker.open(url: fileURL, configuration: accepted, mode: mode, revision: revision) { [weak self] event in
                Task { @MainActor [weak self] in
                    guard let self, self.generation == current else { return }
                    self.receive(event)
                }
            }
            let snapshot = await newWorker.snapshot()
            guard current == generation, !Task.isCancelled else {
                newWorker.shutdown()
                throw CancellationError()
            }
            worker = newWorker
            apply(snapshot)
            // Edits made while the file was opening take effect before output can begin.
            newWorker.update(configuration: configuration, mode: mode, revision: commandRevision)
            state = .ready
            startProgress(generation: current)
        } catch {
            guard current == generation else { throw CancellationError() }
            if error is CancellationError {
                stop()
                throw error
            }
            let failure = Self.failure(error, opening: true)
            state = .failed(failure)
            throw failure
        }
    }

    /// Accepted preferences publish immediately. Later output/DSP failures appear in state.
    public func configure(_ configuration: AudioConfiguration) throws {
        let accepted = try configuration.validated()
        self.configuration = accepted
        worker?.update(configuration: accepted, mode: mode, revision: commandRevision)
    }

    /// Original plays decoded PCM at 1x without creating or calling the C processor/timeline.
    /// Preferences remain editable and are restored when returning to Processed.
    public func setMode(_ mode: PlaybackMode) {
        guard self.mode != mode else { return }
        self.mode = mode
        worker?.update(configuration: configuration, mode: mode, revision: commandRevision)
    }

    public func seek(to seconds: TimeInterval) {
        guard let worker else { return }
        position = min(duration, max(0, seconds.isFinite ? seconds : 0))
        if state == .ended { state = wantsPlaying ? .playing : .paused }
        commandRevision &+= 1
        worker.seek(to: position, revision: commandRevision)
    }

    public func play() {
        guard let worker else { return }
        if case .failed = state { return }
        commandRevision &+= 1
        if state == .ended { position = 0; worker.seek(to: 0, revision: commandRevision) }
        wantsPlaying = true
        state = .playing
        worker.play(revision: commandRevision)
    }

    public func pause() {
        guard let worker else { return }
        if case .failed = state { return }
        commandRevision &+= 1
        wantsPlaying = false
        if state != .ended { state = .paused }
        worker.pause(revision: commandRevision)
    }

    /// Unloads the source, releases output resources and returns to idle with zero counters.
    public func stop() {
        generation &+= 1
        progressTask?.cancel(); progressTask = nil
        worker?.shutdown(); worker = nil
        wantsPlaying = false
        fileURL = nil; position = 0; duration = 0; silenceSaved = 0; sampleRate = 0; measuredLoudness = nil
        state = .idle
    }

    /// Invoke after the host has reactivated its audio session following a media-services reset.
    public func recoverAfterMediaServicesReset() async throws {
        guard let source = fileURL else { return }
        let recoveryGeneration = generation
        let recoveryRevision = commandRevision
        if let worker {
            let snapshot = await worker.snapshot()
            guard recoveryGeneration == generation, recoveryRevision == commandRevision else { throw CancellationError() }
            apply(snapshot)
        }
        let savedPosition = position
        let savedSavings = silenceSaved
        let intent = wantsPlaying
        try await load(fileURL: source)
        commandRevision &+= 1
        worker?.restore(position: savedPosition, saved: savedSavings, revision: commandRevision)
        position = savedPosition; silenceSaved = savedSavings
        if intent { play() }
        else { state = .paused }
    }

    private func startProgress(generation current: UInt64) {
        progressTask = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                await self?.refresh(generation: current)
                try? await Task.sleep(for: .milliseconds(250))
            }
        }
    }

    private func refresh(generation current: UInt64) async {
        guard generation == current, let worker else { return }
        let revision = commandRevision
        let snapshot = await worker.snapshot()
        guard generation == current, commandRevision == revision, !Task.isCancelled else { return }
        apply(snapshot)
    }

    private func apply(_ snapshot: FileSnapshot) {
        position = snapshot.position; duration = snapshot.duration; sampleRate = snapshot.sampleRate
        silenceSaved = snapshot.saved; measuredLoudness = snapshot.loudness
    }

    /// Internal inspection of the serial worker's effective state, including pending transitions.
    func diagnostics() async -> PlayerDiagnostics? {
        guard let worker else { return nil }
        return await worker.diagnostics()
    }

    private func receive(_ event: FileEvent) {
        switch event {
        case .ended(let snapshot, let revision):
            guard wantsPlaying else { return }
            guard revision == commandRevision else {
                // A seek can supersede an EOF callback after the worker clears its old intent.
                // Reassert the current main-actor intent; paused/unloaded sources never reach here.
                worker?.play(revision: commandRevision)
                return
            }
            apply(snapshot); wantsPlaying = false; state = .ended
        case .failed(let error, let snapshot, _):
            // Worker failure is terminal for this loaded source. The load generation gates it;
            // subsequent pause/settings commands cannot revive a failed output graph.
            apply(snapshot); wantsPlaying = false; state = .failed(error)
            progressTask?.cancel(); progressTask = nil
            worker?.shutdown(); worker = nil
        }
    }

    nonisolated fileprivate static func failure(_ error: Error, opening: Bool = false) -> PlayerFailure {
        if let error = error as? PlayerFailure { return error }
        if let error = error as? AudioProcessingError { return .processing(error) }
        return opening ? .open(error.localizedDescription) : .output(error.localizedDescription)
    }
}

struct PlayerDiagnostics: Sendable {
    let mode: PlaybackMode
    let hasProcessor: Bool
    let playingIntent: Bool
    let configurationPending: Bool
    let fadeGain: Float
}

private struct FileSnapshot: Sendable {
    let position: Double
    let duration: Double
    let sampleRate: Double
    let saved: Double
    let loudness: Double?
}

private enum FileEvent: Sendable {
    case ended(FileSnapshot, UInt64)
    case failed(PlayerFailure, FileSnapshot, UInt64)
}

/// All mutable AVFoundation, DSP and timeline state belongs exclusively to queue, including
/// construction and teardown. Only immutable Sendable snapshots and commands cross executors.
/// @unchecked is required because AVFoundation and the deliberately serial DSP API are non-Sendable.
private final class FileWorker: @unchecked Sendable {
    private let queue: DispatchQueue
    private let engine = AVAudioEngine()
    private let node = AVAudioPlayerNode()
    private let file: AVAudioFile
    private let sourceFormat: AVAudioFormat
    private let format: AVAudioFormat
    private let readBuffer: AVAudioPCMBuffer
    private let downmixer: FileDownmixer?
    private let sampleRate: Double
    private let totalFrames: Int64
    private let chunkFrames: AVAudioFrameCount
    private var configuration: AudioConfiguration
    private var mode: PlaybackMode
    private var requestedMode: PlaybackMode
    private var processor: AudioProcessor?
    private var timeline: AudioTimeline?
    private var observer: NSObjectProtocol?
    private let event: @Sendable (FileEvent) -> Void
    private var wantsPlaying = false
    private var acceptingClock = false
    private var generation: UInt64 = 0
    private var configurationRevision: UInt64 = 0
    private var configurationPending = false
    private var commandRevision: UInt64
    private var nextRead: Int64 = 0
    private var baseOriginal: Int64 = 0
    private var scheduled: Int64 = 0
    private var lastPlayed: Int64 = 0
    private var pending = 0
    // One-buffer lookahead makes the actual final PCM buffer a played-back completion.
    private var pendingTail: AVAudioPCMBuffer?
    private var eof = false
    private var ended = false
    private var closed = false
    private var failed = false
    private var savedBefore: Double = 0
    private var loudness: Double?
    private var fadeToken: UInt64 = 0
    private var fadeGain: Float = 1

    static func open(url: URL, configuration: AudioConfiguration, mode: PlaybackMode, revision: UInt64,
                     event: @escaping @Sendable (FileEvent) -> Void) async throws -> FileWorker {
        let queue = DispatchQueue(label: "audio.sideband.local-file", qos: .userInitiated)
        return try await withCheckedThrowingContinuation { continuation in
            queue.async {
                do { continuation.resume(returning: try FileWorker(queue: queue, url: url, configuration: configuration, mode: mode, revision: revision, event: event)) }
                catch { continuation.resume(throwing: error) }
            }
        }
    }

    private init(queue: DispatchQueue, url: URL, configuration: AudioConfiguration, mode: PlaybackMode, revision: UInt64,
                 event: @escaping @Sendable (FileEvent) -> Void) throws {
        self.queue = queue; self.configuration = configuration; self.mode = mode; self.event = event
        requestedMode = mode
        commandRevision = revision
        file = try AVAudioFile(forReading: url, commonFormat: .pcmFormatFloat32, interleaved: false)
        sourceFormat = file.processingFormat
        sampleRate = sourceFormat.sampleRate
        totalFrames = file.length
        guard sampleRate.isFinite, (8_000...192_000).contains(sampleRate), totalFrames > 0,
              sourceFormat.channelCount > 0 else { throw PlayerFailure.open("Unsupported or empty audio file.") }
        chunkFrames = AVAudioFrameCount(min(4096, sampleRate * 0.04))
        guard let layout = AVAudioChannelLayout(layoutTag: sourceFormat.channelCount == 1 ? kAudioChannelLayoutTag_Mono : kAudioChannelLayoutTag_Stereo),
              let buffer = AVAudioPCMBuffer(pcmFormat: sourceFormat, frameCapacity: chunkFrames) else {
            throw PlayerFailure.open("Invalid channel layout.")
        }
        format = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channelLayout: layout)
        readBuffer = buffer
        downmixer = sourceFormat.channelCount > 2 ? try FileDownmixer(source: sourceFormat, destination: format, capacity: chunkFrames) : nil
        if mode == .processed {
            processor = try AudioProcessor(sampleRate: sampleRate, channelCount: Int(format.channelCount), configuration: configuration)
            timeline = try AudioTimeline()
        }
        engine.attach(node)
        connectGraph()
        #if os(iOS)
        engine.isAutoShutdownEnabled = false
        #endif
        // Validate decoding while paused without starting the hardware output.
        try reset(at: 0, preservingSavings: false)
        try fill()
        observer = NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: nil) { [weak self] _ in
            guard let self else { return }
            self.queue.async { [weak self] in self?.rebuild() }
        }
    }

    // shutdown's queue closure owns the final reference until resources have been stopped.
    func shutdown() {
        queue.async { [self] in
            guard !closed else { return }
            closed = true; generation &+= 1; configurationRevision &+= 1; fadeToken &+= 1
            wantsPlaying = false; acceptingClock = false
            if let observer { NotificationCenter.default.removeObserver(observer) }
            observer = nil
            node.stop(); engine.stop()
            engine.disconnectNodeOutput(node); engine.detach(node)
            processor = nil; timeline = nil; pendingTail = nil
        }
    }

    func diagnostics() async -> PlayerDiagnostics {
        await withCheckedContinuation { continuation in
            queue.async { [self] in
                continuation.resume(returning: PlayerDiagnostics(mode: mode, hasProcessor: processor != nil,
                    playingIntent: wantsPlaying, configurationPending: configurationPending, fadeGain: fadeGain))
            }
        }
    }

    func snapshot() async -> FileSnapshot {
        await withCheckedContinuation { continuation in
            queue.async { [self] in continuation.resume(returning: makeSnapshot()) }
        }
    }

    func play(revision: UInt64) {
        queue.async { [self] in
            guard !closed, !failed else { return }
            commandRevision = revision
            wantsPlaying = true
            do {
                try fill()
                if eof && pending == 0 { ended = false; finish(); return }
                if !engine.isRunning { try engine.start() }
                node.play(); acceptingClock = true
                ramp(to: 1, duration: 0.05)
            } catch { fail(error) }
        }
    }

    func pause(revision: UInt64) {
        queue.async { [self] in
            guard !closed, !failed else { return }
            commandRevision = revision
            _ = playedOutput()
            wantsPlaying = false; acceptingClock = false
            fadeToken &+= 1
            node.pause(); engine.pause()
            setFade(1)
        }
    }

    func seek(to seconds: Double, revision: UInt64) {
        queue.async { [self] in
            guard !closed, !failed else { return }
            commandRevision = revision
            do { try restart(at: frame(for: seconds)) } catch { fail(error) }
        }
    }

    func restore(position: Double, saved: Double, revision: UInt64) {
        queue.async { [self] in
            guard !closed, !failed else { return }
            do {
                commandRevision = revision
                try restart(at: frame(for: position))
                savedBefore = saved.isFinite ? max(0, saved * sampleRate) : 0
            } catch { fail(error) }
        }
    }

    func update(configuration updated: AudioConfiguration, mode updatedMode: PlaybackMode, revision: UInt64) {
        queue.async { [self] in
            guard !closed, !failed else { return }
            commandRevision = revision
            requestedMode = updatedMode
            // Invalidate even an apparently redundant edit: it can supersede a pending mode change.
            configurationRevision &+= 1
            guard updated != configuration || updatedMode != mode || configurationPending else { return }
            var withoutRate = updated
            let active = processor?.configuration ?? configuration
            withoutRate.rate = active.rate
            let onlyRate = !configurationPending && mode == updatedMode && withoutRate == active
            configuration = updated
            if mode == .original && updatedMode == .original {
                // A quick return to Original can supersede a pending Processed switch.
                // Cancel its regeneration and undo any fade already started for it.
                configurationPending = false
                ramp(to: 1, duration: wantsPlaying ? 0.03 : 0)
                return
            }
            if onlyRate {
                do { if !eof { try processor?.configure(updated) } }
                catch { fail(error) }
                return
            }
            configurationRevision &+= 1
            configurationPending = true
            let revision = configurationRevision
            // Stop regenerating only after the short fade, so source time is the last audible time.
            ramp(to: 0, duration: wantsPlaying ? 0.03 : 0)
            queue.asyncAfter(deadline: .now() + (wantsPlaying ? 0.03 : 0)) { [self] in
                guard !closed, !failed, configurationRevision == revision else { return }
                let frame = frame(for: makeSnapshot().position)
                do {
                    let heard = savedFrames()
                    try restart(at: frame, saved: heard)
                } catch { fail(error) }
            }
        }
    }

    private func frame(for seconds: Double) -> Int64 {
        Int64(min(Double(totalFrames), max(0, seconds.isFinite ? seconds * sampleRate : 0)))
    }

    private func connectGraph() {
        engine.disconnectNodeOutput(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        engine.prepare()
    }

    private func playedOutput() -> Int64 {
        if acceptingClock, let render = node.lastRenderTime, render.isSampleTimeValid,
           let time = node.playerTime(forNodeTime: render), time.isSampleTimeValid {
            lastPlayed = min(scheduled, max(lastPlayed, max(0, time.sampleTime)))
        }
        if let timeline { return timeline.played(lastPlayed) }
        return lastPlayed
    }

    private func savedFrames() -> Double {
        let played = playedOutput()
        return savedBefore + (timeline?.silenceSaved(at: played) ?? 0)
    }

    private func makeSnapshot() -> FileSnapshot {
        let played = playedOutput()
        let original = ended ? Double(totalFrames) : (timeline?.original(at: played) ?? Double(baseOriginal + played))
        return FileSnapshot(position: min(Double(totalFrames), max(0, original)) / sampleRate,
                            duration: Double(totalFrames) / sampleRate, sampleRate: sampleRate,
                            saved: (savedBefore + (timeline?.silenceSaved(at: played) ?? 0)) / sampleRate, loudness: loudness)
    }

    private func reset(at frame: Int64, preservingSavings: Bool = true, saved: Double? = nil) throws {
        let heard = saved ?? (preservingSavings ? savedFrames() : 0)
        acceptingClock = false
        node.stop()
        generation &+= 1; fadeToken &+= 1; configurationRevision &+= 1
        configurationPending = false
        scheduled = 0; pending = 0; pendingTail = nil; lastPlayed = 0; nextRead = frame; baseOriginal = frame
        eof = false; ended = false; savedBefore = heard
        downmixer?.reset()
        // Read heard savings using the old clock before adopting the latest requested mode.
        // A seek/rebuild may cancel the delayed transition, but must still apply its target.
        mode = requestedMode
        if mode == .processed {
            if processor == nil { processor = try AudioProcessor(sampleRate: sampleRate, channelCount: Int(format.channelCount), configuration: configuration) }
            // The outer accumulator owns heard savings; C reset retains its own history.
            timeline = try AudioTimeline()
            try processor?.reset(at: frame, seedLoudness: loudness)
            try processor?.configure(configuration)
            timeline?.reset(at: frame)
        } else { processor = nil; timeline = nil }
    }

    private func restart(at frame: Int64, saved: Double? = nil) throws {
        try reset(at: frame, saved: saved)
        setFade(wantsPlaying ? 0 : 1)
        try fill()
        if wantsPlaying {
            if eof && pending == 0 { finish(); return }
            if !engine.isRunning { try engine.start() }
            node.play(); acceptingClock = true
            ramp(to: 1, duration: 0.05)
        } else { engine.pause() }
    }

    private func rebuild() {
        guard !closed, !failed else { return }
        let frame = frame(for: makeSnapshot().position)
        let heard = savedFrames()
        acceptingClock = false
        engine.stop()
        connectGraph()
        do { try restart(at: frame, saved: heard) } catch { fail(error) }
    }

    private func fill() throws {
        guard !closed, !failed else { return }
        while !eof && scheduled - playedOutput() < Int64(sampleRate * 0.2) {
            if nextRead < totalFrames {
                file.framePosition = nextRead
                let requestedCount = AVAudioFrameCount(min(Int64(chunkFrames), totalFrames - nextRead))
                try file.read(into: readBuffer, frameCount: requestedCount)
                guard readBuffer.frameLength > 0, readBuffer.frameLength <= requestedCount else {
                    throw PlayerFailure.open("The decoder returned no samples before the end of the file.")
                }
                nextRead += Int64(readBuffer.frameLength)
                let input = try downmixer?.convert(readBuffer) ?? readBuffer
                guard let data = input.floatChannelData else { throw PlayerFailure.open("Invalid decoded samples.") }
                let count = Int(input.frameLength)
                if mode == .original {
                    for channel in 0..<Int(format.channelCount) {
                        for index in 0..<count where !data[channel][index].isFinite { throw PlayerFailure.open("Non-finite decoded samples.") }
                    }
                    try scheduleCopy(left: data[0], right: data[Int(format.channelCount) - 1], count: count)
                } else {
                    try processor?.process(left: data[0], right: data[Int(format.channelCount) - 1], frames: count) { [self] view in
                        try timeline?.append(view)
                        try scheduleCopy(left: view.left.baseAddress!, right: view.right.baseAddress!, count: view.frameCount)
                    }
                }
            } else {
                eof = true
                if mode == .processed {
                    try processor?.finish { [self] view in
                        try timeline?.append(view)
                        try scheduleCopy(left: view.left.baseAddress!, right: view.right.baseAddress!, count: view.frameCount)
                    }
                }
            }
        }
        if eof, let tail = pendingTail {
            pendingTail = nil
            schedule(tail, completionType: .dataPlayedBack)
        }
        if let measured = processor?.loudness { loudness = measured }
        if eof && pending == 0 && wantsPlaying { finish() }
    }

    private func scheduleCopy(left: UnsafePointer<Float>, right: UnsafePointer<Float>, count: Int) throws {
        guard count > 0 else { return }
        guard count <= Int(UInt32.max), let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(count)),
              let data = buffer.floatChannelData else { throw PlayerFailure.output("Cannot allocate an output buffer.") }
        buffer.frameLength = AVAudioFrameCount(count)
        data[0].update(from: left, count: count)
        if format.channelCount == 2 { data[1].update(from: right, count: count) }
        if let previous = pendingTail { schedule(previous, completionType: .dataRendered) }
        pendingTail = buffer
    }

    private func schedule(_ buffer: AVAudioPCMBuffer, completionType: AVAudioPlayerNodeCompletionCallbackType) {
        pending += 1; scheduled += Int64(buffer.frameLength)
        let current = generation
        // Intermediate rendered completions refill ahead of hardware latency. Only the actual
        // last buffer uses played-back completion, including when the final input produces no PCM.
        node.scheduleBuffer(buffer, completionCallbackType: completionType) { [weak self] _ in
            guard let self else { return }
            self.queue.async { [weak self] in
                guard let self, !self.closed, !self.failed, self.generation == current else { return }
                self.pending -= 1
                do { try self.fill() } catch { self.fail(error) }
            }
        }
    }

    private func finish() {
        guard !ended, !closed, !failed, wantsPlaying else { return }
        _ = playedOutput()
        lastPlayed = scheduled
        _ = timeline?.played(scheduled)
        ended = true; wantsPlaying = false; acceptingClock = false
        event(.ended(makeSnapshot(), commandRevision))
    }

    private func fail(_ error: Error) {
        guard !closed, !failed else { return }
        let snapshot = makeSnapshot()
        failed = true; wantsPlaying = false; acceptingClock = false
        generation &+= 1; configurationRevision &+= 1; fadeToken &+= 1
        node.stop(); engine.stop(); pendingTail = nil
        event(.failed(LocalFilePlayer.failure(error), snapshot, commandRevision))
    }

    private func setFade(_ value: Float) {
        fadeGain = value
        engine.mainMixerNode.outputVolume = value
    }

    private func ramp(to target: Float, duration: Double) {
        fadeToken &+= 1
        let token = fadeToken
        let start = fadeGain
        let steps = max(1, Int(duration / 0.005))
        rampStep(1, steps: steps, start: start, target: target, token: token)
    }

    private func rampStep(_ index: Int, steps: Int, start: Float, target: Float, token: UInt64) {
        guard !closed, fadeToken == token else { return }
        setFade(start + (target - start) * Float(index) / Float(steps))
        if index < steps {
            queue.asyncAfter(deadline: .now() + 0.005) { [weak self] in
                self?.rampStep(index + 1, steps: steps, start: start, target: target, token: token)
            }
        }
    }
}

/// AVAudioConverter uses the actual channel layout, preserving center speech in surround files.
final class FileDownmixer {
    private let converter: AVAudioConverter
    private let output: AVAudioPCMBuffer
    init(source: AVAudioFormat, destination: AVAudioFormat, capacity: AVAudioFrameCount) throws {
        guard let converter = AVAudioConverter(from: source, to: destination),
              let output = AVAudioPCMBuffer(pcmFormat: destination, frameCapacity: capacity) else {
            throw PlayerFailure.open("Unsupported channel conversion.")
        }
        converter.downmix = true
        self.converter = converter; self.output = output
    }
    func reset() { converter.reset() }
    func convert(_ input: AVAudioPCMBuffer) throws -> AVAudioPCMBuffer {
        try converter.convert(to: output, from: input)
        guard output.frameLength == input.frameLength else { throw PlayerFailure.open("Incomplete channel conversion.") }
        return output
    }
}
