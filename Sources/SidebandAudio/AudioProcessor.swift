import Foundation
import SBEngine

public struct ProcessedAudio: Sendable {
    public var channels: [[Float]]
    public var sourceFrames: [Double]
    public var silenceSavedFrames: [Double]
    public var frameCount: Int { sourceFrames.count }
    public init(channels: [[Float]], sourceFrames: [Double], silenceSavedFrames: [Double]) {
        self.channels = channels; self.sourceFrames = sourceFrames; self.silenceSavedFrames = silenceSavedFrames
    }
}

/// Borrowed buffers are valid only until the synchronous consume callback returns.
/// For mono input, right aliases left. Source and saved positions use original-media frames.
public struct ProcessedAudioView {
    public let left: UnsafeBufferPointer<Float>
    public let right: UnsafeBufferPointer<Float>
    public let sourceFrames: UnsafeBufferPointer<Double>
    public let silenceSavedFrames: UnsafeBufferPointer<Double>
    public var frameCount: Int { sourceFrames.count }
    public init(left: UnsafeBufferPointer<Float>, right: UnsafeBufferPointer<Float>,
                sourceFrames: UnsafeBufferPointer<Double>, silenceSavedFrames: UnsafeBufferPointer<Double>) {
        self.left = left; self.right = right; self.sourceFrames = sourceFrames; self.silenceSavedFrames = silenceSavedFrames
    }
}

/// A serial-owner processor. Call from one executor; concurrent and reentrant use is unsupported.
/// The pointer/callback path reuses construction-time output storage. The owned adapter copies output.
/// If consume throws, already delivered output and accepted input remain consumed; reset before retrying input.
public final class AudioProcessor {
    private let engine: OpaquePointer
    public let channelCount: Int
    public let sampleRate: Double
    public private(set) var configuration: AudioConfiguration
    private let capacity = 32_768
    private var left = [Float](repeating: 0, count: 32_768)
    private var right = [Float](repeating: 0, count: 32_768)
    private var source = [Double](repeating: 0, count: 32_768)
    private var saved = [Double](repeating: 0, count: 32_768)
    private var finishing = false
    private var busy = false

