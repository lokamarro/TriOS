// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a guest does when Kernarium leaves the foreground. A guest runs only
/// in the foreground unless its owner chose otherwise: the background modes
/// keep the guest's CPUs busy on the battery, so each is opt-in per guest.
enum BackgroundMode: String, CaseIterable, Identifiable {
    /// Write the guest's storage back and pause it, as every app does.
    case off
    /// Keep the guest running while its display shows in a Picture in Picture
    /// window, which iOS keeps alive for as long as the user leaves it open.
    case pictureInPicture
    /// Keep the guest running for a time the user chose, which they start with
    /// a button: iOS runs the app as a continued task and shows its progress.
    /// The guest's graphics wait meanwhile, because iOS withholds the GPU.
    case keepRunning

    var id: String { rawValue }

    var title: String {
        switch self {
        case .off: "Pause the guest"
        case .pictureInPicture: "Keep running in Picture in Picture"
        case .keepRunning: "Keep running for a set time"
        }
    }

    var explanation: String {
        switch self {
        case .off:
            "The guest is paused while Kernarium is in the background and " +
                "continues when you return."
        case .pictureInPicture:
            "The guest keeps running while its display shows in a small " +
                "floating window. Close the window to pause the guest. This " +
                "uses the battery as the guest does in the foreground."
        case .keepRunning:
            "Tap the keep running button, then leave Kernarium: the guest " +
                "keeps working, with its graphics paused, until the time " +
                "is up. iOS shows the progress and lets you stop it at any " +
                "time. Android turns its screen off meanwhile."
        }
    }
}

/// The mode each guest chose, stored under the guest's name. A guest that
/// never chose one pauses.
struct BackgroundSettings {
    static let keyPrefix = "background.mode."
    /// Automation's way to run one launch in a mode without changing what the
    /// user chose, as the other `KERNARIUM_` knobs do.
    static let environmentKey = "KERNARIUM_BACKGROUND_MODE"
    /// "0" keeps the vCPUs at full priority while the guest is kept running,
    /// so a measurement can compare the two.
    static let throttleEnvironmentKey = "KERNARIUM_BACKGROUND_THROTTLE"
    static let limitKeyPrefix = "background.limitMinutes."
    /// Automation's way to run one launch with another time limit, in whole
    /// minutes, so a test does not wait out a user's choice.
    static let limitEnvironmentKey = "KERNARIUM_BACKGROUND_LIMIT_MINUTES"
    /// The times offered for a kept-running job, and the one a guest has until
    /// it chooses.
    static let limitChoicesMinutes = [15, 30, 60, 120]
    static let defaultLimitMinutes = 30
    static let limitRangeMinutes = 1...(24 * 60)

    private let defaults: UserDefaults
    private let environment: [String: String]

    init(
        defaults: UserDefaults = .standard,
        environment: [String: String] = ProcessInfo.processInfo.environment
    ) {
        self.defaults = defaults
        self.environment = environment
    }

    func mode(forGuest guest: String) -> BackgroundMode {
        if let forced = environment[Self.environmentKey]
            .flatMap(BackgroundMode.init(rawValue:)) {
            return forced
        }
        return defaults.string(forKey: Self.keyPrefix + guest)
            .flatMap(BackgroundMode.init(rawValue:)) ?? .off
    }

    /// Whether a guest kept running in the background runs at the system's
    /// background priority. It does unless automation says otherwise.
    var throttlesKeptGuest: Bool {
        environment[Self.throttleEnvironmentKey] != "0"
    }

    /// How long a kept-running job lasts for this guest, in minutes.
    func limitMinutes(forGuest guest: String) -> Int {
        if let forced = environment[Self.limitEnvironmentKey].flatMap(Int.init),
           Self.limitRangeMinutes.contains(forced) {
            return forced
        }
        let stored = defaults.integer(forKey: Self.limitKeyPrefix + guest)
        return Self.limitRangeMinutes.contains(stored) ?
            stored : Self.defaultLimitMinutes
    }

    /// A time limit as its owner reads it: "1 hour", "2 hours", "90 minutes"
    /// or "1 minute".
    static func limitTitle(minutes: Int) -> String {
        if minutes % 60 == 0 {
            return minutes == 60 ? "1 hour" : "\(minutes / 60) hours"
        }
        return minutes == 1 ? "1 minute" : "\(minutes) minutes"
    }

