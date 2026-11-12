// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

enum AndroidBootMode: UInt32, CaseIterable, Identifiable {
    static let environmentKey = "KERNARIUM_ANDROID_BOOT_MODE"

    case automatic = 0
    case normal = 1
    case recovery = 2

    var id: UInt32 { rawValue }

    var title: String {
        switch self {
        case .automatic:
            "Automatic"
        case .normal:
            "Normal"
        case .recovery:
            "Recovery"
        }
    }

    init?(environmentValue: String) {
        switch environmentValue {
        case "auto": self = .automatic
        case "normal": self = .normal
        case "recovery": self = .recovery
        default: return nil
        }
    }
}

struct EmulatorConfiguration: Equatable {
    static let vCPUEnvironmentKey = "KERNARIUM_VCPU_COUNT"
    static let guestMemoryEnvironmentKey = "KERNARIUM_GUEST_MEMORY_MIB"
    static let overlaySizeEnvironmentKey = "KERNARIUM_OVERLAY_SIZE_GIB"
    static let androidDiskSizeEnvironmentKey = "KERNARIUM_ANDROID_DISK_SIZE_GIB"
    static let retinaEnvironmentKey = "KERNARIUM_RETINA_MODE"
    static let autoResizeGuestEnvironmentKey = "KERNARIUM_GPU_AUTO_RESIZE"
    // The three launch parameters of the host GPU's fixed budget
    // B = clamp(L - A - G - E - S, 384 MiB, G / 2): A, the application's own
    // expected ceiling; E_rest, the guest side's memory beyond the gadget
    // cache; and S, the headroom left unused. Only the environment states
    // them, for a measurement run.
    static let gpuBudgetApplicationEnvironmentKey =
        "KERNARIUM_GPU_BUDGET_APP_MIB"
    static let gpuBudgetEngineEnvironmentKey =
        "KERNARIUM_GPU_BUDGET_ENGINE_MIB"
    static let gpuBudgetHeadroomEnvironmentKey =
        "KERNARIUM_GPU_BUDGET_HEADROOM_MIB"
    // They default to the conservative launch set of
    // runtime/guest_memory_budget.hpp, which the loader answers without a
    // runtime: the set the default guest RAM is measured with, so the budget a
    // launch gets is the one that default leaves.
    private static let launchMemorySet =
        kernarium_runtime_loader_launch_memory_set()
    // The set the most guest RAM the settings offer is measured with: it
    // counts none of A, E_rest and S, whatever a launch states for them.
    private static let maximumMemorySet =
        kernarium_runtime_loader_maximum_memory_set()
    static var defaultGPUBudgetApplicationMiB: Int {
        Int(launchMemorySet.application_mib)
    }
    static var defaultGPUBudgetEngineMiB: Int {
        Int(launchMemorySet.engine_mib)
    }
    static var defaultGPUBudgetHeadroomMiB: Int {
        Int(launchMemorySet.headroom_mib)
    }
    static let gpuBudgetParameterRangeMiB = 0...16384
    static let guestMemoryRangeMiB = 512...4096
    static let guestMemoryStepMiB = 64
    static let overlaySizeRangeGiB = 1...32
    // The release's data disk template allows 4 through 64 GiB; the setting
    // keeps its established floor.
    static let androidDiskSizeRangeGiB = 6...64
    /// The guest RAM a new configuration takes at `vCPUCount` vCPUs, as the
    /// runtime's own rule gives it, read through the loader against the memory
    /// limit the system sets this process: the most the conservative launch
    /// set leaves the host GPU its floor for, in half-gibibyte steps within
    /// 1536...4096 MiB and never above the maximum the settings offer. A
    /// device takes less than a Mac, which sets no limit and has 4096. A value
    /// the user stored is kept.
    typealias DefaultGuestMemory = (_ vCPUCount: Int) -> Int

