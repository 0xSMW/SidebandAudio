import Foundation
import SwiftUI
import SidebandAudio

struct EQBandEditor: View {
    @Binding var band: EQBand
    var remove: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Picker("Filter", selection: $band.kind) {
                    ForEach(EQBand.Kind.allCases, id: \.self) { kind in
                        Text(kind.title).tag(kind)
                    }
                }
                .pickerStyle(.menu)
                Spacer(minLength: 0)
                Button(role: .destructive, action: remove) {
                    Image(systemName: "minus.circle").frame(minWidth: 44, minHeight: 44)
                }
                .tint(.primary)
                .accessibilityLabel("Remove \(band.kind.title) band at \(Int(band.frequency)) hertz")
            }
            ValueSlider("Frequency", value: logarithmic($band.frequency),
                        range: log(20)...log(20_000),
                        display: frequencyDescription,
                        accessibilityValue: "\(Int(band.frequency)) hertz")
            ValueSlider("Q", value: logarithmic($band.q), range: log(0.3)...log(8),
                        display: band.q.formatted(.number.precision(.fractionLength(2))))
            ValueSlider("Gain", value: $band.gainDB, range: -12...12, step: 0.1,
                        display: "\(band.gainDB.formatted(.number.precision(.fractionLength(1)))) dB")
                .disabled(!band.kind.supportsGain)
        }
        .padding(16)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 16))
    }

    private var frequencyDescription: String {
        if band.frequency >= 1_000 {
            return "\((band.frequency / 1_000).formatted(.number.precision(.fractionLength(2)))) kHz"
        }
        return "\(Int(band.frequency)) Hz"
    }

    private func logarithmic(_ value: Binding<Double>) -> Binding<Double> {
        Binding(get: { log(value.wrappedValue) }, set: { value.wrappedValue = exp($0) })
    }
}

struct ValueSlider: View {
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    var step: Double? = nil
    let display: String
    var accessibilityValue: String? = nil

    init(_ title: String, value: Binding<Double>, range: ClosedRange<Double>, step: Double? = nil,
         display: String, accessibilityValue: String? = nil) {
        self.title = title
        self._value = value
        self.range = range
        self.step = step
        self.display = display
        self.accessibilityValue = accessibilityValue
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline, spacing: 16) {
                Text(title).frame(maxWidth: .infinity, alignment: .leading)
                Text(display).monospacedDigit().foregroundStyle(.secondary)
            }
            Group {
                if let step { Slider(value: $value, in: range, step: step) }
                else { Slider(value: $value, in: range) }
            }
            .accessibilityLabel(title)
            .accessibilityValue(accessibilityValue ?? display)
        }
    }
}

private extension EQBand.Kind {
    var title: String {
        switch self {
        case .peak: "Peak"
        case .lowPass: "Low pass"
        case .highPass: "High pass"
        case .bandPass: "Band pass"
        case .notch: "Notch"
        case .lowShelf: "Low shelf"
        case .highShelf: "High shelf"
        }
    }

    var supportsGain: Bool {
        switch self {
        case .peak, .lowShelf, .highShelf: true
        case .lowPass, .highPass, .bandPass, .notch: false
        }
    }
}
