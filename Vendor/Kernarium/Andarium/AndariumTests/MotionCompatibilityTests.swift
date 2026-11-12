// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Opt-in real-guest test of the host's motion session against the guest's
/// own sensor service, with a synthetic device (the Mac has no sensors): run
/// with `ANDARIUM_MOTION_COMPAT=1`, `ANDARIUM_START=normal` and an isolated
/// instance. The guest's service is reached through the runtime's vsock, as
/// in the application.
///
/// `ANDARIUM_MOTION_EXPECT=legacy` (the default) is an image whose service
/// never sends a control frame: the session reads both sensors at 100 Hz for
/// as long as the connection holds. `ondemand` is an image whose service
/// sends them: a frame arrives on connect, and the sensors the guest does not
/// use are not read.
///
/// `ANDARIUM_MOTION_DIAG=1` also prints which of the guest's own users keep a
/// sensor active (`dumpsys sensorservice`) with host-driven and with free
/// rotation.
@Suite(.serialized)
@MainActor
struct MotionCompatibilityTests {
    private struct Connector: GuestStreamOpener {
        func openStream() throws -> Int32 {
            let descriptor = kernarium_runtime_loader_connect_vsock(MotionStream.port, 250)
            guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: -descriptor) ?? .EIO) }
            return descriptor
        }
    }

    private final class SyntheticDevice: MotionDevice, @unchecked Sendable {
        let hasAccelerometer = true
        let hasGyroscope = true
        private let lock = NSLock()
        private var running: [MotionSample.Sensor: TimeInterval] = [:]
        private var handlers: [MotionSample.Sensor: @Sendable (Double, Double, Double) -> Void] = [:]
        private var timer: DispatchSourceTimer?
        private(set) var emitted = 0

        func start(
            _ sensor: MotionSample.Sensor, interval: TimeInterval,
            handler: @escaping @Sendable (Double, Double, Double) -> Void, failure: @escaping @Sendable () -> Void
        ) {
            lock.withLock {
                running[sensor] = interval
                handlers[sensor] = handler
            }
        }

        func stop(_ sensor: MotionSample.Sensor) {
            lock.withLock {
                running[sensor] = nil
                handlers[sensor] = nil
            }
        }

        var intervals: [MotionSample.Sensor: TimeInterval] { lock.withLock { running } }

        /// Delivers every running sensor at its own interval, as Core Motion would.
        func run() {
            let source = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "synthetic-motion"))
            source.schedule(deadline: .now(), repeating: .milliseconds(10))
            source.setEventHandler { [self] in
                let current = lock.withLock { handlers }
                for (sensor, handler) in current {
                    handler(0, 0, sensor == .accelerometer ? -1 : 0.1)
                    lock.withLock { emitted += 1 }
                }
            }
            source.resume()
            timer = source
        }

        func finish() { timer?.cancel() }
    }

    /// Which of the guest's own users keep a sensor active: Android's
    /// auto-rotation setting, the window manager's rotation mode and the
    /// sensor service's active connections, with host-driven rotation (as in a
    /// window) and then with free rotation and auto-rotate on (as in a locked
    /// full-screen scene, where Android alone rotates its display).
    private func report(_ operations: AdbGuestOperations) async throws {
        func show(_ title: String, _ command: String) async throws {
            let result = try await operations.shell(command, limit: .seconds(30))
            print("MOTION diag [\(title)]\n\(result.text)")
        }
        let activity = "dumpsys sensorservice | grep -E -i 'Host (Accelerometer|Gyroscope)|active|Connection|rate'"
        try await show("auto-rotate setting", "settings get system accelerometer_rotation")
        try await show("window manager rotation", "wm user-rotation; wm fixed-to-user-rotation")
        try await show("sensors in use, host rotation", activity)
        try await show("free rotation", "wm fixed-to-user-rotation disabled; wm user-rotation free; sleep 3; \(activity)")
        try await show("a rotatable app in front", "am start -a android.settings.SETTINGS >/dev/null; sleep 4; \(activity)")
        try await show("the face-down detector",
                       "dumpsys power | grep -i -B1 -A8 facedown; device_config list attention_manager_service; "
                       + "settings list secure | grep -i -E 'flip|face'")
        try await show("face-down detector off",
                       "device_config put attention_manager_service enable_flip_to_screen_off false; sleep 4; "
                       + "\(activity); device_config delete attention_manager_service enable_flip_to_screen_off")
        try await show("restore", "wm user-rotation lock 0; wm fixed-to-user-rotation enabled; input keyevent KEYCODE_HOME")
    }

    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_MOTION_COMPAT"] == "1"), .timeLimit(.minutes(20)))
    func theSessionAndTheGuestsSensorServiceAgree() async throws {
        let expectation = ProcessInfo.processInfo.environment["ANDARIUM_MOTION_EXPECT"] ?? "legacy"
        try await withAndroidGuest { _, operations in
            if ProcessInfo.processInfo.environment["ANDARIUM_MOTION_DIAG"] == "1" {
                try await report(operations)
            }
            let device = SyntheticDevice()
            device.run()
            defer { device.finish() }
            let session = MotionSession(opener: Connector(), device: device) { problem in
                print("MOTION compat: problem \(String(describing: problem))")
            }
            session.update(enabled: true, foreground: true, axes: .portrait)
            defer { session.update(enabled: false, foreground: false, axes: .portrait) }
            // Connected: a legacy service never speaks, so the sensors start and read continuously. An on-demand
            // service speaks on connect, and its demand is empty when no guest process uses a sensor: the plan
            // then stays off, and the demand frame is the sign of the connection.
            func connected() -> Bool {
                session.plan != .off || (expectation != "legacy" && session.demand != nil)
            }
            let connectDeadline = ContinuousClock.now + .seconds(30)
            while !connected(), ContinuousClock.now < connectDeadline {
                try await Task.sleep(for: .milliseconds(50))
            }
            try #require(connected(), "The guest's motion service did not accept the connection")
            print("MOTION compat: connected; plan \(session.plan)")
            // Hold the connection and watch what the service says for a while.
            let watchUntil = ContinuousClock.now + .seconds(8)
            var seen: [String] = []
            while ContinuousClock.now < watchUntil {
                let line = "\(String(describing: session.demand)) \(session.plan)"
                if seen.last != line { seen.append(line) }
                try await Task.sleep(for: .milliseconds(100))
            }
            print("MOTION compat: observed \(seen)")
            print("MOTION compat: \(device.emitted) readings delivered")
            switch expectation {
            case "legacy":
                #expect(session.demand == nil)
                #expect(session.plan == MotionPlan(accelerometer: 0.01, gyroscope: 0.01))
                #expect(seen.count == 1)
                #expect(device.emitted > 100)
            default:
                try #require(session.demand != nil, "No control frame arrived on the connection")
                let demand = try #require(session.demand)
                #expect((demand.accelerometer == nil) == (session.plan.accelerometer == nil))
                #expect((demand.gyroscope == nil) == (session.plan.gyroscope == nil))
            }
        }
    }
}