    static let systemDefaultGuestMemory: DefaultGuestMemory = { vCPUCount in
        Int(kernarium_runtime_loader_default_guest_memory_mib(
            UInt32(clamping: max(vCPUCount, 1)),
            launchMemorySet
        ))
    }
    static let defaultOverlaySizeGiB = 8
    static let defaultAndroidDiskSizeGiB = 8
    static let defaultRetinaMode = false
    static let defaultAutoResizeGuest = true
    static let defaultAndroidBootMode = AndroidBootMode.automatic

    var vCPUCount: Int
    var guestMemoryMiB: Int
    var overlaySizeGiB: Int
    var androidDiskSizeGiB: Int
    var rootfsFilename: String
    var retinaMode: Bool
    var autoResizeGuest: Bool
    var androidBootMode: AndroidBootMode
    var gpuBudgetApplicationMiB = defaultGPUBudgetApplicationMiB
    var gpuBudgetEngineMiB = defaultGPUBudgetEngineMiB
    var gpuBudgetHeadroomMiB = defaultGPUBudgetHeadroomMiB

    /// The most guest RAM, in MiB, for a vCPU count.
    typealias GuestMemoryBound = (_ vCPUCount: Int) -> Int

    /// What the system allows: the runtime's own budget formula with the
    /// maximum set, read through the loader against the memory limit the
    /// system sets this process, 4096 where it sets none (a Mac).
    static let systemGuestMemoryBound: GuestMemoryBound = { vCPUCount in
        Int(kernarium_runtime_loader_maximum_guest_memory_mib(
            UInt32(clamping: max(vCPUCount, 1)),
            maximumMemorySet
        ))
    }

    /// The most guest RAM the settings offer at `vCPUCount` vCPUs: a size of
    /// the stepper's range that leaves the host GPU its floor under the
    /// memory limit. Each added vCPU holds more of the process's memory, so
    /// the bound falls as the count rises.
    func maximumGuestMemoryMiB(
        vCPUCount: Int,
        bound: GuestMemoryBound = EmulatorConfiguration.systemGuestMemoryBound
    ) -> Int {
        Self.steppedGuestMemoryMiB(bound(vCPUCount), rounding: .down)
    }

    /// Takes guest RAM down to the bound of the vCPU count after the count
    /// went from `oldCount` to the current one. Raising the count lowers the
    /// bound, so RAM above it comes down to it, in the stepper's steps;
    /// lowering the count raises the bound and leaves RAM as it is.
    mutating func vCPUCountChanged(
        from oldCount: Int,
        bound: GuestMemoryBound = EmulatorConfiguration.systemGuestMemoryBound
    ) {
        guard vCPUCount > oldCount else { return }
        guestMemoryMiB = min(
            guestMemoryMiB,
            maximumGuestMemoryMiB(vCPUCount: vCPUCount, bound: bound)
        )
    }

    static var physicalProcessorCount: Int {
        max(ProcessInfo.processInfo.processorCount, 1)
    }

    /// The most vCPUs the settings offer, the default, and the most
    /// `KERNARIUM_VCPU_COUNT` can state on a device with this many physical
    /// cores: every core, at most the count the memory budget is measured for
    /// (`GuestProcessorLimit`).
    static func maximumVCPUCount(physicalProcessorCount: Int) -> Int {
        GuestProcessorLimit.vCPUs(physicalProcessorCount: physicalProcessorCount)
    }

    static func load(defaults: UserDefaults = .standard) -> Self {
        load(
            defaults: defaults,
            physicalProcessorCount: physicalProcessorCount,
            environment: ProcessInfo.processInfo.environment
        )
    }

