// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Observation

/// Keeps what the person sets across launches: the display options chosen on
/// Home and Settings' "Show on screen". Each is written when it changes. A
/// value the launch decided for its own run is not written, so a harness
/// launch never replaces what the person stored (`AppModel.displayOptionsToKeep`,
/// `showStatsToKeep`).
final class PreferenceKeeper {
    private weak var model: AppModel?
    private let defaults: UserDefaults
    /// The write an observed change has queued, which re-arms the observation
    /// when it ends; a test awaits it.
    private(set) var pending: Task<Void, Never>?

    init(model: AppModel, defaults: UserDefaults = .standard) {
        self.model = model
        self.defaults = defaults
    }

    func start() {
        observe()
    }

    /// Writes what the model says is the person's.
    func keep() {
        guard let model else { return }
        if let options = model.displayOptionsToKeep { DisplayOptionsStore.save(options, to: defaults) }
        if let on = model.showStatsToKeep { StatsPreference(defaults: defaults).keep(on) }
    }

    private func observe() {
        guard let model else { return }
        withObservationTracking {
            _ = (model.displayOptionsToKeep, model.showStatsToKeep)
        } onChange: { [weak self] in
            // The model belongs to the main actor, so the change begins there;
            // the write waits until it has been made.
            MainActor.assumeIsolated {
                self?.pending = Task { @MainActor [weak self] in
                    self?.keep()
                    self?.observe()
                }
            }
        }
    }
}
