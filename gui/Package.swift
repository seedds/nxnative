// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "NXNativeGUI",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "NXNativeGUI", targets: ["NXNativeGUI"])
    ],
    targets: [
        .executableTarget(
            name: "NXNativeGUI",
            path: "Sources"
        )
    ]
)
