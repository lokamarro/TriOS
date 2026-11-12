// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The friendly name of a system release, for the screens that show it.
///
/// A release ID reads `YYYYMMDD-<family tag>-r<N>` ("20261109-los24-r20"):
/// the date the release was built, the family it belongs to and its revision.
/// The screens show the revision ("r20") and, on the Info sheet, the date too.
/// The ID itself stays what bug reports, session records, logs and the
/// instance's files carry; an ID that does not read this way is shown as it is.
public struct ReleaseLabel: Equatable, Sendable {
    public let revision: Int
    public let year: Int
    public let month: Int
    public let day: Int

    /// nil for an ID that is not in the scheme.
    public init?(releaseID: String) {
        let parts = releaseID.split(separator: "-", omittingEmptySubsequences: false)
        guard parts.count == 3 else { return nil }

        let date = parts[0]
        guard date.utf8.count == 8, Self.isDigits(date),
              let year = Int(date.prefix(4)), let month = Int(date.dropFirst(4).prefix(2)),
              let day = Int(date.suffix(2)),
              year >= 2000, (1...12).contains(month), (1...31).contains(day)
        else { return nil }

        let tag = parts[1]
        guard !tag.isEmpty, tag.utf8.allSatisfy({ ($0 >= 0x61 && $0 <= 0x7A) || Self.isDigit($0) }) else {
            return nil
        }

        let number = parts[2].dropFirst()
        guard parts[2].first == "r", !number.isEmpty, Self.isDigits(number), number.first != "0",
              let revision = Int(number)
        else { return nil }

        self.revision = revision
        self.year = year
        self.month = month
        self.day = day
    }

    /// "r20".
    public var revisionText: String { "r\(revision)" }

    /// "2026-11-09".
    public var dateText: String {
        String(format: "%04d-%02d-%02d", year, month, day)
    }

    /// The revision a screen shows for `releaseID` ("r20"), or the ID itself
    /// where it is not in the scheme.
    public static func revision(of releaseID: String) -> String {
        ReleaseLabel(releaseID: releaseID)?.revisionText ?? releaseID
    }

    /// The Build row of the Info sheet ("r20 (2026-11-09)"), or the ID itself
    /// where it is not in the scheme.
    public static func build(of releaseID: String) -> String {
        guard let label = ReleaseLabel(releaseID: releaseID) else { return releaseID }
        return "\(label.revisionText) (\(label.dateText))"
    }

    private static func isDigit(_ byte: UInt8) -> Bool { byte >= 0x30 && byte <= 0x39 }

    private static func isDigits(_ text: Substring) -> Bool { text.utf8.allSatisfy(isDigit) }
}

extension SystemInfo {
    /// The revision of this system's release as the boot screens and the
    /// update notices name it ("r20").
    public var releaseRevision: String { ReleaseLabel.revision(of: release) }

    /// The Info sheet's Build row.
    public var buildLabel: String { ReleaseLabel.build(of: release) }
}
