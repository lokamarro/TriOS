// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Private host-to-guest motion protocol on vsock 5556. Each 20-byte record is
/// little endian: "ANS1", Android sensor type (1 or 4), three IEEE float32 SI
/// values. The guest timestamps receipt in its own CLOCK_BOOTTIME domain. The
/// guest answers on the same connection with `MotionDemand` frames, which say
/// which sensors an application uses.
public struct MotionSample: Sendable {
    public enum Sensor: UInt32, Sendable { case accelerometer = 1, gyroscope = 4 }
    /// The host interface orientation the virtual device is mounted to
    /// (`MotionMount` decides when it changes).
    public enum Axes: Equatable, Sendable { case portrait, portraitUpsideDown, landscapeLeft, landscapeRight }
    public let sensor: Sensor
    public let x: Float
    public let y: Float
    public let z: Float

    public init(sensor: Sensor, x: Double, y: Double, z: Double, axes: Axes = .portrait) {
        self.sensor = sensor
        // Core Motion reports gravity in g with the opposite acceleration
        // sign to Android. Both use right-handed, fixed device axes.
        let scale = sensor == .accelerometer ? -9.80665 : 1.0
        let (horizontal, vertical): (Double, Double) = switch axes {
        case .portrait: (x, y)
        case .portraitUpsideDown: (-x, -y)
        case .landscapeLeft: (y, -x)
        case .landscapeRight: (-y, x)
        }
        self.x = Float(horizontal * scale)
        self.y = Float(vertical * scale)
        self.z = Float(z * scale)
    }

    public var encoded: Data? {
        guard x.isFinite, y.isFinite, z.isFinite else { return nil }
        var data = Data([0x41, 0x4e, 0x53, 0x31])
        for word in [sensor.rawValue, x.bitPattern, y.bitPattern, z.bitPattern] {
            var little = word.littleEndian
            withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
        }
        return data
    }
}

/// The private motion connection to the guest on vsock 5556: the host sends
/// samples and reads the guest's `MotionDemand` frames. It sends each sample
/// as it arrives, with bounded memory and no blocking writes (a write that
/// would block closes the connection, so a stalled or paused guest never has
/// old motion queued behind it), and holds no timer while connected. A stop
/// closes the connection and a connection completing after a stop cannot
/// revive it. The opener must bound its connect wait (the app uses 250 ms). A
/// connection that fails or drops is tried again after `retryDelay`.
public final class MotionStream: @unchecked Sendable {
    public static let port: UInt32 = 5556
    private let opener: any GuestStreamOpener
    private let connectionChanged: @Sendable (Bool) -> Void
    private let demandChanged: @Sendable (MotionDemand) -> Void
    private let retryDelay: DispatchTimeInterval
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "andarium-motion-stream")
    private var running = false
    private var generation: UInt64 = 0
    private var descriptor: Int32 = -1
    private var reader: DispatchSourceRead?
    /// The start of a frame that has not all arrived.
    private var partial = Data()

    public init(
        opener: any GuestStreamOpener,
        retryDelay: DispatchTimeInterval = .seconds(1),
        connectionChanged: @escaping @Sendable (Bool) -> Void = { _ in },
        demandChanged: @escaping @Sendable (MotionDemand) -> Void = { _ in }
    ) {
        self.opener = opener
        self.retryDelay = retryDelay
        self.connectionChanged = connectionChanged
        self.demandChanged = demandChanged
    }
    deinit { stop() }

    @discardableResult
    public func start() -> UInt64 {
        let epoch: UInt64? = lock.withLock {
            guard !running else { return nil }
            running = true
            generation &+= 1
            return generation
        }
        guard let epoch else { return lock.withLock { generation } }
        queue.async { [self] in connect(epoch: epoch) }
        return epoch
    }

    public func stop() {
        lock.withLock {
            running = false
            generation &+= 1
            partial.removeAll()
            _ = closeStream()
        }
    }

    public func submit(_ sample: MotionSample, epoch: UInt64) {
        guard let packet = sample.encoded else { return }
        let dropped = lock.withLock { () -> Bool in
            guard running, generation == epoch, descriptor >= 0 else { return false }
            let sent = packet.withUnsafeBytes { Darwin.send(descriptor, $0.baseAddress, $0.count, MSG_DONTWAIT) }
            guard sent != packet.count else { return false }
            // Closing discards a partial record and avoids queuing old motion
            // behind a stalled or paused guest.
            return closeStream()
        }
        if dropped { lost(epoch: epoch) }
    }

    // Called only with the lock held. The descriptor is closed by the reader's
    // cancel handler, after it can no longer be sent on, so a stop cannot
    // close a descriptor number that has been reused. Answers whether a
    // connection was closed.
    private func closeStream() -> Bool {
        guard descriptor >= 0 else { return false }
        let closing = descriptor
        descriptor = -1
        if let reader {
            self.reader = nil
            reader.cancel()
        } else {
            Darwin.close(closing)
        }
        return true
    }

    private func connect(epoch: UInt64) {
        let current = lock.withLock { running && generation == epoch && descriptor < 0 }
        guard current else { return }
        let opened = try? opener.openStream()
        let connected = lock.withLock { () -> Bool? in
            guard running, generation == epoch else {
                if let opened { Darwin.close(opened) }
                return nil
            }
            guard let opened else { return false }
            var one: Int32 = 1
            guard setsockopt(opened, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout.size(ofValue: one))) == 0 else {
                Darwin.close(opened)
                return false
            }
            descriptor = opened
            partial.removeAll()
            let source = DispatchSource.makeReadSource(fileDescriptor: opened, queue: queue)
            source.setEventHandler { [weak self] in self?.readFrames(from: opened, epoch: epoch) }
            source.setCancelHandler { Darwin.close(opened) }
            reader = source
            source.resume()
            return true
        }
        guard let connected else { return }
        connectionChanged(connected)
        if !connected { retry(epoch: epoch) }
    }

    /// The connection ended without a stop: report it and connect again.
    private func lost(epoch: UInt64) {
        connectionChanged(false)
        retry(epoch: epoch)
    }

    private func retry(epoch: UInt64) {
        queue.asyncAfter(deadline: .now() + retryDelay) { [self] in connect(epoch: epoch) }
    }

    /// The guest's frames. An end of stream or a bad frame drops the
    /// connection; the next one starts from what the service says on it.
    private func readFrames(from descriptor: Int32, epoch: UInt64) {
        var buffer = [UInt8](repeating: 0, count: 64)
        let count = recv(descriptor, &buffer, buffer.count, MSG_DONTWAIT)
        if count < 0 && (errno == EAGAIN || errno == EINTR) { return }
        var frames: [MotionDemand] = []
        let dropped = lock.withLock { () -> Bool in
            guard running, generation == epoch, self.descriptor == descriptor else { return false }
            guard count > 0 else { return closeStream() }
            partial.append(contentsOf: buffer.prefix(count))
            while partial.count >= MotionDemand.frameSize {
                let frame = partial.prefix(MotionDemand.frameSize)
                partial.removeFirst(MotionDemand.frameSize)
                guard let demand = MotionDemand(frame: Data(frame)) else { return closeStream() }
                frames.append(demand)
            }
            return false
        }
        // Frames that arrived before a bad one still count.
        for frame in frames { demandChanged(frame) }
        if dropped { lost(epoch: epoch) }
    }
}
