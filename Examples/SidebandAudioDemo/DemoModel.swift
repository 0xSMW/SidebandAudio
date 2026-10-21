import AVFoundation
import Foundation
import Observation
import SidebandAudio
import SidebandAudioPlayer

/// Host integration: local file access, session policy and UI state belong to the app.
@MainActor @Observable
final class DemoModel {
    let player = LocalFilePlayer(configuration: .speech)
    var configuration = AudioConfiguration.speech {
        didSet { applyConfiguration() }
    }
    var sourceTitle = "Moon Challenge"
    var error: String?
    var isLoading = false
    @ObservationIgnored private var configurationTask: Task<Void, Never>?
    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private var resumeAfterInterruption = false
    @ObservationIgnored private var loadRevision = 0

    init() { observeAudioSession() }

    func useSample() async {
        guard let url = Bundle.main.url(forResource: "MoonChallenge", withExtension: "wav") else {
            error = "The bundled speech sample is missing."
            return
        }
        await load(url, title: "Moon Challenge")
    }

    func importAudio(_ url: URL) async {
        // The host handles a document provider's security scope. The SDK receives only a local URL.
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            // Copy into the demo's sandbox so the player never outlives a document-provider lease.
            let local = try await Self.copyImportedFile(url)
            await load(local, title: url.deletingPathExtension().lastPathComponent)
        } catch { self.error = error.localizedDescription }
    }

    @concurrent private static func copyImportedFile(_ source: URL) async throws -> URL {
        guard source.isFileURL else { throw PlayerFailure.invalidFileURL }
        let directory = try FileManager.default.url(for: .cachesDirectory, in: .userDomainMask,
                                                    appropriateFor: nil, create: true)
            .appendingPathComponent("ImportedAudio", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let destination = directory.appendingPathComponent(UUID().uuidString)
            .appendingPathExtension(source.pathExtension)
        try FileManager.default.copyItem(at: source, to: destination)
        return destination
    }

    private func load(_ url: URL, title: String) async {
        loadRevision += 1
        let revision = loadRevision
        isLoading = true
        error = nil
        do {
            try await Self.activateAudioSession()
            guard revision == loadRevision else { return }
            try await player.load(fileURL: url)
            guard revision == loadRevision else { return }
            sourceTitle = title
        } catch is CancellationError {
            // A newer source owns the screen.
        } catch {
            if revision == loadRevision { self.error = error.localizedDescription }
        }
        if revision == loadRevision { isLoading = false }
    }

    func resetConfiguration() { configuration = .speech }

    private func applyConfiguration() {
        configurationTask?.cancel()
        let snapshot = configuration
        configurationTask = Task { [weak self] in
            do {
                try await Task.sleep(for: .milliseconds(40))
                guard !Task.isCancelled, let self else { return }
                try player.configure(snapshot)
            } catch is CancellationError { } catch { self?.error = error.localizedDescription }
        }
    }

    func togglePlayback() {
        resumeAfterInterruption = false
        if player.isPlaying { player.pause() } else { player.play() }
    }
    func skip(_ seconds: Double) { seek(to: player.position + seconds) }
    func seek(to seconds: Double) { player.seek(to: seconds) }
    func setMode(_ mode: PlaybackMode) { player.setMode(mode) }

    @concurrent private static func activateAudioSession() async throws {
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playback, mode: .spokenAudio, policy: .longFormAudio)
        try session.setActive(true)
    }

    private func observeAudioSession() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: AVAudioSession.interruptionNotification,
                                             object: nil, queue: .main) { [weak self] notification in
            let raw = notification.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            let options = notification.userInfo?[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0
            Task { @MainActor [weak self] in self?.handleInterruption(raw, options: options) }
        })
        observers.append(center.addObserver(forName: AVAudioSession.routeChangeNotification,
                                             object: nil, queue: .main) { [weak self] notification in
            let raw = notification.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt
            guard raw == AVAudioSession.RouteChangeReason.oldDeviceUnavailable.rawValue else { return }
            Task { @MainActor [weak self] in
                self?.resumeAfterInterruption = false
                self?.player.pause()
            }
        })
        observers.append(center.addObserver(forName: AVAudioSession.mediaServicesWereResetNotification,
                                             object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                do {
                    try await Self.activateAudioSession()
                    try await player.recoverAfterMediaServicesReset()
                } catch { self.error = error.localizedDescription }
            }
        })
    }

    private func handleInterruption(_ raw: UInt?, options: UInt) {
        guard let raw, let type = AVAudioSession.InterruptionType(rawValue: raw) else { return }
        if type == .began {
            resumeAfterInterruption = player.isPlaying
            player.pause()
        } else {
            let resume = resumeAfterInterruption && AVAudioSession.InterruptionOptions(rawValue: options).contains(.shouldResume)
            resumeAfterInterruption = false
            if resume {
                Task { [weak self] in
                    do { try await Self.activateAudioSession(); self?.player.play() }
                    catch { self?.error = error.localizedDescription }
                }
            }
        }
    }

    isolated deinit {
        configurationTask?.cancel()
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
    }
}
