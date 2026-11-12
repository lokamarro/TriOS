// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The device's storage as the application reads it.
public enum HostStorage {
    /// Free space on the volume of the application's container, as the system
    /// states it for what the user cares about (purgeable space counts).
    public static func freeBytes(on directory: URL = URL(fileURLWithPath: NSHomeDirectory())) -> UInt64 {
        let values = try? directory.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return UInt64(max(values?.volumeAvailableCapacityForImportantUsage ?? 0, 0))
    }
}