    func setLimitMinutes(_ minutes: Int, forGuest guest: String) {
        if minutes == Self.defaultLimitMinutes ||
            !Self.limitRangeMinutes.contains(minutes) {
            defaults.removeObject(forKey: Self.limitKeyPrefix + guest)
        } else {
            defaults.set(minutes, forKey: Self.limitKeyPrefix + guest)
        }
    }

    func setMode(_ mode: BackgroundMode, forGuest guest: String) {
        if mode == .off {
            defaults.removeObject(forKey: Self.keyPrefix + guest)
        } else {
            defaults.set(mode.rawValue, forKey: Self.keyPrefix + guest)
        }
    }
}

/// A scheduled action that can be withdrawn.
@MainActor
protocol BackgroundCancellable: AnyObject {
    func cancel()
}

/// Everything the background policy does to the guest, the display and the
/// system, behind one seam so the policy can be tested without any of them.
@MainActor
protocol BackgroundEffects: AnyObject {
    /// The hold on the guest's GPU requests, which keeps every one off the
    /// host GPU while iOS has revoked it. It can be taken from any thread.
    var gpuHold: GuestGPUHold { get }
    /// Writes the guest's storage back and leaves it running. Waits for the
    /// write-back until `deadline`, or not at all when it is nil.
    func flushGuest(until deadline: DispatchTime?)
    /// Writes the guest's storage back and suspends it.
    func quiesceGuest(reason: String, until deadline: DispatchTime?)
    func resumeGuest()
    /// Asks the guest to trim its page cache while it runs unseen.
    func setPageCacheTrimming(_ enabled: Bool)
    /// Runs the guest's vCPUs at the system's background priority while it
    /// runs unseen, so it keeps going on a fraction of the power.
    func setBackgroundThrottle(_ enabled: Bool)

    /// Whether the user started a job that iOS is running, so the app may keep
    /// the guest running in the background without a window.
    var jobActive: Bool { get }
    /// Whether the running guest is Android, which has a screen to turn off.
    var guestIsAndroid: Bool { get }
    /// Presses and releases the guest's power key, which turns an Android
    /// screen off or on and wakes a guest that suspended itself.
    func pressPowerKey()
    /// Whether the guest's display is on, or nil when the runtime cannot say.
    /// The guest's display is on while it has its primary scanout.
    var guestDisplayActive: Bool? { get }
    /// Waits until `deadline` for the guest's display to go off, and returns
    /// whether it did. The wait ends at once when it is off already.
    func waitForGuestDisplayOff(until deadline: DispatchTime) -> Bool
    /// How long after the edge an automation launch asked for the guest's GPU
    /// to be held, so a measurement can find how long the host GPU stays
    /// usable in the background. Zero, the only value a release build has,
    /// holds it at the edge.
    var automationHoldDelay: TimeInterval { get }

    /// Whether iOS will start the Picture in Picture window as the app leaves.
    var pictureInPictureCanStart: Bool { get }
    var pictureInPictureActive: Bool { get }
    func stopPictureInPicture()

    /// Keeps the process scheduled while the policy waits on the system;
    /// the returned value ends the wait.
    func keepScheduled(named name: String) -> BackgroundCancellable
    func schedule(
        after seconds: TimeInterval,
        _ action: @escaping @MainActor () -> Void
    ) -> BackgroundCancellable
}

/// Decides what happens to the guest at each lifecycle edge. It owns no
/// system facility: it asks its effects, in an order that keeps the GPU safe.
///
/// iOS revokes the host GPU a few seconds after the app enters the
/// background unless something keeps it running, and a guest request that
/// reaches the GPU then makes the guest's Vulkan device lost for good. So the
/// GPU is held at the edge and released only once the guest is safe: quiesced
/// (it issues nothing), or kept alive by a Picture in Picture window that
/// iOS has started.
@MainActor
final class BackgroundController {
    enum Phase: Equatable {
        case foreground
        /// In the background with the GPU held, waiting for the Picture in
        /// Picture window the system starts on its own.
        case awaitingPictureInPicture
        /// In the background, the guest running behind its window.
        case keptRunning
        /// In the background, the guest running for a job the user started,
        /// with the GPU held: iOS gives a continued task no GPU.
        case jobRunning
        /// In the background with the guest paused.
        case quiesced
    }

