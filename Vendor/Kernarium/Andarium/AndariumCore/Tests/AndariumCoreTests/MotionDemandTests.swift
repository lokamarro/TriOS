// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore

/// The host side of the guest's `MotionDemand` frames: the wire format, the
/// state machine that turns them into what the device's sensors do, and the
/// session that drives a fake device through a fake guest connection.
///
/// The session's cases: a guest that never sends a frame is read continuously,
/// sensors are started, stopped and retimed as the guest says, the switch is
/// off, the guest is in the background, a new mounting, a lost connection and
/// a failing device.
struct MotionDemandTests {
    // Independent wire fixtures, little endian.
    private func frame(mask: UInt8, accelerometer: [UInt8], gyroscope: [UInt8]) -> Data {
        Data([0x41, 0x4e, 0x43, 0x31, mask, 0, 0, 0] + accelerometer + gyroscope)
    }

    @Test func aFrameSaysWhichSensorsAreUsedAndHowOften() throws {
        // 20 000 us and 66 667 us.
        let both = frame(mask: 3, accelerometer: [0x20, 0x4e, 0, 0], gyroscope: [0x6b, 0x04, 0x01, 0])
        let demand = try #require(MotionDemand(frame: both))
        #expect(demand == MotionDemand(accelerometer: 20_000, gyroscope: 66_667))
        // 1 s on the gyroscope alone; the accelerometer's period is not read.
        let one = frame(mask: 2, accelerometer: [1, 0, 0, 0], gyroscope: [0x40, 0x42, 0x0f, 0])
        #expect(MotionDemand(frame: one) == MotionDemand(gyroscope: 1_000_000))
        let none = frame(mask: 0, accelerometer: [0x20, 0x4e, 0, 0], gyroscope: [0x20, 0x4e, 0, 0])
        #expect(try #require(MotionDemand(frame: none)).isIdle)
    }

    @Test func aPeriodOutsideTheRangeIsBroughtIntoIt() throws {
        let fast = frame(mask: 1, accelerometer: [0, 0, 0, 0], gyroscope: [0, 0, 0, 0])
        #expect(MotionDemand(frame: fast) == MotionDemand(accelerometer: 10_000))
        let slow = frame(mask: 2, accelerometer: [0, 0, 0, 0], gyroscope: [0x00, 0x2d, 0x31, 0x01])
        #expect(MotionDemand(frame: slow) == MotionDemand(gyroscope: 1_000_000))
    }

    @Test func aFrameThatIsNotOneIsRefused() {
        let good = frame(mask: 1, accelerometer: [0x20, 0x4e, 0, 0], gyroscope: [0, 0, 0, 0])
        #expect(MotionDemand(frame: good) != nil)
        #expect(MotionDemand(frame: good.dropLast()) == nil)
        #expect(MotionDemand(frame: good + [0]) == nil)
        var wrong = good
        wrong[3] = 0x32
        #expect(MotionDemand(frame: wrong) == nil)
        // The sample record's own magic is not a frame.
        wrong[2] = 0x53
        wrong[3] = 0x31
        #expect(MotionDemand(frame: wrong) == nil)
        // A sensor the protocol does not have.
        #expect(MotionDemand(frame: frame(mask: 4, accelerometer: [0, 0, 0, 0], gyroscope: [0, 0, 0, 0])) == nil)
    }

    @Test func theHostReadsEachSensorTwiceAPeriodButNeverFasterThanTheLegacyRate() {
        #expect(abs(MotionSampling.interval(forPeriod: 66_667) - 0.0333335) < 1e-6)
        #expect(MotionSampling.interval(forPeriod: 20_000) == 0.01)
        #expect(MotionSampling.interval(forPeriod: 10_000) == 0.01)
        #expect(MotionSampling.interval(forPeriod: 1_000_000) == 0.5)
    }

    // MARK: The state machine

    private func wanted(connected: Bool = true) -> MotionSampling {
        var sampling = MotionSampling()
        sampling.enabled = true
        sampling.foreground = true
        sampling.connectionChanged(connected)
        return sampling
    }

    @Test func nothingIsReadWithoutTheSwitchTheForegroundOrAConnection() {
        var sampling = wanted()
        #expect(sampling.plan != .off)
        sampling.enabled = false
        #expect(sampling.plan == .off && !sampling.wantsConnection)
        sampling.enabled = true
        sampling.foreground = false
        #expect(sampling.plan == .off && !sampling.wantsConnection)
        sampling.foreground = true
        sampling.connectionChanged(false)
        #expect(sampling.plan == .off && sampling.wantsConnection)
    }

