// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import GuestHostCore
import UIKit

/// iOS can reclaim a suspended application without running any more of its
/// code, and a guest's writable images are ordinary host files that nothing
/// else will flush. Every lifecycle edge that can precede a reclaim therefore
/// pushes what the guest has already written through to storage while the app
/// is still scheduled.
///
/// This is the host half of durability. Pages still dirty inside the guest are
/// written back by the guest kernel on its own schedule; nothing outside it can
/// force that.
@MainActor
final class GuestPersistence {
    static let shared = GuestPersistence()

    /// Long enough for a flush of a few images to finish, short enough that the
    /// app never looks wedged to the watchdog if storage is busy.
    private static let terminationBudget = DispatchTimeInterval.seconds(4)

    private let queue = DispatchQueue(
        label: "com.imxieyi.Kernarium.guest-sync",
        qos: .userInitiated
    )
    private var observers: [NSObjectProtocol] = []

    private init() {}

    /// What the application does about the background: the policy and the
    /// facilities it acts through. Created by `startObserving`.
    private(set) var background: BackgroundController?

    /// What the guest is asked to do at a lifecycle edge.
    enum Departure {
        /// Write back, but keep running. Losing focus is not losing the
        /// process, and freezing the guest for it would stall a user who is
        /// about to come straight back.
        case flush
        /// Write back and enter full system suspend. Backgrounding is the
        /// point after which iOS may never schedule this process again, and a
        /// guest left running would dirty what was just written back.
        case quiesce
    }

    /// - Parameter guest: the name the running guest's background mode is
    ///   stored under.
    func startObserving(
        guest: @escaping () -> String?,
        pictureInPicture: (any PictureInPictureWindow)? = nil
    ) {
        guard observers.isEmpty else { return }
        let settings = BackgroundSettings()
        let effects = SystemBackgroundEffects(
            persistence: self,
            pictureInPicture: pictureInPicture
        )
        let controller = BackgroundController(effects: effects) {
            guest().map(settings.mode(forGuest:)) ?? .off
        }
        background = controller
        BackgroundJob.shared.controller = controller
        pictureInPicture?.controller = controller
        let edges: [(Notification.Name, @MainActor () -> Void)] = [
            (UIApplication.willResignActiveNotification, {
                // A write-back is a console request, which ends the
                // suspension of a guest paused for space.
                guard self.background?.spaceHeld != true else { return }
                self.flush(
                    reason: "inactive",
                    departure: .flush,
                    waitingUntil: nil
                )
            }),
            (UIApplication.didEnterBackgroundNotification, {
                BackgroundSession.shared.didEnterBackground()
                self.background?.didEnterBackground()
            }),
            (UIApplication.didBecomeActiveNotification, {
                self.background?.didBecomeActive()
            }),
            (UIApplication.willTerminateNotification, {
                BackgroundSession.shared.willTerminate()
                // A guest paused for space has written everything back, and
                // a quiesce would write its RAM to a device short of space.
                guard self.background?.spaceHeld != true else { return }
                self.flush(
                    reason: "terminating",
                    departure: .quiesce,
                    waitingUntil: .now() + Self.terminationBudget
                )
            }),
            (UIApplication.willEnterForegroundNotification, {
                // The policy resumes a quiesced guest, which the session
                // must know has begun before it may drop its marker.
                self.background?.willEnterForeground()
                BackgroundSession.shared.willEnterForeground()
            }),
        ]
        observers = edges.map { name, edge in
            NotificationCenter.default.addObserver(
                forName: name,
                object: nil,
                queue: .main
            ) { _ in
                MainActor.assumeIsolated(edge)
            }
        }
    }

    /// Resumes on the flush queue, behind any quiesce still in flight. A quick
    /// return to the app would otherwise hold the main thread until that
    /// quiesce finished, and resuming first would leave the guest suspended.
    ///
    /// A guest whose hibernated RAM cannot be restored ends the process inside
    /// this call, so the session keeps its background marker until it returns.
    func resumeGuest() {
        BackgroundSession.shared.guestResumeStarted()
        queue.async {
            kernarium_runtime_loader_resume()
            Task { @MainActor in
                BackgroundSession.shared.guestResumeFinished()
            }
        }
    }

    /// Like `resumeGuest()`, and says on the main actor when the guest is back.
    func resumeGuest(finished: @escaping @MainActor () -> Void) {
        BackgroundSession.shared.guestResumeStarted()
        queue.async {
            kernarium_runtime_loader_resume()
            Task { @MainActor in
                BackgroundSession.shared.guestResumeFinished()
                finished()
            }
        }
    }