    /// `guestMemoryBound` gives the most guest RAM for a vCPU count; a stored
    /// or default size above it for the loaded count is taken down to it. A
    /// size the environment states is kept as stated, for automation. The
    /// vCPU count defaults to the maximum for `physicalProcessorCount`; a
    /// stored count above it comes down to it, and the environment's must lie
    /// within it.
    static func load(
        defaults: UserDefaults,
        physicalProcessorCount: Int,
        environment: [String: String] = ProcessInfo.processInfo.environment,
        documentsDirectory: URL = RootfsImage.documentsDirectory,
        guestMemoryBound: GuestMemoryBound =
            EmulatorConfiguration.systemGuestMemoryBound,
        defaultGuestMemory: DefaultGuestMemory =
            EmulatorConfiguration.systemDefaultGuestMemory
    ) -> Self {
        let maximumVCPUCount = Self.maximumVCPUCount(
            physicalProcessorCount: physicalProcessorCount
        )
        func stored(_ key: String, _ fallback: Int) -> Int {
            defaults.object(forKey: key) == nil
                ? fallback
                : defaults.integer(forKey: key)
        }
        func stored(_ key: String, _ fallback: Bool) -> Bool {
            defaults.object(forKey: key) == nil
                ? fallback
                : defaults.bool(forKey: key)
        }
        let storedVCPUCount = stored(Key.vCPUCount, maximumVCPUCount)
        let storedOverlaySizeGiB = stored(
            Key.overlaySizeGiB, defaultOverlaySizeGiB
        )
        let storedAndroidDiskSizeGiB = stored(
            Key.androidDiskSizeGiB, defaultAndroidDiskSizeGiB
        )
        let storedRootfsFilename = defaults.string(
            forKey: Key.rootfsFilename
        ) ?? RootfsImage.defaultFilename
        let storedRetinaMode = stored(Key.retinaMode, defaultRetinaMode)
        let storedAutoResizeGuest = stored(
            Key.autoResizeGuest, defaultAutoResizeGuest
        )
        let storedAndroidBootMode = AndroidBootMode(
            rawValue: UInt32(clamping: stored(
                Key.androidBootMode, Int(defaultAndroidBootMode.rawValue)
            ))
        ) ?? defaultAndroidBootMode
        let environmentVCPUCount = environmentInteger(
            environment[vCPUEnvironmentKey], in: 1...maximumVCPUCount
        )
        let environmentGuestMemoryMiB = environmentInteger(
            environment[guestMemoryEnvironmentKey], in: guestMemoryRangeMiB
        ).flatMap { value in
            (value - guestMemoryRangeMiB.lowerBound) % guestMemoryStepMiB == 0
                ? value
                : nil
        }
        let environmentOverlaySizeGiB = environmentInteger(
            environment[overlaySizeEnvironmentKey], in: overlaySizeRangeGiB
        )
        let environmentAndroidDiskSizeGiB = environmentInteger(
            environment[androidDiskSizeEnvironmentKey],
            in: androidDiskSizeRangeGiB
        )
        func environmentFlag(_ key: String) -> Bool? {
            switch environment[key] {
            case "0"?: false
            case "1"?: true
            default: nil
            }
        }
        let environmentRetinaMode = environmentFlag(retinaEnvironmentKey)
        let environmentAutoResizeGuest = environmentFlag(
            autoResizeGuestEnvironmentKey
        )
        let environmentAndroidBootMode = environment[
            AndroidBootMode.environmentKey
        ].flatMap { AndroidBootMode(environmentValue: $0) }

        let rootfsFilename = RootfsImage.validatedFilename(
            storedRootfsFilename
        ) ?? RootfsImage.defaultFilename

        let vCPUCount = environmentVCPUCount ?? min(
            max(storedVCPUCount, 1), maximumVCPUCount
        )
        let storedGuestMemoryMiB = stored(
            Key.guestMemoryMiB, defaultGuestMemory(vCPUCount)
        )
        let gpuBudgetApplicationMiB = environmentInteger(
            environment[gpuBudgetApplicationEnvironmentKey],
            in: gpuBudgetParameterRangeMiB
        ) ?? defaultGPUBudgetApplicationMiB
        let gpuBudgetEngineMiB = environmentInteger(
            environment[gpuBudgetEngineEnvironmentKey],
            in: gpuBudgetParameterRangeMiB
        ) ?? defaultGPUBudgetEngineMiB
        let gpuBudgetHeadroomMiB = environmentInteger(
            environment[gpuBudgetHeadroomEnvironmentKey],
            in: gpuBudgetParameterRangeMiB
        ) ?? defaultGPUBudgetHeadroomMiB
        let maximumGuestMemoryMiB = steppedGuestMemoryMiB(
            guestMemoryBound(vCPUCount),
            rounding: .down
        )

        return Self(
            vCPUCount: vCPUCount,
            guestMemoryMiB: environmentGuestMemoryMiB ?? min(
                normalizedGuestMemoryMiB(storedGuestMemoryMiB),
                maximumGuestMemoryMiB
            ),
            overlaySizeGiB: environmentOverlaySizeGiB ?? min(
                max(storedOverlaySizeGiB, overlaySizeRangeGiB.lowerBound),
                overlaySizeRangeGiB.upperBound
            ),
            androidDiskSizeGiB: environmentAndroidDiskSizeGiB ?? min(
                max(
                    storedAndroidDiskSizeGiB,
                    androidDiskSizeRangeGiB.lowerBound
                ),
                androidDiskSizeRangeGiB.upperBound
            ),
            rootfsFilename: rootfsFilename,
            retinaMode: environmentRetinaMode ?? storedRetinaMode,
            autoResizeGuest: environmentAutoResizeGuest ??
                storedAutoResizeGuest,
            androidBootMode: environmentAndroidBootMode ?? storedAndroidBootMode,
            gpuBudgetApplicationMiB: gpuBudgetApplicationMiB,
            gpuBudgetEngineMiB: gpuBudgetEngineMiB,
            gpuBudgetHeadroomMiB: gpuBudgetHeadroomMiB
        )
    }

