// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore

// SI conversion of a sample and its wire form, the mount's axes as the window
// and the owner of rotation change, the saved choice (unset follows the
// device, on and off stick), socket delivery, stop during connection and stale
// callbacks. The host's reading of the guest's control frames and what the
// sensors then do are in MotionDemandTests.swift.
struct MotionTests {
    @Test func aStationaryFaceUpDeviceReportsPositiveGravity() throws {
        let sample = MotionSample(sensor: .accelerometer, x: 0, y: 0, z: -1)
        #expect(abs(sample.z - 9.80665) < 0.00001)
        let packet = try #require(sample.encoded)
        #expect(packet.prefix(8) == Data([0x41, 0x4e, 0x53, 0x31, 1, 0, 0, 0]))
        #expect(packet.count == 20)
        // Independent IEEE-754 encoding of +9.80665 m/s², little endian.
        #expect(Array(packet.suffix(4)) == [0x0a, 0xe8, 0x1c, 0x41])
    }

    @Test func rotationKeepsItsRadiansPerSecondAndAxes() throws {
        let sample = MotionSample(sensor: .gyroscope, x: 1, y: -2, z: 0.5)
        #expect(sample.x == 1 && sample.y == -2 && sample.z == 0.5)
        #expect(Array(try #require(sample.encoded)) == [
            0x41, 0x4e, 0x53, 0x31, 4, 0, 0, 0,
            0, 0, 0x80, 0x3f, 0, 0, 0, 0xc0, 0, 0, 0, 0x3f,
        ])
        #expect(MotionSample(sensor: .gyroscope, x: .nan, y: 0, z: 0).encoded == nil)
    }

    @Test func axesFollowTheGuestsInitialDisplayWithoutMirroringRotation() {
        for (axes, x, y) in [
            (MotionSample.Axes.landscapeLeft, Float(2), Float(-1)),
            (.landscapeRight, -2, 1), (.portraitUpsideDown, -1, -2),
        ] {
            let gyro = MotionSample(sensor: .gyroscope, x: 1, y: 2, z: 3, axes: axes)
            #expect(gyro.x == x && gyro.y == y && gyro.z == 3)
            let gravity = MotionSample(sensor: .accelerometer, x: 0, y: 0, z: -1, axes: axes)
            #expect(abs(gravity.z - 9.80665) < 0.00001)
        }
    }

    /// The restart answer of one update, taken outside `#expect`, which cannot
    /// call a mutating member.
    private func restarts(
        _ mount: inout MotionMount, _ policy: GuestRotationPolicy, _ interface: MotionSample.Axes?
    ) -> Bool {
        mount.update(policy: policy, interface: interface)
    }

    @Test func theMountFollowsTheWindowWhileTheHostOwnsRotation() {
        var mount = MotionMount()
        mount.begin(interface: .landscapeLeft)
        #expect(mount.axes == .landscapeLeft)
        // A full-screen session starts guest-driven, then becomes a window.
        var restart = restarts(&mount, .guest, .landscapeLeft)
        #expect(restart)
        restart = restarts(&mount, .guest, .landscapeLeft)
        #expect(!restart)
        restart = restarts(&mount, .host, .landscapeLeft)
        #expect(restart)
        // The iPad turns to portrait: the upright guest follows it at once.
        restart = restarts(&mount, .host, .portrait)
        #expect(restart && mount.axes == .portrait)
        // A half turn keeps the size and changes the frame.
        restart = restarts(&mount, .host, .portraitUpsideDown)
        #expect(restart && mount.axes == .portraitUpsideDown)
    }

    @Test func aStormOfLayoutsAtOneOrientationNeverRestartsTheSensors() {
        var mount = MotionMount()
        mount.begin(interface: .landscapeRight)
        let entering = restarts(&mount, .host, .landscapeRight)
        #expect(entering)
        for _ in 0..<100 {
            let restart = restarts(&mount, .host, .landscapeRight)
            #expect(!restart)
        }
        #expect(mount.axes == .landscapeRight)
    }

    @Test func theMountIsFixedWhileAndroidOwnsRotation() {
        var mount = MotionMount()
        mount.begin(interface: .portrait)
        var restart = restarts(&mount, .guest, .landscapeLeft)
        #expect(restart && mount.axes == .landscapeLeft)
        // UIKit holds the locked orientation; a differing reading is ignored.
        restart = restarts(&mount, .guest, .portrait)
        #expect(!restart && mount.axes == .landscapeLeft)
        // Returning to a locked scene mounts the orientation now held, once.
        restart = restarts(&mount, .host, .portrait)
        #expect(restart && mount.axes == .portrait)
        restart = restarts(&mount, .guest, .landscapeRight)
        #expect(restart && mount.axes == .landscapeRight)
        restart = restarts(&mount, .guest, .landscapeRight)
        #expect(!restart)
    }

    @Test func anUnreachableSceneLeavesTheMountAlone() {
        var mount = MotionMount()
        mount.begin(interface: .landscapeLeft)
        mount.begin(interface: nil)
        #expect(mount.axes == .landscapeLeft)
        var restart = restarts(&mount, .host, nil)
        #expect(restart)
        restart = restarts(&mount, .host, nil)
        #expect(!restart && mount.axes == .landscapeLeft)
    }

    @Test func theGyroscopeFollowsTheDeviceUntilThePersonChooses() throws {
        // No choice is stored: a record without the field is unset, and unset
        // is on wherever the device has sensors to share.
        let unset = Data(#"{"cores":2,"memoryMiB":1024,"capacityGiB":8,"microphone":false,"includeInBackups":false}"#.utf8)
        let guest = try JSONDecoder().decode(Guest.self, from: unset)
        #expect(guest.motionChoice == nil)
        #expect(guest.motionSensorsOn(available: true))
        // Without sensors it is off, as the switch shows it.
        #expect(!guest.motionSensorsOn(available: false))
        let host = HostDevice(
            name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
        )
        #expect(Guest.fresh(on: host).motionChoice == nil)
        #expect(String(decoding: try JSONEncoder().encode(guest), as: UTF8.self).contains("motionSensors") == false)
    }

    @Test func aChoiceSticksThroughEraseAndReload() throws {
        let off = Data(
            #"{"cores":2,"memoryMiB":1024,"capacityGiB":8,"microphone":false,"includeInBackups":false,"motionSensors":false}"#.utf8)
        var guest = try JSONDecoder().decode(Guest.self, from: off)
        #expect(guest.motionChoice == false)
        #expect(!guest.motionSensorsOn(available: true))
        let savedOff = try JSONEncoder().encode(guest.erased)
        #expect(try JSONDecoder().decode(Guest.self, from: savedOff).motionChoice == false)
        guest.motionChoice = true
        let savedOn = try JSONEncoder().encode(guest.erased)
        let reloaded = try JSONDecoder().decode(Guest.self, from: savedOn)
        #expect(reloaded.motionChoice == true && reloaded.motionSensorsOn(available: true))
        #expect(!reloaded.motionSensorsOn(available: false))
    }

    private final class Opener: GuestStreamOpener, @unchecked Sendable {
        let lock = NSLock()
        var descriptor: Int32
        var attempts = 0
        let entered = DispatchSemaphore(value: 0)
        let release: DispatchSemaphore?
        init(descriptor: Int32, release: DispatchSemaphore? = nil) {
            self.descriptor = descriptor
            self.release = release
        }
        func openStream() throws -> Int32 {
            entered.signal()
            if let release { _ = release.wait(timeout: .now() + 3) }
            return try lock.withLock {
                attempts += 1
                guard descriptor >= 0 else { throw POSIXError(.ECONNREFUSED) }
                let result = descriptor
                descriptor = -1
                return result
            }
        }
    }

    private func pair() throws -> [Int32] {
        var sockets: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0 else { throw POSIXError(.EIO) }
        return sockets
    }

    private func read(_ fd: Int32, count: Int, timeout: Int32 = 1000) -> Data {
        var result = Data()
        let deadline = ContinuousClock.now + .milliseconds(Int64(timeout))
        while result.count < count, ContinuousClock.now < deadline {
            var descriptor = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            guard poll(&descriptor, 1, 10) > 0 else { continue }
            var bytes = [UInt8](repeating: 0, count: count - result.count)
            let size = recv(fd, &bytes, bytes.count, MSG_DONTWAIT)
            guard size > 0 else { break }
            result.append(contentsOf: bytes.prefix(size))
        }
        return result
    }

    @Test func aStopClosesTheStreamAndOldCallbacksCannotFeedANewSession() throws {
        let sockets = try pair()
        defer { close(sockets[1]) }
        let opener = Opener(descriptor: sockets[0])
        let stream = MotionStream(opener: opener)
        let first = stream.start()
        #expect(opener.entered.wait(timeout: .now() + 2) == .success)
        let sample = MotionSample(sensor: .gyroscope, x: 1, y: 2, z: 3)
        // A connection deliberately drops samples queued before it completed.
        let deadline = ContinuousClock.now + .seconds(2)
        var received = Data()
        while received.isEmpty, ContinuousClock.now < deadline {
            stream.submit(sample, epoch: first)
            received = read(sockets[1], count: 20, timeout: 30)
        }
        #expect(received == sample.encoded)
        stream.stop()
        #expect(read(sockets[1], count: 20).isEmpty)
        let next = try pair()
        defer { close(next[1]) }
        opener.lock.withLock { opener.descriptor = next[0] }
        let second = stream.start()
        #expect(second != first)
        #expect(opener.entered.wait(timeout: .now() + 2) == .success)
        let fresh = MotionSample(sensor: .gyroscope, x: 4, y: 5, z: 6)
        received = Data()
        let nextDeadline = ContinuousClock.now + .seconds(2)
        while received.isEmpty, ContinuousClock.now < nextDeadline {
            stream.submit(fresh, epoch: second)
            received = read(next[1], count: 20, timeout: 30)
        }
        #expect(received == fresh.encoded)
        stream.submit(sample, epoch: first)
        #expect(read(next[1], count: 20, timeout: 50).isEmpty)
        stream.stop()
    }

    @Test func stoppingDuringConnectDiscardsTheLateConnection() throws {
        let sockets = try pair()
        defer { close(sockets[1]) }
        let release = DispatchSemaphore(value: 0)
        let opener = Opener(descriptor: sockets[0], release: release)
        let stream = MotionStream(opener: opener)
        stream.start()
        #expect(opener.entered.wait(timeout: .now() + 2) == .success)
        stream.stop()
        release.signal()
        var descriptor = pollfd(fd: sockets[1], events: Int16(POLLIN), revents: 0)
        #expect(poll(&descriptor, 1, 2000) > 0)
        var byte: UInt8 = 0
        #expect(recv(sockets[1], &byte, 1, MSG_DONTWAIT) == 0)
    }

    @Test func aClosedPeerCannotRaiseSigpipe() throws {
        let sockets = try pair()
        close(sockets[1])
        let opener = Opener(descriptor: sockets[0])
        let stream = MotionStream(opener: opener)
        let epoch = stream.start()
        defer { stream.stop() }
        #expect(opener.entered.wait(timeout: .now() + 2) == .success)
        for _ in 0..<5 {
            stream.submit(MotionSample(sensor: .gyroscope, x: 0, y: 0, z: 0), epoch: epoch)
            usleep(20_000)
        }
    }
}
