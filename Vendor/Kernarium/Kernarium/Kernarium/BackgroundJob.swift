// SPDX-License-Identifier: GPL-2.0-or-later

import BackgroundTasks
import Combine
import Foundation

/// Where a kept-running job stands against its time limit. The system wants
/// progress from a continued task, and the job's only measurable progress is
/// the time it has used.
struct BackgroundJobTimeline: Equatable {
    let started: Date
    let limit: TimeInterval

    var end: Date { started.addingTimeInterval(limit) }

    func elapsed(at date: Date) -> TimeInterval {
        min(max(date.timeIntervalSince(started), 0), limit)
    }

    func remaining(at date: Date) -> TimeInterval {
        limit - elapsed(at: date)
    }

    func isOver(at date: Date) -> Bool { remaining(at: date) <= 0 }

    /// Whole seconds used, held below the total until the job is over so the
    /// system never sees it finished before it is.
    func completedUnits(at date: Date) -> Int64 {
        let seconds = Int64(elapsed(at: date))
        return isOver(at: date) ? Int64(limit) : min(seconds, Int64(limit) - 1)
    }

    var totalUnits: Int64 { Int64(limit) }

    /// What the system's progress card says about the time left.
    func subtitle(at date: Date) -> String {
        let minutes = Int((remaining(at: date) / 60).rounded(.up))
        switch minutes {
        case ...0: return "Finishing"
        case 1: return "1 minute left"
        default: return "\(minutes) minutes left"
        }
    }
}

/// The continued task iOS hands the job, as far as the job uses it. The
/// system's task conforms; the unit tests give the job their own.
@MainActor
protocol BackgroundJobTask: AnyObject {
    var progress: Progress { get }
    var title: String { get }
    /// What to do when iOS ends the task before the job does.
    func onExpiration(_ handler: @escaping () -> Void)
    func updateTitle(_ title: String, subtitle: String)
    func setTaskCompleted(success: Bool)
}

extension BGContinuedProcessingTask: BackgroundJobTask {
    func onExpiration(_ handler: @escaping () -> Void) {
        expirationHandler = handler
    }
}

/// What the job asks of iOS's task scheduler.
@MainActor
protocol BackgroundJobScheduling {
    /// Registers the handler iOS calls, on the main actor, when it starts the
    /// task of `identifier`. Returns whether iOS took the registration.
    func register(
        identifier: String,
        started: @escaping @MainActor (any BackgroundJobTask) -> Void
    ) -> Bool
    /// Asks iOS to start a task. iOS answers later, and `refused` is called,
    /// on the main actor, if the answer is no.
    func submit(
        identifier: String,
        title: String,
        subtitle: String,
        refused: @escaping @MainActor (Error) -> Void
    )
    func cancelRequest(identifier: String)
}

/// The real scheduler.
@MainActor
struct SystemBackgroundJobScheduler: BackgroundJobScheduling {
    func register(
        identifier: String,
        started: @escaping @MainActor (any BackgroundJobTask) -> Void
    ) -> Bool {
        BGTaskScheduler.shared.register(
            forTaskWithIdentifier: identifier,
            using: nil
        ) { task in
            guard let continued = task as? BGContinuedProcessingTask else {
                task.setTaskCompleted(success: false)
                return
            }
            // The handler runs on a system queue; the job lives on the main
            // actor. The task is a reference the system gives for this use.
            nonisolated(unsafe) let handed = continued
            DispatchQueue.main.async {
                MainActor.assumeIsolated { started(handed) }
            }
        }
    }

    func submit(
        identifier: String,
        title: String,
        subtitle: String,
        refused: @escaping @MainActor (Error) -> Void
    ) {
        let request = BGContinuedProcessingTaskRequest(
            identifier: identifier,
            title: title,
            subtitle: subtitle
        )
        request.strategy = .fail
        // iOS 27 submits off the calling thread and reports later, possibly
        // after it has already started the task.
        nonisolated(unsafe) let handed = request
        DispatchQueue.global(qos: .userInitiated).async {
            BGTaskScheduler.shared.submitTaskRequest(handed) { error in
                guard let error else { return }
                DispatchQueue.main.async {
                    MainActor.assumeIsolated { refused(error) }
                }
            }
        }
    }

    func cancelRequest(identifier: String) {
        BGTaskScheduler.shared.cancel(taskRequestWithIdentifier: identifier)
    }
}

/// The job a user starts to keep their guest working while Kernarium is in the
/// background: one iOS continued processing task, submitted on the user's tap,
/// for a time they chose. It ends when the time is up, when the user stops it
/// or iOS does, or when the guest's run ends, and then reports to the policy
/// that decides what the guest does next. It asks for no GPU: iOS offers none
/// to this task on every device, and the guest's GPU requests wait meanwhile.
///
/// The job belongs to one run of a guest. A run that is over, whether the
/// guest powered off, failed to start or kept restarting, has nothing left to
/// keep, and a job that outlived it would hold a dead application in the
/// background for the rest of its time.
@MainActor
final class BackgroundJob: ObservableObject {
    static let shared = BackgroundJob()

    enum State: Equatable {
        case idle
        /// Submitted; iOS has not yet started it.
        case submitted
        case running(BackgroundJobTimeline)
    }

    static let identifier = "com.imxieyi.Kernarium.guest.job"

