import Foundation
import Testing
import SBEngine
@testable import SidebandAudio

@Suite struct AudioProcessorTests {
    private let sampleRate = 24_000.0
    private func tone(_ count: Int) -> [Float] {
        (0..<count).map { Float(0.15 * sin(2 * .pi * 240 * Double($0) / sampleRate)) }
    }
    private func render(_ input: [[Float]], configuration: AudioConfiguration = .init(),
                        sizes: [Int] = [997, 4096, 173]) throws -> ProcessedAudio {
        let processor = try AudioProcessor(sampleRate: sampleRate, channelCount: input.count, configuration: configuration)
        var result = ProcessedAudio(channels: input.map { _ in [] }, sourceFrames: [], silenceSavedFrames: [])
        func append(_ part: ProcessedAudio) {
            for channel in input.indices { result.channels[channel] += part.channels[channel] }
            result.sourceFrames += part.sourceFrames; result.silenceSavedFrames += part.silenceSavedFrames
        }
        var offset = 0, index = 0
        while offset < input[0].count {
            let end = min(input[0].count, offset + sizes[index % sizes.count])
            append(try processor.process(input.map { Array($0[offset..<end]) }))
            offset = end; index += 1
        }
        append(try processor.finish())
        return result
    }
    @Test func tinyEOFAndNeutralPCM() throws {
        for count in [0, 1, 7, 148, 149, 4095, 40_001] {
            let input = tone(count)
            let output = try render([input])
            #expect(output.channels == [input])
            #expect(output.sourceFrames == (0..<count).map(Double.init))
            #expect(output.silenceSavedFrames.allSatisfy { $0 == 0 })
        }
    }
    @Test func neutralSettingsStillLimitUnsafePeaks() throws {
        let input = [Float](repeating: 4, count: 4000)
        let output = try render([input])
        #expect(output.frameCount == input.count)
        #expect(output.channels[0].allSatisfy { $0.isFinite && abs($0) <= 1 })
        #expect(output.channels[0].contains { abs($0) > 0.5 })
    }
    @Test func normalizationDoesNotDependOnVoiceEnhancement() throws {
        let input = tone(200).map { $0 * 0.001 }
        func probe(_ target: Double, enabled: Bool) throws -> [Float] {
            let processor = try AudioProcessor(sampleRate: sampleRate, channelCount: 1,
                configuration: .init(normalizationEnabled: enabled, normalizationTargetLUFS: target))
            try processor.reset(seedLoudness: -24)
            let first = try processor.process([input]); let last = try processor.finish()
            return first.channels[0] + last.channels[0]
        }
        let low = try probe(-20, enabled: true)
        let high = try probe(-8, enabled: true)
        func energy(_ samples: [Float]) -> Double { samples.reduce(0) { $0 + Double($1) * Double($1) } }
        #expect(abs(energy(high) / energy(low) - pow(10, 1.2)) < 0.01)
        #expect(try probe(-20, enabled: false) == probe(-8, enabled: false))
    }
    @Test func inputChunkBoundariesAndBackpressureDoNotChangeResult() throws {
        let input = tone(72_013) + [Float](repeating: 0, count: 24_000) + tone(36_017)
        let config = AudioConfiguration(rate: 0.5, shortenSilence: true)
        let whole = try render([input], configuration: config, sizes: [input.count])
        let fragments = try render([input], configuration: config, sizes: [1, 13, 4093, 97, 32_767])
        #expect(whole.channels == fragments.channels)
        #expect(whole.sourceFrames == fragments.sourceFrames)
        #expect(whole.silenceSavedFrames == fragments.silenceSavedFrames)
    }
    @Test func tempoPreservesPitchAndSeparatesSilenceSavings() throws {
        let input = tone(96_000)
        let fast = try render([input], configuration: .init(rate: 2))
        #expect(fast.frameCount == input.count / 2)
        #expect(fast.silenceSavedFrames.allSatisfy { $0 == 0 })
        let middle = Array(fast.channels[0].dropFirst(2400).dropLast(2400))
        let crossings = zip(middle, middle.dropFirst()).filter { $0 <= 0 && $1 > 0 }.count
        #expect(abs(Double(crossings) * sampleRate / Double(middle.count) - 240) < 8)
        let quiet = tone(24_000) + [Float](repeating: 0, count: 48_000) + tone(24_000)
        let shortened = try render([quiet], configuration: .init(shortenSilence: true))
        #expect(shortened.frameCount < quiet.count - 20_000)
        #expect((shortened.silenceSavedFrames.last ?? 0) > 20_000)
        #expect(zip(shortened.sourceFrames, shortened.sourceFrames.dropFirst()).allSatisfy { $0 <= $1 })
        #expect(zip(shortened.silenceSavedFrames, shortened.silenceSavedFrames.dropFirst()).allSatisfy { $0 <= $1 })
    }
    @Test func resetAndFinishedState() throws {
        let processor = try AudioProcessor(sampleRate: sampleRate, channelCount: 1)
        #expect(processor.latencyFrames > 0)
        #expect(!processor.isFinished)
        _ = try processor.process([tone(300)])
        _ = try processor.finish()
        #expect(processor.isFinished)
        #expect(try processor.finish().frameCount == 0)
        #expect(throws: AudioProcessingError.finished) { try processor.process([[]]) }
        #expect(throws: AudioProcessingError.finished) { try processor.configure(.speech) }
        try processor.reset(at: 90_000, seedLoudness: -24)
        #expect(!processor.isFinished)
        #expect(processor.loudness != nil)
        let first = try processor.process([tone(701)])
        let last = try processor.finish()
        #expect(first.sourceFrames + last.sourceFrames == (90_000..<90_701).map(Double.init))
        #expect((first.silenceSavedFrames + last.silenceSavedFrames).allSatisfy { $0 == 0 })
    }
    @Test func malformedInputAndAtomicConfigurationRejection() throws {
        #expect(throws: AudioProcessingError.invalidFormat) { try AudioProcessor(sampleRate: .nan, channelCount: 1) }
        #expect(throws: AudioProcessingError.invalidFormat) { try AudioProcessor(sampleRate: sampleRate, channelCount: 3) }
        let processor = try AudioProcessor(sampleRate: sampleRate, channelCount: 2)
        #expect(throws: AudioProcessingError.invalidInput) { try processor.process([[1], []]) }
        #expect(throws: AudioProcessingError.invalidInput) { try processor.process([[1, .nan], [1, 2]]) }
        #expect(throws: AudioProcessingError.invalidInput) {
            try processor.process(left: nil, right: nil, frames: 1) { _ in }
        }
        #expect(throws: AudioProcessingError.invalidInput) { try processor.reset(at: -1) }
        var invalid = AudioConfiguration.speech
        invalid.rate = 7
        #expect(throws: AudioProcessingError.self) { try processor.configure(invalid) }
        #expect(processor.configuration == AudioConfiguration())
        let input = tone(701)
        let first = try processor.process([input, input]); let tail = try processor.finish()
        #expect(first.channels[0] + tail.channels[0] == input)
    }
    @Test func configurationValidationAndCodable() throws {
        var invalids = [AudioConfiguration(rate: .infinity), .init(noiseReduction: -0.1),
                        .init(normalizationTargetLUFS: -21), .init(equalizer: [.init(frequency: .nan)]),
                        .init(equalizer: [.init(q: 0.2)]), .init(equalizer: [.init(gainDB: 13)]),
                        .init(equalizer: (0..<13).map { _ in EQBand() })]
        let band = EQBand()
        invalids.append(.init(equalizer: [band, band]))
        for invalid in invalids { #expect(throws: AudioProcessingError.self) { try invalid.validated() } }
        let config = AudioConfiguration.speech
        #expect(try JSONDecoder().decode(AudioConfiguration.self, from: JSONEncoder().encode(config)) == config)
    }
    @Test func monoMixAndBorrowedMonoAlias() throws {
        let input = tone(48_000)
        let mono = try render([input, input.map { -$0 }], configuration: .init(mono: true))
        #expect(mono.channels[0] == mono.channels[1])
        #expect(mono.channels[0].allSatisfy { abs($0) < 0.00001 })
        let processor = try AudioProcessor(sampleRate: sampleRate, channelCount: 1)
        var observed = 0
        func consume(_ view: ProcessedAudioView) throws {
            observed += view.frameCount
            #expect(view.left.baseAddress == view.right.baseAddress)
            #expect(throws: AudioProcessingError.invalidInput) { try processor.reset() }
        }
        try input.withUnsafeBufferPointer { buffer in
            try processor.process(left: buffer.baseAddress, right: nil, frames: input.count, consume: consume)
        }
        try processor.finish(consume: consume)
        #expect(observed == input.count)
    }
    @Test func equalizerResponseSharesCAndFiltersAudio() throws {
        for kind in EQBand.Kind.allCases {
            let config = AudioConfiguration(equalizer: [.init(kind: kind, frequency: 2_000, gainDB: 6)])
            var c = config.engineConfiguration()
            #expect(try config.equalizerGainDB(at: 340, sampleRate: sampleRate) == sb_engine_custom_eq_gain_db(&c, sampleRate, 340))
        }
        #expect(try AudioConfiguration().equalizerGainDB(at: 1000, sampleRate: sampleRate) == 0)
        #expect(throws: AudioProcessingError.invalidFormat) {
            try AudioConfiguration().equalizerGainDB(at: sampleRate / 2, sampleRate: sampleRate)
        }
        let input = tone(48_000)
        let filtered = try render([input], configuration: .init(equalizer: [.init(kind: .highPass, frequency: 2_000)]))
        func energy(_ samples: [Float]) -> Double { samples.dropFirst(2400).reduce(0) { $0 + Double($1 * $1) } }
        #expect(energy(filtered.channels[0]) < energy(input) * 0.1)
    }
}

