// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import UIKit

/// Mock devices, system and guests for the Simulator preview and its
/// screenshots. Nothing here is read from a real device or runtime.
enum PreviewData {
    /// The mock guest's own build, which its recovery screen shows.
    static let release = "20261024"
    /// The release the mock system carries, and the one before it.
    static let systemRelease = "20261109-los24-r20"
    static let previousSystemRelease = "20261108-los24-r18"

    static let system = SystemInfo(
        version: "17", release: systemRelease,
        securityPatch: "5 October 2026", kernel: "Linux 6.18.54",
        architecture: "arm64, 4 KiB pages", abis: ["arm64-v8a", "armeabi-v7a", "armeabi"],
        appVersion: "1.0 (1)"
    )

    /// An iPad Pro or an iPhone, by idiom, or the device a launch names
    /// (`-device`). The memory bounds and defaults of the idiom's mocks are
    /// made-up ones of a wide iPad ruler and a short iPhone one that falls with
    /// the vCPU count, each default under its bound as a device's is, with the
    /// graphics memory of a made-up limit; a device reads all of them from the
    /// runtime's budget formula. The iPad has ten cores and offers eight vCPUs,
    /// the most the budget is measured for.
    static func host(lowSpace: Bool, device: String? = nil) -> HostDevice {
        switch device {
        case "iphone-18-pro-max":
            return HostDevice(
                name: "iPhone", processorCount: 6, memory: iPhone18ProMaxMemory,
                freeStorageBytes: gibibytes(lowSpace ? 1.2 : 41.6)
            )
        case "ipad-mini-6":
            return HostDevice(
                name: "iPad", processorCount: 6, memory: iPadMini6Memory,
                freeStorageBytes: gibibytes(lowSpace ? 1.2 : 118.3)
            )
        default:
            break
        }
        if UIDevice.current.userInterfaceIdiom == .pad {
            return HostDevice(
                name: "iPad", processorCount: 8,
                memory: MemoryPolicy(
                    maximumsMiB: Array(repeating: 4096, count: 8), defaultsMiB: Array(repeating: 3584, count: 8),
                    graphicsMiB: { memory, count in budget(limitMiB: 6144, memoryMiB: memory, vCPUs: count) }
                ),
                freeStorageBytes: gibibytes(lowSpace ? 1.2 : 118.3)
            )
        }
        return HostDevice(
            name: "iPhone", processorCount: 6,
            memory: MemoryPolicy(
                maximumsMiB: [3136, 3072, 3008, 2944, 2880, 2816], defaultsMiB: Array(repeating: 2560, count: 6),
                graphicsMiB: { memory, count in budget(limitMiB: 4096, memoryMiB: memory, vCPUs: count) }
            ),
            freeStorageBytes: gibibytes(lowSpace ? 1.2 : 41.6)
        )
    }

    /// The runtime's rows (`runtime/guest_memory_budget.hpp`) for a memory
    /// limit of 2867 MiB, the iPad mini 6's, at 1 through 6 vCPUs: the bound,
    /// the default (also the size above which iOS is likely to close the
    /// application) and the host GPU's budget of a guest, which `budget`
    /// evaluates as the runtime does for a guest of 1.5 GiB or more.
    static let iPadMini6Memory = MemoryPolicy(
        maximumsMiB: [2304, 2304, 2240, 2240, 2176, 2176], defaultsMiB: Array(repeating: 1536, count: 6),
        graphicsMiB: { memory, count in budget(limitMiB: 2867, memoryMiB: memory, vCPUs: count) }
    )

    /// The same for a memory limit of 5120 MiB, which an iPhone 18 Pro Max is
    /// assumed to have: 4096 MiB at every count, a default of 3 GiB that keeps
    /// the GPU a gibibyte and 3.5 GiB above which iOS is likely to close the
    /// application.
    static let iPhone18ProMaxMemory = MemoryPolicy(
        maximumsMiB: Array(repeating: 4096, count: 6), defaultsMiB: Array(repeating: 3072, count: 6),
        safeMiB: Array(repeating: 3584, count: 6),
        graphicsMiB: { memory, count in budget(limitMiB: 5120, memoryMiB: memory, vCPUs: count) }
    )

    /// B for a mock: L less A (64), E_rest (320), S (149), the gadget cache at
    /// its cap of the vCPU count and 4 MiB of slot tags per vCPU and the
    /// guest, between 384 MiB and half of the guest. The cap is the table in
    /// `runtime/guest_memory_budget.hpp`, which holds for the guests the sheet
    /// offers (1.5 GiB and up).
    private static func budget(limitMiB: Int, memoryMiB: Int, vCPUs: Int) -> Int {
        let caps = [128, 160, 192, 224, 240, 240, 272, 272]
        let engine = 320 + caps[min(max(vCPUs, 1), caps.count) - 1] + 4 * max(vCPUs, 1)
        let available = limitMiB - 64 - memoryMiB - engine - 149
        return min(max(available, 384), memoryMiB / 2)
    }

    /// A mock size, in gibibytes.
    private static func gibibytes(_ count: Double) -> UInt64 { UInt64(count * Double(Limits.bytesPerGiB)) }

    /// The seed the mock storage would write: about as large as the real one.
    static let createRequirementBytes: UInt64 = 3_274_702_848 + Limits.firstStartReserveBytes

    /// A guest in everyday use, with the memory a new guest gets.
    static func sampleGuest(on host: HostDevice) -> Guest {
        Guest(
            cores: host.processorCount, memoryMiB: host.memory.defaultMemoryMiB(vCPUs: host.processorCount),
            capacityGiB: 16, usedBytes: gibibytes(5.4)
        )
    }

    /// The same guest with the most memory the device offers, which is above
    /// what a new guest gets.
    static func highMemoryGuest(on host: HostDevice) -> Guest {
        var guest = sampleGuest(on: host)
        guest.memoryMiB = host.memory.maximumMiB(vCPUs: guest.cores)
        return guest
    }

    static let sampleApkName = "Signal-7.21.4.apk"
    static let sampleApkSizeBytes = UInt64(38.2 * Double(Limits.bytesPerMiB))

    /// A sparse file named like the sample APK, for the preview's install.
    static func sampleApk() -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(sampleApkName)
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
            if let handle = try? FileHandle(forWritingTo: url) {
                try? handle.truncate(atOffset: sampleApkSizeBytes)
                try? handle.close()
            }
        }
        return url
    }
}
