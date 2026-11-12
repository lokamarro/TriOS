// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
import UIKit

/// Polls `condition` on the main actor every `interval` until it holds or
/// `limit` passes.
@MainActor
func eventually(
    within limit: Duration = .seconds(10),
    every interval: Duration = .milliseconds(10),
    _ condition: () -> Bool
) async throws -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: limit)
    while !condition() {
        guard clock.now < deadline else { return false }
        try await Task.sleep(for: interval)
    }
    return true
}

/// A new, empty directory below the temporary directory, named for `label`
/// and a UUID, with `pathExtension` when given. The test removes it.
func makeScratchDirectory(
    _ label: String,
    pathExtension: String? = nil
) throws -> URL {
    var directory = FileManager.default.temporaryDirectory
        .appendingPathComponent(
            "\(label)-\(UUID().uuidString)",
            isDirectory: true
        )
    if let pathExtension {
        directory.appendPathExtension(pathExtension)
    }
    try FileManager.default.createDirectory(
        at: directory,
        withIntermediateDirectories: true
    )
    return directory
}

/// The application's window scene.
@MainActor
func windowScene() throws -> UIWindowScene {
    try #require(
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .first,
        "the application has no window scene"
    )
}

/// The accessibility element below `node` carrying `identifier`, found the
/// way an assistive client walks the application: through each node's
/// accessibility elements and each view's subviews. SwiftUI exposes its
/// controls this way only while application accessibility is on; see
/// docs/testing.md.
@MainActor
func accessibilityElement(_ identifier: String, in node: NSObject) -> NSObject? {
    accessibilityElement(identifier, in: node, depth: 0)
}

@MainActor
private func accessibilityElement(
    _ identifier: String,
    in node: NSObject,
    depth: Int
) -> NSObject? {
    // SwiftUI's accessibility nodes answer the identifier without declaring
    // UIAccessibilityIdentification.
    let getter = NSSelectorFromString("accessibilityIdentifier")
    if node.responds(to: getter),
       node.perform(getter)?.takeUnretainedValue() as? String == identifier {
        return node
    }
    guard depth < 64 else { return nil }
    var children: [NSObject] = []
    if let elements = node.accessibilityElements {
        children += elements.compactMap { $0 as? NSObject }
    }
    if let view = node as? UIView {
        children += view.subviews
    }
    for child in children {
        if let found = accessibilityElement(
            identifier, in: child, depth: depth + 1
        ) {
            return found
        }
    }
    return nil
}
