// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import CoreImage
import CoreVideo
import Foundation
import Testing
import UIKit
@testable import Andarium

/// The pieces of the device application that touch the host: the release it
/// carries, the console it reads and the options it keeps.
struct SystemFolderTests {
    private static let manifest = #"{"release_id": "20261031-los24-r4", "android": {"fingerprint": "Andarium/lineage_andarium/andarium:17/a/b:user/release-keys", "build_number": 1793463711, "security_patch": "2026-10-01"}}"#

    private func layout(size: Int) -> String {
        #"{"system_disk": {"size_bytes": \#(size), "sha256": "\#(String(repeating: "a", count: 64))"}, "data_disk": {"layout_version": 1, "capacity_gib": {"minimum": 4, "default": 8, "maximum": 64}}}"#
    }

    /// A release directory with a system disk of `disk` bytes.
    private func folder(disk: Int = 4096, declared: Int = 4096, omit: String? = nil) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("andarium-system-\(UUID().uuidString)")
        let files: [String: Data] = [
            SystemFolder.manifest: Data(Self.manifest.utf8),
            SystemFolder.commandLine: Data("init=/init\n".utf8),
            SystemFolder.layout: Data(layout(size: declared).utf8),
            SystemFolder.systemDisk: Data(count: disk),
        ]
        for (name, data) in files where name != omit {
            let file = url.appendingPathComponent(name)
            try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: file)
        }
        return url
    }

    @Test func theReleaseIsTheDirectoryWithItsSystemDisk() throws {
        let url = try folder()
        defer { try? FileManager.default.removeItem(at: url) }
        let release = try SystemFolder.release(in: url)
        #expect(release.identity.releaseID == "20261031-los24-r4")
        #expect(release.identity.buildNumber == 1_793_463_711)
    }

    @Test func aFileThatIsMissingIsNamed() throws {
        for name in [SystemFolder.manifest, SystemFolder.commandLine, SystemFolder.layout, SystemFolder.systemDisk] {
            let url = try folder(omit: name)
            defer { try? FileManager.default.removeItem(at: url) }
            #expect(throws: SystemFolder.Problem.missing(name)) { try SystemFolder.release(in: url) }
        }
    }

    @Test func aSystemDiskOfAnotherSizeIsRefused() throws {
        let url = try folder(disk: 4096, declared: 8192)
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(throws: SystemFolder.Problem.sizeDiffers(declared: 8192, actual: 4096)) { try SystemFolder.release(in: url) }
    }
}

struct ConsoleSinkTests {
    private nonisolated final class Collector: @unchecked Sendable {
        private let lock = NSLock()
        private var found: [BootMilestone] = []
        private var completions = 0
        var milestones: [BootMilestone] { lock.withLock { found } }
        var bootCompletions: Int { lock.withLock { completions } }
        func add(_ more: [BootMilestone], _ completed: Bool) {
            lock.withLock {
                found += more
                if completed { completions += 1 }
            }
        }
    }

    @Test func milestonesArriveFromAnyThreadInOrder() async {
        let collector = Collector()
        let sink = ConsoleSink { collector.add($0, $1) }
        sink.feed(Data("Linux ver".utf8))
        await Task.detached { sink.feed(Data("sion 6.18\ninit: init first stage started!\n".utf8)) }.value
        #expect(collector.milestones == [.kernelRunning, .androidStarting])
        #expect(collector.bootCompletions == 0)
    }

    @Test func onlyAndroidBootingIsABootCompletion() {
        let collector = Collector()
        let sink = ConsoleSink { collector.add($0, $1) }
        sink.feed(Data("init: starting service 'recovery'...\n".utf8))
        #expect(collector.milestones == [.ready] && collector.bootCompletions == 0)
        sink.feed(Data("init: processing action (sys.boot_completed=1) from (/x:1)\n".utf8))
        #expect(collector.bootCompletions == 1)
    }
}

struct DisplayOptionsStoreTests {
    @Test func optionsSurviveTheStore() {
        let saved = DisplayOptionsStore.load()
        defer { DisplayOptionsStore.save(saved) }
        for options in [DisplayOptions(chosenRetina: true), DisplayOptions(chosenRetina: false), DisplayOptions()] {
            DisplayOptionsStore.save(options)
            #expect(DisplayOptionsStore.load() == options)
        }
    }

