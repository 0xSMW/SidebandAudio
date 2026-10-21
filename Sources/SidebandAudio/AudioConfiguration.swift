import Foundation
import SBEngine

public enum DeEsser: String, Codable, CaseIterable, Sendable {
    case off, normal, strong
}

public struct EQBand: Codable, Equatable, Sendable, Identifiable {
    public enum Kind: Int32, Codable, CaseIterable, Sendable {
        case peak = 0, lowPass = 1, highPass = 2, bandPass = 3, notch = 4, lowShelf = 5, highShelf = 6
    }
    public var id: UUID
    public var kind: Kind
    public var frequency: Double
    public var q: Double
    public var gainDB: Double
    public init(id: UUID = UUID(), kind: Kind = .peak, frequency: Double = 1_000,
                q: Double = 0.707, gainDB: Double = 0) {
        self.id = id; self.kind = kind; self.frequency = frequency; self.q = q; self.gainDB = gainDB
    }
}

/// Independent DSP controls. Even the neutral defaults retain the engine's safety limiter.
public struct AudioConfiguration: Codable, Equatable, Sendable {
    public var rate: Double
    public var shortenSilence: Bool
    /// Noise-aware pause detection sensitivity, from 0 (strict quiet-only detection) to 1.
    /// Higher values allow noisier pauses to be shortened while the detector adapts to speech and noise.
    /// This controls pause detection independently of `noiseReduction`, which changes the audible signal.
    public var silenceSensitivity: Double
    public var voiceEnhancement: Bool
    public var mono: Bool
    public var noiseReduction: Double
    public var normalizationEnabled: Bool
    public var normalizationTargetLUFS: Double
    public var deEsser: DeEsser
    public var equalizer: [EQBand]
    public init(rate: Double = 1, shortenSilence: Bool = false, silenceSensitivity: Double = 0.5,
                voiceEnhancement: Bool = false,
                mono: Bool = false, noiseReduction: Double = 0, normalizationEnabled: Bool = false,
                normalizationTargetLUFS: Double = -14, deEsser: DeEsser = .off, equalizer: [EQBand] = []) {
        self.rate = rate; self.shortenSilence = shortenSilence; self.voiceEnhancement = voiceEnhancement
        self.silenceSensitivity = silenceSensitivity
        self.mono = mono; self.noiseReduction = noiseReduction; self.normalizationEnabled = normalizationEnabled
        self.normalizationTargetLUFS = normalizationTargetLUFS; self.deEsser = deEsser; self.equalizer = equalizer
    }
    private enum CodingKeys: String, CodingKey {
        case rate, shortenSilence, silenceSensitivity, voiceEnhancement, mono, noiseReduction
        case normalizationEnabled, normalizationTargetLUFS, deEsser, equalizer
    }
    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        rate = try values.decode(Double.self, forKey: .rate)
        shortenSilence = try values.decode(Bool.self, forKey: .shortenSilence)
        silenceSensitivity = values.contains(.silenceSensitivity)
            ? try values.decode(Double.self, forKey: .silenceSensitivity) : 0.5
        voiceEnhancement = try values.decode(Bool.self, forKey: .voiceEnhancement)
        mono = try values.decode(Bool.self, forKey: .mono)
        noiseReduction = try values.decode(Double.self, forKey: .noiseReduction)
        normalizationEnabled = try values.decode(Bool.self, forKey: .normalizationEnabled)
        normalizationTargetLUFS = try values.decode(Double.self, forKey: .normalizationTargetLUFS)
        deEsser = try values.decode(DeEsser.self, forKey: .deEsser)
        equalizer = try values.decode([EQBand].self, forKey: .equalizer)
    }
    public static var speech: Self {
        Self(shortenSilence: true, voiceEnhancement: true, normalizationEnabled: true)
    }
    /// Rejects invalid values instead of silently altering the requested sound.
    public func validated() throws -> Self {
        func require(_ value: Double, _ range: ClosedRange<Double>, _ name: String) throws {
            guard value.isFinite, range.contains(value) else { throw AudioProcessingError.invalidConfiguration(name) }
        }
        try require(rate, 0.5...5, "rate")
        try require(silenceSensitivity, 0...1, "silenceSensitivity")
        try require(noiseReduction, 0...1, "noiseReduction")
        try require(normalizationTargetLUFS, -20 ... -6, "normalizationTargetLUFS")
        guard equalizer.count <= 12 else { throw AudioProcessingError.invalidConfiguration("equalizer count") }
        for (index, band) in equalizer.enumerated() {
            try require(band.frequency, 20...20_000, "equalizer frequency")
            try require(band.q, 0.3...8, "equalizer Q")
            try require(band.gainDB, -12...12, "equalizer gain")
            guard !equalizer[..<index].contains(where: { $0.id == band.id }) else {
                throw AudioProcessingError.invalidConfiguration("duplicate equalizer id")
            }
        }
        return self
    }
    /// Uses exactly the custom-band coefficients and frequency clamps used by processing.
    /// A transfer zero may return negative infinity; chart clipping is the caller's choice.
    public func equalizerGainDB(at frequency: Double, sampleRate: Double) throws -> Double {
        _ = try validated()
        guard sampleRate.isFinite, (8_000...192_000).contains(sampleRate),
              frequency.isFinite, frequency > 0, frequency < sampleRate / 2 else {
            throw AudioProcessingError.invalidFormat
        }
        var config = engineConfiguration()
        let gain = sb_engine_custom_eq_gain_db(&config, sampleRate, frequency)
        guard !gain.isNaN else { throw AudioProcessingError.invalidConfiguration("equalizer response") }
        return gain
    }
    func engineConfiguration() -> SBEngineConfig {
        var config = sb_engine_default_config()
        config.rate = Float(rate); config.smart_speed = shortenSilence ? 1 : 0
        config.silence_sensitivity = Float(silenceSensitivity)
        config.voice_boost = voiceEnhancement ? 1 : 0; config.mono = mono ? 1 : 0
        config.noise_reduction = Float(noiseReduction)
        config.normalization_mode = normalizationEnabled ? 2 : 1
        config.target_lufs = Float(normalizationTargetLUFS)
        config.deesser = switch deEsser { case .off: 0; case .normal: 1; case .strong: 2 }
        for (index, band) in equalizer.enumerated() {
            sb_engine_config_set_band(&config, index, band.kind.rawValue, Float(band.frequency), Float(band.q), Float(band.gainDB))
        }
        return config
    }
}

public enum AudioProcessingError: Error, Equatable, Sendable, LocalizedError {
    case invalidFormat, invalidConfiguration(String), invalidInput, finished, stalled, allocationFailed
    public var errorDescription: String? {
        switch self {
        case .invalidFormat: "Expected 8–192 kHz mono or stereo audio."
        case .invalidConfiguration(let field): "Invalid audio configuration: \(field)."
        case .invalidInput: "Invalid audio input or reentrant access."
        case .finished: "Reset the finished processor before supplying input or changing its configuration."
        case .stalled: "The audio engine could not make progress."
        case .allocationFailed: "The audio engine could not allocate storage."
        }
    }
}
