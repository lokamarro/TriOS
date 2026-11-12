// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

public enum Phase: Equatable, Sendable {
    case home
    case booting(BootKind)
    case running(StartMode)
    case failed
}

public enum HomeNotice: Equatable, Sendable {
    /// iOS ended Andarium while the guest was paused out of the foreground.
    case closedInBackground
    /// The previous session ended badly, as its session record says.
    case sessionEnded(SessionEndNotice)
}

/// Whether a start may go ahead.
public enum StartVerdict: Equatable, Sendable {
    case allowed(BootKind)
    /// This process has an active or failed run and cannot start another.
    case relaunchRequired
    case noGuest
    /// The device's GPU cannot draw the guest (`HostDevice.runsGuestGraphics`).
    case needsNewerChip
    /// The host has less free space than Android's own writes need.
    case lowSpace(neededBytes: UInt64, freeBytes: UInt64)
    /// An update did not complete and this start cannot try it again (it is
    /// Recovery, or this Android is not the one updating): Restore or Erase
    /// is the way on.
    case unfinishedUpdate
    /// The guest cannot start with this Android; erasing it is the way out.
    case blocked(StartBlock)
}

/// A clean power-off returns to Home. A failed run requires a fresh process;
/// a restart Android asks for happens inside the run.
public struct Lifecycle: Equatable, Sendable {
    /// Andarium holds at most one guest.
    public var guest: Guest?
    /// Moves only through the methods below in the application; the preview
    /// and the tests set it to reach a screen directly.
    public var phase: Phase = .home
    public var notice: HomeNotice?

    public init(guest: Guest?) {
        self.guest = guest
    }

    public mutating func create(_ guest: Guest) {
        self.guest = guest
        notice = nil
    }

    /// Erase guest: nothing is left of the guest, its choices included, and
    /// the application is where it was before the first one was set up.
    public mutating func erase() {
        guest = nil
        notice = nil
    }

    /// Android's own factory reset from Recovery: its data erased from
    /// inside, the guest and its choices kept.
    public mutating func factoryReset() {
        guard let guest else { return }
        self.guest = guest.erased
    }

    /// `plan` is what the instance on disk says a start would be. Start
    /// tries an unfinished update again when it can; Recovery and a start
    /// that cannot wait for Restore or Erase.
    public func startVerdict(
        _ mode: StartMode, host: HostDevice, plan: StartPlan = .normal
    ) -> StartVerdict {
        guard phase == .home else { return .relaunchRequired }
        guard host.runsGuestGraphics else { return .needsNewerChip }
        guard guest != nil else { return .noGuest }
        guard host.hasRoom(for: Limits.startReserveBytes) else {
            return .lowSpace(neededBytes: Limits.startReserveBytes, freeBytes: host.freeStorageBytes)
        }
        switch plan {
        case .blocked(let reason):
            return .blocked(reason)
        case .unfinishedUpdate:
            return plan.allowsStart(mode) ? .allowed(.upgrade) : .unfinishedUpdate
        case .firstStart, .normal, .update:
            return .allowed(plan.bootKind(mode))
        }
    }

    /// Starts the guest if the host allows it; the verdict says why not.
    @discardableResult
    public mutating func start(
        _ mode: StartMode, host: HostDevice, plan: StartPlan = .normal
    ) -> StartVerdict {
        let verdict = startVerdict(mode, host: host, plan: plan)
        if case let .allowed(kind) = verdict {
            phase = .booting(kind)
        }
        return verdict
    }

    /// The boot reached Android (or Recovery). A recovery boot changes
    /// nothing in the guest; any other applies a pending growth.
    public mutating func finishBoot(_ kind: BootKind) {
        if kind != .recovery {
            guest?.didBoot()
        }
        phase = .running(kind == .recovery ? .recovery : .normal)
    }

    public mutating func powerOff() { phase = .home }

    public mutating func fail() { phase = .failed }

    /// Android's own Restart boots the guest again in place.
    public mutating func restart() { phase = .booting(.restart) }
}

/// How a run ended, from the runtime's result.
public struct RunEnd: Equatable, Sendable {
    public var result: Int32
    /// The runtime stopped a guest that kept restarting within seconds of
    /// booting.
    public var restartedInALoop: Bool
    /// Why the run could not begin, when it did not.
    public var startFailure: String?

    public init(result: Int32, restartedInALoop: Bool = false, startFailure: String? = nil) {
        self.result = result
        self.restartedInALoop = restartedInALoop
        self.startFailure = startFailure
    }

    public var poweredOff: Bool { result == 0 && !restartedInALoop && startFailure == nil }

    public var phase: Phase { poweredOff ? .home : .failed }

    /// The lines the failure screen offers to copy.
    public func report(
        appVersion: String, system: SystemInfo, guest: Guest?, uptimeSeconds: Int?
    ) -> String {
        var lines = [
            "Andarium \(appVersion) · \(system.release)",
            "\(system.kernel) · \(Formats.vcpus(guest?.cores ?? 0)) · \(Formats.size(mib: guest?.memoryMiB ?? 0))",
        ]
        let reason = if let startFailure {
            "it could not start (\(startFailure))"
        } else if restartedInALoop {
            "the guest kept restarting"
        } else {
            "runtime result \(result)"
        }
        if let uptimeSeconds {
            lines.append("Guest ended: \(reason) after \(Formats.elapsed(uptimeSeconds))")
        } else {
            lines.append("Guest ended: \(reason)")
        }
        return lines.joined(separator: "\n")
    }
}
