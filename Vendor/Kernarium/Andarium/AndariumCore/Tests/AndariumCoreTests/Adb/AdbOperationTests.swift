// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// A clock source the test moves by hand.
final class ManualSource: @unchecked Sendable {
    private let lock = NSLock()
    private var current = Duration.zero

    var now: Duration { lock.withLock { current } }

    func advance(_ amount: Duration) {
        lock.withLock { current += amount }
    }
}

/// Counts calls from any thread.
final class AttemptCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0

    func next() -> Int { lock.withLock { count += 1; return count } }
}

struct GuestRunClockTests {
    @Test func itCountsOnlyWhileTheGuestRuns() {
        let source = ManualSource()
        let clock = GuestRunClock(source: { source.now })
        source.advance(.seconds(10))
        #expect(clock.now == .seconds(10))
        clock.pause()
        #expect(clock.isPaused)
        source.advance(.seconds(500))
        #expect(clock.now == .seconds(10))
        clock.pause()
        clock.resume()
        #expect(!clock.isPaused)
        source.advance(.seconds(5))
        clock.resume()
        #expect(clock.now == .seconds(15))
    }

    @Test func theSystemClockMoves() throws {
        let clock = GuestRunClock()
        let first = clock.now
        Thread.sleep(forTimeInterval: 0.02)
        #expect(clock.now > first)
    }
}

@Suite(.timeLimit(.minutes(1)))
struct AdbWatchdogTests {
    /// Waits, up to a bound, for `condition`.
    private func eventually(_ condition: () -> Bool) -> Bool {
        let deadline = Date(timeIntervalSinceNow: 10)
        while Date() < deadline {
            if condition() { return true }
            Thread.sleep(forTimeInterval: 0.01)
        }
        return condition()
    }

    @Test func silenceForTheIdleLimitEndsTheConnection() {
        let source = ManualSource()
        let clock = GuestRunClock(source: { source.now })
        let watchdog = AdbWatchdog(clock: clock, tick: 0.01)
        defer { watchdog.stop() }
        let transport = CountingTransport()
        watchdog.attach(transport)
        watchdog.arm(idle: .seconds(60))
        source.advance(.seconds(59))
        Thread.sleep(forTimeInterval: 0.1)
        #expect(watchdog.currentCause == .none && transport.shutdowns == 0)
        watchdog.touch()
        source.advance(.seconds(59))
        Thread.sleep(forTimeInterval: 0.1)
        #expect(watchdog.currentCause == .none)
        source.advance(.seconds(2))
        #expect(eventually { watchdog.currentCause == .timedOut })
        #expect(eventually { transport.shutdowns >= 1 })
    }

    @Test func aPausedGuestDoesNotTimeOut() {
        let source = ManualSource()
        let clock = GuestRunClock(source: { source.now })
        let watchdog = AdbWatchdog(clock: clock, tick: 0.01)
        defer { watchdog.stop() }
        let transport = CountingTransport()
        watchdog.attach(transport)
        watchdog.arm(idle: .seconds(60), overall: .seconds(600))
        clock.pause()
        source.advance(.seconds(100_000))
        Thread.sleep(forTimeInterval: 0.15)
        #expect(watchdog.currentCause == .none && transport.shutdowns == 0)
        clock.resume()
        source.advance(.seconds(61))
        #expect(eventually { watchdog.currentCause == .timedOut })
    }

    @Test func theOverallLimitEndsAConnectionThatKeepsTalking() {
        let source = ManualSource()
        let clock = GuestRunClock(source: { source.now })
        let watchdog = AdbWatchdog(clock: clock, tick: 0.01)
        defer { watchdog.stop() }
        watchdog.attach(CountingTransport())
        watchdog.arm(idle: .seconds(300), overall: .seconds(2700))
        for _ in 0..<9 {
            source.advance(.seconds(290))
            watchdog.touch()
        }
        Thread.sleep(forTimeInterval: 0.1)
        #expect(watchdog.currentCause == .none)
        source.advance(.seconds(200))
        watchdog.touch()
        #expect(eventually { watchdog.currentCause == .timedOut })
    }