@Suite struct AudioTimelineTests {
    private func chunk(_ source: [Double], _ saved: [Double]) -> ProcessedAudio {
        .init(channels: [[Float](repeating: 0, count: source.count)], sourceFrames: source, silenceSavedFrames: saved)
    }
    @Test func discreteCutsAndMalformedAppend() throws {
        let timeline = try AudioTimeline()
        let source = [0.0, 1, 104, 105, 106, 201, 203, 205, 207]
        let saved = [0.0, 0, 102, 102, 102, 196, 196, 196, 196]
        try timeline.append(chunk(Array(source.prefix(5)), Array(saved.prefix(5))))
        try timeline.append(chunk(Array(source.suffix(4)), Array(saved.suffix(4))))
        for index in source.indices {
            #expect(timeline.original(at: Int64(index)) == source[index])
            #expect(timeline.silenceSaved(at: Int64(index)) == saved[index])
        }
        #expect(throws: AudioProcessingError.invalidInput) { try timeline.append(chunk([208], [])) }
        #expect(throws: AudioProcessingError.invalidInput) { try timeline.append(chunk([100], [200])) }
        #expect(timeline.outputEnd == 9)
    }
    @Test func pruningAndResetKeepOnlyAudibleSavings() throws {
        let timeline = try AudioTimeline()
        for index in 0..<600 {
            timeline.played(timeline.outputEnd)
            try timeline.append(chunk((0..<128).map { Double(index * 256 + $0) },
                                      [Double](repeating: Double(index * 128), count: 128)))
        }
        #expect(timeline.outputEnd == 76_800)
        timeline.played(76_700)
        let heard = timeline.silenceSaved(at: 76_700)
        #expect(heard > 0)
        try timeline.append(chunk([200_000, 200_001], [100_000, 100_000]))
        timeline.reset(at: 400_000)
        #expect(timeline.outputEnd == 0)
        #expect(timeline.original(at: 0) == 400_000)
        #expect(timeline.silenceSaved(at: 0) == heard)
        try timeline.append(chunk([400_000, 400_001, 400_102], [0, 0, 100]))
        #expect(timeline.silenceSaved(at: 2) == heard + 100)
        timeline.played(1)
        timeline.reset(at: 800_000)
        #expect(timeline.silenceSaved(at: 0) == heard)
        timeline.reset(at: -1)
        #expect(timeline.baseOriginal == 0)
        #expect(timeline.played(Int64.max) == 0)
    }
}