    @Test func aServiceThatSaysNothingIsServedContinuouslyAsItAlwaysWas() {
        let sampling = wanted()
        #expect(sampling.demand == nil)
        #expect(sampling.plan == MotionPlan(accelerometer: 0.01, gyroscope: 0.01))
    }

    @Test func aServiceThatSaysWhatItNeedsIsReadForThat() {
        var sampling = wanted()
        sampling.received(MotionDemand(accelerometer: 20_000))
        #expect(sampling.plan == MotionPlan(accelerometer: 0.01, gyroscope: nil))
        sampling.received(MotionDemand(accelerometer: 66_667, gyroscope: 200_000))
        #expect(sampling.plan.accelerometer.map { abs($0 - 0.0333335) < 1e-6 } == true)
        #expect(sampling.plan.gyroscope == 0.1)
        // Nothing in use: no sensor runs and the connection stays wanted.
        sampling.received(MotionDemand())
        #expect(sampling.plan == .off && sampling.wantsConnection)
        sampling.received(MotionDemand(gyroscope: 1_000_000))
        #expect(sampling.plan == MotionPlan(accelerometer: nil, gyroscope: 0.5))
    }

    @Test func theSwitchAndTheForegroundGateAWholeDemand() {
        var sampling = wanted()
        sampling.received(MotionDemand(accelerometer: 20_000, gyroscope: 20_000))
        sampling.enabled = false
        #expect(sampling.plan == .off)
        sampling.enabled = true
        #expect(sampling.plan == MotionPlan(accelerometer: 0.01, gyroscope: 0.01))
        sampling.foreground = false
        #expect(sampling.plan == .off)
    }

    @Test func aNewConnectionForgetsWhatTheLastOneSaid() {
        var sampling = wanted()
        sampling.received(MotionDemand())
        #expect(sampling.plan == .off)
        sampling.connectionChanged(false)
        sampling.connectionChanged(true)
        #expect(sampling.demand == nil)
        #expect(sampling.plan == MotionPlan(accelerometer: 0.01, gyroscope: 0.01))
    }

    // MARK: A session over a fake guest

    /// A device with sensors that records what the session does with them.
    private final class FakeDevice: MotionDevice, @unchecked Sendable {
        let hasAccelerometer: Bool
        let hasGyroscope: Bool
        private let lock = NSLock()
        private var running: [MotionSample.Sensor: TimeInterval] = [:]
        private var handlers: [MotionSample.Sensor: @Sendable (Double, Double, Double) -> Void] = [:]
        private var failures: [@Sendable () -> Void] = []
        private(set) var starts = 0

        init(accelerometer: Bool = true, gyroscope: Bool = true) {
            hasAccelerometer = accelerometer
            hasGyroscope = gyroscope
        }

        func start(
            _ sensor: MotionSample.Sensor, interval: TimeInterval,
            handler: @escaping @Sendable (Double, Double, Double) -> Void,
            failure: @escaping @Sendable () -> Void
        ) {
            lock.withLock {
                running[sensor] = interval
                handlers[sensor] = handler
                failures.append(failure)
                starts += 1
            }
        }

        func stop(_ sensor: MotionSample.Sensor) {
            lock.withLock {
                running[sensor] = nil
                handlers[sensor] = nil
            }
        }

        var intervals: [MotionSample.Sensor: TimeInterval] { lock.withLock { running } }
        func emit(_ sensor: MotionSample.Sensor, _ x: Double, _ y: Double, _ z: Double) {
            let handler = lock.withLock { handlers[sensor] }
            handler?(x, y, z)
        }
        func fail() {
            let failure = lock.withLock { failures.last }
            failure?()
        }
    }

    /// Hands out one end of a socket pair per connection; the other end is the
    /// guest's.
    private final class FakeGuest: GuestStreamOpener, @unchecked Sendable {
        private let lock = NSLock()
        private var ends: [Int32] = []
        func openStream() throws -> Int32 {
            var sockets: [Int32] = [-1, -1]
            guard socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0 else { throw POSIXError(.EIO) }
            lock.withLock { ends.append(sockets[1]) }
            return sockets[0]
        }
        var connections: Int { lock.withLock { ends.count } }
        var current: Int32? { lock.withLock { ends.last } }
        deinit { for end in ends { close(end) } }

