// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// The progress a continued task reports is the time it has used, and the
/// system must never see it complete before the time is up.
struct BackgroundJobTests {
    private let start = Date(timeIntervalSinceReferenceDate: 1_000_000)

    private func timeline(minutes: Int) -> BackgroundJobTimeline {
        BackgroundJobTimeline(started: start, limit: TimeInterval(minutes * 60))
    }

    @Test
    func progressFollowsTheTimeUsedAndNeverCompletesEarly() {
        let job = timeline(minutes: 30)
        #expect(job.totalUnits == 1800)
        #expect(job.completedUnits(at: start) == 0)
        #expect(job.completedUnits(at: start.addingTimeInterval(600)) == 600)
        // Held below the total until the time is up.
        #expect(job.completedUnits(at: start.addingTimeInterval(1799.5)) == 1799)
        #expect(!job.isOver(at: start.addingTimeInterval(1799.5)))
        #expect(job.isOver(at: start.addingTimeInterval(1800)))
        #expect(job.completedUnits(at: start.addingTimeInterval(1800)) == 1800)
        #expect(job.completedUnits(at: start.addingTimeInterval(9000)) == 1800)
    }

    @Test
    func aClockBeforeTheStartCountsAsNothingUsed() {
        let job = timeline(minutes: 15)
        #expect(job.elapsed(at: start.addingTimeInterval(-30)) == 0)
        #expect(job.remaining(at: start.addingTimeInterval(-30)) == 900)
    }

    @Test
    func theCardSaysHowMuchTimeIsLeftInWholeMinutes() {
        let job = timeline(minutes: 30)
        #expect(job.subtitle(at: start) == "30 minutes left")
        #expect(job.subtitle(at: start.addingTimeInterval(61)) == "29 minutes left")
        #expect(job.subtitle(at: start.addingTimeInterval(1750)) == "1 minute left")
        #expect(job.subtitle(at: start.addingTimeInterval(1800)) == "Finishing")
    }
}

/// An automation launch can ask for the job to start with its guest. Nothing
/// else does: the release app starts a job only on the user's tap.
@MainActor
struct BackgroundJobAtStartTests {
    private let timeline = BackgroundJobTimeline(
        started: Date(timeIntervalSinceReferenceDate: 1_000_000),
        limit: 900
    )

    @Test
    func aGuestThatKeepsRunningStartsItsJobWhenAutomationAsks() {
        #expect(BackgroundJob.startsWithGuest(
            asked: true, mode: .keepRunning, state: .idle
        ))
    }

    @Test
    func nothingStartsAJobWithoutTheAsk() {
        for mode in [BackgroundMode.off, .pictureInPicture, .keepRunning] {
            #expect(!BackgroundJob.startsWithGuest(
                asked: false, mode: mode, state: .idle
            ))
        }
    }

    @Test
    func theAskStartsNothingForAGuestThatChoseAnotherMode() {
        for mode in [BackgroundMode.off, .pictureInPicture] {
            #expect(!BackgroundJob.startsWithGuest(
                asked: true, mode: mode, state: .idle
            ))
        }
    }

    @Test
    func theAskStartsNoSecondJob() {
        for state in [BackgroundJob.State.submitted, .running(timeline)] {
            #expect(!BackgroundJob.startsWithGuest(
                asked: true, mode: .keepRunning, state: state
            ))
        }
    }

    /// The setting's name is in the automation runtimes alone, so a launch
    /// that does not set it, or a runtime that has no such name, is not asked.
    @Test
    func aLaunchThatDoesNotAskIsNeverAsked() {
        #expect(
            ProcessInfo.processInfo.environment[
                "KERNARIUM_BACKGROUND_JOB_AT_START"
            ] == nil,
            "this test needs a launch that did not ask"
        )
        #expect(kernarium_runtime_loader_automation_job_at_start() == 0)
    }

    /// The same holds for the delay that moves the GPU hold, which only a
    /// runtime built for automation can be given.
    @Test
    func aLaunchThatDoesNotDelayTheHoldIsNeverDelayed() {
        #expect(
            ProcessInfo.processInfo.environment[
                "KERNARIUM_BACKGROUND_HOLD_DELAY_MS"
            ] == nil,
            "this test needs a launch that did not ask"
        )
        #expect(kernarium_runtime_loader_automation_hold_delay_ms() == 0)
    }
}