    @Test func theMemoryLimitIsReadOrNone() {
        // Zero where the system sets none (a Mac); otherwise the footprint
        // plus what it may still grow by, which is above the footprint.
        let limit = MemoryLimit.bytes()
        #expect(limit == 0 || limit > 64 << 20)
        #expect(limit < 1 << 40)
    }
}

struct HostDeviceTests {
    @Test func theHostReadsThisDevice() {
        let host = AppEntry.hostDevice()
        #expect(host.processorCount >= 1)
        // A guest has every processor, at most the eight the memory budget is
        // measured for.
        let physical = ProcessInfo.processInfo.activeProcessorCount
        #expect(host.processorCount <= 8)
        #expect(host.processorCount == min(physical, 8))
        #expect(host.processorCount == GuestProcessorLimit.vCPUs(physicalProcessorCount: physical))
        // The bound comes from the runtime's budget formula through the loader:
        // inside the runtime's range, never rising with the vCPU count, and the
        // largest size where the system sets no limit (a Mac).
        let bounds = (1...host.processorCount).map { host.memory.maximumMiB(vCPUs: $0) }
        #expect(bounds.allSatisfy { Limits.runtimeMemoryRangeMiB.contains($0) })
        #expect(bounds == bounds.sorted(by: >))
        if ProcessInfo.processInfo.isiOSAppOnMac {
            #expect(bounds.allSatisfy { $0 == Limits.runtimeMemoryRangeMiB.upperBound })
        }
        // A new guest takes every vCPU the device offers and the default
        // memory of that count, which is the loader's answer for the
        // conservative launch set and the one the host GPU's budget uses.
        let fresh = Guest.fresh(on: host)
        #expect(fresh.cores == host.processorCount)
        #expect(fresh.memoryMiB == host.memory.defaultMemoryMiB(vCPUs: host.processorCount))
        #expect(fresh.memoryMiB <= host.memory.maximumMiB(vCPUs: host.processorCount))
        let launch = kernarium_runtime_loader_launch_memory_set()
        let maximumSet = kernarium_runtime_loader_maximum_memory_set()
        for count in 1...host.processorCount {
            #expect(host.memory.defaultMemoryMiB(vCPUs: count)
                == Int(kernarium_runtime_loader_default_guest_memory_mib(UInt32(count), launch)))
            #expect(host.memory.safeMemoryMiB(vCPUs: count)
                == Int(kernarium_runtime_loader_conservative_guest_memory_mib(UInt32(count), launch)))
            #expect(host.memory.defaultMemoryMiB(vCPUs: count) <= host.memory.safeMemoryMiB(vCPUs: count))
            // The estimate is the loader's, for the guest's size and count.
            for mib in [1536, host.memory.defaultMemoryMiB(vCPUs: count), host.memory.maximumMiB(vCPUs: count)] {
                #expect(host.memory.graphicsMemoryMiB(forMemory: mib, vCPUs: count)
                    == Int(kernarium_runtime_loader_gpu_budget_mib(UInt32(mib), UInt32(count), launch)))
            }
            #expect(host.memory.maximumMiB(vCPUs: count)
                == Int(kernarium_runtime_loader_maximum_guest_memory_mib(UInt32(count), maximumSet)))
            #expect(host.memory.defaultMemoryMiB(vCPUs: count) <= host.memory.maximumMiB(vCPUs: count))
            #expect(Limits.defaultMemoryRangeMiB.contains(host.memory.defaultMemoryMiB(vCPUs: count))
                || host.memory.defaultMemoryMiB(vCPUs: count) == host.memory.maximumMiB(vCPUs: count))
            // A new guest is not one the application warns about; one with the
            // most the picker offers is, wherever the picker offers more.
            #expect(!host.memory.isAboveDefault(host.memory.defaultMemoryMiB(vCPUs: count), vCPUs: count))
            #expect(host.memory.isAboveDefault(host.memory.maximumMiB(vCPUs: count), vCPUs: count)
                == (host.memory.maximumMiB(vCPUs: count) > host.memory.defaultMemoryMiB(vCPUs: count)))
            #expect(host.memory.isAboveSafe(host.memory.maximumMiB(vCPUs: count), vCPUs: count)
                == (host.memory.maximumMiB(vCPUs: count) > host.memory.safeMemoryMiB(vCPUs: count)))
        }
        if ProcessInfo.processInfo.isiOSAppOnMac {
            // A Mac sets no limit: the whole range, and nothing to warn about.
            #expect((1...host.processorCount).allSatisfy {
                host.memory.defaultMemoryMiB(vCPUs: $0) == Limits.runtimeMemoryRangeMiB.upperBound
                    && !host.memory.isAboveDefault(host.memory.maximumMiB(vCPUs: $0), vCPUs: $0)
                    && !host.memory.isAboveSafe(host.memory.maximumMiB(vCPUs: $0), vCPUs: $0)
            })
        }
        #expect(AppEntry.freeStorageBytes() > 0)
    }
}

