import Foundation
import SwiftUI
import UniformTypeIdentifiers
import SidebandAudio
import SidebandAudioPlayer

struct PlayerScreen: View {
    @Bindable var model: DemoModel
    @State private var importing = false
    @State private var loadedSample = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                sourceHeader


                settingsGroup("Playback") {
                    ValueSlider("Rate", value: $model.configuration.rate, range: 0.5...5,
                                step: 0.05, display: "\(rateDescription)×",
                                accessibilityValue: "\(rateDescription) times")
                        .accessibilityIdentifier("rateSlider")
                    Toggle("Shorten silence", isOn: $model.configuration.shortenSilence)
                        .tint(.blue)
                        .accessibilityIdentifier("shortenSilenceToggle")
                }

                settingsGroup("Voice") {
                    Toggle("Voice enhancement", isOn: $model.configuration.voiceEnhancement)
                        .tint(.blue)
                        .accessibilityIdentifier("voiceEnhancementToggle")
                    ValueSlider("Noise reduction", value: $model.configuration.noiseReduction,
                                range: 0...1, step: 0.01,
                                display: model.configuration.noiseReduction.formatted(.percent.precision(.fractionLength(0))))
                    Toggle("Mono", isOn: $model.configuration.mono).tint(.blue)
                    VStack(alignment: .leading, spacing: 8) {
                        Text("De-esser")
                        Picker("De-esser", selection: $model.configuration.deEsser) {
                            ForEach(DeEsser.allCases, id: \.self) { mode in
                                Text(mode.rawValue.capitalized).tag(mode)
                            }
                        }
                        .pickerStyle(.segmented)
                    }
                }

                settingsGroup("Normalization") {
                    Toggle("Normalization", isOn: $model.configuration.normalizationEnabled)
                        .tint(.blue)
                        .accessibilityIdentifier("normalizationToggle")
                    ValueSlider("Target loudness", value: $model.configuration.normalizationTargetLUFS,
                                range: -20 ... -6, step: 1,
                                display: "\(Int(model.configuration.normalizationTargetLUFS)) LUFS")
                        .disabled(!model.configuration.normalizationEnabled)
                }

                equalizer

