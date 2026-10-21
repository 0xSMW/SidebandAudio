import Foundation
import SBEngine

/// Serial-owner clock history. Savings advance with audible playback; reset discards queued savings.
/// Positions and savings are measured in original-media frames, independent of the output rate.
public final class AudioTimeline {
    private let timeline: OpaquePointer
    private var lastSource: Double?
    private var lastSaved: Double?
    public init() throws {
        guard let timeline = sb_timeline_create() else { throw AudioProcessingError.allocationFailed }
        self.timeline = timeline
    }
    deinit { sb_timeline_destroy(timeline) }
    public var outputEnd: Int64 { sb_timeline_output_end(timeline) }
    public var lastPlayed: Int64 { sb_timeline_last_played(timeline) }
    public var baseOriginal: Int64 { sb_timeline_base_original(timeline) }
    public var savedBefore: Double { sb_timeline_saved_before(timeline) }
    /// Negative original positions clamp to zero, matching the engine's C clock convention.
    public func reset(at originalFrame: Int64) {
        sb_timeline_reset(timeline, max(0, originalFrame))
        lastSource = nil; lastSaved = nil
    }
    /// Clamps to queued output, so queued-but-unheard silence never becomes audible savings.
    @discardableResult public func played(_ outputFrame: Int64) -> Int64 {
        sb_timeline_played(timeline, min(max(0, outputFrame), outputEnd))
    }
    public func append(_ chunk: ProcessedAudio) throws {
        guard (1...2).contains(chunk.channels.count),
              chunk.channels.allSatisfy({ $0.count == chunk.frameCount }) else { throw AudioProcessingError.invalidInput }
        try chunk.sourceFrames.withUnsafeBufferPointer { source in
            try chunk.silenceSavedFrames.withUnsafeBufferPointer { saved in
                try append(source: source, saved: saved)
            }
        }
    }
    public func append(_ view: ProcessedAudioView) throws {
        guard view.left.count == view.frameCount, view.right.count == view.frameCount else {
            throw AudioProcessingError.invalidInput
        }
        try append(source: view.sourceFrames, saved: view.silenceSavedFrames)
    }
    private func append(source: UnsafeBufferPointer<Double>, saved: UnsafeBufferPointer<Double>) throws {
        guard source.count == saved.count, Int64(source.count) <= Int64.max - outputEnd else {
            throw AudioProcessingError.invalidInput
        }
        guard !source.isEmpty else { return }
        var previousSource = lastSource ?? Double(baseOriginal)
        var previousSaved = lastSaved ?? 0
        for index in source.indices {
            guard source[index].isFinite, saved[index].isFinite,
                  source[index] >= previousSource, saved[index] >= previousSaved else {
                throw AudioProcessingError.invalidInput
            }
            previousSource = source[index]; previousSaved = saved[index]
        }
        guard sb_timeline_append(timeline, source.baseAddress, saved.baseAddress, source.count) != 0 else {
            throw AudioProcessingError.allocationFailed
        }
        lastSource = previousSource; lastSaved = previousSaved
    }
    public func original(at outputFrame: Int64) -> Double { sb_timeline_original_at(timeline, outputFrame) }
    public func silenceSaved(at outputFrame: Int64) -> Double { sb_timeline_saved_at(timeline, outputFrame) }
}