        /// The guest's service says what it needs.
        func say(_ demand: MotionDemand) {
            var mask: UInt8 = 0
            if demand.accelerometer != nil { mask |= 1 }
            if demand.gyroscope != nil { mask |= 2 }
            var bytes: [UInt8] = [0x41, 0x4e, 0x43, 0x31, mask, 0, 0, 0]
            for period in [demand.accelerometer ?? 10_000, demand.gyroscope ?? 10_000] {
                bytes += (0..<4).map { UInt8((period >> UInt32(8 * $0)) & 0xff) }
            }
            if let end = current { _ = write(end, bytes, bytes.count) }
        }

        /// What the guest has received, up to `count` bytes within `timeout`.
        func receive(count: Int, timeout: Int = 1500) -> Data {
            guard let end = current else { return Data() }
            var result = Data()
            let deadline = ContinuousClock.now + .milliseconds(Int64(timeout))
            while result.count < count, ContinuousClock.now < deadline {
                var descriptor = pollfd(fd: end, events: Int16(POLLIN), revents: 0)
                guard poll(&descriptor, 1, 10) > 0 else { continue }
                var bytes = [UInt8](repeating: 0, count: count - result.count)
                let size = recv(end, &bytes, bytes.count, MSG_DONTWAIT)
                guard size > 0 else { break }
                result.append(contentsOf: bytes.prefix(size))
            }
            return result
        }

        /// The host closed the connection: the guest reads the end of it.
        func sawClose(timeout: Int = 1500) -> Bool {
            guard let end = current else { return false }
            var descriptor = pollfd(fd: end, events: Int16(POLLIN), revents: 0)
            guard poll(&descriptor, 1, Int32(timeout)) > 0 else { return false }
            var byte: UInt8 = 0
            return recv(end, &byte, 1, MSG_DONTWAIT) == 0
        }

        /// The connection is open and quiet: nothing to read, no end.
        func isQuiet() -> Bool {
            guard let end = current else { return false }
            var byte: UInt8 = 0
            return recv(end, &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN
        }

        func hangUp() {
            if let end = current { shutdown(end, SHUT_RDWR) }
        }
    }

    private func eventually(_ timeout: Double = 3, _ condition: () -> Bool) -> Bool {
        let deadline = ContinuousClock.now + .milliseconds(Int64(timeout * 1000))
        while ContinuousClock.now < deadline {
            if condition() { return true }
            usleep(5_000)
        }
        return condition()
    }

    private final class Problems: @unchecked Sendable {
        private let lock = NSLock()
        private var seen: [MotionProblem?] = []
        func record(_ problem: MotionProblem?) { lock.withLock { seen.append(problem) } }
        var all: [MotionProblem?] { lock.withLock { seen } }
    }

    private func session(
        _ guest: FakeGuest, _ device: FakeDevice, problems: Problems = Problems()
    ) -> MotionSession {
        MotionSession(opener: guest, device: device, retryDelay: .milliseconds(40), problem: problems.record)
    }