                Button("Reset all settings", systemImage: "arrow.counterclockwise") {
                    model.resetConfiguration()
                }
                .buttonStyle(.bordered)
                .controlSize(.large)
                .frame(maxWidth: .infinity)
                .padding(.bottom, 8)
            }
            .padding(16)
        }
        .accessibilityIdentifier("settingsScroll")
        .background(Color(uiColor: .systemGroupedBackground))
        .tint(.primary)
        .safeAreaInset(edge: .bottom, spacing: 0) { transport }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.audio], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                if let url = urls.first { Task { await model.importAudio(url) } }
            case .failure(let error): model.error = error.localizedDescription
            }
        }
        .alert("Audio error", isPresented: Binding(
            get: { model.error != nil },
            set: { if !$0 { model.error = nil } }
        )) {
            Button("OK", role: .cancel) { model.error = nil }
        } message: { Text(model.error ?? "") }
        .onChange(of: model.player.state) { _, state in
            if case .failed(let failure) = state { model.error = failure.localizedDescription }
        }
        .task {
            guard !loadedSample else { return }
            loadedSample = true
            await model.useSample()
        }
    }

    private var sourceHeader: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Sideband Audio").font(.largeTitle.bold()).accessibilityAddTraits(.isHeader)
            HStack(alignment: .top, spacing: 16) {
                Text(model.sourceTitle).font(.title2.weight(.semibold))
                    .frame(maxWidth: .infinity, alignment: .leading)
                if model.isLoading { ProgressView().accessibilityLabel("Loading audio") }
            }
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 16) { sourceButtons }
                VStack(alignment: .leading, spacing: 8) { sourceButtons }
            }
        }
    }

    @ViewBuilder private var sourceButtons: some View {
        Button("Sample", systemImage: "play.rectangle") { Task { await model.useSample() } }
            .buttonStyle(.bordered).controlSize(.large)
            .accessibilityIdentifier("sampleButton")
        Button("Import audio", systemImage: "square.and.arrow.down") { importing = true }
            .buttonStyle(.bordered).controlSize(.large)
            .accessibilityIdentifier("importButton")
    }

    private var equalizer: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .firstTextBaseline) {
                Text("Equalizer").font(.title3.bold()).accessibilityAddTraits(.isHeader)
                Spacer(minLength: 8)
                Text("\(model.configuration.equalizer.count)/12").foregroundStyle(.secondary)
                    .accessibilityLabel("\(model.configuration.equalizer.count) of 12 bands")
            }
            ForEach($model.configuration.equalizer) { $band in
                EQBandEditor(band: $band) {
                    model.configuration.equalizer.removeAll { $0.id == band.id }
                }
            }
            Button("Add band", systemImage: "plus") {
                model.configuration.equalizer.append(EQBand())
            }
            .buttonStyle(.bordered)
            .controlSize(.large)
            .disabled(model.configuration.equalizer.count >= 12)
            .accessibilityIdentifier("addBandButton")
        }
    }

    private var transport: some View {
        VStack(spacing: 8) {
            Picker("Playback mode", selection: Binding(
                get: { model.player.mode },
                set: { model.setMode($0) }
            )) {
                Text("Original").tag(PlaybackMode.original)
                Text("Processed").tag(PlaybackMode.processed)
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("modePicker")

            Slider(value: Binding(
                get: { min(max(0, model.player.position), max(0, model.player.duration)) },
                set: { model.seek(to: $0) }
            ), in: 0...max(0.001, model.player.duration))
            .disabled(model.player.duration <= 0 || model.isLoading)
            .accessibilityLabel("Original audio position")
            .accessibilityValue("\(spokenTime(model.player.position)) of \(spokenTime(model.player.duration))")

            HStack(alignment: .firstTextBaseline) {
                Text(time(model.player.position))
                    .accessibilityLabel("Original audio position, \(spokenTime(model.player.position))")
                    .accessibilityIdentifier("sourceTime")
                Spacer(minLength: 8)
                Text(time(model.player.duration))
                    .accessibilityLabel("Original audio duration, \(spokenTime(model.player.duration))")
            }
            .font(.caption.monospacedDigit())
            .foregroundStyle(.secondary)

            HStack(spacing: 32) {
                Button { model.skip(-15) } label: {
                    Image(systemName: "gobackward.15").font(.title2).frame(minWidth: 44, minHeight: 44)
                }
                .accessibilityLabel("Back 15 seconds")
                Button { model.togglePlayback() } label: {
                    Image(systemName: model.player.isPlaying ? "pause.fill" : "play.fill")
                        .font(.title2).frame(width: 56, height: 56)
                        .background(.primary.opacity(0.08), in: Circle())
                }
                .accessibilityLabel(model.player.isPlaying ? "Pause" : "Play")
                .accessibilityIdentifier("playButton")
                Button { model.skip(30) } label: {
                    Image(systemName: "goforward.30").font(.title2).frame(minWidth: 44, minHeight: 44)
                }
                .accessibilityLabel("Forward 30 seconds")
            }
            .buttonStyle(.plain)
            .disabled(model.player.duration <= 0 || model.isLoading)

            Text("Silence saved: \(time(model.player.silenceSaved))")
                .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                .accessibilityLabel("Silence saved, \(spokenTime(model.player.silenceSaved))")
                .accessibilityIdentifier("silenceSaved")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(.regularMaterial)
    }

    private var rateDescription: String {
        model.configuration.rate.formatted(.number.precision(.fractionLength(2)))
    }

    private func settingsGroup<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(title).font(.title3.bold()).accessibilityAddTraits(.isHeader)
            VStack(alignment: .leading, spacing: 16, content: content)
                .padding(16)
                .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 16))
        }
    }

    private func time(_ seconds: Double) -> String {
        let total = seconds.isFinite ? max(0, Int(seconds)) : 0
        if total >= 3_600 { return String(format: "%d:%02d:%02d", total / 3_600, total / 60 % 60, total % 60) }
        return String(format: "%d:%02d", total / 60, total % 60)
    }

    private func spokenTime(_ seconds: Double) -> String {
        let total = seconds.isFinite ? max(0, Int(seconds)) : 0
        return "\(total / 60) minutes, \(total % 60) seconds"
    }
}
