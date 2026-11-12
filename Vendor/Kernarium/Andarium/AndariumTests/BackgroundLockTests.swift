// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Darwin
import Foundation
import Testing
import UIKit
@testable import Andarium

/// iOS ends a process it suspends while the process holds a file lock, so a
/// guest's disks may carry none at any moment iOS could suspend Andarium: while
/// the guest runs, at the background edge, all through the quiesce that
/// follows it (the guest-sync task can expire at any point of it), and after
/// the return. A guest that was drawing when the GPU was held must still
/// suspend: its tasks wait for the host's answer to a buffer they are
/// creating, and the freezer has to freeze them even though no answer comes
/// while the GPU is held. A guest that did not suspend keeps running and
/// answers the test, so the test requires that nothing answers while the
/// application is away. A real guest runs through the application's own
/// lifecycle edges, posted as UIKit posts them:
///
///     TEST_RUNNER_ANDARIUM_BACKGROUND_LOCK_TESTS=1 \
///         TEST_RUNNER_ANDARIUM_GUEST_INSTANCE=<test instance> \
///         xcodebuild test -scheme Andarium -destination 'platform=macOS,variant=Designed for iPad' \
///         -only-testing:AndariumTests/BackgroundLockTests
///
/// `ANDARIUM_BACKGROUND_LOCK_LOAD` selects what the guest does while the
/// application goes away: `allocating` (the default) opens and closes
/// activities, so windows keep allocating buffers; `swiping` swipes in the
/// foreground activity; `idle` leaves the launcher still. Each run prints a
/// `BGLOCK:` summary: the locks seen and when, the tasks in D state while the
/// application was away and the framework's not-responding reports; the
/// console shows how the suspend went.
@Suite(.serialized)
@MainActor
struct BackgroundLockTests {
    private static var environment: [String: String] { ProcessInfo.processInfo.environment }
    private static var enabled: Bool { environment["ANDARIUM_BACKGROUND_LOCK_TESTS"] == "1" }
    private static var load: String { environment["ANDARIUM_BACKGROUND_LOCK_LOAD"] ?? "allocating" }

    /// Marks the load's shell, so the guest can stop it by name.
    private static let loopMarker = "bglock-loop"

    /// The shell loop for a load, or nil for an idle guest.
    private static var loadScript: String? {
        switch load {
        case "swiping":
            return "while :; do input swipe 560 600 560 200 250; input swipe 560 200 560 600 250; done"
        case "idle":
            return nil
        default:
            // Each activity that opens takes new window buffers, and each
            // buffer is a resource the host must create.
            return "while :; do am start -W -a android.settings.SETTINGS; input keyevent KEYCODE_BACK; " +
                "am start -W -a android.settings.DISPLAY_SETTINGS; input keyevent KEYCODE_BACK; done"
        }
    }

    /// How long the application stays away: the whole quiesce, a suspend that
    /// outlasts its ten-second deadline included, and a while after it.
    private static let away = Duration.seconds(30)

    /// The guest's two disks, as the runtime opens them.
    private static func guestDisks() throws -> [URL] {
        let entry = AppEntry.fromArguments()
        let data = GuestLocations(directory: AppEntry.guestDirectory(instance: entry.overrides.instance)).data
        let release = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        return [data, release.appendingPathComponent(SystemFolder.systemDisk)]
    }

