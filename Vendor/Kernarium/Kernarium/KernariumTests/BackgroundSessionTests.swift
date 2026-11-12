// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// iOS can end a suspended application without running any of its code, and
/// the guest it hosted goes with it. The application records the background
/// edge it can see so that the next launch can say what happened.
@MainActor
struct BackgroundSessionTests {
    /// The marker file of a test, in the scratch directory the test removes.
    private func scratchMarker(in directory: URL) -> BackgroundMarker {
        BackgroundMarker(url: directory.appendingPathComponent("marker.json"))
    }

    @Test
    func aMarkerLeftInTheBackgroundIsReportedOnceAtTheNextLaunch() throws {
        let directory = try makeScratchDirectory("background-session")
        defer { try? FileManager.default.removeItem(at: directory) }
        let marker = scratchMarker(in: directory)
        let entered = Date(timeIntervalSince1970: 1_800_000_000)
        #expect(marker.arm(at: entered))

        let relaunched = BackgroundSession(marker: marker)
        #expect(relaunched.restart?.enteredBackgroundAt == entered)
        #expect(relaunched.restart?.title == "Guest restarted")
        #expect(relaunched.restart?.message.contains("closed") == true)

        // Reported once: the launch after it starts clean.
        #expect(BackgroundSession(marker: marker).restart == nil)
    }

    @Test
    func returningToTheForegroundMeansThereIsNothingToReport() throws {
        let directory = try makeScratchDirectory("background-session")
        defer { try? FileManager.default.removeItem(at: directory) }
        let marker = scratchMarker(in: directory)
        let session = BackgroundSession(marker: marker)
        session.didEnterBackground()
        #expect(FileManager.default.fileExists(atPath: marker.url.path))
        session.willEnterForeground()
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
        #expect(BackgroundSession(marker: marker).restart == nil)
    }

    /// A hibernated guest that cannot be restored ends the process inside the
    /// resume. The marker must still be there, so that the next launch says the
    /// guest restarted.
    @Test
    func theMarkerStaysUntilTheGuestHasResumed() throws {
        let directory = try makeScratchDirectory("background-session")
        defer { try? FileManager.default.removeItem(at: directory) }
        let marker = scratchMarker(in: directory)
        let session = BackgroundSession(marker: marker)
        session.didEnterBackground()
        session.guestResumeStarted()
        session.willEnterForeground()
        #expect(FileManager.default.fileExists(atPath: marker.url.path))
        session.guestResumeFinished()
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
        #expect(BackgroundSession(marker: marker).restart == nil)
    }

    @Test
    func aResumeThatEndsInTheBackgroundLeavesTheMarker() throws {
        let directory = try makeScratchDirectory("background-session")
        defer { try? FileManager.default.removeItem(at: directory) }
        let marker = scratchMarker(in: directory)
        let session = BackgroundSession(marker: marker)
        session.didEnterBackground()
        session.guestResumeStarted()
        session.guestResumeFinished()
        #expect(FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test
    func aProcessThatExitsOnItsOwnAccountLeavesNoMarker() throws {
        let directory = try makeScratchDirectory("background-session")
        defer { try? FileManager.default.removeItem(at: directory) }
        let marker = scratchMarker(in: directory)
        let session = BackgroundSession(marker: marker)
        session.didEnterBackground()
        session.willTerminate()
        #expect(BackgroundSession(marker: marker).restart == nil)
    }

    @Test
    func anUnreadableMarkerStillCountsAndNoticeCanBeDismissed() throws {
        let directory = try makeScratchDirectory("background-session")
        defer { try? FileManager.default.removeItem(at: directory) }
        let marker = scratchMarker(in: directory)
        try Data("not json".utf8).write(to: marker.url)
        let session = BackgroundSession(marker: marker)
        #expect(session.restart != nil)
        session.dismissRestartNotice()
        #expect(session.restart == nil)
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test
    func aLaunchWithNoMarkerReportsNothing() throws {
        let directory = try makeScratchDirectory("background-session")
        defer { try? FileManager.default.removeItem(at: directory) }
        let marker = scratchMarker(in: directory)
        #expect(BackgroundSession(marker: marker).restart == nil)
    }
}