    /// How long iOS gets to show the window before the guest is paused: it
    /// starts it about a second and a half after the app leaves.
    static let pictureInPictureStartBudget: TimeInterval = 3
    /// What the edge handler's own work may take: the watchdog answers to the
    /// handler as a whole.
    static let edgeBudget = DispatchTimeInterval.seconds(4)
    /// The latest the GPU of a kept-running job is held after the edge. An
    /// Android guest gets this long to turn its screen off first: a screen
    /// that is still drawing when the hold lands leaves its threads waiting on
    /// a GPU that does not answer, so the screen goes off while the GPU still
    /// works. The bound is a timer off the main thread, so what the main
    /// thread is doing cannot move it.
    ///
    /// iOS first denies the app the GPU 2.73 s after the edge at the earliest
    /// (measured over five edges on an iPad: 2.73 to 3.26 s, and 4.1 to 4.7 s
    /// after the app resigns active), and a hold that lands before that costs
    /// the guest nothing. 1.5 s leaves 1.2 s. The hold is not taken when the
    /// app resigns active, 0.8 to 1.7 s before the edge, because the app
    /// resigns active for a pulled-down shade or a call banner as well, and
    /// freezing the guest's graphics for a user who stays would be wrong.
    static let gpuHoldBound = DispatchTimeInterval.milliseconds(1500)

    private(set) var phase = Phase.foreground {
        didSet {
            guard phase != oldValue else { return }
            print("Kernarium: background policy \(oldValue) -> \(phase)")
        }
    }
    private let effects: any BackgroundEffects
    private let mode: () -> BackgroundMode
    private var startDeadline: (any BackgroundCancellable)?
    private var keepScheduled: (any BackgroundCancellable)?
    private var guestQuiesced = false
    /// The guest is paused for want of space on the device
    /// (`GuestSpacePause`), which only the person ends. While it is, no edge
    /// resumes the guest, writes its storage back or quiesces it: any of them
    /// would end the suspension, and a quiesce would write the RAM image the
    /// device has no room for.
    private(set) var spaceHeld = false
    /// The Android screen this controller turned off, to turn on again.
    private var screenTurnedOff = false
    /// What keeps the process scheduled while a hold an automation launch put
    /// off has not landed.
    private var holdDelayScheduled: (any BackgroundCancellable)?

    init(
        effects: any BackgroundEffects,
        mode: @escaping () -> BackgroundMode
    ) {
        self.effects = effects
        self.mode = mode
    }

    func didEnterBackground() {
        guard phase == .foreground else { return }
        let deadline = DispatchTime.now() + Self.edgeBudget
        let chosen = mode()
        // The edge the hold's timing is measured from.
        print("Kernarium: background edge, mode \(chosen.rawValue)")
        // A guest paused for space stays as it is, and resident: the GPU is
        // held as for any guest that leaves the foreground, and nothing else
        // is asked of it.
        if spaceHeld {
            holdGPU()
            phase = .quiesced
            return
        }
        if chosen == .keepRunning && effects.jobActive {
            enterBackgroundWithJob(until: deadline)
            return
        }
        holdGPU()
        switch chosen {
        case .pictureInPicture
        where effects.pictureInPictureActive ||
            effects.pictureInPictureCanStart:
            effects.flushGuest(until: deadline)
            if effects.pictureInPictureActive {
                keepGuestRunning()
            } else {
                awaitPictureInPicture()
            }
        case .off, .pictureInPicture, .keepRunning:
            quiesce(reason: "background", until: deadline)
        }
    }

    /// The guest keeps running for a job, with the GPU held for the whole stay.
    /// An Android screen that stays on keeps the guest busy, and with it off
    /// the guest suspends itself and only its wakelock holders and alarms run,
    /// as on a phone. The screen goes off before the GPU is held: its
    /// turning off draws, and a visible app that is drawing when the hold
    /// lands has its render and upload threads waiting on a GPU that does not
    /// answer, so it cannot handle the framework's pause and the screen-off
    /// broadcast, and Android kills it as not responding. A screen that is off
    /// already is left as it is, which the return leaves as well.
    ///
    /// The hold has its own bound, armed before anything here can wait: it is
    /// taken when the screen is off or `gpuHoldBound` after the edge, whichever
    /// comes first, and neither the wait for the screen nor the guest's
    /// storage settling afterwards can delay it.
    private func enterBackgroundWithJob(until deadline: DispatchTime) {
        let delay = effects.automationHoldDelay
        armGPUHold(delay: delay)
        if effects.guestIsAndroid {
            turnAndroidScreenOff(before: deadline)
        }
        // A launch that asked for a later hold keeps it later.
        if delay <= 0 {
            holdGPU()
        }
        effects.flushGuest(until: deadline)
        effects.setPageCacheTrimming(true)
        phase = .jobRunning
    }

