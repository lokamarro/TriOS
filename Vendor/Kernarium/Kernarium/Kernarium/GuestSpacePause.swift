// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import Foundation
import GuestHostCore

/// What the space pause needs to know of the guest's run, and to do to it.
@MainActor
protocol SpaceRun: AnyObject {
    /// Whether a run is on and is not between two boots.
    var isActive: Bool { get }
    /// What the screen calls the running guest: "Android" or "Linux".
    var guestName: String { get }
    /// Ends the run the way leaving the guest does.
    func stop()
}

/// An answer the harness gives the screen over a paused guest, as the person
/// would by touch.
enum SpaceAnswer: String {
    case resume
    case shutdown
    case check
}

/// Pauses the running guest when the device runs short of space, and says so
/// on a screen over it.
///
/// A guest's disk is a sparse file that grows as the guest writes, so a
/// device that runs out of space under it fails a write the guest cannot
/// survive. While a run is on, the free space is read now and then
/// (`RunSpace.nextCheck`) and the guest is suspended below `RunSpace
/// .pauseBelowBytes`, with its RAM kept in the process, because the device
/// has no room for an image of it (`SpacePauser`). The person then resumes
/// the guest once there is room, or shuts it down, which ends the run the way
/// leaving the guest does.
///
/// The background policy knows the guest is paused (`BackgroundController
/// .spacePauseBegan`), so no lifecycle edge, Picture in Picture window or
/// keep-running job wakes, writes back or quiesces it. The space is read only
/// while the policy leaves the guest running.
@MainActor
final class GuestSpacePause: ObservableObject {
    static let shared: GuestSpacePause = GuestSpacePause(
        effects: SystemSpacePauseEffects(persistence: GuestPersistence.shared),
        freeSpace: SpaceHarness.current.freeSpace,
        run: EmulatorLauncher.shared,
        controller: { GuestPersistence.shared.background },
        endJob: { BackgroundJob.shared.cancel() },
        refreshWindow: { GuestPictureInPicture.shared.refresh() },
        harness: SpaceHarness.current
    )

    /// How long Shut down may take before its buttons come back: the run
    /// ending takes the screen away.
    static let shutDownBound = Duration.seconds(60)
    /// How often the watch looks again while there is nothing to read: the
    /// guest is between boots, or the policy has paused it.
    nonisolated static let idleInterval = Duration.seconds(5)
    nonisolated static let answerInterval = Duration.seconds(1)

    /// The screen over the guest while it is paused or being paused; nil while
    /// the guest runs.
    @Published private(set) var pause: SpacePause?

    private let freeSpace: @Sendable () -> UInt64
    private let run: any SpaceRun
    private let controller: () -> BackgroundController?
    private let endJob: () -> Void
    private let refreshWindow: () -> Void
    private let harness: SpaceHarness?
    private let idleInterval: Duration
    private let answerInterval: Duration
    private let checkInterval: (UInt64) -> Duration
    private let pauser: SpacePauser
    private var watch: Task<Void, Never>?
    private var answers: Task<Void, Never>?

    init(
        effects: any SpacePauseEffects,
        freeSpace: @escaping @Sendable () -> UInt64,
        run: any SpaceRun,
        controller: @escaping () -> BackgroundController?,
        endJob: @escaping () -> Void = {},
        refreshWindow: @escaping () -> Void = {},
        harness: SpaceHarness? = nil,
        answerInterval: Duration = GuestSpacePause.answerInterval,
        idleInterval: Duration = GuestSpacePause.idleInterval,
        checkInterval: @escaping (UInt64) -> Duration = {
            RunSpace.nextCheck(freeBytes: $0)
        },
        wait: @escaping (Duration) async -> Void = {
            try? await Task.sleep(for: $0)
        }
    ) {
        self.idleInterval = idleInterval
        self.answerInterval = answerInterval
        self.checkInterval = checkInterval
        self.freeSpace = freeSpace
        self.run = run
        self.controller = controller
        self.endJob = endJob
        self.refreshWindow = refreshWindow
        self.harness = harness
        pauser = SpacePauser(effects: effects, wait: wait)
    }

    /// Whether the guest is paused for space, or being paused, so that no
    /// window or job is wanted for it.
    var holdsGuest: Bool { pause != nil }

    // MARK: Watch

    /// A run started: the space is watched until it ends.
    func runStarted() {
        guard watch == nil else { return }
        watch = Task { [weak self] in await self?.watchSpace() }
        if harness?.enabled == true, answers == nil {
            answers = Task { [weak self] in await self?.answerFromHarness() }
        }
    }

    /// The run returned: nothing is left to pause or resume.
    func runEnded() {
        watch?.cancel()
        watch = nil
        answers?.cancel()
        answers = nil
        pauser.end()
        if pause != nil {
            pause = nil
            controller()?.spacePauseEnded(guestSuspended: true)
        }
    }

    /// The guest is running as far as the policy can say, and nothing asks
    /// for the space to be left alone.
    private var wantsWatch: Bool {
        run.isActive && pause == nil && (controller()?.guestRuns ?? true)
    }