/// The unit fixtures of the bounds and defaults are the shared budget rule's
/// own answers; this asks the loader, which holds that rule, for the same
/// rows, and for the vCPU count a device with a given number of cores offers.
struct DefaultGuestTests {
    @Test func theFixturesAreTheLoadersBoundsAndTheCoreLimitIsEightAtMost() {
        let launch = kernarium_runtime_loader_launch_memory_set()
        let maximumSet = kernarium_runtime_loader_maximum_memory_set()
        for (cores, expected) in [(1, 1), (4, 4), (6, 6), (8, 8), (10, 8), (12, 8), (20, 8)] {
            #expect(GuestProcessorLimit.vCPUs(physicalProcessorCount: cores) == expected)
        }
        for (limitMiB, bounds, defaults, safe) in [
            (1536, [960, 960, 896, 896, 896, 896, 896, 896], [960, 960, 896, 896, 896, 896, 896, 896],
             [960, 960, 896, 896, 896, 896, 896, 896]),
            (2048, [1472, 1472, 1408, 1408, 1344, 1344, 1344, 1344], [1472, 1472, 1408, 1408, 1344, 1344, 1344, 1344],
             [1472, 1472, 1408, 1408, 1344, 1344, 1344, 1344]),
            (2304, [1728, 1728, 1664, 1664, 1600, 1600, 1600, 1600], Array(repeating: 1536, count: 8),
             Array(repeating: 1536, count: 8)),
            (2867, [2304, 2304, 2240, 2240, 2176, 2176, 2176, 2176], Array(repeating: 1536, count: 8),
             Array(repeating: 1536, count: 8)),
            (3584, [3008, 3008, 2944, 2944, 2880, 2880, 2880, 2880], Array(repeating: 2048, count: 8),
             Array(repeating: 2048, count: 8)),
            (4096, [3520, 3520, 3456, 3456, 3392, 3392, 3392, 3392], Array(repeating: 2560, count: 8),
             Array(repeating: 2560, count: 8)),
            (4700, [4096, 4096, 4096, 4032, 4032, 4032, 3968, 3968], Array(repeating: 2560, count: 8),
             [3584, 3584, 3072, 3072, 3072, 3072, 3072, 3072]),
            (5120, Array(repeating: 4096, count: 8), Array(repeating: 3072, count: 8),
             Array(repeating: 3584, count: 8)),
            (6144, Array(repeating: 4096, count: 8), Array(repeating: 4096, count: 8),
             Array(repeating: 4096, count: 8)),
            (0, Array(repeating: 4096, count: 8), Array(repeating: 4096, count: 8), Array(repeating: 4096, count: 8)),
        ] as [(Int, [Int], [Int], [Int])] {
            let policy = MemoryPolicy(
                processorCount: 8,
                bound: { count in
                    Int(kernarium_runtime_loader_maximum_guest_memory_mib_for_limit(
                        UInt64(limitMiB) << 20, UInt32(count), maximumSet))
                },
                defaultMemory: { count in
                    Int(kernarium_runtime_loader_default_guest_memory_mib_for_limit(
                        UInt64(limitMiB) << 20, UInt32(count), launch))
                },
                safeMemory: { count in
                    Int(kernarium_runtime_loader_conservative_guest_memory_mib_for_limit(
                        UInt64(limitMiB) << 20, UInt32(count), launch))
                },
                graphicsMemory: { memoryMiB, count in
                    Int(kernarium_runtime_loader_gpu_budget_mib_for_limit(
                        UInt64(limitMiB) << 20, UInt32(memoryMiB), UInt32(count), launch))
                }
            )
            #expect((1...8).map { policy.maximumMiB(vCPUs: $0) } == bounds, "limit \(limitMiB) MiB")
            #expect((1...8).map { policy.defaultMemoryMiB(vCPUs: $0) } == defaults, "limit \(limitMiB) MiB")
            #expect((1...8).map { policy.safeMemoryMiB(vCPUs: $0) } == safe, "limit \(limitMiB) MiB")
            // A limit sets the budget a guest of the default size launches with: the
            // loader's evaluation, half the guest with no limit, never under 384 MiB
            // otherwise unless half the guest is less.
            for count in 1...8 {
                let memory = policy.defaultMemoryMiB(vCPUs: count)
                let budget = policy.graphicsMemoryMiB(forMemory: memory, vCPUs: count)
                #expect(budget == Int(kernarium_runtime_loader_gpu_budget_mib_for_limit(
                    UInt64(limitMiB) << 20, UInt32(memory), UInt32(count), launch)))
                if limitMiB == 0 {
                    #expect(budget == memory / 2)
                } else if bounds[0] == Limits.runtimeMemoryRangeMiB.upperBound {
                    // On a device that offers 4 GiB, a new guest keeps a gibibyte for the GPU.
                    #expect((budget ?? 0) >= 1024, "limit \(limitMiB) MiB at \(count) vCPUs")
                }
            }
        }
        // The loader's launch set is the conservative one; the maximum counts none.
        #expect(launch.application_mib == 64 && launch.engine_mib == 320 && launch.headroom_mib == 149)
        #expect(maximumSet.application_mib == 0 && maximumSet.engine_mib == 0 && maximumSet.headroom_mib == 0)
    }

    /// The app's constant for the range a default is held within is the
    /// rule's: an iPad mini 6 sits on its bottom and a limit with room on its top.
    @Test func theDefaultRangeIsTheRulesRange() {
        let launch = kernarium_runtime_loader_launch_memory_set()
        func defaultMiB(limitMiB: UInt64) -> Int {
            Int(kernarium_runtime_loader_default_guest_memory_mib_for_limit(limitMiB << 20, 6, launch))
        }
        #expect(defaultMiB(limitMiB: 2867) == Limits.defaultMemoryRangeMiB.lowerBound)
        #expect(defaultMiB(limitMiB: 0) == Limits.defaultMemoryRangeMiB.upperBound)
        #expect(defaultMiB(limitMiB: 1 << 20) == Limits.defaultMemoryRangeMiB.upperBound)
    }
}

