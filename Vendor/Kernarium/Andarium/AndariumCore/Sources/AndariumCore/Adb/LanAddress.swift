// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// One IPv4 address of a network interface, as `getifaddrs` lists it.
public struct InterfaceAddress: Equatable, Sendable {
    public var name: String
    /// The interface is up and running.
    public var isActive: Bool
    public var isLoopback: Bool
    /// The four bytes of the address, most significant first.
    public var octets: [UInt8]

    public init(name: String, isActive: Bool, isLoopback: Bool, octets: [UInt8]) {
        self.name = name
        self.isActive = isActive
        self.isLoopback = isLoopback
        self.octets = octets
    }
}

/// The address a computer on the same network connects to: the device's
/// IPv4 address on Wi-Fi (or on a wired adapter), which a person types after
/// `adb connect`.
public enum LanAddress {
    /// The address to show, or nil when the device has none on a local
    /// network. `en0` is the Wi-Fi interface of an iPhone and an iPad and
    /// comes first; other `en` interfaces are wired adapters. Cellular
    /// (`pdp_ip`), tunnels (`utun`), peer-to-peer (`awdl`, `llw`) and
    /// loopback never count, and neither does a link-local address.
    public static func choose(from addresses: [InterfaceAddress]) -> String? {
        let usable = addresses.filter { address in
            address.isActive && !address.isLoopback && address.octets.count == 4
                && address.name.hasPrefix("en") && !isUnusable(address.octets)
        }
        let best = usable.first { $0.name == "en0" } ?? usable.first
        return best.map { $0.octets.map { String($0) }.joined(separator: ".") }
    }

    private static func isUnusable(_ octets: [UInt8]) -> Bool {
        // 0.0.0.0, loopback, and link-local (no DHCP answer yet).
        octets == [0, 0, 0, 0] || octets[0] == 127 || (octets[0] == 169 && octets[1] == 254)
    }

    /// The interfaces' addresses now.
    public static func interfaceAddresses() -> [InterfaceAddress] {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return [] }
        defer { freeifaddrs(first) }
        var result: [InterfaceAddress] = []
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            guard let address = entry.pointee.ifa_addr, address.pointee.sa_family == sa_family_t(AF_INET) else {
                continue
            }
            let flags = Int32(entry.pointee.ifa_flags)
            let value = address.withMemoryRebound(to: sockaddr_in.self, capacity: 1) {
                UInt32(bigEndian: $0.pointee.sin_addr.s_addr)
            }
            result.append(
                InterfaceAddress(
                    name: String(cString: entry.pointee.ifa_name),
                    isActive: flags & IFF_UP != 0 && flags & IFF_RUNNING != 0,
                    isLoopback: flags & IFF_LOOPBACK != 0,
                    octets: [
                        UInt8(truncatingIfNeeded: value >> 24), UInt8(truncatingIfNeeded: value >> 16),
                        UInt8(truncatingIfNeeded: value >> 8), UInt8(truncatingIfNeeded: value),
                    ]
                ))
        }
        return result
    }

    public static func current() -> String? {
        choose(from: interfaceAddresses())
    }
}

/// The guest's own "USB debugging" setting, which ADB over the network needs
/// (the guest asks to allow a computer only while it is on).
public enum UsbDebugging {
    /// What to run in the guest's shell to read it.
    public static let command = "settings get global adb_enabled"

    /// The setting from the command's output: true for `1`, false for `0` or
    /// an unset value (`null`), nil for anything else.
    public static func parse(_ output: String) -> Bool? {
        switch output.trimmingCharacters(in: .whitespacesAndNewlines) {
        case "1": true
        case "0", "null": false
        default: nil
        }
    }

    /// The setting from a finished command: nil when it failed.
    public static func parse(_ result: ShellResult) -> Bool? {
        guard result.exitCode == 0 else { return nil }
        return parse(result.text)
    }
}
