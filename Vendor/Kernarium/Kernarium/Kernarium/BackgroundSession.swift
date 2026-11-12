// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import Foundation

/// A file that outlives the process and says the application was in the
/// background when it ended.
///
/// iOS can end a suspended application without running any of its code, and
/// the guest it hosted is then gone: the next launch boots it again from
/// scratch. The application cannot observe that ending, so it records the
/// edge it can observe. It writes the marker as it enters the background and
/// removes it once it is back in the foreground and the guest runs again, or
/// when it is told it will terminate. A marker found at launch therefore means
/// the previous process ended while it was in the background, by iOS
/// reclaiming it or by the user closing it from the app switcher, or while the
/// guest was coming back, as when the RAM a hibernated guest wrote out could
/// not be restored and the runtime ended the process.
struct BackgroundMarker {
    struct Record: Codable, Equatable {
        var enteredAt: Date
    }

    let url: URL

    static let defaultURL: URL = {
        let directory = (try? FileManager.default.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )) ?? FileManager.default.temporaryDirectory
        return directory.appendingPathComponent("background-session.json")
    }()

    /// Records that the application entered the background at `date`.
    /// Returns whether the marker reached storage.
    @discardableResult
    func arm(at date: Date = Date()) -> Bool {
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            let encoder = JSONEncoder()
            encoder.dateEncodingStrategy = .secondsSince1970
            try encoder.encode(Record(enteredAt: date))
                .write(to: url, options: .atomic)
            return true
        } catch {
            print("Kernarium: cannot record the background edge: \(error)")
            return false
        }
    }

    /// Removes the marker: the application is back in the foreground or is
    /// exiting by its own account.
    func disarm() {
        try? FileManager.default.removeItem(at: url)
    }

    /// The record a previous process left, removed so that it is reported
    /// once. A file that cannot be read still counts as a marker.
    func consume() -> Record? {
        guard FileManager.default.fileExists(atPath: url.path) else {
            return nil
        }
        defer { disarm() }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        return (try? Data(contentsOf: url))
            .flatMap { try? decoder.decode(Record.self, from: $0) }
            ?? Record(enteredAt: .distantPast)
    }
}

/// That the previous process ended in the background, so this launch's guest
/// started again from boot.
struct GuestRestart: Equatable {
    let enteredBackgroundAt: Date

    var title: String { "Guest restarted" }

    var message: String {
        "Kernarium was closed while it was in the background, so the guest " +
            "started again from boot."
    }

    var systemName: String { "arrow.clockwise" }
}

/// What the application knows about its time in the background across
/// launches.
@MainActor
final class BackgroundSession: ObservableObject {
    static let shared = BackgroundSession(marker: BackgroundMarker(
        url: BackgroundMarker.defaultURL
    ))

    private let marker: BackgroundMarker
    private var inForeground = true
    /// Resumes of a quiesced guest that have started and not returned.
    private var resumesInFlight = 0
    /// Set when this launch follows a process that ended in the background,
    /// until the user dismisses it.
    @Published private(set) var restart: GuestRestart?

    init(marker: BackgroundMarker) {
        self.marker = marker
        restart = marker.consume().map {
            GuestRestart(enteredBackgroundAt: $0.enteredAt)
        }
        if let restart {
            print(
                "Kernarium: the previous process ended in the background " +
                    "(entered at \(restart.enteredBackgroundAt)); the guest " +
                    "started again from boot"
            )
        }
    }

    func didEnterBackground() {
        inForeground = false
        marker.arm()
    }

    /// The marker stays while a resume is under way: a guest whose
    /// hibernated RAM cannot be restored ends the process inside it, and the
    /// next launch should still say the guest restarted.
    func willEnterForeground() {
        inForeground = true
        disarmWhenSettled()
    }

    func willTerminate() {
        marker.disarm()
    }

    /// A quiesced guest is being resumed.
    func guestResumeStarted() {
        resumesInFlight += 1
    }

    /// The resume returned, so the guest and everything it hibernated are
    /// back.
    func guestResumeFinished() {
        resumesInFlight = max(resumesInFlight - 1, 0)
        disarmWhenSettled()
    }

    private func disarmWhenSettled() {
        if inForeground && resumesInFlight == 0 {
            marker.disarm()
        }
    }

    func dismissRestartNotice() {
        restart = nil
    }
}
