// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "SidebandAudio",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [
        .library(name: "SBEngine", targets: ["SBEngine"]),
        .library(name: "SidebandAudio", targets: ["SidebandAudio"]),
        .library(name: "SidebandAudioPlayer", targets: ["SidebandAudioPlayer"]),
    ],
    targets: [
        .target(
            name: "SBEngine",
            publicHeadersPath: "include",
            cSettings: [.define("_USE_MATH_DEFINES")],
            linkerSettings: [
                .linkedLibrary("m"),
                .linkedFramework("Accelerate", .when(platforms: [.iOS, .macOS, .watchOS, .tvOS, .visionOS])),
            ]
        ),
        .target(name: "SidebandAudio", dependencies: ["SBEngine"]),
        .target(name: "SidebandAudioPlayer", dependencies: ["SidebandAudio"]),
        .testTarget(name: "SidebandAudioTests", dependencies: ["SidebandAudio", "SBEngine"]),
        .testTarget(name: "SidebandAudioPlayerTests", dependencies: ["SidebandAudioPlayer", "SidebandAudio"]),
    ],
    swiftLanguageModes: [.v6],
    cLanguageStandard: .c11
)