    private func turnAndroidScreenOff(before deadline: DispatchTime) {
        guard effects.guestDisplayActive != false else {
            print("Kernarium: guest display already off")
            return
        }
        effects.pressPowerKey()
        screenTurnedOff = true
        // A launch that puts the hold off to measure it does not wait for the
        // screen, which would move the hold it asked for.
        guard effects.automationHoldDelay <= 0 else { return }
        let limit = min(deadline, DispatchTime.now() + Self.gpuHoldBound)
        let started = DispatchTime.now()
        let off = effects.waitForGuestDisplayOff(until: limit)
        let milliseconds = (
            DispatchTime.now().uptimeNanoseconds - started.uptimeNanoseconds
        ) / 1_000_000
        print(
            off
                ? "Kernarium: guest display off after \(milliseconds) ms"
                : "Kernarium: guest display still on after \(milliseconds) ms; holding the GPU"
        )
    }

    /// The job ended: its time ran out, the user stopped it, or iOS
    /// expired it. In the background nothing keeps the process running any
    /// more, so the guest pauses; in the foreground it carries on.
    func jobDidEnd() {
        guard phase == .jobRunning else { return }
        effects.setPageCacheTrimming(false)
        quiesce(reason: "job ended")
    }

    /// Whether the guest is running as far as the policy can say: it has not
    /// paused it for the background, and the space pause has not stopped it.
    var guestRuns: Bool { phase != .quiesced && !spaceHeld }

    /// Whether the application is in the foreground.
    var isForeground: Bool { phase == .foreground }

    /// The guest is being paused for space. A guest the policy keeps running
    /// out of sight stops being kept: whatever runs it unseen ends first, so
    /// the suspension is not throttled, and the policy stands as for a guest
    /// it paused, without a quiesce of its own.
    func spacePauseBegan() {
        guard !spaceHeld else { return }
        spaceHeld = true
        switch phase {
        case .keptRunning:
            endKeptRunning()
            holdGPU()
            effects.stopPictureInPicture()
            phase = .quiesced
        case .jobRunning:
            // The job's GPU hold stays; the guest no longer needs trimming.
            effects.setPageCacheTrimming(false)
            phase = .quiesced
        case .awaitingPictureInPicture:
            endWait()
            phase = .quiesced
        case .foreground, .quiesced:
            break
        }
    }

    /// The pause for space ended: the person resumed the guest, or it could
    /// not be paused and runs on. A guest that runs on, behind an application
    /// that has left the foreground, is paused as the background asks, which
    /// is what the edge would have done.
    func spacePauseEnded(guestSuspended: Bool) {
        guard spaceHeld else { return }
        spaceHeld = false
        if !guestSuspended && phase == .quiesced && !guestQuiesced {
            quiesce(reason: "background")
        }
    }

    /// The Picture in Picture window is on screen.
    func pictureInPictureDidStart() {
        guard phase == .awaitingPictureInPicture else { return }
        endWait()
        keepGuestRunning()
    }

    /// iOS would not show the window.
    func pictureInPictureDidFailToStart() {
        guard phase == .awaitingPictureInPicture else { return }
        endWait()
        quiesce(reason: "picture in picture did not start")
    }

    /// The user closed the window. In the foreground that is the app's own
    /// doing and changes nothing; in the background nothing keeps the process
    /// running any more.
    func pictureInPictureDidStop() {
        switch phase {
        case .awaitingPictureInPicture:
            pictureInPictureDidFailToStart()
        case .keptRunning:
            endKeptRunning()
            holdGPU()
            quiesce(reason: "picture in picture closed")
        case .foreground, .jobRunning, .quiesced:
            break
        }
    }

    /// The app is on screen again: the window has done its work.
    func didBecomeActive() {
        if effects.pictureInPictureActive {
            effects.stopPictureInPicture()
        }
    }