    @Test func cancellingShutsTheTransportDownOnceAndKeepsTheCause() {
        let watchdog = AdbWatchdog(clock: GuestRunClock(), tick: 0.01)
        defer { watchdog.stop() }
        let transport = CountingTransport()
        watchdog.attach(transport)
        watchdog.cancel()
        #expect(watchdog.currentCause == .cancelled)
        #expect(transport.shutdowns == 1)
    }

    @Test func aTransportAttachedAfterTheEndIsShutDownAtOnce() {
        let watchdog = AdbWatchdog(clock: GuestRunClock(), tick: 0.01)
        defer { watchdog.stop() }
        watchdog.cancel()
        let transport = CountingTransport()
        watchdog.attach(transport)
        #expect(transport.shutdowns == 1)
    }
}

@Suite(.timeLimit(.minutes(1)))
struct AdbSessionTests {
    private func session(
        _ connector: any AdbConnector, signer: (any AdbSigner)? = nil,
        connectLimit: Duration = .seconds(20)
    ) -> AdbSession {
        AdbSession(
            connector: connector, signer: signer, clock: GuestRunClock(), connectLimit: connectLimit,
            retryInterval: 0.01, tick: 0.01
        )
    }

    @Test func refusalsAreRetriedUntilAdbdAnswers() throws {
        let adbd = FakeAdbd(services: ["shell,v2,raw:true": { stream in
            try stream.send(ShellPacket(id: ShellPacket.exit, data: Data([0])).encoded())
        }])
        defer { adbd.shutdown() }
        let connector = ScriptedConnector(failures: [.refused, .refused, .refused]) { adbd.client }
        let exit = try session(connector).run { connection in
            try connection.runShell("true").exitCode
        }
        #expect(exit == 0)
        #expect(connector.attemptCount == 4)
    }