/// A task scheduler that iOS does not answer until the test says so.
@MainActor
final class FakeJobScheduler: BackgroundJobScheduling {
    private(set) var registrations: [String] = []
    private(set) var submissions: [(title: String, subtitle: String)] = []
    private(set) var withdrawn: [String] = []
    private var started: (@MainActor (any BackgroundJobTask) -> Void)?

    func register(
        identifier: String,
        started: @escaping @MainActor (any BackgroundJobTask) -> Void
    ) -> Bool {
        registrations.append(identifier)
        self.started = started
        return true
    }

    func submit(
        identifier: String,
        title: String,
        subtitle: String,
        refused: @escaping @MainActor (Error) -> Void
    ) {
        submissions.append((title, subtitle))
    }

    func cancelRequest(identifier: String) {
        withdrawn.append(identifier)
    }

    /// iOS starts the task it was asked for, now or late.
    func start(_ task: FakeJobTask) {
        started?(task)
    }
}

/// The continued task iOS gives a job: it records how the job completes it.
@MainActor
final class FakeJobTask: BackgroundJobTask {
    let progress = Progress()
    let title = "Android is running"
    private(set) var completions: [Bool] = []
    private var expiration: (() -> Void)?

    func onExpiration(_ handler: @escaping () -> Void) {
        expiration = handler
    }

    func updateTitle(_ title: String, subtitle: String) {}

    func setTaskCompleted(success: Bool) {
        completions.append(success)
    }

    /// iOS ends the task before the job does.
    func expire() {
        expiration?()
    }
}

/// What the background policy does at each edge; only the job's part of the
/// story is recorded.
@MainActor
final class RecordingEffects: BackgroundEffects {
    private let log = CallLog()
    var calls: [String] {
        get { log.entries }
        set { log.entries = newValue }
    }
    let gpuHold: GuestGPUHold
    var jobActive = false
    var guestIsAndroid = false
    var pictureInPictureCanStart = false
    var pictureInPictureActive = false
    var guestDisplayActive: Bool?
    var automationHoldDelay: TimeInterval = 0

    init() {
        gpuHold = GuestGPUHold(gate: RecordingGate(log))
    }

    func waitForGuestDisplayOff(until deadline: DispatchTime) -> Bool { true }
    func flushGuest(until deadline: DispatchTime?) { log.add("flush") }
    func quiesceGuest(reason: String, until deadline: DispatchTime?) {
        log.add("quiesce \(reason)")
    }
    func resumeGuest() { log.add("resume") }
    func setPageCacheTrimming(_ enabled: Bool) {
        log.add("trim \(enabled ? "on" : "off")")
    }
    func setBackgroundThrottle(_ enabled: Bool) {}
    func pressPowerKey() { log.add("power") }
    func stopPictureInPicture() {}
    func keepScheduled(named name: String) -> any BackgroundCancellable {
        Handle()
    }
    func schedule(
        after seconds: TimeInterval,
        _ action: @escaping @MainActor () -> Void
    ) -> any BackgroundCancellable {
        Handle()
    }

    private final class Handle: BackgroundCancellable {
        func cancel() {}
    }
}

/// How a run of the guest can end. Every one of them returns the runtime's
/// status to the launcher.
struct RunEnd: CustomTestStringConvertible, Sendable {
    let name: String
    let result: Int32
    var testDescription: String { name }

    static let all = [
        RunEnd(name: "the guest powered off", result: 0),
        RunEnd(name: "the runtime refused to start it", result: -4),
        RunEnd(name: "the guest stopped with an error", result: -5),
        RunEnd(
            name: "the guest kept restarting",
            result: Int32(KERNARIUM_RUNTIME_RESULT_RESTART_LOOP)
        ),
    ]
}

