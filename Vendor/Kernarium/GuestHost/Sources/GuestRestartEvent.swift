// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A restart the guest asked for, as the runtime reports it. The guest runs
/// on after it, in the same launch.
enum GuestRestartEvent: Equatable {
    /// The guest asked to restart and is gone; the next boot is being
    /// prepared, and waits while the GPU is held or the guest is paused.
    case began(restart: Int)
    /// The restarted guest's console answers.
    case booted(restart: Int)

    init?(event: UInt32, restartCount: UInt32) {
        switch Int(event) {
        case KERNARIUM_RUNTIME_RESTART_BEGAN:
            self = .began(restart: Int(restartCount))
        case KERNARIUM_RUNTIME_RESTART_BOOTED:
            self = .booted(restart: Int(restartCount))
        default:
            return nil
        }
    }
}

/// Whether the guest is between two boots. A restart the guest asked for
/// shows no notice, only a badge while the next boot is under way.
struct GuestRestartIndicator: Equatable {
    private(set) var restarting = false
    private(set) var restarts = 0

    mutating func apply(_ event: GuestRestartEvent) {
        switch event {
        case let .began(restart):
            restarts = max(restarts, restart)
            restarting = true
        case let .booted(restart):
            // An event of an earlier restart ends nothing of a later one.
            if restart >= restarts { restarting = false }
        }
    }

    /// The run ended, so nothing restarts any more.
    mutating func runEnded() {
        restarting = false
    }
}