    @Test func aGuestThatNeverAnswersIsNotReadyAtTheConnectLimit() {
        let connector = ScriptedConnector(failures: Array(repeating: .refused, count: 100_000)) {
            CountingTransport()
        }
        #expect(throws: AdbError.refused) {
            try session(connector, connectLimit: .milliseconds(150)).run { _ in 1 }
        }
    }

    @Test func aGuestThatNeverAnswersTheConnectionIsNotUpAtTheConnectLimit() {
        let connector = ScriptedConnector(failures: Array(repeating: .notAnswering, count: 100_000)) {
            CountingTransport()
        }
        #expect(throws: AdbError.notAnswering) {
            try session(connector, connectLimit: .milliseconds(150)).run { _ in 1 }
        }
        #expect(connector.attemptCount > 1)
    }

    @Test func aGuestThatAnswersAfterSilenceIsRetriedLikeARefusal() throws {
        let adbd = FakeAdbd(services: ["shell,v2,raw:true": { stream in
            try stream.send(ShellPacket(id: ShellPacket.exit, data: Data([0])).encoded())
        }])
        defer { adbd.shutdown() }
        let connector = ScriptedConnector(failures: [.notAnswering, .refused, .notAnswering]) { adbd.client }
        let exit = try session(connector).run { connection in
            try connection.runShell("true").exitCode
        }
        #expect(exit == 0)
        #expect(connector.attemptCount == 4)
    }

    @Test func silenceFollowedByAConnectionThatStallsIsNotReadyNotUnanswered() {
        // The guest answered the last attempt, so it is up; the handshake
        // that went nowhere is the refusal.
        final class Mute: AdbTransport, @unchecked Sendable {
            private let gate = DispatchSemaphore(value: 0)
            func readExactly(_ count: Int) throws -> Data {
                // The watchdog's shutdown opens it; the bound is for one
                // that never comes.
                _ = gate.wait(timeout: .now() + 30)
                throw AdbError.guestGone
            }
            func writeAll(_ data: Data) throws {}
            func shutdown() { gate.signal() }
        }
        let connector = ScriptedConnector(failures: [.notAnswering]) { Mute() }
        #expect(throws: AdbError.refused) {
            try session(connector, connectLimit: .milliseconds(150)).run { _ in 1 }
        }
    }

    @Test func aGuestThatAnswersTheConnectButNotTheHandshakeIsNotReady() {
        let quiet = FakeAdbd(auth: .on { _, _ in true }, services: [:])
        defer { quiet.shutdown() }
        // The scripted adbd waits for CNXN; a transport that goes nowhere
        // never lets it answer, so the connect limit ends the handshake.
        final class Mute: AdbTransport, @unchecked Sendable {
            private let gate = DispatchSemaphore(value: 0)
            func readExactly(_ count: Int) throws -> Data {
                // The watchdog's shutdown opens it; the bound is for one
                // that never comes.
                _ = gate.wait(timeout: .now() + 30)
                throw AdbError.guestGone
            }
            func writeAll(_ data: Data) throws {}
            func shutdown() { gate.signal() }
        }
        #expect(throws: AdbError.refused) {
            try session(ScriptedConnector { Mute() }, connectLimit: .milliseconds(150)).run { _ in 1 }
        }
    }

    @Test func aHandshakeCutByAnAdbdRestartIsRetriedOnANewConnection() throws {
        let adbd = FakeAdbd(services: ["shell,v2,raw:true": { stream in
            try stream.send(ShellPacket(id: ShellPacket.exit, data: Data([0])).encoded())
        }])
        defer { adbd.shutdown() }
        // The first connection is the old adbd, which ends it at once; the
        // second reaches the new one.
        let first = CountingTransport()
        let attempts = AttemptCounter()
        let connector = ScriptedConnector { () -> any AdbTransport in
            if attempts.next() == 1 { return first }
            return adbd.client
        }
        let exit = try session(connector).run { connection in
            try connection.runShell("true").exitCode
        }
        #expect(exit == 0)
        #expect(connector.attemptCount == 2)
        #expect(first.shutdowns >= 1)
    }

    @Test func adbdThatKeepsEndingTheHandshakeIsNotReadyAtTheLimit() {
        let connector = ScriptedConnector { CountingTransport() }
        #expect(throws: AdbError.refused) {
            try session(connector, connectLimit: .milliseconds(150)).run { _ in 1 }
        }
        #expect(connector.attemptCount > 1)
    }

    @Test func aGuestThatIsGoneEndsTheOperationWithoutRetrying() {
        let connector = ScriptedConnector(failures: [.guestGone]) { CountingTransport() }
        #expect(throws: AdbError.guestGone) { try session(connector).run { _ in 1 } }
        #expect(connector.attemptCount == 1)
    }

    @Test func aDeviceThatStaysSilentInAPhaseTimesOutThere() {
        let quiet = FakeAdbd(services: ["silent:": { stream in _ = try stream.receive() }])
        defer { quiet.shutdown() }
        let operation = session(ScriptedConnector { quiet.client })
        #expect(throws: AdbError.timedOut) {
            try operation.run { connection in
                let stream = try connection.open("silent:")
                operation.watchdog.arm(idle: .milliseconds(100))
                return try stream.read()
            }
        }
    }

    @Test func cancellingFromAnotherThreadEndsTheBlockedRead() {
        let adbd = FakeAdbd(services: ["silent:": { stream in _ = try stream.receive() }])
        defer { adbd.shutdown() }
        let operation = session(ScriptedConnector { adbd.client })
        let canceller = Thread {
            Thread.sleep(forTimeInterval: 0.2)
            operation.cancel()
        }
        canceller.start()
        #expect(throws: AdbError.cancelled) {
            try operation.run { connection in
                let stream = try connection.open("silent:")
                return try stream.read()
            }
        }
    }

    @Test func cancellingDuringTheConnectRetryEndsIt() {
        let connector = ScriptedConnector(failures: Array(repeating: .refused, count: 100_000)) {
            CountingTransport()
        }
        let operation = AdbSession(
            connector: connector, signer: nil, clock: GuestRunClock(), connectLimit: .seconds(60),
            retryInterval: 5, tick: 0.01
        )
        let canceller = Thread {
            Thread.sleep(forTimeInterval: 0.2)
            operation.cancel()
        }
        canceller.start()
        let started = Date()
        #expect(throws: AdbError.cancelled) { try operation.run { _ in 1 } }
        #expect(Date().timeIntervalSince(started) < 5)
    }

    @Test func aRejectedKeyIsReportedAsSuch() throws {
        let adbd = FakeAdbd(auth: .on { _, _ in false })
        defer { adbd.shutdown() }
        struct AnySigner: AdbSigner {
            func sign(token: Data) throws -> Data { Data(count: 256) }
        }
        #expect(throws: AdbError.keyRejected) {
            try session(ScriptedConnector { adbd.client }, signer: AnySigner()).run { _ in 1 }
        }
    }
}