    func save(defaults: UserDefaults = .standard) {
        defaults.set(vCPUCount, forKey: Key.vCPUCount)
        defaults.set(guestMemoryMiB, forKey: Key.guestMemoryMiB)
        defaults.set(overlaySizeGiB, forKey: Key.overlaySizeGiB)
        defaults.set(androidDiskSizeGiB, forKey: Key.androidDiskSizeGiB)
        defaults.set(
            RootfsImage.validatedFilename(rootfsFilename) ??
                RootfsImage.defaultFilename,
            forKey: Key.rootfsFilename
        )
        defaults.set(retinaMode, forKey: Key.retinaMode)
        defaults.set(autoResizeGuest, forKey: Key.autoResizeGuest)
        defaults.set(androidBootMode.rawValue, forKey: Key.androidBootMode)
    }

    private static func normalizedGuestMemoryMiB(_ value: Int) -> Int {
        steppedGuestMemoryMiB(value, rounding: .nearest)
    }

    /// `value` taken into the stepper's range and onto its steps.
    private static func steppedGuestMemoryMiB(
        _ value: Int,
        rounding: Rounding
    ) -> Int {
        let clamped = min(
            max(value, guestMemoryRangeMiB.lowerBound),
            guestMemoryRangeMiB.upperBound
        )
        let offset = clamped - guestMemoryRangeMiB.lowerBound
        let steps = switch rounding {
        case .nearest: (offset + guestMemoryStepMiB / 2) / guestMemoryStepMiB
        case .down: offset / guestMemoryStepMiB
        }
        return guestMemoryRangeMiB.lowerBound + steps * guestMemoryStepMiB
    }

    private enum Rounding {
        case nearest
        case down
    }

    private static func environmentInteger(
        _ value: String?,
        in range: ClosedRange<Int>
    ) -> Int? {
        guard let value, let parsed = Int(value), range.contains(parsed) else {
            return nil
        }
        return parsed
    }

    private enum Key {
        static let vCPUCount = "emulator.vCPUCount"
        static let guestMemoryMiB = "emulator.guestMemoryMiB"
        static let overlaySizeGiB = "emulator.overlaySizeGiB"
        static let androidDiskSizeGiB = "emulator.androidDiskSizeGiB"
        static let rootfsFilename = "emulator.rootfsFilename"
        static let retinaMode = "emulator.retinaMode"
        static let autoResizeGuest = "emulator.autoResizeGuest"
        static let androidBootMode = "emulator.androidBootMode"
    }
}

