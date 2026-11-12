// swift-tools-version: 6.0
// SPDX-License-Identifier: GPL-2.0-or-later

import PackageDescription

// The Andarium application's model and policy, with no UIKit, SwiftUI or
// runtime dependency: `swift test` runs it on macOS, and the application and
// its Simulator preview import it. It re-exports GuestHostCore, what both
// applications decide the same way.
let package = Package(
    name: "AndariumCore",
    platforms: [.macOS("27.0"), .iOS("27.0")],
    products: [
        .library(name: "AndariumCore", targets: ["AndariumCore"]),
    ],
    dependencies: [
        .package(path: "../../GuestHost/Core"),
    ],
    targets: [
        .target(
            name: "AndariumCore",
            dependencies: [.product(name: "GuestHostCore", package: "Core")]
        ),
        .testTarget(
            name: "AndariumCoreTests",
            dependencies: ["AndariumCore"]
        ),
    ]
)