@Suite(.timeLimit(.minutes(1)))
struct BugReportTests {
    private func shell(_ id: UInt8, _ text: String) -> Data {
        ShellPacket(id: id, data: Data(text.utf8)).encoded()
    }

    private func connected(_ adbd: FakeAdbd) throws -> AdbConnection {
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        return connection
    }

    @Test func progressIsAFractionThatNeverMovesBackAndTheReportIsAtTheOKPath() throws {
        let adbd = FakeAdbd(services: ["shell,v2,raw:bugreportz -p": { stream in
            try stream.send(shell(ShellPacket.stdout, "BEGIN:/data/b/bugreport-x.zip\nPROGRESS:10/100\n"))
            try stream.send(shell(ShellPacket.stdout, "PROGRESS:5/100\nPROGRESS:60/100\nPROG"))
            try stream.send(shell(ShellPacket.stdout, "RESS:100/100\nOK:/data/b/bugreport-x.zip\n"))
            try stream.send(shell(ShellPacket.exit, "\0"))
        }])
        defer { adbd.shutdown() }
        var fractions: [Double] = []
        var alive = 0
        let path = try BugReport.generate(
            on: try connected(adbd), progress: { fractions.append($0) }, alive: { alive += 1 })
        #expect(path == "/data/b/bugreport-x.zip")
        #expect(fractions == [0.1, 0.6, 1.0])
        #expect(alive > 0)
    }