    /// The guest restarted itself and its new kernel's console answers. What
    /// the policy applied to the guest belongs to a kernel, and the new one
    /// boots with neither the page-cache trimming nor the throttled vCPUs, so
    /// a guest kept running in the background gets both again. A guest in the
    /// foreground or paused wants neither; a guest that paused or is waiting
    /// for the window cannot have restarted into a running boot, since the
    /// runtime holds the next boot back while the GPU is held.
    func guestDidRestart() {
        switch phase {
        case .keptRunning:
            effects.setPageCacheTrimming(true)
            effects.setBackgroundThrottle(true)
        case .jobRunning:
            // iOS already runs a continued task at background priority.
            effects.setPageCacheTrimming(true)
        case .foreground, .awaitingPictureInPicture, .quiesced:
            break
        }
    }

    func willEnterForeground() {
        endWait()
        endHoldDelay()
        if phase == .keptRunning {
            endKeptRunning()
        }
        if phase == .jobRunning {
            effects.setPageCacheTrimming(false)
        }
        effects.gpuHold.release()
        if guestQuiesced {
            effects.resumeGuest()
            guestQuiesced = false
        }
        // The screen needs the GPU and a running guest, so it comes last.
        if screenTurnedOff {
            effects.pressPowerKey()
            screenTurnedOff = false
        }
        phase = .foreground
    }

    private func holdGPU() {
        effects.gpuHold.hold()
        endHoldDelay()
    }

    /// Arms the bound of a job's hold: `gpuHoldBound` after the edge, or, when
    /// an automation launch asked for the hold after a delay, that long after
    /// it. Only a job is delayed: the other modes release or keep the GPU on
    /// their own terms.
    private func armGPUHold(delay: TimeInterval) {
        guard delay > 0 else {
            effects.gpuHold.arm(after: Self.gpuHoldBound.seconds)
            return
        }
        print("Kernarium: automation holds the GPU \(delay) s after the edge")
        let scheduled = effects.keepScheduled(named: "gpu-hold-delay")
        holdDelayScheduled = scheduled
        // Ending the wait twice is harmless, so the hold that landed may end
        // it here and the return again.
        effects.gpuHold.arm(after: delay) {
            Task { @MainActor in scheduled.cancel() }
        }
    }

    private func endHoldDelay() {
        holdDelayScheduled?.cancel()
        holdDelayScheduled = nil
    }

    private func awaitPictureInPicture() {
        phase = .awaitingPictureInPicture
        keepScheduled = effects.keepScheduled(named: "picture-in-picture")
        startDeadline = effects.schedule(
            after: Self.pictureInPictureStartBudget
        ) { [weak self] in
            self?.pictureInPictureDidFailToStart()
        }
    }

    /// The window is up, so the host GPU stays available: the guest may use
    /// it again.
    private func keepGuestRunning() {
        effects.gpuHold.release()
        effects.setPageCacheTrimming(true)
        effects.setBackgroundThrottle(true)
        phase = .keptRunning
    }

    /// The guest is no longer to run unseen: it returns to full speed before
    /// anything else is asked of it, so a quiesce is not throttled.
    private func endKeptRunning() {
        effects.setBackgroundThrottle(false)
        effects.setPageCacheTrimming(false)
    }

    /// Quiesces with the whole of the edge budget, from now.
    private func quiesce(reason: String) {
        quiesce(reason: reason, until: DispatchTime.now() + Self.edgeBudget)
    }

    private func quiesce(reason: String, until deadline: DispatchTime) {
        effects.quiesceGuest(reason: reason, until: deadline)
        guestQuiesced = true
        phase = .quiesced
    }

    private func endWait() {
        startDeadline?.cancel()
        startDeadline = nil
        keepScheduled?.cancel()
        keepScheduled = nil
    }
}

private extension DispatchTimeInterval {
    /// The interval in seconds, for the timers that take them.
    var seconds: TimeInterval {
        switch self {
        case .seconds(let value): TimeInterval(value)
        case .milliseconds(let value): TimeInterval(value) / 1_000
        case .microseconds(let value): TimeInterval(value) / 1_000_000
        case .nanoseconds(let value): TimeInterval(value) / 1_000_000_000
        case .never: .infinity
        @unknown default: .infinity
        }
    }
}
