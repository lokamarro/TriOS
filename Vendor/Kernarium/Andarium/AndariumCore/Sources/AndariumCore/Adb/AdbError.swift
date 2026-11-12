// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Why an adb operation ended without its result. The application words each
/// case for the person; nothing here is shown as it is.
public enum AdbError: Error, Equatable, Sendable {
    /// Nothing listens on the guest's adb port, or the guest never answered:
    /// adbd is not running (USB debugging is off in Android, or Android is
    /// still starting).
    case refused
    /// The guest did not answer the connection at all within the connect
    /// limit: it is still starting, or not running yet. Retried like
    /// `refused` until then.
    case notAnswering
    /// adbd closed or reset the connection, or the guest stopped.
    case guestGone
    /// The guest gave no sign of life for the phase's whole allowance, counted
    /// in the time the guest ran.
    case timedOut
    /// The caller cancelled the operation.
    case cancelled
    /// adbd did not accept the host's key: it asked for a signature again.
    case keyRejected
    /// adbd answered an OPEN with CLSE: it has no such service.
    case serviceRefused(String)
    /// The service closed its stream before the writer was done; what it said
    /// first is still there to `read`.
    case streamClosed
    /// The peer said something the protocol does not allow.
    case protocolViolation(String)
    /// The peer reported a failure of its own: a sync FAIL, or a FAIL line of
    /// `bugreportz`.
    case remote(String)
    /// A file or descriptor failed locally, with the POSIX error number.
    case io(Int32)
}
