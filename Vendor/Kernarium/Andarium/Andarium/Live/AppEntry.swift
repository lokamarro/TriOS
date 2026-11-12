// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Metal
import os
import TipKit
import UIKit

/// The device application's entry: the guest on disk, this device and the
/// system Andarium carries, and what a harness launch asks for in the
/// environment (`Andarium/README.md#automation`).
struct AppEntry {
    var overrides: LaunchOverrides

    /// The live application opens no screen directly.
    var hints: ScreenHints { ScreenHints() }

    static func fromArguments() -> AppEntry {
        #if APP_STORE
        // An App Store build reads no launch setting.
        return AppEntry(overrides: LaunchOverrides())
        #else
        let info = ProcessInfo.processInfo
        return AppEntry(
            overrides: LaunchOverrides(
                environment: info.environment,
                processorCount: GuestProcessorLimit.vCPUs(physicalProcessorCount: info.activeProcessorCount)
            )
        )
        #endif
    }

    /// The adb key of the guest this launch runs.
    var adbKeyStore: AdbKeyStore {
        #if APP_STORE
        return AdbKeyStore()
        #else
        return AdbKeyStore(instance: overrides.instance)
        #endif
    }

    func configureTips() {
        try? Tips.configure([.displayFrequency(.immediate)])
    }

    /// The model for this launch. A copy of Andarium without its system, or
    /// with one it cannot read, still opens, and says so.
    @MainActor
    /// `freeSpace` is the device's free space a run watches: the application's
    /// own reading unless a test supplies another.
    func makeModel(freeSpace: @escaping @Sendable () -> UInt64 = { AppEntry.freeStorageBytes() }) -> AppModel {
        let launch = overrides
        let bundle = Bundle.main
        let version = Self.appVersion(bundle)
        let systemFolder = bundle.resourceURL?.appendingPathComponent(SystemFolder.name)
        let host = Self.hostDevice()

        var problem: String?
        var system = SystemInfo(
            version: "", release: "unknown", securityPatch: nil,
            kernel: "", architecture: "", appVersion: version
        )
        var carried: SystemFolder.Release?
        if let systemFolder {
            do {
                let manifest = try Data(contentsOf: systemFolder.appendingPathComponent(SystemFolder.manifest))
                system = try SystemInfo(manifest: manifest, appVersion: version)
                carried = try SystemFolder.release(in: systemFolder)
            } catch {
                problem = "This copy of Andarium does not carry a usable system: \(error.localizedDescription)"
            }
        } else {
            problem = "This copy of Andarium does not carry a system."
        }

        let locations = GuestLocations(directory: Self.guestDirectory(instance: launch.instance))
        let storage = TwoDiskStorage(
            locations: locations,
            release: carried?.identity ?? ReleaseIdentity(
                releaseID: "unavailable", buildFingerprint: "unavailable", buildNumber: 1, securityPatch: "",
                systemDiskSHA256: String(repeating: "0", count: 64), dataLayout: 1, minimumGiB: 4, defaultGiB: 8,
                maximumGiB: 64
            ),
            releaseDirectory: systemFolder ?? locations.directory
        )
        var guest: Guest?
        do {
            guest = try storage.load()
        } catch {
            problem = problem ?? "The guest\u{2019}s settings could not be read: \(error.localizedDescription)"
        }
        // A stored memory size above the bound of its vCPU count comes down to
        // it; a size the launch states stays as stated.
        guest = guest.map {
            launch.applied(to: $0, on: host, capacityMaximumGiB: storage.release.maximumGiB)
        }

        let model = AppModel(
            guest: guest, host: host, system: system, capabilities: .device,
            createRequirementBytes: storage.createRequirementBytes
        )
        // The rulers offer no size the carried system's data disk would refuse.
        model.releaseMaximumGiB = storage.release.maximumGiB
        // Retina mode is the person's choice where they made one, else on when
        // the system lets this application use more than 4 GiB.
        let options = DisplayOptionsStore.load()
        model.retina = launch.retina ?? options.retina(memoryLimitBytes: MemoryLimit.bytes())
        model.retinaChosen = options.chosenRetina != nil
        // A value a launch states is that run's, and what the person chose
        // stays stored.
        model.retinaStatedByLaunch = launch.retina != nil
        // The statistics show from the start where the person left them,
        // unless the launch is a harness's, which decides for its run alone
        // (`StatsPreference.launch`). An App Store build carries no setting.
        let stats = StatsPreference().launch(for: launch)
        model.session.showStats = stats.showOnScreen
        model.statsDecidedByLaunch = !stats.keepsChanges
        #if !APP_STORE
        model.printsStatistics = launch.statistics == true
        #endif

        #if APP_STORE
        let markerDirectory = Self.supportDirectory()
        #else
        let markerDirectory = launch.instance == nil ? Self.supportDirectory() : locations.directory
        #endif
        let marker = PauseMarker(url: markerDirectory.appendingPathComponent("pause.json"))
        let closedInBackground = marker.consume()
        if closedInBackground, guest != nil {
            model.notice = .closedInBackground
            print("Andarium: the previous process ended out of the foreground; the guest starts again from boot")
        }
        // The session records and MetricKit's reports live beside the marker.
        let sessionRecord = markerDirectory.appendingPathComponent(BugReportAttachments.sessionRecord)
        model.adoptDiagnostics(
            in: markerDirectory, containers: Self.containerPaths(), endedOutOfForeground: closedInBackground)
        ExitReports.shared.start(keepingIn: markerDirectory)
        if carried != nil {
            #if APP_STORE
            let watched = freeSpace
            #else
            // A harness launch of an isolated instance may state the free space.
            let watched = launch.instance == nil
                ? freeSpace : Self.harnessFreeSpace(in: locations.directory, fallback: freeSpace)
            if launch.instance != nil {
                Self.answerSpaceScreenFromHarness(model: model, directory: locations.directory)
            }
            #endif
            let runtime = LiveRuntime(
                model: model, storage: storage, marker: marker, keyStore: adbKeyStore,
                sessionRecord: sessionRecord, freeSpace: watched)
            model.driver = runtime
            model.refreshPlan()
            // Access the person took away while Andarium was closed takes the
            // Microphone choice with it.
            model.microphone?.refresh()
        }
        model.alert = problem.map { AppAlert(title: "Andarium is incomplete", message: $0) }
        return model
    }