    /// Writes the guest's storage back and suspends it with its RAM kept in
    /// the process, for a device too short of space for a hibernation image,
    /// behind whatever the queue already holds. Says on the main actor once
    /// the runtime has returned.
    func quiesceResident(
        reason: String,
        finished: @escaping @MainActor () -> Void
    ) {
        let assertion = BackgroundTaskAssertion(named: "guest-sync")
        queue.async {
            let unflushed = kernarium_runtime_loader_quiesce_resident()
            Task { @MainActor in
                print(
                    "Kernarium: guest storage settled on \(reason), RAM " +
                        "kept (\(unflushed) unflushed)"
                )
                assertion.end()
                finished()
            }
        }
    }

    /// Switches the guest's periodic page-cache trimming through `set` on the
    /// flush queue, behind any write-back or quiesce already asked for. The
    /// switch may wait for the guest's console, and one made while the guest
    /// is quiesced reaches it on resume.
    func setPageCacheTrimming(
        _ enabled: Bool,
        through set: @escaping @Sendable (Int32) -> Int32
    ) {
        queue.async {
            let result = set(enabled ? 1 : 0)
            print(
                "Kernarium: guest page-cache trimming " +
                    "\(enabled ? "on" : "off") (\(result))"
            )
        }
    }

    func stopObserving() {
        for observer in observers {
            NotificationCenter.default.removeObserver(observer)
        }
        observers.removeAll()
    }

    /// Writes the guest's storage back, and suspends it for `.quiesce`.
    ///
    /// - Parameter waitingUntil: when to stop holding the main thread for the
    ///   flush to finish, or nil to return at once. Backgrounding and
    ///   termination wait, because the process may not be scheduled again;
    ///   merely losing focus does not, since the app keeps running and the
    ///   flush would only stall the UI.
    func flush(
        reason: String,
        departure: Departure,
        waitingUntil deadline: DispatchTime?
    ) {
        let assertion = BackgroundTaskAssertion(named: "guest-sync")
        let finished = DispatchSemaphore(value: 0)
        queue.async {
            let unflushed = switch departure {
            case .flush: kernarium_runtime_loader_sync()
            case .quiesce: kernarium_runtime_loader_quiesce()
            }
            finished.signal()
            Task { @MainActor in
                GuestPersistence.report(reason: reason, unflushed: unflushed)
                assertion.end()
            }
        }
        if let deadline {
            _ = finished.wait(timeout: deadline)
        }
    }

    private static func report(reason: String, unflushed: Int32) {
        if unflushed > 0 {
            print(
                "Kernarium: \(reason) sync left \(unflushed) guest " +
                "file(s) unflushed"
            )
        } else if unflushed == 0 {
            print("Kernarium: guest storage settled on \(reason)")
        }
    }
}

/// A background-task assertion that keeps the process scheduled until it
/// ends: the caller ends it, or iOS expires it. Ending it twice is harmless.
@MainActor
final class BackgroundTaskAssertion: BackgroundCancellable {
    private var identifier = UIBackgroundTaskIdentifier.invalid

    init(named name: String) {
        identifier = UIApplication.shared.beginBackgroundTask(withName: name) {
            self.end()
        }
    }

    func end() {
        guard identifier != .invalid else { return }
        UIApplication.shared.endBackgroundTask(identifier)
        identifier = .invalid
    }

    func cancel() {
        end()
    }
}

/// The window that keeps the guest alive in the background, as the policy
/// sees it. The window reports its own edges to `controller`.
@MainActor
protocol PictureInPictureWindow: AnyObject {
    var controller: BackgroundController? { get set }
    /// Whether iOS will show the window as the app leaves.
    var canStart: Bool { get }
    var isActive: Bool { get }
    func stop()
}

/// Pausing a guest for want of space on the real runtime, on the queue the
/// other guest requests share so that a quick resume comes after the suspend.
@MainActor
final class SystemSpacePauseEffects: SpacePauseEffects {
    private let persistence: GuestPersistence
    /// Statistics requests wait for the guest and never overlap.
    private let statisticsQueue = DispatchQueue(
        label: "com.imxieyi.Kernarium.space-statistics",
        qos: .utility
    )

    init(persistence: GuestPersistence) {
        self.persistence = persistence
    }

    func resumeGuest(finished: @escaping @MainActor () -> Void) {
        persistence.resumeGuest(finished: finished)
    }

    func suspendGuestResident(
        reason: String,
        finished: @escaping @MainActor () -> Void
    ) {
        persistence.quiesceResident(reason: reason, finished: finished)
    }

    func guestProcessorTime() async -> UInt64? {
        await withCheckedContinuation { continuation in
            statisticsQueue.async {
                var reading = kernarium_runtime_statistics()
                reading.struct_size = UInt32(
                    MemoryLayout<kernarium_runtime_statistics>.size
                )
                let answered =
                    kernarium_runtime_loader_read_statistics(&reading) == 0
                continuation.resume(
                    returning: answered ? reading.processor_time_ns : nil
                )
            }
        }
    }
}

/// The runtime's request gate for the guest's GPU.
nonisolated struct RuntimeGPUGate: GuestGPUGate {
    func hold() {
        let result = kernarium_runtime_loader_set_gpu_hold(1)
        print("Kernarium: guest GPU requests held on background (\(result))")
    }

    func release() {
        let result = kernarium_runtime_loader_set_gpu_hold(0)
        print("Kernarium: guest GPU requests released (\(result))")
    }
}

