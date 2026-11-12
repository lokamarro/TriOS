// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A guest paused for want of space, as the screen over the session shows it.
public struct SpacePause: Equatable, Sendable {
    public enum Stage: Equatable, Sendable {
        /// The guest is being suspended and its suspension confirmed; the
        /// screen says it is being paused and offers nothing yet.
        case pausing
        /// The guest holds still.
        case paused
        /// The guest did not hold still after every attempt and runs on: the
        /// screen says so and offers Shut down.
        case notPaused
    }

    public enum Working: Equatable, Sendable {
        /// Resume was asked and the guest is waking.
        case resuming
        /// Shut down was asked and the guest is going down.
        case shuttingDown
    }

    /// How Shut down ends the guest, which the screen says.
    public enum Ending: Equatable, Sendable {
        /// The guest is asked to power off and writes what it has first.
        case powerOff
        /// The run ends the way leaving the guest ends it: nothing is asked of
        /// the guest, and what it had stored when it was paused is kept.
        case stop
    }

    /// What the screen calls the guest: "Android" for an Android guest.
    public var guest: String
    /// Free space when it was last read.
    public var freeBytes: UInt64
    /// A resume read the space again and found it still short.
    public var stillShort = false
    public var stage = Stage.paused
    public var working: Working?
    public var ending = Ending.powerOff

    public init(
        guest: String = "Android", freeBytes: UInt64, stillShort: Bool = false,
        stage: Stage = .paused, working: Working? = nil, ending: Ending = .powerOff
    ) {
        self.guest = guest
        self.ending = ending
        self.freeBytes = freeBytes
        self.stillShort = stillShort
        self.stage = stage
        self.working = working
    }

    public var title: String {
        switch stage {
        case .pausing: "Pausing \(guest)"
        case .paused: "\(guest) is paused"
        case .notPaused: "\(guest) is still running"
        }
    }

    /// What is the matter and what to do about it, in the figures the device
    /// has now.
    public var detail: String {
        let needed = Formats.size(bytes: RunSpace.resumeAtBytes)
        let free = Formats.size(bytes: freeBytes)
        if working == .shuttingDown {
            return switch ending {
            case .powerOff: "\(guest) is saving what it has and shutting down."
            case .stop: "\(guest) is stopping. What it had stored is kept."
            }
        }
        // Said where Shut down is offered, so it is clear what it does.
        let endingNote = ending == .stop ? " Shut down ends the run, as leaving the guest does." : ""
        switch stage {
        case .pausing:
            return "This device has \(free) of free space left. \(guest) is saving what it has to storage."
        case .paused:
            if stillShort {
                return "Only \(free) is free. Free up more space, then resume. \(guest) needs \(needed) free to continue."
                    + endingNote
            }
            return "This device has \(free) of free space left, too little for \(guest) to keep writing. "
                + "Free up space, then resume. \(guest) needs \(needed) free to continue." + endingNote
        case .notPaused:
            if stillShort {
                return "Only \(free) is free, and \(guest) is still running. Shut it down before the space "
                    + "runs out, or free up more space and check again. \(guest) needs \(needed) free to continue."
                    + endingNote
            }
            return "This device has \(free) of free space left and \(guest) could not be paused. Shut it "
                + "down before the space runs out, or free up space and check again." + endingNote
        }
    }
}