    @Test func aFailLineIsTheGuestsReason() throws {
        let adbd = FakeAdbd(services: ["shell,v2,raw:bugreportz -p": { stream in
            try stream.send(shell(ShellPacket.stdout, "FAIL:There is an existing bugreport in progress\n"))
        }])
        defer { adbd.shutdown() }
        #expect(throws: AdbError.remote("There is an existing bugreport in progress")) {
            try BugReport.generate(on: try connected(adbd), progress: { _ in }, alive: {})
        }
    }

    @Test func aStreamThatEndsWithoutAnOKIsTheGuestStopping() throws {
        let adbd = FakeAdbd(services: ["shell,v2,raw:bugreportz -p": { stream in
            try stream.send(shell(ShellPacket.stdout, "BEGIN:/x\nPROGRESS:1/2\n"))
        }])
        defer { adbd.shutdown() }
        #expect(throws: AdbError.guestGone) {
            try BugReport.generate(on: try connected(adbd), progress: { _ in }, alive: {})
        }
    }

    private func syncService(path: String, body: Data) -> FakeAdbd.Service {
        { stream in
            func request() throws -> (String, String) {
                let header = try stream.receive(exactly: 8)
                let path = try stream.receive(exactly: Int(header.littleEndian32(at: 4)))
                return (String(decoding: header.prefix(4), as: UTF8.self), String(decoding: path, as: UTF8.self))
            }
            let (kind, asked) = try request()
            #expect(kind == "STA2" && asked == path)
            var stat = Data("STA2".utf8)
            stat.appendLittleEndian(UInt32(0))
            stat.appendLittleEndian(UInt64(0))
            stat.appendLittleEndian(UInt64(0))
            stat.appendLittleEndian(UInt32(0o100660))
            stat.appendLittleEndian(UInt32(1))
            stat.appendLittleEndian(UInt32(2000))
            stat.appendLittleEndian(UInt32(2000))
            stat.appendLittleEndian(UInt64(body.count))
            for _ in 0..<3 { stat.appendLittleEndian(UInt64(0)) }
            try stream.send(stat)
            let (kind2, asked2) = try request()
            // A caller that stops after STAT says goodbye instead.
            if kind2 == "QUIT" { return }
            #expect(kind2 == "RECV" && asked2 == path)
            var offset = 0
            while offset < body.count {
                let end = min(offset + AdbSync.maxChunk, body.count)
                var wire = Data("DATA".utf8)
                wire.appendLittleEndian(UInt32(end - offset))
                wire.append(body[offset..<end])
                try stream.send(wire)
                offset = end
            }
            var done = Data("DONE".utf8)
            done.appendLittleEndian(UInt32(0))
            try stream.send(done)
            _ = try? stream.receive()
        }
    }

    @Test func theReportIsPulledIntoTheStagedSinkWithItsProgress() throws {
        let body = Data((0..<300_000).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 1) })
        let adbd = FakeAdbd(services: ["sync:": syncService(path: "/data/b/r.zip", body: body)])
        defer { adbd.shutdown() }
        let sink = MemorySink()
        var staged: (String, UInt64)?
        var steps: [UInt64] = []
        var hooks = BugReport.Hooks()
        hooks.stage = { path, size in
            staged = (path, size)
            return sink
        }
        hooks.pulling = { received, _ in steps.append(received) }
        let result = try BugReport.pull(on: try connected(adbd), remotePath: "/data/b/r.zip", hooks: hooks)
        #expect(result == BugReport.Result(remotePath: "/data/b/r.zip", bytes: UInt64(body.count)))
        #expect(staged?.0 == "/data/b/r.zip" && staged?.1 == UInt64(body.count))
        #expect(sink.data == body)
        #expect(steps.first == 0 && steps.last == UInt64(body.count))
        #expect(steps == steps.sorted())
    }

    @Test func stagingMayRefuseBeforeAnyByteIsPulled() throws {
        let adbd = FakeAdbd(services: ["sync:": syncService(path: "/r.zip", body: Data(count: 10))])
        defer { adbd.shutdown() }
        var hooks = BugReport.Hooks()
        struct NoRoom: Error {}
        hooks.stage = { _, _ in throw NoRoom() }
        #expect(throws: NoRoom.self) {
            try BugReport.pull(on: try connected(adbd), remotePath: "/r.zip", hooks: hooks)
        }
    }

    @Test func theStagedNameKeepsOnlySafeCharactersAndEndsInZip() {
        #expect(BugReport.stagedName(for: "/data/user_de/0/com.android.shell/files/bugreports/bugreport-lineage-2026-11-03-10-00-00.zip")
            == "bugreport-lineage-2026-11-03-10-00-00.zip")
        #expect(BugReport.stagedName(for: "/x/a b;c$(d).zip") == "a_b_c__d_.zip")
        #expect(BugReport.stagedName(for: "/x/..hidden") == "hidden.zip")
        #expect(BugReport.stagedName(for: "/") == "bugreport.zip")
        #expect(BugReport.stagedName(for: "/x/" + String(repeating: "a", count: 300)).count <= 120)
    }

    @Test func theCleanupRemovesTheReportAndOlderLeftovers() {
        let command = BugReport.cleanupCommand(afterPulling: "/data/b/bugreport-x-1.zip")
        #expect(command == "cd '/data/b' && for f in bugreport-* dumpstate_log-*; do [ \"$f\" -ot 'bugreport-x-1.zip' ] && rm -f -- \"$f\"; done; rm -f -- 'bugreport-x-1.zip'")
        // A path the shell would read as more than a name gets only its own removal.
        #expect(BugReport.cleanupCommand(afterPulling: "/data/b/it's.zip") == #"rm -f '/data/b/it'\''s.zip'"#)
        #expect(BugReport.cleanupCommand(afterPulling: "relative.zip") == "rm -f 'relative.zip'")
        #expect(BugReport.cleanupCommand(afterPulling: "/") == "rm -f '/'")
    }

    @Test func theGuestCommandsAreQuoted() {
        #expect(BugReport.removalCommand(for: "/data/b/it's.zip") == #"rm -f '/data/b/it'\''s.zip'"#)
        #expect(BugReport.stopCommand == "setprop ctl.stop dumpstatez")
    }
}