    @Published private(set) var state = State.idle
    weak var controller: BackgroundController?

    private let scheduler: any BackgroundJobScheduling
    private var registered = false
    private var submissions = 0
    private var task: (any BackgroundJobTask)?
    private var ticker: Timer?
    private var lastSubtitleMinutes = Int.max
    private var pendingLimitMinutes = BackgroundSettings.defaultLimitMinutes

    /// The unit tests give a job a scheduler of their own.
    init(scheduler: (any BackgroundJobScheduling)? = nil) {
        self.scheduler = scheduler ?? SystemBackgroundJobScheduler()
    }

    var isRunning: Bool {
        if case .running = state { return true }
        return false
    }

    var timeline: BackgroundJobTimeline? {
        if case let .running(timeline) = state { return timeline }
        return nil
    }

    /// Starts a job on the user's action. A guest needs no job while it is in
    /// front, so this only submits the request; the job runs from the moment
    /// iOS starts it.
    func start(limitMinutes: Int, guestName: String) {
        guard state == .idle else { return }
        pendingLimitMinutes = limitMinutes
        registerHandler()
        // iOS 27 submits off the calling thread and reports later, possibly
        // after it has already started the task, so the job counts as
        // submitted from here and a refusal puts it back.
        state = .submitted
        submissions += 1
        let submission = submissions
        scheduler.submit(
            identifier: Self.identifier,
            title: "\(guestName) is running",
            subtitle: "Stops after " +
                BackgroundSettings.limitTitle(minutes: limitMinutes)
        ) { [weak self] error in
            self?.refuse(submission, error: error)
        }
        print("Kernarium: background job submitted (\(limitMinutes) min)")
    }

    /// iOS refused the request. Only the submission that is still waiting
    /// is undone: the user may have stopped it, and started another, since.
    private func refuse(_ submission: Int, error: Error) {
        print("Kernarium: background job refused: \(error)")
        guard state == .submitted, submission == submissions else { return }
        state = .idle
    }

    /// Whether an automation launch starts the job with its guest: it asked
    /// to, the guest chose the keep-running mode, and no job exists yet.
    static func startsWithGuest(
        asked: Bool,
        mode: BackgroundMode,
        state: State
    ) -> Bool {
        asked && mode == .keepRunning && state == .idle
    }

    /// The guest's settings may have changed: a job belongs to the keep-running
    /// mode, so leaving the mode ends it.
    func reconcile(mode: BackgroundMode) {
        if mode != .keepRunning { cancel() }
    }

    /// The user stopped the job, or the guest or its mode went away.
    func cancel() {
        guard state != .idle else { return }
        if state == .submitted {
            scheduler.cancelRequest(identifier: Self.identifier)
        }
        finish(success: false, reason: "stopped")
    }

    /// The guest's run returned with `result`: it powered off, was stopped,
    /// failed to start or kept restarting. Whatever the job was keeping is
    /// gone, so the job ends now, as completed when the guest simply finished,
    /// and as failed when the run returned an error. A job that iOS has not
    /// started yet is withdrawn, and one it starts anyway is completed at once.
    func guestRunEnded(result: Int32) {
        guard state != .idle else { return }
        if state == .submitted {
            scheduler.cancelRequest(identifier: Self.identifier)
        }
        finish(success: result == 0, reason: "guest ended")
    }

    private func registerHandler() {
        guard !registered else { return }
        registered = scheduler.register(identifier: Self.identifier) {
            [weak self] started in
            guard let self else {
                started.setTaskCompleted(success: false)
                return
            }
            self.begin(task: started)
        }
        print("Kernarium: background job handler registered=\(registered)")
    }

    private func begin(task: any BackgroundJobTask) {
        guard state == .submitted else {
            // The user stopped it, or the guest ended, before iOS started it.
            task.setTaskCompleted(success: false)
            return
        }
        let limitMinutes = pendingLimitMinutes
        let timeline = BackgroundJobTimeline(
            started: Date(),
            limit: TimeInterval(limitMinutes * 60)
        )
        self.task = task
        task.progress.totalUnitCount = timeline.totalUnits
        task.progress.completedUnitCount = 0
        task.onExpiration { [weak self] in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    self?.finish(success: false, reason: "expired")
                }
            }
        }
        lastSubtitleMinutes = Int.max
        state = .running(timeline)
        ticker = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in
            MainActor.assumeIsolated { BackgroundJob.shared.tick() }
        }
        print("Kernarium: background job running for \(limitMinutes) min")
    }

    private func tick() {
        guard case let .running(timeline) = state else { return }
        let now = Date()
        if timeline.isOver(at: now) {
            finish(success: true, reason: "time is up")
            return
        }
        task?.progress.completedUnitCount = timeline.completedUnits(at: now)
        let minutes = Int((timeline.remaining(at: now) / 60).rounded(.up))
        if minutes != lastSubtitleMinutes {
            lastSubtitleMinutes = minutes
            task?.updateTitle(
                task?.title ?? "",
                subtitle: timeline.subtitle(at: now)
            )
        }
    }

    private func finish(success: Bool, reason: String) {
        ticker?.invalidate()
        ticker = nil
        let finished = task
        task = nil
        state = .idle
        finished?.setTaskCompleted(success: success)
        print("Kernarium: background job ended (\(reason))")
        controller?.jobDidEnd()
    }
}