/// The background policy's effects on the real guest, display and system.
@MainActor
final class SystemBackgroundEffects: BackgroundEffects {
    private final class Token: BackgroundCancellable {
        private var action: (@MainActor () -> Void)?

        init(_ action: @escaping @MainActor () -> Void) {
            self.action = action
        }

        func cancel() {
            action?()
            action = nil
        }
    }

    /// How often the wait for the guest's display to go off looks again.
    private static let displayPoll: TimeInterval = 0.02

    private let persistence: GuestPersistence
    private let window: (any PictureInPictureWindow)?
    private let pageCacheTrimming: @Sendable (Int32) -> Int32
    private let displayActive: @Sendable () -> Int32
    private let holdDelayMilliseconds: @Sendable () -> Int32
    let gpuHold: GuestGPUHold

    /// - Parameters:
    ///   - pageCacheTrimming: the runtime's trimming switch.
    ///   - displayActive: the runtime's answer for the guest's display, 1 on,
    ///     0 off, negative when it cannot say.
    ///   - holdDelayMilliseconds: the delay an automation launch asked for
    ///     before the GPU is held, zero for none.
    ///   - gpuHold: the hold on the guest's GPU requests, the runtime's gate
    ///     unless a test brings its own.
    ///
    ///   The unit tests replace all four.
    init(
        persistence: GuestPersistence,
        pictureInPicture: (any PictureInPictureWindow)?,
        pageCacheTrimming: @escaping @Sendable (Int32) -> Int32 = {
            kernarium_runtime_loader_set_page_cache_trimming($0)
        },
        displayActive: @escaping @Sendable () -> Int32 = {
            kernarium_runtime_loader_guest_display_active()
        },
        holdDelayMilliseconds: @escaping @Sendable () -> Int32 = {
            kernarium_runtime_loader_automation_hold_delay_ms()
        },
        gpuHold: GuestGPUHold = GuestGPUHold(gate: RuntimeGPUGate())
    ) {
        self.persistence = persistence
        window = pictureInPicture
        self.pageCacheTrimming = pageCacheTrimming
        self.displayActive = displayActive
        self.holdDelayMilliseconds = holdDelayMilliseconds
        self.gpuHold = gpuHold
    }

    func flushGuest(until deadline: DispatchTime?) {
        persistence.flush(
            reason: "background",
            departure: .flush,
            waitingUntil: deadline
        )
    }

    func quiesceGuest(reason: String, until deadline: DispatchTime?) {
        persistence.flush(
            reason: reason,
            departure: .quiesce,
            waitingUntil: deadline
        )
    }

    func resumeGuest() {
        persistence.resumeGuest()
    }

    func setPageCacheTrimming(_ enabled: Bool) {
        persistence.setPageCacheTrimming(enabled, through: pageCacheTrimming)
    }

    func setBackgroundThrottle(_ enabled: Bool) {
        guard enabled == false || BackgroundSettings().throttlesKeptGuest else {
            return
        }
        let changed = kernarium_runtime_loader_set_background_throttle(
            enabled ? 1 : 0
        )
        print(
            "Kernarium: guest vCPUs \(enabled ? "throttled" : "restored") " +
                "(\(changed) thread(s))"
        )
    }

    var jobActive: Bool { BackgroundJob.shared.isRunning }

    var guestIsAndroid: Bool { EmulatorLauncher.shared.runningGuestIsAndroid }

    func pressPowerKey() {
        GuestSpecialKey.power.press()
    }

    var guestDisplayActive: Bool? {
        let answer = displayActive()
        return answer < 0 ? nil : answer != 0
    }

    func waitForGuestDisplayOff(until deadline: DispatchTime) -> Bool {
        while true {
            if displayActive() == 0 { return true }
            if DispatchTime.now() >= deadline { return false }
            Thread.sleep(forTimeInterval: Self.displayPoll)
        }
    }

    var automationHoldDelay: TimeInterval {
        TimeInterval(max(holdDelayMilliseconds(), 0)) / 1000
    }

    var pictureInPictureCanStart: Bool { window?.canStart ?? false }
    var pictureInPictureActive: Bool { window?.isActive ?? false }

    func stopPictureInPicture() {
        window?.stop()
    }

    func keepScheduled(named name: String) -> any BackgroundCancellable {
        BackgroundTaskAssertion(named: name)
    }

    func schedule(
        after seconds: TimeInterval,
        _ action: @escaping @MainActor () -> Void
    ) -> any BackgroundCancellable {
        let timer = Timer(timeInterval: seconds, repeats: false) { _ in
            MainActor.assumeIsolated { action() }
        }
        RunLoop.main.add(timer, forMode: .common)
        return Token { timer.invalidate() }
    }
}
