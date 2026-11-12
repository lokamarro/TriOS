// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

struct RunSpaceTests {
    private let gib = Bytes.gib
    private let mib = Bytes.mib

    @Test func theThresholdsFormALadderBelowAStart() {
        #expect(RunSpace.pauseBelowBytes == gib)
        #expect(RunSpace.resumeAtBytes == RunSpace.startReserveBytes)
        // A guest is paused below what is needed to start it, and resumed at
        // that, so a resume is never followed straight away by a pause.
        #expect(RunSpace.pauseBelowBytes < RunSpace.resumeAtBytes)
    }

    @Test func aGuestRunsAtAndAboveTheThreshold() {
        #expect(RunSpace.verdict(freeBytes: 40 * gib) == .keepRunning)
        #expect(RunSpace.verdict(freeBytes: gib) == .keepRunning)
    }

    @Test func aGuestIsPausedBelowTheThreshold() {
        #expect(RunSpace.verdict(freeBytes: gib - 1) == .pause)
        #expect(RunSpace.verdict(freeBytes: 0) == .pause)
    }

    @Test func aPausedGuestResumesOnlyAtTheResumeSpace() {
        // The pause threshold is not enough to resume: the same space would
        // pause the guest again at the next write.
        #expect(!RunSpace.allowsResume(freeBytes: 900 * mib))
        #expect(!RunSpace.allowsResume(freeBytes: gib))
        #expect(!RunSpace.allowsResume(freeBytes: RunSpace.resumeAtBytes - 1))
        #expect(RunSpace.allowsResume(freeBytes: RunSpace.resumeAtBytes))
        #expect(RunSpace.allowsResume(freeBytes: 40 * gib))
    }

    @Test func theNextCheckFollowsTheSpaceThatCouldBeWrittenBeforeThePause() {
        // 128 MiB a second over the margin to the pause.
        #expect(RunSpace.nextCheck(freeBytes: gib + 512 * mib) == .seconds(4))
        #expect(RunSpace.nextCheck(freeBytes: gib + 1024 * mib) == .seconds(8))
    }

    @Test func theNextCheckIsBoundedBothWays() {
        #expect(RunSpace.nextCheck(freeBytes: gib) == RunSpace.shortestInterval)
        #expect(RunSpace.nextCheck(freeBytes: gib + mib) == RunSpace.shortestInterval)
        #expect(RunSpace.nextCheck(freeBytes: 0) == RunSpace.shortestInterval)
        #expect(RunSpace.nextCheck(freeBytes: 100 * gib) == RunSpace.longestInterval)
        #expect(RunSpace.nextCheck(freeBytes: .max) == RunSpace.longestInterval)
    }

    @Test func theNextCheckNeverLetsTheGuestWriteIntoThePauseMargin() {
        // Between the two bounds the interval is the time to write the margin
        // to the pause, so the guest writing flat out reaches the pause at the
        // next reading and not past it. The shortest interval's writes are
        // what the threshold leaves room for: well under half of it.
        var free = RunSpace.pauseBelowBytes
        while free < 40 * gib {
            let interval = RunSpace.nextCheck(freeBytes: free)
            if interval > RunSpace.shortestInterval, interval < RunSpace.longestInterval {
                let seconds = Double(interval.components.seconds)
                let written = UInt64(seconds) * RunSpace.assumedWriteRate
                #expect(written <= free - RunSpace.pauseBelowBytes)
            }
            free += 97 * mib
        }
        let shortest = UInt64(RunSpace.shortestInterval.components.seconds) * RunSpace.assumedWriteRate
        #expect(shortest < RunSpace.pauseBelowBytes / 2)
    }
}

struct SpacePauseTests {
    @Test func theGuestIsNamedByTheApplicationThatHostsIt() {
        let pause = SpacePause(guest: "Linux", freeBytes: 0, stage: .notPaused)
        #expect(pause.title == "Linux is still running")
        #expect(!pause.detail.contains("Android"))
        #expect(SpacePause(guest: "Linux", freeBytes: 0, stage: .pausing).title == "Pausing Linux")
    }
}

struct SpacePauseEndingTests {
    @Test func aStopSaysItEndsTheRunLikeLeavingTheGuest() {
        let paused = SpacePause(guest: "Linux", freeBytes: 820 * Bytes.mib, ending: .stop)
        #expect(paused.detail.hasSuffix("Shut down ends the run, as leaving the guest does."))
        let notPaused = SpacePause(guest: "Linux", freeBytes: 820 * Bytes.mib, stage: .notPaused, ending: .stop)
        #expect(notPaused.detail.hasSuffix("Shut down ends the run, as leaving the guest does."))
        let stopping = SpacePause(guest: "Linux", freeBytes: 0, working: .shuttingDown, ending: .stop)
        #expect(stopping.detail == "Linux is stopping. What it had stored is kept.")
    }

    @Test func aPowerOffSaysNothingOfStopping() {
        let paused = SpacePause(freeBytes: 820 * Bytes.mib)
        #expect(!paused.detail.contains("ends the run"))
        #expect(SpacePause(freeBytes: 0, working: .shuttingDown).detail
            == "Android is saving what it has and shutting down.")
    }
}

