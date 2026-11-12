// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import MetricKit
import Synchronization

/// The exit counts and diagnostics iOS delivers through MetricKit, kept on the
/// device beside the session records (`MetricKitReports`) for the bug report:
/// a day's `MXAppExitMetric` counts the ends at the memory limit, which leave
/// no other trace. iOS delivers a day's metrics at most once a day, on a
/// queue of its own.
nonisolated final class ExitReports: NSObject, MXMetricManagerSubscriber, Sendable {
    static let shared = ExitReports()

    /// Set once, before the subscription starts.
    private let directory = Mutex<URL?>(nil)

    func start(keepingIn directory: URL) {
        let first = self.directory.withLock { kept -> Bool in
            guard kept == nil else { return false }
            kept = directory
            return true
        }
        guard first else { return }
        let manager = MXMetricManager.shared
        manager.add(self)
        // What arrived while nothing listened.
        didReceive(manager.pastPayloads)
        didReceive(manager.pastDiagnosticPayloads)
    }

    func didReceive(_ payloads: [MXMetricPayload]) {
        for payload in payloads {
            keep(payload.jsonRepresentation(), kind: "metrics", ending: payload.timeStampEnd)
        }
    }

    func didReceive(_ payloads: [MXDiagnosticPayload]) {
        for payload in payloads {
            keep(payload.jsonRepresentation(), kind: "diagnostics", ending: payload.timeStampEnd)
        }
    }

    /// Named by the end of the period a report covers, so the names sort by
    /// time and the oldest go first.
    private func keep(_ json: Data, kind: String, ending: Date) {
        guard let directory = directory.withLock({ $0 }) else { return }
        let stamp = ISO8601DateFormatter.string(
            from: ending, timeZone: TimeZone(identifier: "UTC")!,
            formatOptions: [.withYear, .withMonth, .withDay, .withTime, .withTimeZone])
        MetricKitReports.keep(json, named: "\(stamp)-\(kind).json", in: directory)
    }
}