    #if !APP_STORE
    /// An automation launch creates the guest if there is none, erases it
    /// when asked, and starts it; a normal launch stops at Home.
    @MainActor
    func applyDeferred(to model: AppModel) async {
        let launch = overrides
        guard launch.start != nil || launch.erase else { return }
        if model.guest == nil {
            model.create(Self.launchGuest(launch, on: model))
            guard await Self.idle(model) else { return }
        }
        if launch.erase {
            model.erase()
            guard await Self.idle(model) else { return }
            // Erasing leaves no guest; a launch that starts one sets up a new
            // guest first.
            if launch.start != nil, model.guest == nil {
                model.create(Self.launchGuest(launch, on: model))
                guard await Self.idle(model) else { return }
            }
        }
        if let mode = launch.start, model.alert == nil {
            model.start(mode)
        }
    }

    /// A new guest as the launch asks for it, within the carried system's data
    /// disk.
    @MainActor
    private static func launchGuest(_ launch: LaunchOverrides, on model: AppModel) -> Guest {
        launch.applied(
            to: Guest.fresh(on: model.host), on: model.host, capacityMaximumGiB: model.releaseMaximumGiB
        )
    }

    /// Waits for the busy overlay to go, within a bound; false if it did not.
    @MainActor
    private static func idle(_ model: AppModel) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(900)
        while model.busy != nil {
            if ContinuousClock.now > deadline { return false }
            try? await Task.sleep(for: .milliseconds(200))
        }
        return model.alert == nil
    }
    #endif

    // MARK: Device

    private static func appVersion(_ bundle: Bundle) -> String {
        let short = bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0"
        let build = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "1"
        return "\(short) (\(build))"
    }

    /// `Library/Guest/`. The disk's path goes to Linux in a kernel argument
    /// that cannot carry a space, so the guest does not live in `Application
    /// Support`; the library directory's name is safe on a device and on a Mac.
    /// A harness launch's isolated `instance` lives beside it; an App Store
    /// build has only the one guest.
    static func guestDirectory(instance: String? = nil) -> URL {
        let library = (try? FileManager.default.url(
            for: .libraryDirectory, in: .userDomainMask, appropriateFor: nil, create: true
        )) ?? FileManager.default.temporaryDirectory
        #if !APP_STORE
        if let instance {
            return library.appendingPathComponent("TestGuests", isDirectory: true)
                .appendingPathComponent(instance, isDirectory: true)
        }
        #endif
        return library.appendingPathComponent("Guest", isDirectory: true)
    }

    /// `Library/Application Support/`, for the application's own small files.
    /// The paths that name this install of Andarium, which leave the device
    /// in no bug report.
    static func containerPaths() -> [String] {
        [
            NSHomeDirectory(),
            Bundle.main.bundleURL.deletingLastPathComponent().path,
            FileManager.default.temporaryDirectory.path,
        ]
    }

    static func supportDirectory() -> URL {
        (try? FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true
        )) ?? FileManager.default.temporaryDirectory
    }

    @MainActor
    static func hostDevice() -> HostDevice {
        let info = ProcessInfo.processInfo
        let name: String
        if info.isiOSAppOnMac {
            name = "Mac"
        } else {
            name = UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone"
        }
        // A guest has every processor, at most the count the memory budget is
        // measured for; the memory policy has a row for each of those counts.
        let limit = GuestProcessorLimit.vCPUs(physicalProcessorCount: info.activeProcessorCount)
        return HostDevice(
            name: name, processorCount: limit,
            memory: memoryPolicy(processorCount: limit),
            freeStorageBytes: freeStorageBytes(),
            runsGuestGraphics: drawsGuestGraphics()
        )
    }

    /// The guest is drawn with Metal 4 (the A14 and later). The Simulator
    /// draws it with the CPU and has no Metal 4 device, so it always can.
    static func drawsGuestGraphics() -> Bool {
        #if targetEnvironment(simulator)
        return true
        #else
        return MTLCreateSystemDefaultDevice()?.supportsFamily(.metal4) ?? false
        #endif
    }

    /// The most guest RAM at each vCPU count, the default a new guest gets, the
    /// size above which iOS is likely to end the application and the host
    /// GPU's budget a guest would launch with: the runtime's own budget
    /// formula, answered by the loader as it is for Kernarium, against the
    /// memory limit the system sets this process (none on a Mac). The maximum
    /// counts none of the launch allowances; the others count the conservative
    /// launch set the host GPU's budget uses too.
    static func memoryPolicy(processorCount: Int) -> MemoryPolicy {
        let maximumSet = kernarium_runtime_loader_maximum_memory_set()
        let launchSet = kernarium_runtime_loader_launch_memory_set()
        return MemoryPolicy(
            processorCount: processorCount,
            bound: { count in
                Int(kernarium_runtime_loader_maximum_guest_memory_mib(UInt32(count), maximumSet))
            },
            defaultMemory: { count in
                Int(kernarium_runtime_loader_default_guest_memory_mib(UInt32(count), launchSet))
            },
            safeMemory: { count in
                Int(kernarium_runtime_loader_conservative_guest_memory_mib(UInt32(count), launchSet))
            },
            graphicsMemory: { memoryMiB, count in
                Int(kernarium_runtime_loader_gpu_budget_mib(UInt32(memoryMiB), UInt32(count), launchSet))
            }
        )
    }

    static func freeStorageBytes() -> UInt64 {
        HostStorage.freeBytes()
    }
}

