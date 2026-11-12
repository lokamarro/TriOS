// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Opt-in real-device test: the production model, display, runtime, Core Motion
/// source, vsock transport, Android HAL and SensorManager event queue. The
/// foreground probe APK is copied to Library/Caches/andarium-motion-probe.apk before launch.
///
/// The probe reads both sensors through Android's public SensorManager API.
/// The automated mode checks sustained delivery and off/on/off; the separate
/// `ANDARIUM_MOTION_INTERACTIVE=1` mode checks physical tilt and rotation
/// (`tests/integration/android/motion/README.md` has the probe and the
/// invocation). Physical sensor delivery requires a sensor-capable iOS device
/// and a guest image carrying the motion sub-HAL; Simulator and Mac tests do
/// not establish that path.
@Suite(.serialized)
@MainActor
struct MotionOverVsockTests {
    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_MOTION_TESTS"] == "1"
        && ProcessInfo.processInfo.environment["ANDARIUM_MOTION_INTERACTIVE"] != "1"), .timeLimit(.minutes(20)))
    func sustainedSensorDeliveryAndTheSwitch() async throws {
        try await withProbe { model, operations in
            try await RotationOverVsockTests().checkPreferences(model: model, operations: operations)
            try await check("silence", seconds: 3, operations: operations)
            model.setMotionSensors(true)
            try await check("events", seconds: 5, operations: operations)
            try await check("events", seconds: 75, operations: operations)
            try #require(model.motionSensorsProblem == nil, "\(model.motionSensorsProblem ?? "")")
            model.setMotionSensors(false)
            try await Task.sleep(for: .seconds(1))
            try await check("silence", seconds: 3, operations: operations)
            print("MOTION: sustained accelerometer, gyroscope, opt-out and rotation preference checks passed")
        }
    }

    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_MOTION_INTERACTIVE"] == "1"), .timeLimit(.minutes(30)))
    func physicalMotionAndSceneTransitions() async throws {
        try await withProbe { model, operations in
            model.setMotionSensors(true)
            try await check("events", seconds: 3, operations: operations)
            print("MOTION: ready for physical movement; tilt and rotate the iPad (up to ten minutes)")
            let deadline = ContinuousClock.now + .seconds(600)
            var moved = false
            while ContinuousClock.now < deadline {
                let remaining = Int((deadline - ContinuousClock.now).components.seconds)
                if remaining < 2 { break }
                let seconds = min(120, remaining)
                let value = try await capture("movement", seconds: seconds, operations: operations)
                try #require(value.valid)
                if value.passed { moved = true; break }
                // Absence of a gesture is a wait, but stale or missing input
                // is a sensor failure and must not be retried as human delay.
                try #require(value.accelerometer.count >= 75 && value.gyroscope.count >= 75)
                let minimumSpan = Int64(seconds - 1) * 1_000_000_000
                try #require(value.accelerometer.last_ns - value.accelerometer.first_ns >= minimumSpan)
                try #require(value.gyroscope.last_ns - value.gyroscope.first_ns >= minimumSpan)
            }
            try #require(moved, "No physical tilt and rotation was observed within ten minutes")
            print("MOTION: real accelerometer and gyroscope movement passed")
            if ProcessInfo.processInfo.environment["ANDARIUM_ROTATION_VISUAL"] == "1" {
                try await RotationOverVsockTests().checkSceneTransitions(model: model, operations: operations)
            }
        }
    }

    private func withProbe(_ body: @MainActor (AppModel, AdbGuestOperations) async throws -> Void) async throws {
        let probeURL = try #require(FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first)
            .appendingPathComponent("andarium-motion-probe.apk")
        let probe = try Data(contentsOf: probeURL)
        try #require(probe.prefix(4) == Data([0x50, 0x4b, 0x03, 0x04]))
        defer { try? FileManager.default.removeItem(at: probeURL) }
        try await withAndroidGuest(cleanupCommand: "pm uninstall org.example.andarium.motionprobe >/dev/null 2>&1") { model, operations in
            try #require(model.motionSensorsAvailable)
            try await operations.installApk(at: probeURL, sizeBytes: probe.count, update: { _ in })
            try await body(model, operations)
        }
    }

    private func capture(_ mode: String, seconds: Int, operations: AdbGuestOperations) async throws -> Report {
        let runID = UUID().uuidString
        let launch = try await operations.shell(
            "am start -W -n org.example.andarium.motionprobe/.MotionActivity --es mode \(mode) --ei seconds \(seconds) --es run_id \(runID)")
        try #require(launch.exitCode == 0, "\(launch.text)")
        let deadline = ContinuousClock.now + .seconds(seconds + 20)
        var report: Report?
        while ContinuousClock.now < deadline {
            let result = try await operations.shell(
                "run-as org.example.andarium.motionprobe cat files/result.json", limit: .seconds(10))
            if result.exitCode == 0, let value = try? JSONDecoder().decode(Report.self, from: result.stdout),
               value.run_id == runID {
                report = value
                print("MOTION \(mode) \(seconds)s: \(result.text)")
                break
            }
            try await Task.sleep(for: .milliseconds(500))
        }
        return try #require(report, "Foreground sensor probe did not finish")
    }

    private func check(_ mode: String, seconds: Int, operations: AdbGuestOperations) async throws {
        let value = try await capture(mode, seconds: seconds, operations: operations)
        try #require(value.valid && value.passed)
        if mode == "silence" {
            #expect(value.accelerometer.count == 0 && value.gyroscope.count == 0)
        } else {
            #expect(value.accelerometer.count >= 75 && value.gyroscope.count >= 75)
            if mode == "events", seconds >= 60 {
                let minimumSpan = Int64(seconds - 1) * 1_000_000_000
                try #require(value.accelerometer.last_ns - value.accelerometer.first_ns >= minimumSpan)
                try #require(value.gyroscope.last_ns - value.gyroscope.first_ns >= minimumSpan)
            }
        }
    }

    private struct Report: Decodable {
        struct Sensor: Decodable { var count: Int; var first_ns: Int64; var last_ns: Int64 }
        var accelerometer: Sensor
        var gyroscope: Sensor
        var valid: Bool
        var passed: Bool
        var run_id: String
    }
}
