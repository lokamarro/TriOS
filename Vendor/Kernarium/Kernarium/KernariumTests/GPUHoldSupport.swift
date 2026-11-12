// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
@testable import Kernarium

/// What a test double was asked for, in order. The hold can be taken from a
/// thread that is not the main thread, so the log takes a lock.
nonisolated final class CallLog: @unchecked Sendable {
    private let lock = NSLock()
    private var recorded: [String] = []

    var entries: [String] {
        get { lock.withLock { recorded } }
        set { lock.withLock { recorded = newValue } }
    }

    func add(_ call: String) {
        lock.withLock { recorded.append(call) }
    }
}

/// A gate that logs the hold and the release it is asked for.
nonisolated final class RecordingGate: GuestGPUGate {
    let log: CallLog

    init(_ log: CallLog) {
        self.log = log
    }

    func hold() { log.add("hold") }
    func release() { log.add("release") }
}

/// The bound's timer under a test's control: nothing fires until the test
/// says so, and a test can fire from whichever thread it likes.
nonisolated final class ManualHoldTimer: @unchecked Sendable {
    private struct Armed {
        let id: Int
        let action: @Sendable () -> Void
        var withdrawn = false
    }

    private let lock = NSLock()
    private var armed: [Armed] = []
    private var nextID = 0
    private var armedDelays: [TimeInterval] = []
    /// Whether withdrawing a bound keeps it from firing. A real timer can fire
    /// as it is withdrawn, which a test sets this to false to reproduce.
    var honorsWithdrawal = true

    /// The delay of every bound armed, in order.
    var delays: [TimeInterval] { lock.withLock { armedDelays } }

    var pending: Int {
        lock.withLock { armed.filter { !$0.withdrawn }.count }
    }

    var timer: GuestGPUHoldTimer {
        { [self] seconds, action in
            lock.withLock {
                nextID += 1
                armedDelays.append(seconds)
                armed.append(Armed(id: nextID, action: action))
                let id = nextID
                return { [self] in
                    lock.withLock {
                        if let index = armed.firstIndex(where: { $0.id == id }) {
                            armed[index].withdrawn = true
                        }
                    }
                }
            }
        }
    }

    /// Fires every bound still armed, and the withdrawn ones too when
    /// withdrawal is not honored. Each fires once.
    func fire() {
        let due = lock.withLock {
            let due = armed.filter { honorsWithdrawal ? !$0.withdrawn : true }
            armed.removeAll()
            return due
        }
        for bound in due { bound.action() }
    }
}
