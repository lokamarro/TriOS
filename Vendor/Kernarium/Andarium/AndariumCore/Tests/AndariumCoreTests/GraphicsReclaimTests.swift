// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

private let mebibyte: UInt64 = 1 << 20
private let budget = 534 * mebibyte

/// A reading with `held` MiB credited to contexts and a host that holds
/// `overhead` MiB besides.
private func reading(
    held: UInt64, overhead: UInt64 = 40, refusals: UInt64 = 0, guestReclaims: Bool = false,
    holders: [GraphicsHolder] = []
) -> GraphicsMemoryReading {
    GraphicsMemoryReading(
        budgetBytes: budget, usageBytes: (held + overhead) * mebibyte, contextBytes: held * mebibyte,
        refusals: refusals, guestReclaims: guestReclaims, holders: holders)
}

private func holder(_ pid: UInt32, mib: UInt64, idle: Int = 60, name: String = "app") -> GraphicsHolder {
    GraphicsHolder(pid: pid, bytes: mib * mebibyte, idle: .seconds(idle), name: name)
}

/// A policy read at moments `seconds` after a start; a class so an expectation
/// can take a reading.
private final class Watch {
    var policy = GraphicsReclaim()
    private let start = ContinuousClock.now

    func at(_ seconds: Int) -> ContinuousClock.Instant { start.advanced(by: .seconds(seconds)) }

    func observe(_ reading: GraphicsMemoryReading, at seconds: Int) -> Bool {
        policy.observe(reading, at: at(seconds))
    }

    func record(_ result: GraphicsReclaimResult, at seconds: Int) {
        policy.record(result, at: at(seconds))
    }

    func ended(_ pid: UInt32, at seconds: Int) {
        record(.ended(package: "p\(pid)", pid: pid, bytes: 100 * mebibyte), at: seconds)
    }
}

/// What `dumpsys activity lru` printed on an Android 17 guest: the sections,
/// a line to a process, a cached process's place in its level ("+ 5", "+15")
/// and processes that are not apps of a user (uid 1000, 2000).
private let lruText = """
    ACTIVITY MANAGER LRU PROCESSES (dumpsys activity lru)
      Activities:
      #30: fg       TOP  LCMNFUATI 1133:com.android.launcher3/u0a83 act:activities|recents
      Other:
      #29: fg       IMPF -------TI 922:com.android.permissioncontroller/u0a117
      #28: psvc     PER  LCMNFUATI 1385:com.android.providers.media.module/u0a99
      #27: cch  + 5 CEM  --------- 1550:android.process.acore/u0a41
      #26: prev     LAST --------I 1493:android.process.media/u0a47
      #25: cch  + 5 CEM  -------T- 2044:com.android.rkpdapp/u0a114
      #23: cch  +15 CEM  -------T- 2065:com.android.shell/2000
      #22: cch  +15 CAC  --------- 1056:com.android.settings/1000
      #21: cch  +15 CAC  --------- 2285:org.lineageos.jelly/u0a118 act:activities|recents
      #20: cch  +25 CACC --------- 2240:org.lineageos.glimpse/u0a75
      #17: cch      LAST --------- 2267:org.lineageos.etar/u0a74 act:activities|recents
      #14: vis      BFGS ---NFUATI 1958:android.ext.services/u0a97
      #13: cch  + 5 CEM  --------- 1944:.adservices/u0a97
      #12: cch  +45 CEM  --------- 2206:com.android.calculator2:remote/u0a74
      # 8: prcp + 1 IMPB -------TI 1226:com.android.inputmethod.latin/u0a77
      # 7: home     HOME --------- 640:org.lineageos.trebuchet/u0a30
      # 6: fg       PER  --------- 459:system/1000
    """

struct GraphicsReclaimTests {
    // MARK: Process list

