// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Whether the session's statistics show on screen (Settings' "Show on
/// screen"), kept for the install across launches. A fresh install is off.
///
/// It is a stored value of its own and not a field of `DisplayOptions`, whose
/// stored form stays as an earlier version wrote it, so the person's Retina
/// choice is never rewritten by this.
public struct StatsPreference {
    /// The stored value's key in the defaults database.
    public static let key = "andarium.statistics"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// The last value kept; off until one is.
    public var isOn: Bool { defaults.bool(forKey: Self.key) }

    public func keep(_ on: Bool) {
        defaults.set(on, forKey: Self.key)
    }

    /// How a launch begins.
    public struct Launch: Equatable, Sendable {
        /// Whether the statistics show from the start.
        public var showOnScreen: Bool
        /// Whether what the person sets from here on is kept. A launch that
        /// carries a launch setting (automation) is the harness's, so the
        /// value kept from the person's own choice is left as it is.
        public var keepsChanges: Bool
    }

    /// The start of a launch. One that states the value starts as stated.
    /// Any other launch that carries a launch setting starts off: reading the
    /// statistics costs processor time, which a measured run must not
    /// inherit from the person's overlay. Only a launch with no setting
    /// restores the kept value.
    public func launch(for overrides: LaunchOverrides) -> Launch {
        if let stated = overrides.statistics { return Launch(showOnScreen: stated, keepsChanges: false) }
        if !overrides.isEmpty { return Launch(showOnScreen: false, keepsChanges: false) }
        return Launch(showOnScreen: isOn, keepsChanges: true)
    }
}