/// What the launcher was asked to boot, once the stored name has been resolved
/// against Documents.
enum GuestSelection {
    /// A single root filesystem image and the runtime profile it requires.
    case image(URL, flavor: GuestFlavor)
    /// A self-contained bundle carrying its own payload and writable disk.
    case bundle(GuestBundle)

    var flavor: GuestFlavor {
        switch self {
        case let .image(_, flavor):
            flavor
        case let .bundle(bundle):
            bundle.flavor
        }
    }
}

enum RootfsImage {
    /// The accepted KDE desktop image (tools/rootfs/desktop/images.json),
    /// which carries its own session setup. It applies only while the
    /// settings store no name: a stored name, `rootfs.erofs` included, is kept.
    static let defaultFilename = "kde-20260929-r2.erofs"
    static let environmentKey = "KERNARIUM_ROOTFS"

    nonisolated static var documentsDirectory: URL {
        FileManager.default.urls(
            for: .documentDirectory,
            in: .userDomainMask
        )[0]
    }

    /// The runtime names the image in a UBD kernel argument, which cannot
    /// carry kernel whitespace (including byte 0xa0, which the kernel's
    /// Latin-1 table counts), a double quote, a comma or a colon. Such a name
    /// is not offered, rather than failing to boot (darwin/device/block/
    /// ubd_path.hpp holds the same rule).
    static func namesOnKernelCommandLine(_ filename: String) -> Bool {
        !filename.utf8.contains { byte in
            (0x09...0x0d).contains(byte) || byte == 0x20 || byte == 0xa0 ||
                byte == 0x22 || byte == 0x2c || byte == 0x3a
        }
    }

    static func validatedFilename(_ filename: String) -> String? {
        let pathExtension = (filename as NSString).pathExtension.lowercased()
        guard !filename.isEmpty,
              (filename as NSString).lastPathComponent == filename,
              namesOnKernelCommandLine(filename),
              pathExtension == "erofs" || pathExtension == "ext4" ||
                  pathExtension == GuestBundle.pathExtension
        else {
            return nil
        }
        return filename
    }

    /// Resolves a stored name to what it actually is on disk. A bundle name is
    /// only accepted when its payload is complete, so the picker and the
    /// launcher agree on what can be booted.
    static func selection(
        named filename: String,
        in directory: URL = documentsDirectory,
        fileManager: FileManager = .default
    ) -> GuestSelection? {
        guard let validated = validatedFilename(filename) else { return nil }
        let url = directory.appendingPathComponent(validated)
        if (validated as NSString).pathExtension.lowercased() ==
            GuestBundle.pathExtension {
            return GuestBundle.load(at: url, fileManager: fileManager)
                .map(GuestSelection.bundle)
        }
        guard fileManager.fileExists(atPath: url.path) else { return nil }
        return .image(url, flavor: .linux)
    }

    static func selectedFilename(
        configuredFilename: String,
        environment: [String: String] = ProcessInfo.processInfo.environment
    ) -> String? {
        validatedFilename(environment[environmentKey] ?? configuredFilename)
    }

    static func availableFilenames(
        in directory: URL = documentsDirectory,
        fileManager: FileManager = .default
    ) -> [String] {
        let resourceKeys: [URLResourceKey] = [.isRegularFileKey]
        guard let urls = try? fileManager.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: resourceKeys,
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }
        let images: [String] = urls.compactMap { url -> String? in
            guard let filename = validatedFilename(url.lastPathComponent),
                  let values = try? url.resourceValues(
                      forKeys: Set(resourceKeys)
                  ),
                  values.isRegularFile == true else {
                return nil
            }
            return filename
        }
        let bundles = GuestBundle.availableNames(
            in: directory, fileManager: fileManager
        )
        return (images + bundles).sorted {
            $0.localizedStandardCompare($1) == .orderedAscending
        }
    }
}