    public init(sampleRate: Double, channelCount: Int, configuration: AudioConfiguration = .init()) throws {
        guard sampleRate.isFinite, (8_000...192_000).contains(sampleRate), (1...2).contains(channelCount) else {
            throw AudioProcessingError.invalidFormat
        }
        self.configuration = try configuration.validated()
        self.sampleRate = sampleRate; self.channelCount = channelCount
        guard let engine = sb_engine_create(sampleRate, UInt32(channelCount), 32_768) else {
            throw AudioProcessingError.allocationFailed
        }
        self.engine = engine
        var config = configuration.engineConfiguration()
        guard sb_engine_configure(engine, &config) != 0 else {
            throw AudioProcessingError.invalidConfiguration("engine configuration")
        }
        sb_engine_reset(engine, 0)
    }
    deinit { sb_engine_destroy(engine) }
    public var loudness: Double? {
        let value = Double(sb_engine_loudness(engine))
        return value.isFinite && value > -150 ? value : nil
    }
    public var latencyFrames: Int { sb_engine_latency(engine) }
    /// True once all final output has been drained. Input is prohibited as soon as finish starts.
    public var isFinished: Bool { finishing && sb_engine_is_drained(engine) != 0 }
    public func configure(_ configuration: AudioConfiguration) throws {
        guard !busy else { throw AudioProcessingError.invalidInput }
        guard !finishing else { throw AudioProcessingError.finished }
        let valid = try configuration.validated()
        var config = valid.engineConfiguration()
        guard sb_engine_configure(engine, &config) != 0 else {
            throw AudioProcessingError.invalidConfiguration("engine configuration")
        }
        self.configuration = valid
    }
    public func reset(at originalFrame: Int64 = 0, seedLoudness: Double? = nil) throws {
        guard !busy, originalFrame >= 0,
              seedLoudness == nil || (seedLoudness!.isFinite && Float(seedLoudness!).isFinite) else {
            throw AudioProcessingError.invalidInput
        }
        sb_engine_reset(engine, originalFrame)
        if let seedLoudness { sb_engine_seed_loudness(engine, Float(seedLoudness)) }
        finishing = false
    }
    public func process(_ input: [[Float]]) throws -> ProcessedAudio {
        guard input.count == channelCount, let first = input.first,
              input.allSatisfy({ $0.count == first.count }) else { throw AudioProcessingError.invalidInput }
        var result = emptyChunk()
        try input[0].withUnsafeBufferPointer { l in
            try input[channelCount - 1].withUnsafeBufferPointer { r in
                try process(left: l.baseAddress, right: r.baseAddress, frames: first.count) { append($0, into: &result) }
            }
        }
        return result
    }
    public func process(left: UnsafePointer<Float>?, right: UnsafePointer<Float>?, frames: Int,
                        consume: (ProcessedAudioView) throws -> Void) throws {
        guard !busy else { throw AudioProcessingError.invalidInput }
        guard !finishing else { throw AudioProcessingError.finished }
        guard frames >= 0, frames == 0 || (left != nil && (channelCount == 1 || right != nil)) else {
            throw AudioProcessingError.invalidInput
        }
        // Validate before accepting any input, so malformed owned chunks do not partially mutate DSP.
        for index in 0..<frames {
            guard left![index].isFinite, channelCount == 1 || right![index].isFinite else {
                throw AudioProcessingError.invalidInput
            }
        }
        busy = true
        defer { busy = false }
        var offset = 0
        while offset < frames {
            let accepted = sb_engine_push(engine, left!.advanced(by: offset),
                                          (right ?? left!).advanced(by: offset), min(capacity, frames - offset))
            offset += accepted
            let produced = try drain(consume)
            if accepted == 0 && produced == 0 { throw AudioProcessingError.stalled }
        }
        _ = try drain(consume)
    }
    public func finish() throws -> ProcessedAudio {
        var result = emptyChunk()
        try finish { append($0, into: &result) }
        return result
    }
    /// Idempotent after draining; a later finish may resume draining after a throwing callback.
    public func finish(consume: (ProcessedAudioView) throws -> Void) throws {
        guard !busy else { throw AudioProcessingError.invalidInput }
        busy = true
        defer { busy = false }
        if !finishing { finishing = true; sb_engine_finish(engine) }
        while sb_engine_is_drained(engine) == 0 {
            if try drain(consume) == 0 && sb_engine_is_drained(engine) == 0 { throw AudioProcessingError.stalled }
        }
    }
    private func emptyChunk() -> ProcessedAudio {
        ProcessedAudio(channels: Array(repeating: [], count: channelCount), sourceFrames: [], silenceSavedFrames: [])
    }
    private func append(_ view: ProcessedAudioView, into result: inout ProcessedAudio) {
        result.channels[0].append(contentsOf: view.left)
        if channelCount == 2 { result.channels[1].append(contentsOf: view.right) }
        result.sourceFrames.append(contentsOf: view.sourceFrames)
        result.silenceSavedFrames.append(contentsOf: view.silenceSavedFrames)
    }
    @discardableResult private func drain(_ consume: (ProcessedAudioView) throws -> Void) throws -> Int {
        var total = 0
        while true {
            let count = try left.withUnsafeMutableBufferPointer { l in
                try right.withUnsafeMutableBufferPointer { r in
                    try source.withUnsafeMutableBufferPointer { s in
                        try saved.withUnsafeMutableBufferPointer { v in
                            let count = sb_engine_read(engine, l.baseAddress, r.baseAddress, s.baseAddress, v.baseAddress, capacity)
                            if count > 0 {
                                try consume(ProcessedAudioView(
                                    left: UnsafeBufferPointer(start: l.baseAddress, count: count),
                                    right: UnsafeBufferPointer(start: channelCount == 1 ? l.baseAddress : r.baseAddress, count: count),
                                    sourceFrames: UnsafeBufferPointer(start: s.baseAddress, count: count),
                                    silenceSavedFrames: UnsafeBufferPointer(start: v.baseAddress, count: count)))
                            }
                            return count
                        }
                    }
                }
            }
            guard count > 0 else { return total }
            total += count
        }
    }
}
