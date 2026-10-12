// swift-tools-version: 6.0
import PackageDescription

// The same C core the CMake build compiles, declared as a SwiftPM C target, with the Swift face over it:
//
//   CBCFileSystem            the C core (core/src/*.c) behind its one public header (core/include/)
//   BCFileSystem             the Swift face: the engine, the file system, the path values, the errors
//   BCFileSystemTestFixtures the temporary directory and the observed engine a consumer's tests use
//
// BCFileSystem imports only the C module, so neither this package nor one that depends on it needs C++
// interoperability switched on. The C standard is the one line every build system states the same way
// (AGENTS.md, "Build shape"): gnu2x here, C_STANDARD 23 with extensions in CMake.
let swiftSettings: [SwiftSetting] = [
    .swiftLanguageMode(.v6)
]

let package = Package(
    name: "bc-file-system",
    platforms: [
        .macOS(.v14),
        .iOS(.v17),
    ],
    products: [
        .library(name: "BCFileSystem", targets: ["BCFileSystem"]),
        .library(name: "BCFileSystemTestFixtures", targets: ["BCFileSystemTestFixtures"]),
    ],
    targets: [
        .target(
            name: "CBCFileSystem",
            path: "core",
            sources: ["src"],
            publicHeadersPath: "include"
        ),
        .target(
            name: "BCFileSystem",
            dependencies: ["CBCFileSystem"],
            swiftSettings: swiftSettings
        ),
        // The fixtures name the C module too: the observed-engine factory and the fault struct are C calls.
        .target(
            name: "BCFileSystemTestFixtures",
            dependencies: ["BCFileSystem", "CBCFileSystem"],
            swiftSettings: swiftSettings
        ),
        .testTarget(
            name: "BCFileSystemTests",
            dependencies: ["BCFileSystem", "BCFileSystemTestFixtures", "CBCFileSystem"],
            swiftSettings: swiftSettings
        ),
    ],
    cLanguageStandard: .gnu2x
)
