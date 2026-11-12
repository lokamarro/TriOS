// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The host's time zone as the guest is given it at every start.
///
/// A new guest starts on GMT and Android's automatic time zone has no source
/// in it (no telephony, no location provider), so Andarium states the zone the
/// host is in. The runtime passes it on the kernel command line
/// (`androidboot.kernarium.timezone`, `kernarium_runtime_boot.android_time_zone`)
/// and the system image's time zone script applies it while the person leaves
/// "Set time zone automatically" on; a zone chosen with it off is never
/// replaced (`android/lineage-24/README.md`, "Time zone").
public enum GuestTimeZone {
    /// The longest name the boot request may carry. The longest IANA name is
    /// 32 characters.
    public static let maximumLength = 64

    /// The name to put in the boot request for `identifier`, or nil when it is
    /// not an IANA time zone name in the form the request carries.
    ///
    /// That is 1 through 64 characters of `A-Z a-z 0-9 _ + -` and `/`, in
    /// slash-separated components that are never empty and start with a letter
    /// or digit: every name of the IANA database (`America/Argentina/Buenos_Aires`,
    /// `Etc/GMT+5`, `EST5EDT`). Anything else, a space or a quotation mark
    /// above all, could not be told apart from another kernel argument, so the
    /// runtime refuses it as well (`KernariumGuestTimeZone.hpp`). Whether the
    /// guest's own tzdata has the name is the guest's to decide.
    public static func bootName(_ identifier: String) -> String? {
        let bytes = Array(identifier.utf8)
        guard !bytes.isEmpty, bytes.count <= maximumLength else { return nil }
        var componentStart = true
        for byte in bytes {
            if byte == UInt8(ascii: "/") {
                if componentStart { return nil }
                componentStart = true
                continue
            }
            let alphanumeric =
                (UInt8(ascii: "A")...UInt8(ascii: "Z")).contains(byte)
                || (UInt8(ascii: "a")...UInt8(ascii: "z")).contains(byte)
                || (UInt8(ascii: "0")...UInt8(ascii: "9")).contains(byte)
            let joiner = byte == UInt8(ascii: "_") || byte == UInt8(ascii: "+") || byte == UInt8(ascii: "-")
            if !alphanumeric && (componentStart || !joiner) { return nil }
            componentStart = false
        }
        return componentStart ? nil : identifier
    }

    /// The name of the zone the host is in now, for the boot request, or nil
    /// when it has none the guest could be given. The default zone is the
    /// system's live one: `TimeZone.current` keeps the zone of the first read
    /// for the life of the process, and a person can change the host's zone
    /// between two starts.
    public static func current(of zone: TimeZone = .autoupdatingCurrent) -> String? {
        bootName(zone.identifier)
    }
}
