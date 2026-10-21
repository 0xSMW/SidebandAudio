import Foundation
import Testing
@preconcurrency import AVFoundation
import SidebandAudio

struct SampleSilenceTests {
    @Test func archivalPausesShortenWithoutChangingAnnotatedSpeech() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let url = root.appendingPathComponent("Examples/SidebandAudioDemo/Resources/MoonChallenge.wav")
        let file = try AVAudioFile(forReading: url)
        let rate = file.processingFormat.sampleRate
        #expect(rate == 44_100)
        #expect(file.length == 506_292) // Original recording, with both added one-second pads removed.
        let buffer = try #require(AVAudioPCMBuffer(pcmFormat: file.processingFormat,
                                                 frameCapacity: AVAudioFrameCount(file.length)))
        try file.read(into: buffer)
        let channel = try #require(buffer.floatChannelData?[0])
        let input = Array(UnsafeBufferPointer(start: channel, count: Int(buffer.frameLength)))
        func render(shorten: Bool, chunks: [Int], noiseReduction: Double = 0) throws -> ProcessedAudio {
            var config = AudioConfiguration.speech
            config.shortenSilence = shorten
            config.noiseReduction = noiseReduction
            let processor = try AudioProcessor(sampleRate: rate, channelCount: 1, configuration: config)
            var output = ProcessedAudio(channels: [[]], sourceFrames: [], silenceSavedFrames: [])
            func append(_ part: ProcessedAudio) {
                output.channels[0] += part.channels[0]
                output.sourceFrames += part.sourceFrames
                output.silenceSavedFrames += part.silenceSavedFrames
            }
            var offset = 0, turn = 0
            while offset < input.count {
                let end = min(input.count, offset + chunks[turn % chunks.count])
                append(try processor.process([Array(input[offset..<end])]))
                offset = end; turn += 1
            }
            append(try processor.finish())
            return output
        }
        let original = try render(shorten: false, chunks: [4096])
        let shortened = try render(shorten: true, chunks: [4096])
        let fragmented = try render(shorten: true, chunks: [1, 997, 8192, 31])
        let denoised = try render(shorten: true, chunks: [4096], noiseReduction: 1)
        #expect(denoised.sourceFrames == shortened.sourceFrames)
        #expect(denoised.silenceSavedFrames == shortened.silenceSavedFrames)
        #expect(original.frameCount == input.count)
        #expect(shortened.channels == fragmented.channels)
        #expect(shortened.sourceFrames == fragmented.sourceFrames)
        #expect(shortened.silenceSavedFrames == fragmented.silenceSavedFrames)

        // Conservative manually annotated pause interiors, in original-recording seconds.
        // Everything outside these intervals, including quiet speech and both boundaries,
        // must retain every source frame and the exact mastered PCM sample.
        let pauses = [2.70..<3.45, 4.65..<5.32, 6.82..<7.45, 11.10..<11.480545]
        var retained = [Bool](repeating: false, count: input.count)
        for index in shortened.sourceFrames.indices {
            let source = Int(shortened.sourceFrames[index])
            #expect(source >= 0 && source < input.count)
            retained[source] = true
            let seconds = Double(source) / rate
            if !pauses.contains(where: { $0.contains(seconds) }) {
                #expect(shortened.channels[0][index] == original.channels[0][source])
            }
        }
        for source in input.indices where !retained[source] {
            #expect(pauses.contains(where: { $0.contains(Double(source) / rate) }))
        }
        for pause in pauses.prefix(3) {
            let removed = input.indices.filter { pause.contains(Double($0) / rate) && !retained[$0] }.count
            #expect(Double(removed) / rate > 0.04)
        }
        let saved = try #require(shortened.silenceSavedFrames.last)
        #expect(saved == Double(input.count - shortened.frameCount))
        #expect(saved / rate > 0.25)
    }
}
