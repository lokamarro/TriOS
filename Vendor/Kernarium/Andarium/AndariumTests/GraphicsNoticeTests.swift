// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Holds the loop at its wait between readings until a test lets it go.
@MainActor
private final class Pace {
    private(set) var waits: [Duration] = []
    private var held: CheckedContinuation<Void, Never>?
    /// The moment the next reading is taken at.
    var now = ContinuousClock.now

    func sleep(_ duration: Duration) async {
        waits.append(duration)
        await withTaskCancellationHandler {
            await withCheckedContinuation { held = $0 }
        } onCancel: {
            Task { @MainActor in self.release() }
        }
    }

    var isWaiting: Bool { held != nil }

    /// Lets the loop go on, `seconds` later by the clock the reading uses.
    func release(after seconds: Int = 2) {
        now = now.advanced(by: .seconds(seconds))
        let continuation = held
        held = nil
        continuation?.resume()
    }
}

/// The session's notice that Android ran out of graphics memory against a
/// scripted runtime, with the pause between readings held by the test: the
/// count is read while a normal session runs in the foreground, whether or
/// not a statistics view is up; a rise shows the notice once per burst; the
/// person's dismissal ends it; the end of the run, a restart and the
/// background take the readings or the notice away. `GraphicsNotice` itself
/// (the burst rule) is tested in `andarium-core`.
@MainActor
struct GraphicsNoticeFlowTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func runningModel(_ driver: StubDriver?, pace: Pace, mode: StartMode = .normal) -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        model.driver = driver
        model.graphicsNoticeSleep = { duration in await pace.sleep(duration) }
        model.graphicsNoticeClock = { pace.now }
        model.phase = .running(mode)
        return model
    }

    /// Lets the loop run until `condition` holds, for at most five seconds.
    private func until(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while !condition() {
            if ContinuousClock.now > deadline { return false }
            try? await Task.sleep(for: .milliseconds(1))
        }
        return true
    }

    /// Time enough for a loop that should not run to show that it does.
    private func settle() async {
        try? await Task.sleep(for: .milliseconds(60))
    }

    /// Takes one reading and waits for the loop to rest at its pause again.
    private func nextReading(_ pace: Pace, _ driver: StubDriver) async -> Bool {
        let asked = driver.refusalsAsked
        pace.release()
        return await until { driver.refusalsAsked == asked + 1 && pace.isWaiting }
    }

    @Test func theCountIsReadEveryTwoSecondsWithNoStatisticsViewUp() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0]
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(driver.refusalsAsked == 1)
        #expect(await nextReading(pace, driver))
        #expect(await nextReading(pace, driver))
        #expect(pace.waits == [.seconds(2), .seconds(2), .seconds(2)])
        #expect(driver.statisticsAsked == 0, "the statistics were read for a notice")
        #expect(!model.graphicsNotice.isShowing)
        model.phase = .home
    }

    @Test func aRefusalShowsTheNoticeOnceAndDismissingItEndsIt() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0, 0, 2, 2, 5]
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(await nextReading(pace, driver))
        #expect(!model.graphicsNotice.isShowing)
        #expect(await nextReading(pace, driver))
        #expect(model.graphicsNotice.isShowing, "two refusals were not announced")
        #expect(await nextReading(pace, driver))
        #expect(model.graphicsNotice.isShowing)
        model.dismissGraphicsNotice()
        #expect(!model.graphicsNotice.isShowing)
        // Three more refusals a moment later are the same burst.
        #expect(await nextReading(pace, driver))
        #expect(!model.graphicsNotice.isShowing, "a dismissed burst announced itself again")
        model.phase = .home
    }

    @Test func aNoticeNobodyDismissesGoesAfterThirtySeconds() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0, 1]
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(await nextReading(pace, driver))
        #expect(model.graphicsNotice.isShowing)
        pace.release(after: 28)
        #expect(await until { driver.refusalsAsked == 3 && pace.isWaiting })
        #expect(model.graphicsNotice.isShowing)
        pace.release(after: 2)
        #expect(await until { driver.refusalsAsked == 4 && pace.isWaiting })
        #expect(!model.graphicsNotice.isShowing)
        model.phase = .home
    }

    @Test func refusalsBeforeTheFirstReadingAreNotAnnounced() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [40]
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(await nextReading(pace, driver))
        #expect(!model.graphicsNotice.isShowing)
        model.phase = .home
    }

    @Test func nothingIsReadInRecoveryWithoutADriverOrAGuest() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0, 1]
        let recovery = runningModel(driver, pace: pace, mode: .recovery)
        await settle()
        #expect(driver.refusalsAsked == 0, "recovery runs no app to lose graphics")
        recovery.phase = .home

        let preview = runningModel(nil, pace: pace)
        await settle()
        #expect(pace.waits.isEmpty)
        preview.phase = .home

        let idle = runningModel(driver, pace: pace)
        idle.phase = .home
        await settle()
        let asked = driver.refusalsAsked
        pace.release()
        await settle()
        #expect(driver.refusalsAsked == asked, "a guest that is not running was read")
    }

    @Test func theBackgroundStopsTheReadingsAndTheForegroundFindsWhatWasRefused() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        // The first reading is the baseline; the count rose while the
        // application was away.
        driver.refusalCounts = [0, 3]
        let model = runningModel(driver, pace: pace)
        #expect(await until { driver.refusalsAsked == 1 && pace.isWaiting })
        model.applicationActive = false
        await settle()
        let asked = driver.refusalsAsked
        pace.release()
        await settle()
        #expect(driver.refusalsAsked == asked, "the count was read in the background")
        model.applicationActive = true
        #expect(await until { model.graphicsNotice.isShowing }, "refusals made while away were lost")
        model.phase = .home
    }

    @Test func theEndOfARunTakesTheNoticeAway() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0, 4]
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(await nextReading(pace, driver))
        #expect(model.graphicsNotice.isShowing)
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(!model.graphicsNotice.isShowing)
        await settle()
        let asked = driver.refusalsAsked
        pace.release()
        await settle()
        #expect(driver.refusalsAsked == asked, "a run that ended was read")
    }

    @Test func aRestartStartsTheCountOverFromItsNextReading() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0, 4]
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(await nextReading(pace, driver))
        #expect(model.graphicsNotice.isShowing)
        model.restart()
        #expect(!model.graphicsNotice.isShowing)
        // The guest boots again and runs: the display, and its count of 4,
        // outlived it, so the first reading is a baseline.
        model.phase = .running(.normal)
        #expect(await until { driver.refusalsAsked >= 3 && pace.isWaiting })
        #expect(!model.graphicsNotice.isShowing, "a refusal made before the restart was announced after it")
        model.phase = .home
    }
    // MARK: Freeing graphics memory

    private static let mebibyte: UInt64 = 1 << 20

    /// A budget of 534 MiB of which contexts hold `held` MiB, and one big
    /// quiet holder.
    private func memory(held: UInt64, refusals: UInt64 = 0) -> GraphicsMemoryReading {
        GraphicsMemoryReading(
            budgetBytes: 534 * Self.mebibyte, usageBytes: (held + 20) * Self.mebibyte,
            contextBytes: held * Self.mebibyte, refusals: refusals,
            holders: [GraphicsHolder(pid: 2285, bytes: 130 * Self.mebibyte, idle: .seconds(60), name: "jelly")])
    }

    @Test func theBudgetIsReadWithTheNoticeAndNothingIsEndedWhileThereIsRoom() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0]
        driver.graphicsMemories = [memory(held: 200)]
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(await nextReading(pace, driver))
        #expect(await nextReading(pace, driver))
        #expect(driver.graphicsMemoryAsked == 3, "the budget was not read with each count")
        #expect(driver.reclaims.isEmpty)
        #expect(driver.graphicsMemoryFilters.allSatisfy {
            $0.idle == GraphicsReclaim.minimumIdle && $0.bytes == GraphicsReclaim.minimumHolderBytes
        }, "the holders asked for were not the ones a reclaim may end")
        model.phase = .home
    }

    @Test func aBudgetThatStaysNearlyFullEndsTheHolderOnceAndSaysNothing() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0]
        driver.graphicsMemories = [memory(held: 460)]
        driver.reclaimAnswer = .ended(package: "org.lineageos.jelly", pid: 2285, bytes: 130 * Self.mebibyte)
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(driver.reclaims.isEmpty, "one reading was taken for pressure")
        #expect(await nextReading(pace, driver))
        #expect(driver.reclaims.count == 1, "a budget nearly full for two readings ended nothing")
        // The app is given time to go, and the next reading is inside the
        // three seconds between two apps.
        #expect(await nextReading(pace, driver))
        #expect(driver.reclaims.count == 1, "a second app ended within three seconds")
        #expect(!model.graphicsNotice.isShowing, "freeing memory announced itself")
        model.phase = .home
    }

    @Test func aRefusalEndsTheHolderAtOnce() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0]
        driver.graphicsMemories = [memory(held: 200), memory(held: 200, refusals: 1)]
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(driver.reclaims.isEmpty)
        #expect(await nextReading(pace, driver))
        #expect(driver.reclaims.count == 1, "a refusal ended nothing")
        model.phase = .home
    }

    @Test func aTryThatEndedNothingIsNotRepeatedEveryReading() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0]
        driver.graphicsMemories = [memory(held: 460)]
        driver.reclaimAnswer = .nothingToEnd
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(await nextReading(pace, driver))
        #expect(driver.reclaims.count == 1)
        for _ in 0..<3 { #expect(await nextReading(pace, driver)) }
        #expect(driver.reclaims.count == 1, "the guest was asked for its processes every two seconds")
        #expect(await nextReading(pace, driver))
        #expect(await nextReading(pace, driver))
        #expect(driver.reclaims.count == 2, "the try was never repeated")
        model.phase = .home
    }

    @Test func aGuestWithNoBudgetReadingEndsNothingAndKeepsTheNotice() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0, 2]
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(await nextReading(pace, driver))
        #expect(driver.reclaims.isEmpty)
        #expect(model.graphicsNotice.isShowing)
        model.phase = .home
    }

    @Test func aRestartForgetsWhatWasEnded() async {
        let pace = Pace()
        let driver = StubDriver(plan: .normal)
        driver.refusalCounts = [0]
        driver.graphicsMemories = [memory(held: 460)]
        driver.reclaimAnswer = .ended(package: "org.lineageos.jelly", pid: 2285, bytes: 130 * Self.mebibyte)
        let model = runningModel(driver, pace: pace)
        #expect(await until { pace.isWaiting })
        #expect(await nextReading(pace, driver))
        #expect(driver.reclaims.count == 1)
        model.restart()
        model.phase = .running(.normal)
        #expect(await until { driver.graphicsMemoryAsked >= 3 && pace.isWaiting })
        #expect(driver.reclaims.count == 1, "a restarted guest was judged by the old one's readings")
        model.phase = .home
    }
}
