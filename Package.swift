// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "AmigaMac",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "amigarun", targets: ["amigarun"]),
        .executable(name: "AmigaMac", targets: ["AmigaMac"]),
    ],
    targets: [
        .target(
            name: "CAmiga",
            path: "Sources/CAmiga",
            cSettings: [.unsafeFlags(["-O2", "-Wall", "-Wno-unused-function"])],
            linkerSettings: [.linkedFramework("CoreFoundation"), .linkedFramework("AudioToolbox")]
        ),
        .executableTarget(
            name: "AmigaMac",
            dependencies: ["CAmiga"],
            path: "Sources/AmigaMac"
        ),
        .executableTarget(
            name: "amigarun",
            dependencies: ["CAmiga"],
            path: "Sources/amigarun"
        ),
    ]
)
