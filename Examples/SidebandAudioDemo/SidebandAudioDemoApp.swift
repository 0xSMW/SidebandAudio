import SwiftUI

@main
struct SidebandAudioDemoApp: App {
    @State private var model = DemoModel()
    var body: some Scene {
        WindowGroup { PlayerScreen(model: model) }
    }
}
