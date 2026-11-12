// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import GuestHostCore

/// A harness launch's way to drive the low-space pause without filling the
/// disk and without a finger: with `KERNARIUM_SPACE_HARNESS=1`, a file
/// `free-space-mib` in Documents, holding a number, stands for the device's
/// free space while it exists, and a file `space-answer` (`resume`,
/// `shutdown` or `check`) answers the screen over a paused guest once it
/// offers that answer. Without the variable the device's own free space is
/// read and nothing is answered.
nonisolated struct SpaceHarness: Sendable {
    static let environmentKey = "KERNARIUM_SPACE_HARNESS"
    static let freeSpaceFile = "free-space-mib"
    static let answerFile = "space-answer"

    /// This launch's harness, from the environment.
    static let current = SpaceHarness(
        enabled: ProcessInfo.processInfo.environment[environmentKey] == "1",
        directory: RootfsImage.documentsDirectory
    )

    let enabled: Bool
    let directory: URL

    /// The device's free space, or what the harness says it is.
    @Sendable
    func freeSpace() -> UInt64 {
        guard enabled,
              let text = try? String(
                contentsOf: directory.appendingPathComponent(Self.freeSpaceFile),
                encoding: .utf8
              ),
              let mebibytes = UInt64(
                text.trimmingCharacters(in: .whitespacesAndNewlines)
              )
        else { return HostStorage.freeBytes() }
        return mebibytes * Bytes.mib
    }

    /// The answer waiting in the file, if any.
    func answer() -> SpaceAnswer? {
        guard enabled,
              let text = try? String(
                contentsOf: directory.appendingPathComponent(Self.answerFile),
                encoding: .utf8
              )
        else { return nil }
        return SpaceAnswer(
            rawValue: text.trimmingCharacters(in: .whitespacesAndNewlines)
        )
    }

    /// The answer was given: the file goes.
    func consumeAnswer() {
        try? FileManager.default.removeItem(
            at: directory.appendingPathComponent(Self.answerFile)
        )
    }
}