    private func readFreeSpace() async -> UInt64 {
        let source = freeSpace
        return await Task.detached(priority: .utility) { source() }.value
    }

    private func watchSpace() async {
        while !Task.isCancelled {
            guard wantsWatch else {
                try? await Task.sleep(for: idleInterval)
                continue
            }
            let free = await readFreeSpace()
            guard !Task.isCancelled, wantsWatch else { continue }
            if RunSpace.verdict(freeBytes: free) == .pause {
                await pauseGuest(freeBytes: free)
                continue
            }
            try? await Task.sleep(for: checkInterval(free))
        }
    }

    // MARK: Pause

    /// Suspends the guest and says what came of it: the screen offers Resume
    /// over a guest that holds still and Shut down over one that did not.
    private func pauseGuest(freeBytes: UInt64) async {
        guard run.isActive else { return }
        pauser.begin()
        print(
            "Kernarium: the guest is pausing: " +
                "\(Formats.size(bytes: freeBytes)) of the device's space is free"
        )
        controller()?.spacePauseBegan()
        pause = SpacePause(
            guest: run.guestName, freeBytes: freeBytes, stage: .pausing,
            ending: .stop
        )
        // A job keeps a guest running, and this one does not; a window shows
        // one that runs.
        endJob()
        refreshWindow()
        await settle()
    }

    private func settle() async {
        guard let outcome = await pauser.settle() else { return }
        switch outcome {
        case .held:
            print("Kernarium: the guest is paused")
            pause?.stage = .paused
        case .running:
            print("Kernarium: the guest could not be paused")
            controller()?.spacePauseEnded(guestSuspended: false)
            pause?.stage = .notPaused
        }
    }

    // MARK: Answers

    /// Resume: reads the space again and wakes the guest when there is enough,
    /// and otherwise says what is free now.
    func resume() {
        guard var current = pause, current.stage == .paused,
              current.working == nil else { return }
        current.working = .resuming
        pause = current
        Task { await resumeAfterReading() }
    }

    private func resumeAfterReading() async {
        let free = await readFreeSpace()
        guard pause?.working == .resuming else { return }
        let enough = RunSpace.allowsResume(freeBytes: free)
        // A guest is woken in the foreground only: an edge that came
        // meanwhile leaves it paused.
        guard enough, controller()?.isForeground ?? true else {
            print(
                "Kernarium: Resume waits: \(Formats.size(bytes: free)) of " +
                    "the device's space is free" +
                    (enough ? ", in the background" : "")
            )
            pause?.freeBytes = free
            pause?.stillShort = !enough
            pause?.working = nil
            return
        }
        await wake()
        guard pause?.working == .resuming else { return }
        print(
            "Kernarium: the guest resumes: " +
                "\(Formats.size(bytes: free)) of the device's space is free"
        )
        controller()?.spacePauseEnded(guestSuspended: true)
        pause = nil
        refreshWindow()
    }

    private func wake() async {
        await withCheckedContinuation {
            (continuation: CheckedContinuation<Void, Never>) in
            if !pauser.resume(finished: { continuation.resume() }) {
                continuation.resume()
            }
        }
    }

    /// Shut down: the run ends the way leaving the guest does. The suspension
    /// already wrote the guest's storage back, so nothing is asked of the
    /// guest. The run ending takes the screen away.
    func shutDown() {
        guard var current = pause, current.stage != .pausing,
              current.working == nil else { return }
        current.working = .shuttingDown
        pause = current
        run.stop()
        Task { [weak self] in
            try? await Task.sleep(for: Self.shutDownBound)
            guard let self, self.pause?.working == .shuttingDown else { return }
            self.pause?.working = nil
        }
    }

    /// Check again, over a guest that did not pause: the screen goes when
    /// there is room for the guest to run on, and otherwise the guest is
    /// paused once more.
    func checkAgain() {
        guard var current = pause, current.stage == .notPaused,
              current.working == nil else { return }
        current.stage = .pausing
        pause = current
        Task { await checkAfterReading() }
    }

    private func checkAfterReading() async {
        let free = await readFreeSpace()
        guard pause?.stage == .pausing else { return }
        guard !RunSpace.allowsResume(freeBytes: free) else {
            pause = nil
            refreshWindow()
            return
        }
        pause?.freeBytes = free
        pauser.begin()
        controller()?.spacePauseBegan()
        await settle()
    }

    // MARK: Harness

    private func answerFromHarness() async {
        guard let harness else { return }
        while !Task.isCancelled {
            try? await Task.sleep(for: answerInterval)
            guard let current = pause, current.working == nil,
                  let answer = harness.answer() else { continue }
            switch (answer, current.stage) {
            case (.resume, .paused): resume()
            case (.shutdown, .paused), (.shutdown, .notPaused): shutDown()
            case (.check, .notPaused): checkAgain()
            default: continue
            }
            harness.consumeAnswer()
            print("Kernarium: the harness answered the space screen: \(answer)")
        }
    }
}