    @Test func theProcessListIsReadLineByLine() {
        let processes = AndroidProcessList.parse(lruText)
        #expect(
            processes.map(\.pid)
                == [1133, 922, 1385, 1550, 1493, 2044, 2065, 1056, 2285, 2240, 2267, 1958, 1944, 2206, 1226, 640, 459])
        let jelly = processes[8]
        #expect(jelly.name == "org.lineageos.jelly" && jelly.uid == "u0a118")
        #expect(jelly.label == "cch" && jelly.state == "CAC" && jelly.standing == .cached)
        #expect(processes[3].label == "cch" && processes[3].state == "CEM", "a place in the level hid the state")
        #expect(processes[3].standing == .cached && processes[14].label == "prcp" && processes[14].state == "IMPB")
        #expect(processes[9].state == "CACC" && processes[9].standing == .cached)
        #expect(
            processes[10].state == "LAST" && processes[10].standing == .cached,
            "the cached activity of an app that was in front is cached")
        #expect(processes[13].standing == .cached && processes[13].package == "com.android.calculator2")
        #expect(processes[4].standing == .previous && processes[4].package == "android.process.media")
        for index in [0, 1, 2, 11, 14, 15, 16] { #expect(processes[index].standing == .kept, "\(processes[index].name)") }
        #expect(processes[16].user == nil && processes[6].user == nil && jelly.user == 0)
        #expect(AndroidProcessList.parse("").isEmpty)
        #expect(AndroidProcessList.parse("Error: no such service\nDump failure\n").isEmpty)
    }

    @Test func aUserAndAnAppAreReadFromTheUid() {
        func process(_ uid: String) -> AndroidProcess {
            AndroidProcess(pid: 1, name: "p", uid: uid, label: "cch", state: "CAC")
        }
        #expect(process("u0a118").user == 0)
        #expect(process("u10a5").user == 10)
        for uid in ["1000", "u0i3", "u0", "ua5", "u0a", "u0axyz", ""] {
            #expect(process(uid).user == nil, "\(uid)")
        }
    }

    // MARK: When to act

    @Test func nothingHappensWhileThereIsRoom() {
        let watch = Watch()
        for second in stride(from: 0, through: 120, by: 2) {
            #expect(!watch.observe(reading(held: 200, holders: [holder(1, mib: 150)]), at: second))
        }
    }

    @Test func aBudgetThatIsNearlyFullActsOnTheSecondReadingInARow() {
        let watch = Watch()
        let full = reading(held: 410, overhead: 60, holders: [holder(2285, mib: 130)])
        #expect(!watch.observe(full, at: 0), "one reading is not pressure")
        #expect(watch.observe(full, at: 2))
        let other = Watch()
        _ = other.observe(full, at: 0)
        #expect(!other.observe(reading(held: 300), at: 2))
        #expect(!other.observe(full, at: 4), "the reading that fell back did not start the count again")
        #expect(other.observe(full, at: 6))
    }

    @Test func cachedMemoryTheHostCouldReuseIsNotPressure() {
        let watch = Watch()
        let bigHolder = [holder(1, mib: 150)]
        // The host held 40 MiB besides the contexts' at its leanest.
        _ = watch.observe(reading(held: 100, overhead: 40, holders: bigHolder), at: 0)
        // Churn since then left 165 MiB cached: the pool says 535 MiB, a
        // budget's worth, but contexts hold 330 and the leanest overhead was
        // 40.
        let churned = reading(held: 330, overhead: 205, holders: bigHolder)
        for second in stride(from: 2, to: 60, by: 2) { #expect(!watch.observe(churned, at: second)) }
        // Contexts at 420 are 460 with that overhead: past the margin.
        let rising = reading(held: 420, overhead: 205, holders: bigHolder)
        #expect(!watch.observe(rising, at: 60))
        #expect(watch.observe(rising, at: 62))
        // A host never seen lean counts what it sees.
        let fresh = Watch()
        let heavy = reading(held: 360, overhead: 205, holders: bigHolder)
        _ = fresh.observe(heavy, at: 0)
        #expect(fresh.observe(heavy, at: 2))
    }

    @Test func aRefusalActsAtOnceButTheFirstReadingIsOnlyABaseline() {
        let watch = Watch()
        let quiet = reading(held: 200, refusals: 4, holders: [holder(7, mib: 100)])
        #expect(!watch.observe(quiet, at: 0), "refusals before the first reading were acted on")
        #expect(!watch.observe(quiet, at: 2))
        #expect(watch.observe(reading(held: 200, refusals: 5, holders: [holder(7, mib: 100)]), at: 4))
    }

    @Test func aCountThatWentBackwardsIsANewDisplay() {
        let watch = Watch()
        _ = watch.observe(reading(held: 200, refusals: 9, holders: [holder(7, mib: 100)]), at: 0)
        #expect(!watch.observe(reading(held: 200, refusals: 1, holders: [holder(7, mib: 100)]), at: 2))
        #expect(watch.observe(reading(held: 200, refusals: 2, holders: [holder(7, mib: 100)]), at: 4))
    }

    @Test func aBudgetOfNoneIsNoPressure() {
        let watch = Watch()
        let none = GraphicsMemoryReading(
            budgetBytes: 0, usageBytes: 900 * mebibyte, contextBytes: 900 * mebibyte, refusals: 3,
            holders: [holder(1, mib: 500)])
        for second in stride(from: 0, to: 20, by: 2) { #expect(!watch.observe(none, at: second)) }
    }

    @Test func theHostStandsDownWhileTheGuestReclaims() {
        let watch = Watch()
        let full = [holder(7, mib: 100)]
        _ = watch.observe(reading(held: 500, holders: full), at: 0)
        #expect(watch.observe(reading(held: 500, holders: full), at: 2), "the policy never acted without the guest")
        for second in stride(from: 4, to: 40, by: 2) {
            #expect(
                !watch.observe(reading(held: 500, refusals: UInt64(second), guestReclaims: true, holders: full), at: second),
                "acted at \(second) s while the guest's own killer reclaims")
        }
        // The guest's killer stops: the policy starts from nothing, so it needs
        // its confirming readings again, and a refusal it saw before is no edge.
        #expect(!watch.observe(reading(held: 500, refusals: 38, holders: full), at: 40))
        #expect(watch.observe(reading(held: 500, refusals: 38, holders: full), at: 42))
    }

    // MARK: Who may be asked

    @Test func onlyAHolderTheHostNamesAndThatIsBigAndQuietIsACandidate() {
        let watch = Watch()
        let holders = [
            holder(1, mib: 200),  // a candidate
            holder(0, mib: 180, name: "RenderThread"),  // no process
            holder(2, mib: 31),  // too small
            holder(3, mib: 150, idle: 9),  // still active
            holder(4, mib: 32, idle: 10),  // the smallest that counts
        ]
        let candidates = watch.policy.candidates(in: reading(held: 500, holders: holders))
        #expect(candidates.map(\.pid) == [1, 4])
    }

    @Test func withoutTheKernelsProcessNamingNothingHappens() {
        // Every context unnamed, as a kernel without the topic reports them.
        let watch = Watch()
        let unnamed = reading(
            held: 500, overhead: 20,
            holders: [holder(0, mib: 300, name: "RenderThread"), holder(0, mib: 120, name: "binder:459_1")])
        _ = watch.observe(unnamed, at: 0)
        #expect(!watch.observe(unnamed, at: 2))
        #expect(!watch.observe(reading(held: 500, refusals: 3, holders: unnamed.holders), at: 4))
    }

    // MARK: Rate limits

    @Test func oneAppEndsPerThreeSecondsAndThreePerEpisode() {
        let watch = Watch()
        func pressure(_ holders: [GraphicsHolder]) -> GraphicsMemoryReading {
            reading(held: 480, overhead: 20, holders: holders)
        }
        let holders = (1...6).map { holder(UInt32($0), mib: 100) }
        _ = watch.observe(pressure(holders), at: 0)
        #expect(watch.observe(pressure(holders), at: 2))
        watch.ended(1, at: 2)
        #expect(!watch.observe(pressure(holders), at: 3), "two apps ended within three seconds")
        #expect(watch.observe(pressure(holders), at: 5))
        watch.ended(2, at: 5)
        #expect(watch.observe(pressure(holders), at: 8))
        watch.ended(3, at: 8)
        for second in stride(from: 11, through: 60, by: 3) {
            #expect(!watch.observe(pressure(holders), at: second), "a fourth app ended in one episode")
        }
    }

    @Test func anEpisodeEndsAfterFiveCalmReadingsAndAnotherMayBegin() {
        let watch = Watch()
        let holders = (1...8).map { holder(UInt32($0), mib: 100) }
        let pressured = reading(held: 480, overhead: 20, holders: holders)
        let calm = reading(held: 100)
        _ = watch.observe(pressured, at: 0)
        for (second, pid) in [(2, UInt32(1)), (6, 2), (10, 3)] {
            #expect(watch.observe(pressured, at: second))
            watch.ended(pid, at: second)
        }
        #expect(!watch.observe(pressured, at: 14))
        for second in stride(from: 16, through: 24, by: 2) { #expect(!watch.observe(calm, at: second)) }
        _ = watch.observe(pressured, at: 26)
        #expect(watch.observe(pressured, at: 28), "a new episode could not begin")
    }

    @Test func aTryThatEndedNothingWaitsBeforeAskingTheGuestAgain() {
        let watch = Watch()
        let pressured = reading(held: 480, overhead: 20, holders: [holder(1, mib: 100)])
        _ = watch.observe(pressured, at: 0)
        #expect(watch.observe(pressured, at: 2))
        watch.record(.nothingToEnd, at: 2)
        for second in stride(from: 4, to: 12, by: 2) {
            #expect(!watch.observe(pressured, at: second), "the guest was asked again at \(second)")
        }
        #expect(watch.observe(pressured, at: 12))
        watch.record(.failed, at: 12)
        #expect(!watch.observe(pressured, at: 14))
        #expect(watch.observe(pressured, at: 22))
    }

    // MARK: Apps that stay

    @Test func anAppThatStaysAfterBeingAskedIsNotAskedAgain() {
        let watch = Watch()
        let stays = reading(held: 480, overhead: 20, holders: [holder(5, mib: 100), holder(6, mib: 90)])
        _ = watch.observe(stays, at: 0)
        #expect(watch.observe(stays, at: 2))
        watch.ended(5, at: 2)
        // The app is given time to free its memory, and is not a candidate
        // meanwhile.
        #expect(watch.policy.candidates(in: stays).map(\.pid) == [6])
        #expect(watch.observe(stays, at: 6))
        #expect(watch.policy.survivors.isEmpty)
        watch.ended(6, at: 6)
        // Two readings later pid 5 still holds its memory; pid 6 has gone.
        let after = reading(held: 480, overhead: 20, holders: [holder(5, mib: 100)])
        #expect(!watch.observe(after, at: 10))
        #expect(watch.policy.survivors == [5], "an app that stayed was not remembered")
        #expect(watch.policy.candidates(in: after).isEmpty)
        // It is forgotten once its memory is gone.
        _ = watch.observe(reading(held: 100), at: 12)
        #expect(watch.policy.survivors.isEmpty)
    }

    @Test func anAppThatWentIsNotASurvivor() {
        let watch = Watch()
        let pressured = reading(held: 480, overhead: 20, holders: [holder(5, mib: 100)])
        _ = watch.observe(pressured, at: 0)
        #expect(watch.observe(pressured, at: 2))
        watch.ended(5, at: 2)
        #expect(!watch.observe(reading(held: 380, overhead: 20), at: 4))
        #expect(watch.policy.survivors.isEmpty)
    }

    // MARK: Choosing the app

    private func target(_ holders: [GraphicsHolder], processes: String = lruText) -> AndroidProcess? {
        let policy = GraphicsReclaim()
        return policy.choose(from: reading(held: 480, holders: holders), processes: AndroidProcessList.parse(processes))?
            .process
    }

    @Test func theLargestCachedHolderIsChosenOverTheOneInFront() {
        let holders = [
            holder(1133, mib: 220, name: "launcher3"),  // in front: never
            holder(2285, mib: 130, name: "ageos.jelly"),
            holder(2240, mib: 60, name: "lineageos.glimpse"),
        ]
        #expect(target(holders)?.name == "org.lineageos.jelly")
    }

    @Test func thePreviousAppComesLastAndIsChosenOnlyWhenNothingCachedHolds() {
        let previous = holder(1493, mib: 300, name: "process.media")
        let cached = holder(2240, mib: 40, name: "lineageos.glimpse")
        #expect(target([previous, cached])?.name == "org.lineageos.glimpse")
        #expect(target([previous])?.name == "android.process.media")
    }

    @Test func nothingForegroundVisiblePerceptibleOrHomeIsEverChosen() {
        let kept = [
            holder(1133, mib: 300, name: "launcher3"), holder(922, mib: 250, name: "ontroller"),
            holder(1385, mib: 250, name: "media.module"), holder(1958, mib: 250, name: "services"),
            holder(640, mib: 200, name: "trebuchet"), holder(1226, mib: 200, name: "latin"),
            holder(459, mib: 200, name: "system"),
        ]
        #expect(target(kept) == nil)
    }

    @Test func aHolderTheListDoesNotNameIsNeverChosen() {
        #expect(target([holder(99999, mib: 300, name: "ghost")]) == nil, "a pid the list does not have")
        #expect(target([holder(0, mib: 300, name: "jelly")]) == nil, "a context with no process")
        // The pid is the list's, but a different app has it now.
        #expect(target([holder(2285, mib: 300, name: "other.process")]) == nil, "a recycled pid was trusted")
        #expect(target([]) == nil)
    }

    @Test func aProcessNameThatIsNotAPackageIsNeverRunInAShell() {
        let text = """
            #21: cch CAC --------- 2285:evil;reboot/u0a118
            #20: cch CAC --------- 2286:a$(id)/u0a119
            #19: cch CAC --------- 2287:system_server/1000
            """
        #expect(target([holder(1944, mib: 100, name: "")]) == nil, "a process name that starts with a dot")
        for pid in [2285, 2286, 2287] as [UInt32] {
            #expect(target([holder(pid, mib: 100, name: "")], processes: text) == nil, "\(pid)")
        }
    }

    // MARK: Reaching the guest

    private final class Shell: GuestShell, @unchecked Sendable {
        var answers: [String: GuestShellOutput] = [:]
        var failing = false
        private let lock = NSLock()
        private var ran: [String] = []

        var commands: [String] { lock.withLock { ran } }

        func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
            lock.withLock { ran.append(command) }
            if failing { throw AdbError.guestGone }
            return answers[command] ?? GuestShellOutput(text: "", exitCode: 0)
        }
    }

    @Test func theCachedHolderIsEndedWithItsPackageAndUser() async {
        let shell = Shell()
        shell.answers[AndroidProcessList.command] = GuestShellOutput(text: lruText, exitCode: 0)
        let policy = GraphicsReclaim()
        let result = await policy.reclaim(
            from: reading(held: 480, holders: [holder(2285, mib: 130, name: "ageos.jelly"), holder(1133, mib: 200, name: "launcher3")]),
            shell: shell)
        #expect(result == .ended(package: "org.lineageos.jelly", pid: 2285, bytes: 130 * mebibyte))
        #expect(shell.commands == ["dumpsys activity lru", "cmd activity kill --user 0 org.lineageos.jelly"])
    }

    @Test func aHolderWithNothingToEndAsksOnlyForTheList() async {
        let shell = Shell()
        shell.answers[AndroidProcessList.command] = GuestShellOutput(text: lruText, exitCode: 0)
        let policy = GraphicsReclaim()
        let result = await policy.reclaim(
            from: reading(held: 480, holders: [holder(1133, mib: 200, name: "launcher3")]), shell: shell)
        #expect(result == .nothingToEnd)
        #expect(shell.commands == ["dumpsys activity lru"])
    }

    @Test func aGuestThatCannotBeAskedOrRefusesIsAFailure() async {
        let policy = GraphicsReclaim()
        let candidate = reading(held: 480, holders: [holder(2285, mib: 130, name: "ageos.jelly")])
        let unreachable = Shell()
        unreachable.failing = true
        #expect(await policy.reclaim(from: candidate, shell: unreachable) == .failed)

        let refusing = Shell()
        refusing.answers[AndroidProcessList.command] = GuestShellOutput(text: lruText, exitCode: 255)
        #expect(await policy.reclaim(from: candidate, shell: refusing) == .failed)
        #expect(refusing.commands == ["dumpsys activity lru"], "the list was used though the command failed")

        let killFails = Shell()
        killFails.answers[AndroidProcessList.command] = GuestShellOutput(text: lruText, exitCode: 0)
        killFails.answers["cmd activity kill --user 0 org.lineageos.jelly"] = GuestShellOutput(text: "", exitCode: 1)
        #expect(await policy.reclaim(from: candidate, shell: killFails) == .failed)
    }
}
