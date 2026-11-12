// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The device's motion sensors as a session drives them. Each sensor is
/// started at an interval or stopped; `handler` is called on a queue of the
/// device's own with the reading in the device's own units, and `failure`
/// when the device can no longer be read.
public protocol MotionDevice: AnyObject, Sendable {
    var hasAccelerometer: Bool { get }
    var hasGyroscope: Bool { get }
    /// Starts the sensor, or restarts it at the new interval if it runs.
    func start(
        _ sensor: MotionSample.Sensor, interval: TimeInterval,
        handler: @escaping @Sendable (_ x: Double, _ y: Double, _ z: Double) -> Void,
        failure: @escaping @Sendable () -> Void
    )
    func stop(_ sensor: MotionSample.Sensor)
}

/// Why the guest is not getting the device's motion.
public enum MotionProblem: Equatable, Sendable {
    /// The guest's motion sensor service is not there (an image without it).
    case guestUnavailable
    /// The device's sensors could not be read.
    case deviceFailed
}

/// Shares the device's sensors with the guest while the person's switch is
/// on and the guest is in front, and reads each sensor only while the guest
/// says an application uses it (`MotionSampling`). The axes are set when a run
/// begins and never change per sample; a new mounting is a new run
/// (`MotionMount` decides when). No samples are retained across a run.
///
/// Every call returns at once; the work is in order on one queue, and the
/// device and the stream are only touched there.
public final class MotionSession: @unchecked Sendable {
    private let opener: any GuestStreamOpener
    private let device: any MotionDevice
    private let retryDelay: DispatchTimeInterval
    private let problem: @Sendable (MotionProblem?) -> Void
    private let queue = DispatchQueue(label: "andarium-motion-session")

    // Touched only on `queue`.
    private var sampling = MotionSampling()
    private var axes = MotionSample.Axes.portrait
    private var stream: MotionStream?
    private var streamEpoch: UInt64 = 0
    private var run: UInt64 = 0
    private var applied = MotionPlan.off

    public init(
        opener: any GuestStreamOpener, device: any MotionDevice,
        retryDelay: DispatchTimeInterval = .seconds(1),
        problem: @escaping @Sendable (MotionProblem?) -> Void
    ) {
        self.opener = opener
        self.device = device
        self.retryDelay = retryDelay
        self.problem = problem
    }

    deinit { stream?.stop() }

    /// What is wanted now. A run begins when the switch is on and the guest
    /// is in front, and ends when either stops being true or the axes change.
    public func update(enabled: Bool, foreground: Bool, axes: MotionSample.Axes) {
        queue.async { [self] in
            sampling.enabled = enabled && (device.hasAccelerometer || device.hasGyroscope)
            sampling.foreground = foreground
            if axes != self.axes, stream != nil { endRun() }
            self.axes = axes
            if sampling.wantsConnection {
                if stream == nil { beginRun() }
                reconcile()
            } else if stream != nil {
                endRun()
            }
        }
    }

    /// Ends the run now: the pending samples belong to a period that is over.
    /// The next `update` begins a new one.
    public func restart() {
        queue.async { [self] in
            if stream != nil { endRun() }
        }
    }

    /// Returns when everything asked of the session so far has been done.
    public func settle() {
        queue.sync {}
    }

    /// What the guest's service last said on the connection; nil until it
    /// says anything (an older image never does).
    public var demand: MotionDemand? { queue.sync { sampling.demand } }

    /// How the device's sensors are being read now.
    public var plan: MotionPlan { queue.sync { applied } }

    // MARK: A run

    private func beginRun() {
        run &+= 1
        let current = run
        sampling.connectionChanged(false)
        applied = .off
        let stream = MotionStream(
            opener: opener, retryDelay: retryDelay,
            connectionChanged: { [weak self] connected in
                self?.queue.async { [weak self] in
                    guard let self, run == current else { return }
                    sampling.connectionChanged(connected)
                    problem(connected ? nil : .guestUnavailable)
                    reconcile()
                }
            },
            demandChanged: { [weak self] demand in
                self?.queue.async { [weak self] in
                    guard let self, run == current else { return }
                    sampling.received(demand)
                    reconcile()
                }
            }
        )
        self.stream = stream
        problem(nil)
        streamEpoch = stream.start()
    }

    private func endRun() {
        run &+= 1
        for sensor in [MotionSample.Sensor.accelerometer, .gyroscope] where applied.interval(of: sensor) != nil {
            device.stop(sensor)
        }
        applied = .off
        stream?.stop()
        stream = nil
        sampling.connectionChanged(false)
    }

    /// Starts, stops and retimes the device's sensors to match the plan.
    private func reconcile() {
        guard let stream else { return }
        let plan = sampling.plan
        let current = run
        let epoch = streamEpoch
        let axes = axes
        for sensor in [MotionSample.Sensor.accelerometer, .gyroscope] {
            let wanted = hasSensor(sensor) ? plan.interval(of: sensor) : nil
            guard wanted != applied.interval(of: sensor) else { continue }
            if let wanted {
                device.start(sensor, interval: wanted, handler: { x, y, z in
                    stream.submit(MotionSample(sensor: sensor, x: x, y: y, z: z, axes: axes), epoch: epoch)
                }, failure: { [weak self] in
                    self?.queue.async { [weak self] in
                        guard let self, run == current else { return }
                        endRun()
                        problem(.deviceFailed)
                    }
                })
            } else {
                device.stop(sensor)
            }
            switch sensor {
            case .accelerometer: applied.accelerometer = wanted
            case .gyroscope: applied.gyroscope = wanted
            }
        }
    }

    private func hasSensor(_ sensor: MotionSample.Sensor) -> Bool {
        switch sensor {
        case .accelerometer: device.hasAccelerometer
        case .gyroscope: device.hasGyroscope
        }
    }
}