/// What the system holds this application's memory to: the footprint plus what
/// the system lets it grow by, which is the limit iOS enforces (jetsam) and
/// 2,867 MiB on an iPad mini 6. Zero where the system sets none (a Mac, or an
/// iOS application on one). The same figure as `uml::platform::
/// os_memory_limit_bytes` (`darwin/platform/apple/os_memory_limit.hpp`), which
/// the runtime holds the guest's memory to; this application does not link
/// that header.
enum MemoryLimit {
    static func bytes() -> UInt64 {
        var info = task_vm_info_data_t()
        let words = MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size
        var count = mach_msg_type_number_t(words)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: words) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        guard result == KERN_SUCCESS, Int(count) >= words, info.limit_bytes_remaining != 0 else { return 0 }
        return info.phys_footprint + info.limit_bytes_remaining
    }
}

/// The display option, kept across launches.
enum DisplayOptionsStore {
    private static let key = DisplayOptions.storageKey

    static func load(from defaults: UserDefaults = .standard) -> DisplayOptions {
        guard let data = defaults.data(forKey: key),
              let options = try? JSONDecoder().decode(DisplayOptions.self, from: data)
        else { return DisplayOptions() }
        return options
    }

    static func save(_ options: DisplayOptions, to defaults: UserDefaults = .standard) {
        defaults.set(try? JSONEncoder().encode(options), forKey: key)
    }
}

/// The release Andarium carries: the staged directory `andarium-system` in
/// the application bundle (`Andarium/scripts/stage-system-image.py`), laid out
/// as an installed Android release: `release-manifest.json`,
/// `boot/kernel-command-line.txt`, `guest/system.img` (the read-only system
/// disk) and `guest/partition-layout.json`. The runtime reads the directory
/// itself; the application reads the release's identity.
enum SystemFolder {
    static let name = "andarium-system"
    static let manifest = "release-manifest.json"
    static let commandLine = "boot/kernel-command-line.txt"
    static let layout = "guest/partition-layout.json"
    static let systemDisk = "guest/system.img"

    enum Problem: LocalizedError, Equatable {
        case missing(String)
        case sizeDiffers(declared: UInt64, actual: UInt64)

        var errorDescription: String? {
            switch self {
            case .missing(let file): "\(file) is missing"
            case .sizeDiffers(let declared, let actual):
                "the system disk is \(actual) bytes, not the \(declared) its layout declares"
            }
        }
    }

    struct Release {
        var identity: ReleaseIdentity
    }

    static func release(in folder: URL) throws -> Release {
        let manager = FileManager.default
        for file in [manifest, commandLine, layout, systemDisk]
        where !manager.fileExists(atPath: folder.appendingPathComponent(file).path) {
            throw Problem.missing(file)
        }
        let layoutData = try Data(contentsOf: folder.appendingPathComponent(layout))
        let identity = try ReleaseIdentity(
            manifest: Data(contentsOf: folder.appendingPathComponent(manifest)), layout: layoutData
        )
        guard let object = try JSONSerialization.jsonObject(with: layoutData) as? [String: Any],
              let system = object["system_disk"] as? [String: Any],
              let declared = (system["size_bytes"] as? NSNumber)?.uint64Value, declared > 0
        else { throw Problem.missing("the system disk's size in \(layout)") }
        let actual = UInt64((try? folder.appendingPathComponent(systemDisk).resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        guard actual == declared else { throw Problem.sizeDiffers(declared: declared, actual: actual) }
        return Release(identity: identity)
    }
}
