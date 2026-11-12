// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Real guest sessions through the production Home and boot views. Runs alone
/// with ANDARIUM_SHUTDOWN_TESTS=1 and ANDARIUM_GUEST_INSTANCE=shutdown-flow,
/// without ANDARIUM_START. The ordinary guest directory is never opened.
///
/// `cleanPowerOffReturnsHomeAndAnUncleanEndRequiresRelaunch`: two clean
/// power-offs each return Home, three boots reuse the app process, and a
/// host-stopped third run stays failed until relaunch. Before the first start
/// Home offers no recovery start and no storage in use; once the first run
/// has returned Home it offers it and states the storage the data disk
/// holds. It deletes only that isolated test guest. ANDARIUM_END_HOLD_SECONDS
/// keeps the third run's End screen up for that long, for a look at it.
///
/// `cleanShutdownMemorySnapshots` (ANDARIUM_MEMORY_SNAPSHOTS=1) runs five clean
/// boot/power-off cycles. It saves allocation totals, thread names and
/// VM-region metadata (no memory contents) to the app's `tmp/ShutdownMemory/`
/// before boot, while running, and immediately and ten seconds after each
/// return Home. Each settled snapshot requires zero wired memory and zero
/// dirty or swapped pages in the guest-domain and scratch heaps, and the
/// retired guest display must also deallocate. The initial thirty-second pause
/// allows Instruments Allocations and VM Tracker to attach.
@MainActor
struct ShutdownFlowTests {
    @Test(.timeLimit(.minutes(20)))
    func cleanPowerOffReturnsHomeAndAnUncleanEndRequiresRelaunch() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_SHUTDOWN_TESTS"] == "1" else { return }
        try await exercise(boots: 3, memorySnapshots: false)
    }

    @Test(.timeLimit(.minutes(20)))
    func cleanShutdownMemorySnapshots() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_MEMORY_SNAPSHOTS"] == "1" else { return }
        try await exercise(boots: 5, memorySnapshots: true)
    }

    private func exercise(boots: Int, memorySnapshots: Bool) async throws {
        #expect(ProcessInfo.processInfo.environment["ANDARIUM_START"] == nil)
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "shutdown-flow")
        let directory = AppEntry.guestDirectory(instance: "shutdown-flow")
        if FileManager.default.fileExists(atPath: directory.path) {
            try FileManager.default.removeItem(at: directory)
        }
        defer { try? FileManager.default.removeItem(at: directory) }
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        try #require(model.alert == nil, "\(String(describing: model.alert))")
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let window = lease.window
        window.rootViewController = UIHostingController(rootView: RootView(entry: entry).environment(model))
        model.create(Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8))
        let createDeadline = ContinuousClock.now + .seconds(30)
        while model.busy != nil && ContinuousClock.now < createDeadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        try #require(model.guest != nil && model.busy == nil && model.alert == nil)

        // The guest has not started: no data disk, so Start in recovery waits
        // and nothing is in use.
        #expect(model.plan == .firstStart && !model.plan.offersRecovery)
        #expect(model.guest?.usedBytes == 0)

        let identity = try entry.adbKeyStore.loadOrCreate()
        let operations = AdbGuestOperations(identity: identity, clock: GuestRunClock(), device: "device")
        weak var display: GuestMetalView?
        if memorySnapshots {
            // Allow Instruments to attach before the first boot.
            print("ANDROID_MEMORY: ready for Instruments")
            try await Task.sleep(for: .seconds(30))
            try memorySnapshot("before-boot")
        }
        for boot in 1...boots {
            model.start(.normal)
            let bootDeadline = ContinuousClock.now + .seconds(5 * 60)
            var ready = false
            while ContinuousClock.now < bootDeadline {
                if case .failed = model.phase { break }
                if let result = try? await operations.shell("getprop sys.boot_completed", limit: .seconds(10)),
                    result.text.hasPrefix("1"), model.phase == .running(.normal) {
                    ready = true
                    break
                }
                try await Task.sleep(for: .seconds(2))
            }
            try #require(ready, "Boot \(boot) did not reach Android: \(model.runReport)")
            display = guestDisplay(in: window)
            let displayWasCreated = display != nil
            try #require(displayWasCreated)
            print("ANDROID_SHUTDOWN: boot \(boot) reached Android in the same app process")
            if memorySnapshots { try memorySnapshot("running-\(boot)") }
            let expected: Phase
            if memorySnapshots || boot < boots {
                expected = .home
                _ = try? await operations.shell("reboot -p", limit: .seconds(30))
            } else {
                expected = .failed
                kernarium_runtime_loader_request_stop()
            }
            let endDeadline = ContinuousClock.now + .seconds(120)
            while model.phase != expected && ContinuousClock.now < endDeadline {
                try await Task.sleep(for: .milliseconds(200))
            }
            try #require(model.phase == expected, "Boot \(boot) ended at \(model.phase): \(model.runReport)")
            print("ANDROID_SHUTDOWN: boot \(boot) ended at \(model.phase)")
            if memorySnapshots || boot < boots {
                #expect(model.runEnd.poweredOff)
                #expect(model.plan == .normal)
                if boot == 1 {
                    // The first run created the data disk and Android wrote to
                    // it: Home offers recovery and states what the disk holds
                    // now, not what it held when the guest loaded.
                    #expect(model.plan.offersRecovery)
                    let disk = directory.appendingPathComponent(InstanceFiles.dataDisk)
                    let held = try URL(fileURLWithPath: disk.path).resourceValues(forKeys: [.fileAllocatedSizeKey])
                    let onDisk = UInt64(held.fileAllocatedSize ?? 0)
                    #expect(onDisk > 0 && model.guest?.usedBytes == onDisk, "Home states \(String(describing: model.guest?.usedBytes)) of \(onDisk)")
                    print("ANDROID_SHUTDOWN: after the first run Home states \(model.guest?.usedBytes ?? 0) bytes of the data disk")
                }
                if memorySnapshots {
                    try memorySnapshot("home-\(boot)-immediate")
                    try await Task.sleep(for: .seconds(10))
                    #expect(display == nil, "The retired guest display must be released at Home")
                    try memorySnapshot("home-\(boot)-settled")
                }
            } else {
                #expect(!model.runEnd.poweredOff)
                model.start(.normal)
                #expect(model.phase == .failed)
                // An inspection of the End screen asks for it to stay up.
                if let text = ProcessInfo.processInfo.environment["ANDARIUM_END_HOLD_SECONDS"],
                    let seconds = Double(text), seconds > 0 {
                    try await Task.sleep(for: .seconds(seconds))
                }
            }
        }
    }

    private func memorySnapshot(_ label: String) throws {
        try autoreleasepool {
            var snapshot = andarium_test_memory_snapshot()
            snapshot["label"] = label
            snapshot["time"] = Date().timeIntervalSince1970
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent("ShutdownMemory")
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let data = try JSONSerialization.data(withJSONObject: snapshot, options: [.sortedKeys])
            try data.write(to: directory.appendingPathComponent("\(label).json"), options: .atomic)
            let fields = ["footprint", "wired", "malloc_in_use", "malloc_allocated", "heap_dirty", "scratch_dirty", "graphics"]
            let figures = fields.map { "\($0)=\(snapshot[$0] ?? "?")" }.joined(separator: " ")
            print("ANDROID_MEMORY: \(label) \(figures)")
            #expect((snapshot["task_info_result"] as? NSNumber)?.intValue == 0)
            #expect((snapshot["thread_info_result"] as? NSNumber)?.intValue == 0)
            #expect((snapshot["vm_walk_result"] as? NSNumber)?.int32Value == KERN_INVALID_ADDRESS)
            if label.hasSuffix("settled") {
                #expect((snapshot["heap_dirty"] as? NSNumber)?.uint64Value == 0)
                #expect((snapshot["scratch_dirty"] as? NSNumber)?.uint64Value == 0)
                #expect((snapshot["wired"] as? NSNumber)?.uint64Value == 0)
            }
        }
    }

    private func guestDisplay(in view: UIView) -> GuestMetalView? {
        if let display = view as? GuestMetalView { return display }
        for child in view.subviews {
            if let display = guestDisplay(in: child) { return display }
        }
        return nil
    }
}