/// The job keeps a guest running in the background, so it belongs to one run of
/// that guest. When the run returns, whichever way it ended, the job completes
/// its task and leaves the application free to be suspended. A job that outlived
/// the run held a dead application in the background until its time was up. The
/// timer button and the automation launch start the same job, so one test of the
/// job covers both.
@MainActor
struct BackgroundJobRunEndTests {
    private let scheduler = FakeJobScheduler()
    private let job: BackgroundJob
    private let task = FakeJobTask()
    private let launcher: EmulatorLauncher

    init() {
        let scheduler = scheduler
        job = BackgroundJob(scheduler: scheduler)
        launcher = EmulatorLauncher(configuration: .load(), job: job)
    }

    /// Starts the job and lets iOS run it, as for a guest that is up.
    private func startRunningJob() {
        job.start(limitMinutes: 15, guestName: "Android")
        scheduler.start(task)
    }

    @Test
    func theJobAsksForATaskNamedForTheGuest() {
        job.start(limitMinutes: 15, guestName: "Android")
        #expect(job.state == .submitted)
        #expect(scheduler.registrations == [BackgroundJob.identifier])
        #expect(scheduler.submissions.map(\.title) == ["Android is running"])
        #expect(scheduler.submissions.first?.subtitle.hasPrefix("Stops after ") == true)
    }

    @Test(arguments: RunEnd.all)
    func aRunningJobCompletesWhenTheRunEnds(_ end: RunEnd) {
        startRunningJob()
        #expect(job.isRunning)

        launcher.finished(result: end.result)

        #expect(launcher.phase == .ended(result: end.result))
        #expect(job.state == .idle)
        // The guest finishing is the job's work done; an error is not.
        #expect(task.completions == [end.result == 0])
    }

    @Test(arguments: RunEnd.all)
    func aJobThatIOSHasNotStartedIsWithdrawnWhenTheRunEnds(_ end: RunEnd) {
        job.start(limitMinutes: 15, guestName: "Android")
        #expect(job.state == .submitted)

        launcher.finished(result: end.result)

        #expect(job.state == .idle)
        #expect(scheduler.withdrawn == [BackgroundJob.identifier])
        #expect(task.completions.isEmpty)

        // iOS may have started it already: the task is completed at once and
        // does not bring the job back.
        scheduler.start(task)
        #expect(job.state == .idle)
        #expect(task.completions == [false])
    }

    @Test(arguments: RunEnd.all)
    func aRunEndedWithNoJobAsksForNothing(_ end: RunEnd) {
        launcher.finished(result: end.result)

        #expect(job.state == .idle)
        #expect(scheduler.registrations.isEmpty)
        #expect(scheduler.withdrawn.isEmpty)
        #expect(task.completions.isEmpty)
    }

    @Test
    func theTaskIsCompletedOnce() {
        startRunningJob()
        launcher.finished(result: 0)
        job.guestRunEnded(result: 0)
        job.cancel()
        #expect(task.completions == [true])
    }

    /// In the background the job is what keeps the process running, so its end
    /// is what lets the policy pause the guest: a guest that ended behind a
    /// job leaves nothing to run on.
    @Test(arguments: RunEnd.all)
    func theJobEndingWithTheRunLetsThePolicyPause(_ end: RunEnd) {
        let effects = RecordingEffects()
        let controller = BackgroundController(
            effects: effects, mode: { .keepRunning }
        )
        job.controller = controller
        startRunningJob()
        effects.jobActive = job.isRunning
        controller.didEnterBackground()
        #expect(controller.phase == .jobRunning)

        launcher.finished(result: end.result)

        #expect(effects.calls.filter { $0 == "quiesce job ended" }.count == 1)
        #expect(controller.phase == .quiesced)
        #expect(task.completions == [end.result == 0])
    }

    /// The other ends of the job are unchanged by the run's.
    @Test
    func theUserStoppingTheJobCompletesItsTaskAsFailed() {
        startRunningJob()
        job.cancel()
        #expect(job.state == .idle)
        #expect(task.completions == [false])
    }

    @Test
    func iOSExpiringTheTaskEndsTheJob() async throws {
        startRunningJob()
        task.expire()
        let ended = try await eventually { job.state == .idle }
        #expect(ended)
        #expect(task.completions == [false])
    }
}