    @Test(.timeLimit(.minutes(30))) func noDiskLockOutlivesTheBackgroundEdge() async throws {
        guard Self.enabled else { return }
        let disks = try Self.guestDisks()
        // Outlives the guest: its probes close after the guest has ended.
        var watcher: DiskLockWatcher?
        defer { withExtendedLifetime(watcher) {} }
        try await withAndroidGuest(cleanupCommand: "pkill -f 'bglock-loo[p]'; true") { _, operations in
            for disk in disks {
                try #require(FileManager.default.fileExists(atPath: disk.path), "\(disk.path) is missing")
            }
            let probe = DiskLockWatcher(disks: disks)
            watcher = probe
            let running = probe.lockedDisks()
            var loaded = "no load"
            if let script = Self.loadScript {
                let loop = try await operations.shell(
                    "nohup sh -c ': \(Self.loopMarker); \(script)' </dev/null >/dev/null 2>&1 &",
                    limit: .seconds(15))
                try #require(loop.exitCode == 0)
                try await Task.sleep(for: .seconds(10))
                if Self.load != "swiping" {
                    let settings = try await operations.shell("pidof com.android.settings", limit: .seconds(15))
                    loaded = settings.text.trimmingCharacters(in: .whitespacesAndNewlines)
                    try #require(!loaded.isEmpty, "the load never opened an activity")
                }
            }

            probe.start()
            let center = NotificationCenter.default
            center.post(name: UIApplication.willResignActiveNotification, object: UIApplication.shared)
            center.post(name: UIApplication.didEnterBackgroundNotification, object: UIApplication.shared)
            try await Task.sleep(for: Self.away)
            // A suspended guest answers nothing. One that did not suspend
            // runs on, with the tasks the held GPU keeps waiting in D state.
            let away = try? await operations.shell(
                "ps -A -o PID,STAT,WCHAN:40,NAME | grep -E '^ *[0-9]+ +D'", limit: .seconds(10))
            center.post(name: UIApplication.willEnterForegroundNotification, object: UIApplication.shared)
            center.post(name: UIApplication.didBecomeActiveNotification, object: UIApplication.shared)

            var answered = false
            let deadline = ContinuousClock.now + .seconds(120)
            while !answered, ContinuousClock.now < deadline {
                answered = (try? await operations.shell("echo ok", limit: .seconds(10)))?.text == "ok\n"
                if !answered { try await Task.sleep(for: .seconds(1)) }
            }
            let seen = probe.stop()
            _ = try? await operations.shell("pkill -f 'bglock-loo[p]'; true", limit: .seconds(15))
            let reports = try? await operations.shell(
                "logcat -b all -d | grep -E 'ANR i[n]|am_an[r]|WATCHDO[G]' | tail -n 5", limit: .seconds(30))

            print("BGLOCK: load \(Self.load) (\(loaded)); locked while running: \(running)")
            print("BGLOCK: locks seen from the edge to the return: \(seen.count) samples, first \(seen.first ?? "none")")
            print("BGLOCK: tasks in D state while away: \(away.map { $0.text.isEmpty ? "none" : $0.text } ?? "no answer")")
            print("BGLOCK: not-responding reports: \(reports.map { $0.text.isEmpty ? "none" : $0.text } ?? "no answer")")
            #expect(away == nil, "the guest answered while the application was away; it did not suspend: \(away?.text ?? "")")
            #expect(running.isEmpty, "a running guest's disks carry a host lock: \(running)")
            #expect(seen.isEmpty, "a disk carried a host lock while the application was away: \(seen.prefix(5))")
            #expect(answered, "the guest did not answer after the return")
        }
    }
}

/// Which of the disks this process holds a lock on. A lock of the process's own
/// never blocks it, but it does block an open file description lock asked for
/// through a descriptor of its own, which sees POSIX and flock locks alike.
/// Each disk keeps one probe descriptor for the whole watch: closing any
/// descriptor of a file ends every POSIX lock the process holds on it, so a
/// probe that opened and closed the disk for each look would remove the locks
/// it looks for. Close the probes only once the guest has ended.
nonisolated final class DiskLockWatcher: @unchecked Sendable {
    private let disks: [URL]
    private let probes: [Int32]
    private let lock = NSLock()
    private var stopping = false
    private var seen: [String] = []
    private var thread: Thread?
    private var started = DispatchTime.now()

    init(disks: [URL]) {
        self.disks = disks
        probes = disks.map { open($0.path, O_RDONLY) }
    }

    deinit {
        for probe in probes where probe >= 0 { close(probe) }
    }

    func lockedDisks() -> [String] {
        zip(disks, probes).compactMap { disk, probe in
            guard probe >= 0 else { return "\(disk.lastPathComponent): no probe" }
            var query = flock()
            query.l_type = Int16(F_WRLCK)
            query.l_whence = Int16(SEEK_SET)
            guard fcntl(probe, F_OFD_GETLK, &query) == 0 else {
                return "\(disk.lastPathComponent): query failed (\(errno))"
            }
            return query.l_type == Int16(F_UNLCK) ? nil : disk.lastPathComponent
        }
    }

    func start() {
        started = DispatchTime.now()
        let thread = Thread { [self] in
            while !lock.withLock({ stopping }) {
                let locked = lockedDisks()
                if !locked.isEmpty {
                    let milliseconds = (DispatchTime.now().uptimeNanoseconds - started.uptimeNanoseconds) / 1_000_000
                    lock.withLock { seen.append("+\(milliseconds) ms \(locked.joined(separator: ","))") }
                }
                usleep(5_000)
            }
        }
        thread.name = "bglock-watcher"
        self.thread = thread
        thread.start()
    }

    /// Stops the watch and returns every sample that found a lock.
    func stop() -> [String] {
        lock.withLock { stopping = true }
        while thread?.isFinished == false { usleep(1_000) }
        return lock.withLock { seen }
    }
}
