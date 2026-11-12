// swift-tools-version: 6.0
// SPDX-License-Identifier: GPL-2.0-or-later

import PackageDescription

// What Kernarium and Andarium decide the same way about a guest they host,
// with no UIKit, SwiftUI or runtime dependency: `swift test` runs it on macOS,
// and both applications import it. The rest of GuestHost is compiled into each
// application and is not a package (GuestHost/README.md).
let package = Package(
    name: "GuestHostCore",
    platforms: [.macOS("27.0"), .iOS("27.0")],
    products: [
        .library(name: "GuestHostCore", targets: ["GuestHostCore"]),
    ],
    targets: [
        .target(name: "GuestHostCore"),
        .testTarget(
            name: "GuestHostCoreTests",
            dependencies: ["GuestHostCore"]
        ),
    ]
)