/// A late layout cannot retain a retired display while Home is showing.
@MainActor
struct RuntimeDisplayLifetimeTests {
    @Test func homeDoesNotRetainALayoutFromTheRetiredDisplay() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let release = try SystemFolder.release(in: systemDirectory)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory), release: release.identity, releaseDirectory: systemDirectory
        )
        let model = AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: storage.createRequirementBytes
        )
        let runtime = LiveRuntime(model: model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")))
        weak var display: GuestMetalView?
        autoreleasepool {
            let view = GuestMetalView(frame: .zero, device: nil)
            display = view
        }
        #expect(display == nil)
        autoreleasepool {
            let view = GuestMetalView(frame: .zero, device: nil)
            display = view
            runtime.viewDidLayout(view, GuestDisplayGeometry(pixelSize: CGSize(width: 100, height: 100), dotsPerInch: 160))
        }
        #expect(display == nil, "A late layout must not retain a display while the guest is off")
    }
}

private final class GuestFrameCapture: @unchecked Sendable {
    private let lock = NSLock()
    private var frame: CVPixelBuffer?

    func receive(_ buffer: CVPixelBuffer) {
        lock.withLock { if frame == nil { frame = buffer } }
    }

    var image: CVPixelBuffer? { lock.withLock { frame } }
}