    @Test func aServiceThatNeverSpeaksIsReadContinuouslyAndItsSamplesArrive() throws {
        let guest = FakeGuest()
        let device = FakeDevice()
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals == [.accelerometer: 0.01, .gyroscope: 0.01] })
        device.emit(.accelerometer, 0, 0, -1)
        let packet = try #require(MotionSample(sensor: .accelerometer, x: 0, y: 0, z: -1).encoded)
        #expect(guest.receive(count: 20) == packet)
        session.update(enabled: false, foreground: true, axes: .portrait)
    }

    @Test func aServiceThatSpeaksIsReadOnlyForTheSensorsInUseAtTheirRate() {
        let guest = FakeGuest()
        let device = FakeDevice()
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        // An application takes the accelerometer at 20 ms: the gyroscope stops.
        guest.say(MotionDemand(accelerometer: 20_000))
        #expect(eventually { device.intervals == [.accelerometer: 0.01] })
        // Another takes the gyroscope at 200 ms and the first slows to the UI rate.
        guest.say(MotionDemand(accelerometer: 66_667, gyroscope: 200_000))
        #expect(eventually { device.intervals[.gyroscope] == 0.1 && device.intervals[.accelerometer] != 0.01 })
        // Nobody uses a sensor: nothing runs, and the connection stays up.
        guest.say(MotionDemand())
        #expect(eventually { device.intervals.isEmpty })
        session.settle()
        #expect(guest.isQuiet() && guest.connections == 1)
        // A sample the device still delivers is not sent.
        device.emit(.accelerometer, 0, 0, -1)
        #expect(guest.receive(count: 20, timeout: 60).isEmpty)
        guest.say(MotionDemand(gyroscope: 1_000_000))
        #expect(eventually { device.intervals == [.gyroscope: 0.5] })
        session.update(enabled: false, foreground: true, axes: .portrait)
    }

    @Test func turningTheSwitchOffStopsTheSensorsAndClosesTheConnection() {
        let guest = FakeGuest()
        let device = FakeDevice()
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        guest.say(MotionDemand(accelerometer: 20_000))
        #expect(eventually { device.intervals == [.accelerometer: 0.01] })
        session.update(enabled: false, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.isEmpty })
        #expect(guest.sawClose())
        // On again: a new connection, and the service has not spoken on it,
        // so it is read continuously, not as the last one said.
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals == [.accelerometer: 0.01, .gyroscope: 0.01] && guest.connections == 2 })
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func leavingTheForegroundStopsTheSensorsAndReturningStartsAfresh() {
        let guest = FakeGuest()
        let device = FakeDevice()
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        guest.say(MotionDemand(gyroscope: 100_000))
        #expect(eventually { device.intervals == [.gyroscope: 0.05] })
        session.update(enabled: true, foreground: false, axes: .portrait)
        #expect(eventually { device.intervals.isEmpty })
        #expect(guest.sawClose())
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 && guest.connections == 2 })
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func aNewMountingBeginsANewRunWithItsAxes() throws {
        let guest = FakeGuest()
        let device = FakeDevice()
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        session.update(enabled: true, foreground: true, axes: .landscapeLeft)
        #expect(eventually { guest.connections == 2 && device.intervals.count == 2 })
        device.emit(.gyroscope, 1, 2, 3)
        let packet = try #require(MotionSample(sensor: .gyroscope, x: 1, y: 2, z: 3, axes: .landscapeLeft).encoded)
        #expect(guest.receive(count: 20) == packet)
        // Restarting without a change of axes is a run too, and starts at the next update.
        session.restart()
        #expect(eventually { device.intervals.isEmpty })
        session.update(enabled: true, foreground: true, axes: .landscapeLeft)
        #expect(eventually { guest.connections == 3 && device.intervals.count == 2 })
        session.update(enabled: false, foreground: false, axes: .landscapeLeft)
    }

    @Test func aDeviceWithNoSensorsNeverConnects() {
        let guest = FakeGuest()
        let device = FakeDevice(accelerometer: false, gyroscope: false)
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        session.settle()
        usleep(100_000)
        #expect(guest.connections == 0 && device.starts == 0)
    }

    @Test func aDeviceWithOneSensorReadsOnlyThatOne() {
        let guest = FakeGuest()
        let device = FakeDevice(accelerometer: true, gyroscope: false)
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals == [.accelerometer: 0.01] })
        guest.say(MotionDemand(gyroscope: 20_000))
        #expect(eventually { device.intervals.isEmpty })
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func aLostConnectionStopsTheSensorsAndIsMadeAgain() {
        let guest = FakeGuest()
        let device = FakeDevice()
        let problems = Problems()
        let session = session(guest, device, problems: problems)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        guest.say(MotionDemand(accelerometer: 20_000))
        #expect(eventually { device.intervals == [.accelerometer: 0.01] })
        guest.hangUp()
        #expect(eventually { problems.all.contains(.guestUnavailable) })
        // A new connection, and the new service is read as it says (here, nothing yet).
        #expect(eventually { guest.connections >= 2 && device.intervals.count == 2 })
        #expect(eventually { problems.all.last == .some(nil) })
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func aGarbledFrameDropsTheConnection() {
        let guest = FakeGuest()
        let device = FakeDevice()
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        if let end = guest.current {
            let junk = [UInt8](repeating: 0x55, count: 16)
            _ = write(end, junk, junk.count)
        }
        #expect(eventually { guest.connections >= 2 })
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func aFrameThatArrivesInPiecesIsRead() {
        let guest = FakeGuest()
        let device = FakeDevice()
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        let bytes: [UInt8] = [0x41, 0x4e, 0x43, 0x31, 1, 0, 0, 0, 0x20, 0x4e, 0, 0, 0x20, 0x4e, 0, 0]
        if let end = guest.current {
            _ = write(end, Array(bytes[0..<5]), 5)
            usleep(30_000)
            #expect(device.intervals.count == 2)
            _ = write(end, Array(bytes[5...]), 11)
        }
        #expect(eventually { device.intervals == [.accelerometer: 0.01] })
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func aFailingDeviceEndsTheRunAndSaysSo() {
        let guest = FakeGuest()
        let device = FakeDevice()
        let problems = Problems()
        let session = session(guest, device, problems: problems)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { device.intervals.count == 2 })
        device.fail()
        #expect(eventually { problems.all.contains(.deviceFailed) && device.intervals.isEmpty })
        #expect(guest.sawClose())
    }
}
