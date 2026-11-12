// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Metal
import Testing
import UIKit
@testable import Andarium

/// Opt-in physical-device comparison of app surfaces and retained allocations:
/// repeated system-app launches at Retina and point resolution, at the guest
/// memory and vCPU count the launch states, which every snapshot records. The
/// optional full-resolution WebGL Aquarium workload records the actual
/// renderer, frames and FPS alongside process, guest and GPU memory
/// (`tests/integration/android/webgl/README.md`).
@MainActor
struct RetinaMemoryTests {
    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_RETINA_MEMORY_TESTS"] == "1"), .timeLimit(.minutes(20)))
    func repeatedAppLaunches() async throws {
        let environment = ProcessInfo.processInfo.environment
        try #require(environment["ANDARIUM_GUEST_INSTANCE"] == "retina-memory"
            || environment["ANDARIUM_PROFILE_EXISTING_GUEST"] == "1")
        let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let model = try #require((scene.delegate as? AndariumSceneDelegate)?.model)
        let original = model.retina
        defer { model.retina = original }
        model.retina = environment["ANDARIUM_RETINA_PROFILE"] == "1"
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("RetinaMemory")
        if FileManager.default.fileExists(atPath: directory.path) {
            try FileManager.default.removeItem(at: directory)
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let output = directory.appendingPathComponent(model.retina ? "retina.jsonl" : "points.jsonl")
        try Data().write(to: output)
        let file = try FileHandle(forWritingTo: output)
        defer { try? file.close() }
        let webGL = environment["ANDARIUM_WEBGL_PROFILE"] == "1"
        let apk = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent("andarium-webgl-probe.apk")
        defer { if webGL { try? FileManager.default.removeItem(at: apk) } }
        @discardableResult
        func snapshot(_ label: String) throws -> [String: Any] {
            var values = andarium_test_memory_snapshot()
            try #require((values["task_info_result"] as? NSNumber)?.intValue == 0)
            values["label"] = label
            values["time"] = Date().timeIntervalSince1970
            values["retina"] = model.retina
            // The configuration the numbers belong to, whatever the device.
            values["guest_memory_mib"] = model.guest?.memoryMiB ?? 0
            values["guest_cores"] = model.guest?.cores ?? 0
            values["metal_allocated"] = MTLCreateSystemDefaultDevice()?.currentAllocatedSize ?? 0
            var statistics = kernarium_runtime_statistics()
            statistics.struct_size = UInt32(MemoryLayout<kernarium_runtime_statistics>.size)
            if kernarium_runtime_loader_read_statistics(&statistics) == 0 {
                values["guest_held"] = statistics.memory_held_bytes
            }
            let fields = ["guest_memory_mib", "guest_cores", "footprint", "remaining", "graphics", "metal_allocated", "malloc_in_use", "guest_held"]
            print("RETINA_MEMORY: \(label) \(fields.map { "\($0)=\(values[$0] ?? "?")" }.joined(separator: " "))")
            try file.write(contentsOf: JSONSerialization.data(withJSONObject: values, options: [.sortedKeys]))
            try file.write(contentsOf: Data([10]))
            try file.synchronize()
            return values
        }
        try snapshot("home")
        print("RETINA_MEMORY: ready for Instruments; starting in 20 seconds")
        try await Task.sleep(for: .seconds(20))
        try await withAndroidGuest(cleanupCommand: webGL ? "pm uninstall org.example.andarium.webglprobe >/dev/null 2>&1" : "true") { model, operations in
            // The launch states the configuration to profile
            // (KERNARIUM_GUEST_MEMORY_MIB, KERNARIUM_VCPU_COUNT); the guest
            // must have what it states. Either way the configuration is
            // recorded with every snapshot.
            let launched = AppEntry.fromArguments().overrides
            if let expected = launched.memoryMiB { try #require(model.guest?.memoryMiB == expected) }
            if let expected = launched.vCPUCount { try #require(model.guest?.cores == expected) }
            print("RETINA_MEMORY: guest \(model.guest?.memoryMiB ?? 0) MiB, \(model.guest?.cores ?? 0) vCPUs")
            let display = try await operations.shell("wm size; wm density; getprop ro.build.version.incremental")
            print("RETINA_MEMORY: display \(display.text)")
            let installed = try await operations.shell("pm list packages -3; cmd package query-activities --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER")
            try installed.stdout.write(to: directory.appendingPathComponent("installed-apps.txt"))
            print("RETINA_MEMORY: installed apps \(installed.text)")
            if webGL {
                let bytes = try apk.resourceValues(forKeys: [.fileSizeKey]).fileSize!
                try await operations.installApk(at: apk, sizeBytes: bytes, update: { _ in })
            }
            try await Task.sleep(for: .seconds(10))
            try snapshot("booted")
            let packages = ["com.android.settings", "org.lineageos.etar", "org.lineageos.glimpse",
                            "org.lineageos.twelve", "com.android.calculator2", "com.android.documentsui"]
            var apps: [(String, String)] = []
            for package in packages {
                let result = try await operations.shell("cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER \(package)")
                if let component = result.text.split(whereSeparator: \.isNewline).last.map(String.init),
                   component.contains("/"), component.utf8.allSatisfy({
                       (65...90).contains($0) || (97...122).contains($0) || (48...57).contains($0)
                           || [46, 47, 95, 36].contains($0)
                   }) { apps.append((package, component)) }
            }
            try #require(apps.count == packages.count, "Need all six system apps for this workload")
            for round in 1...2 {
                for (package, component) in apps {
                    let launch = try await operations.shell("am start -W -n '\(component)'", limit: .seconds(60))
                    try #require(launch.exitCode == 0, "\(launch.text)")
                    try await Task.sleep(for: .seconds(5))
                    try snapshot("round-\(round)-\(package)")
                }
                if webGL {
                    let runID = UUID().uuidString
                    try snapshot("webgl-\(round)-before")
                    _ = try await operations.shell("am start -W -n org.example.andarium.webglprobe/.WebGLActivity --es run_id \(runID)", limit: .seconds(60))
                    let deadline = ContinuousClock.now + .seconds(200)
                    var completed = false
                    var previousSamples = -1
                    while ContinuousClock.now < deadline {
                        let result = try await operations.shell("run-as org.example.andarium.webglprobe cat files/result.json", limit: .seconds(10))
                        if result.exitCode == 0,
                           let report = try? JSONSerialization.jsonObject(with: result.stdout) as? [String: Any],
                           report["run_id"] as? String == runID {
                            let samples = report["samples"] as? Int ?? 0
                            if previousSamples < 0 || samples / 15 != previousSamples / 15 {
                                print("RETINA_WEBGL: \(result.text)")
                                try snapshot("webgl-\(round)-samples-\(samples)")
                                previousSamples = samples
                            }
                            if report["done"] as? Bool == true {
                                print("RETINA_WEBGL: \(result.text)")
                                try result.stdout.write(to: directory.appendingPathComponent("webgl-\(round).json"))
                                try #require(report["passed"] as? Bool == true, "\(result.text)")
                                completed = true
                                break
                            }
                        }
                        try await Task.sleep(for: .seconds(2))
                    }
                    try #require(completed, "WebGL did not complete within its deadline")
                    _ = try await operations.shell("am force-stop org.example.andarium.webglprobe")
                    try snapshot("webgl-\(round)-closed")
                }
                _ = try await operations.shell("input keyevent KEYCODE_HOME")
                for (package, _) in apps { _ = try await operations.shell("am force-stop \(package)") }
                try await Task.sleep(for: .seconds(10))
                try snapshot("round-\(round)-closed")
                if environment["ANDARIUM_GPU_CACHE_TRIM_PROFILE"] == "1" {
                    let released = andarium_test_release_gpu_caches()
                    print("RETINA_MEMORY: released \(released) cached GPU allocations")
                    try #require(released >= 0, "GPU cache release entry point unavailable")
                    try await Task.sleep(for: .seconds(2))
                    try snapshot("round-\(round)-trimmed")
                }
                let guest = try await operations.shell("cat /proc/meminfo; dumpsys meminfo -s", limit: .seconds(30))
                try guest.stdout.write(to: directory.appendingPathComponent("guest-\(round).txt"))
            }
        }
        try await Task.sleep(for: .seconds(10))
        let settled = try snapshot("shutdown")
        for field in ["heap_dirty", "scratch_dirty", "wired"] {
            #expect((settled[field] as? NSNumber)?.uint64Value == 0, "Retained guest memory: \(field)")
        }
        print("RETINA_MEMORY: completed")
    }
}