/// Opt-in physical display check. Saves the guest's rendered frame with each
/// KERNARIUM_RETINA_MODE value so their displayed text size can be compared.
@MainActor
struct RecoveryDisplayTests {
    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_RECOVERY_TESTS"] == "1"), .timeLimit(.minutes(5)))
    func recoveryRendersAndItsMenuPowersOff() async throws {
        let environment = ProcessInfo.processInfo.environment
        try #require(environment["ANDARIUM_GUEST_INSTANCE"] == "recovery-display")
        try #require(environment["ANDARIUM_START"] == nil)
        let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let model = try #require((scene.delegate as? AndariumSceneDelegate)?.model)
        if model.guest == nil {
            let entry = AppEntry.fromArguments()
            model.create(entry.overrides.applied(to: Guest.fresh(on: model.host), on: model.host))
            let deadline = ContinuousClock.now + .seconds(30)
            while model.busy != nil, ContinuousClock.now < deadline {
                try await Task.sleep(for: .milliseconds(100))
            }
        }
        try #require(model.guest != nil && model.busy == nil && model.alert == nil)
        model.start(.recovery)
        let readyDeadline = ContinuousClock.now + .seconds(120)
        while model.phase != .running(.recovery), ContinuousClock.now < readyDeadline {
            if case .failed = model.phase { break }
            try await Task.sleep(for: .milliseconds(200))
        }
        try #require(model.phase == .running(.recovery), "\(model.runReport)")
        try await Task.sleep(for: .seconds(10))
        // Check the guest property boundary too: a valid launch DPI alone
        // does not mean Recovery can read it under its SELinux policy.
        do {
            let identity = try AppEntry.fromArguments().adbKeyStore.loadOrCreate()
            let operations = AdbGuestOperations(identity: identity, clock: GuestRunClock(), device: "device")
            let density = try await operations.shell("getprop ro.sf.lcd_density", limit: .seconds(5))
            let scale = model.retina ? scene.screen.nativeScale : 1
            #expect(density.text.trimmingCharacters(in: .whitespacesAndNewlines) == String(Int((160 * scale).rounded())))
        } catch {
            Issue.record("Recovery display density could not be read: \(error)")
        }
        do {
            try await captureGuestFrame(named: model.retina ? "recovery-retina.png" : "recovery-points.png")
        } catch {
            Issue.record("Recovery frame could not be captured: \(error)")
        }
        print("RECOVERY_DISPLAY: ready retina=\(model.retina); capture the screen within 30 seconds")
        try await Task.sleep(for: .seconds(30))
        // End selects Advanced, then End selects Power off. Use Recovery's
        // own menu so the run must report a clean guest power-off.
        for code in [UInt16(107), 28, 107, 28] {
            kernarium_runtime_loader_send_key(code, 1)
            try await Task.sleep(for: .milliseconds(100))
            kernarium_runtime_loader_send_key(code, 0)
            try await Task.sleep(for: .seconds(1))
        }
        let offDeadline = ContinuousClock.now + .seconds(60)
        while model.phase != .home, ContinuousClock.now < offDeadline {
            try await Task.sleep(for: .milliseconds(200))
        }
        try #require(model.phase == .home && model.runEnd.poweredOff, "\(model.runReport)")
    }
}

/// Saves the guest's display as a PNG through the runtime's frame mirror. An
/// App Store build has neither the mirror nor this.
@MainActor
func captureGuestFrame(named name: String) async throws {
    #if !APP_STORE
    let capture = GuestFrameCapture()
    let result = kernarium_runtime_loader_set_frame_sink({ context, buffer in
        guard let context, let buffer else { return }
        let capture = Unmanaged<GuestFrameCapture>.fromOpaque(context).takeUnretainedValue()
        capture.receive(Unmanaged<CVPixelBuffer>.fromOpaque(buffer).takeUnretainedValue())
    }, Unmanaged.passUnretained(capture).toOpaque(), 4096, 1)
    defer {
        _ = kernarium_runtime_loader_set_frame_sink(nil, nil, 0, 0)
        withExtendedLifetime(capture) {}
    }
    try #require(result == 0)
    let deadline = ContinuousClock.now + .seconds(5)
    while capture.image == nil, ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(100))
    }
    let buffer = try #require(capture.image)
    let image = CIImage(cvPixelBuffer: buffer)
    let rendered = try #require(CIContext().createCGImage(image, from: image.extent))
    let png = try #require(UIImage(cgImage: rendered).pngData())
    try png.write(to: FileManager.default.temporaryDirectory.appendingPathComponent(name))
    print("GUEST_FRAME: saved tmp/\(name)")
    #endif
}

/// The system Andarium carries, written the way Create writes it: decoded
/// sparse from the bundled archive and grown to the guest's size. It is the
/// one test that reads the real archive, so it takes tens of seconds.
/// A harness launch on a Mac starts Andarium through `xcodebuild test`, which
/// ends the application with the test run. An automation launch that sets
/// `ANDARIUM_HOLD_SECONDS` keeps the host up that long, so its guest can boot
/// and run; without it the test returns at once.
struct AutomationHoldTests {
    @Test func anAutomationLaunchHoldsTheHostForTheRequestedTime() async throws {
        guard let text = ProcessInfo.processInfo.environment["ANDARIUM_HOLD_SECONDS"], let seconds = Double(text), seconds > 0 else { return }
        try await Task.sleep(for: .seconds(seconds))
    }
}
