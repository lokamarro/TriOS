// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

#if !APP_STORE

/// The lines a run prints for the harness to read. Everything else on the
/// console is the runtime's own `Kernarium:` output. An App Store build compiles this file
/// and every call to it out, and its bundle check fails on a binary that
/// carries one of these lines.
enum ConsoleReport {
    enum ThermalPhase: String {
        case start, change, finish
    }

    static func thermalName(_ state: ProcessInfo.ThermalState) -> String {
        switch state {
        case .nominal: "nominal"
        case .fair: "fair"
        case .serious: "serious"
        case .critical: "critical"
        @unknown default: "unknown"
        }
    }

    static func thermal(phase: ThermalPhase, state: ProcessInfo.ThermalState, uptime: TimeInterval) -> String {
        let seconds = String(format: "%.3f", locale: Locale(identifier: "en_US_POSIX"), uptime)
        return "Andarium thermal state: phase=\(phase.rawValue) state=\(thermalName(state)) uptime=\(seconds)"
    }

    static func runtimeReturned(_ result: Int32) -> String {
        "Andarium: runtime returned \(result)"
    }

    /// The application lost or regained focus (a system alert, such as iOS
    /// asking for local network access, takes it until a person answers).
    static func application(active: Bool) -> String {
        active ? "Andarium: application active" : "Andarium: application inactive"
    }

    /// The line a statistics launch prints after each reading, for the iPad
    /// harness to read: the figures the screens show, the guest RAM the iPad
    /// holds and the boot they belong to. Sizes are whole mebibytes; a figure
    /// not yet known is a dash.
    static func statistics(_ meter: StatsMeter, guestMemoryMiB: Int) -> String {
        func mebibytes(_ bytes: UInt64?) -> String {
            bytes.map { String($0 / Limits.bytesPerMiB) } ?? "-"
        }
        let cpu = meter.cpuPercent.map { "\($0)%" } ?? "-"
        let fps = meter.framesPerSecond.map(String.init) ?? "-"
        let boot = meter.bootIndex.map(String.init) ?? "-"
        return "Andarium stats: cpu=\(cpu) fps=\(fps) memory-used=\(mebibytes(meter.memoryInUseBytes))"
            + " memory=\(guestMemoryMiB) held=\(mebibytes(meter.ramHeldBytes)) boot=\(boot)"
    }
}

#endif