struct SpacePauseWordingTests {
    @Test func theFirstWordsGiveTheFreeSpaceAndWhatIsNeeded() {
        let pause = SpacePause(freeBytes: 820 * Bytes.mib)
        #expect(pause.title == "Android is paused")
        #expect(pause.detail.contains("820 MiB"))
        #expect(pause.detail.contains("1.5 GiB"))
        #expect(!pause.detail.contains("Only"))
    }

    @Test func aResumeThatFoundTheSpaceShortSaysSo() {
        let pause = SpacePause(freeBytes: 1_200 * Bytes.mib, stillShort: true)
        #expect(pause.detail.hasPrefix("Only "))
        #expect(pause.detail.contains("Free up more space"))
    }

    @Test func aShutdownNoLongerAsksForSpace() {
        let pause = SpacePause(freeBytes: 0, working: .shuttingDown)
        #expect(!pause.detail.contains("Free up"))
    }
}

struct SuspensionCheckTests {
    /// Readings in turn, the last repeating, and a wait that takes no time.
    @MainActor
    private func verdict(_ readings: [UInt64?]) async -> (SuspensionCheck.Verdict, Int) {
        var index = 0
        var waits = 0
        let verdict = await SuspensionCheck.confirm(
            reading: {
                defer { index += 1 }
                return readings[min(index, readings.count - 1)]
            },
            wait: { _ in waits += 1 })
        return (verdict, waits)
    }

    @Test @MainActor func aGuestWhoseProcessorTimeStandsStillIsHeldAfterTwoIntervals() async {
        let (verdict, waits) = await verdict([7_000_000_000])
        #expect(verdict == .held)
        #expect(waits == SuspensionCheck.stillIntervals)
    }

    @Test @MainActor func aGuestThatRunsIsNeverHeldAndIsReadWithinTheBound() async {
        let runs = (0..<32).map { UInt64($0) * 50_000_000 }
        let (verdict, waits) = await verdict(runs)
        #expect(verdict == .running)
        #expect(waits == SuspensionCheck.maximumReadings - 1)
    }

    @Test @MainActor func aGuestThatSettlesAfterStragglingIsHeld() async {
        // Its threads use time for three intervals, then none.
        let readings: [UInt64?] = [0, 90_000_000, 180_000_000, 270_000_000, 270_000_000, 270_000_000]
        let (verdict, _) = await verdict(readings)
        #expect(verdict == .held)
    }

    @Test @MainActor func oneStillIntervalBetweenTimerTicksIsNotEnough() async {
        // Still once, then running, then still twice: held only at the end.
        let readings: [UInt64?] = [0, 0, 80_000_000, 80_000_000, 80_000_000]
        let (verdict, waits) = await verdict(readings)
        #expect(verdict == .held)
        #expect(waits == 4)
    }

    @Test @MainActor func aSmallerCounterIsAnotherBootAndRuns() async {
        let (verdict, _) = await verdict([9_000_000_000] + (1...8).map { UInt64($0) * 50_000_000 })
        #expect(verdict == .running)
    }

    @Test @MainActor func noReadingIsNoVerdict() async {
        let (first, _) = await verdict([nil])
        #expect(first == .unknown)
        let (later, _) = await verdict([0, nil])
        #expect(later == .unknown)
    }

    @Test func theBoundIsAFewSeconds() {
        let seconds = Int(SuspensionCheck.interval.components.seconds) * (SuspensionCheck.maximumReadings - 1)
        #expect(seconds >= 5 && seconds <= 10)
    }
}

struct StartSpaceTests {
    @Test func aStartWithRoomHasNoShortage() {
        #expect(StartSpace(freeBytes: RunSpace.startReserveBytes) == nil)
        #expect(StartSpace(freeBytes: 40 * Bytes.gib) == nil)
    }

    @Test func aStartBelowTheReserveStatesWhatItNeedsAndWhatTheDeviceHas() throws {
        let shortage = try #require(StartSpace(freeBytes: 900 * Bytes.mib))
        #expect(shortage.neededBytes == RunSpace.startReserveBytes)
        #expect(StartSpace.title == "Not enough free space")
        #expect(shortage.message(device: "iPad")
            == "Starting needs 1.5 GiB free for the guest's own writes. This iPad has 900 MiB free.")
    }

    @Test func theFirstStartOfAnAndroidGuestNeedsMore() throws {
        #expect(StartSpace(freeBytes: 2 * Bytes.gib) == nil)
        let shortage = try #require(StartSpace(freeBytes: 2 * Bytes.gib, firstStart: true))
        #expect(shortage.neededBytes == RunSpace.firstStartReserveBytes)
        #expect(shortage.message(device: "iPhone").hasPrefix("Starting needs 3 GiB free"))
        #expect(shortage.advice(device: "iPhone") == "Free up space in Settings \u{203A} General \u{203A} iPhone Storage.")
    }
}

struct HostStorageTests {
    @Test func theVolumeOfADirectoryHasSomeFreeSpace() {
        #expect(HostStorage.freeBytes(on: URL(fileURLWithPath: NSTemporaryDirectory())) > 0)
    }
}
