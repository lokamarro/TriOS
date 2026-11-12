// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How the virtual device is mounted: the interface orientation whose frame
/// the sensor axes are reported in.
///
/// While the host owns rotation the guest is held at rotation zero and drawn
/// upright in the host's window, so its frame is the host's interface
/// orientation and the mount follows it. While Android owns rotation, UIKit
/// holds the orientation and the mount stays as it was when that period began.
public struct MotionMount: Equatable, Sendable {
    public private(set) var axes = MotionSample.Axes.portrait
    private var policy: GuestRotationPolicy?

    public init() {}

    /// A run begins, mounted to the orientation now; `nil` when the scene
    /// cannot be reached keeps the mount as it is.
    public mutating func begin(interface: MotionSample.Axes?) {
        if let interface { axes = interface }
    }

    /// Applies the rotation policy and the interface orientation now. The
    /// answer is whether a stream in flight must restart: the policy changed,
    /// whose pending samples belong to the previous period, or the mount moved
    /// with the host's window. A call that changes neither answers `false`, so
    /// a storm of layouts at one orientation never restarts the sensors.
    public mutating func update(policy: GuestRotationPolicy, interface: MotionSample.Axes?) -> Bool {
        let entering = self.policy != policy
        self.policy = policy
        guard let interface else { return entering }
        switch policy {
        case .host:
            guard axes != interface else { return entering }
            axes = interface
            return true
        case .guest:
            // A new period of guest-driven rotation mounts the orientation
            // UIKit now holds and keeps it until the host owns rotation again.
            if entering { axes = interface }
            return entering
        }
    }
}
