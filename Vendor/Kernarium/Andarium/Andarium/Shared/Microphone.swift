// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What the system says about Andarium's use of the microphone, and the one
/// call that can ask. The device application reads iOS; a test gives its own.
protocol MicrophoneAccessSource {
    /// The access now. Reading it never shows a prompt.
    var state: MicrophoneState { get }
    /// Asks the person, which iOS does only while they have not answered
    /// before; otherwise it answers with what they said. `answered` runs on
    /// the main actor.
    func request(_ answered: @escaping @MainActor @Sendable (MicrophoneState) -> Void)
}

/// The Microphone setting's conversation with the system, for the device
/// application and the Simulator preview alike: turning it on asks iOS at
/// once, the first time; an answer, or access the person gave or took away in
/// Settings, goes to the model, which keeps the switch; and the guest hears
/// the microphone only while the switch shows on.
@MainActor
final class MicrophoneController {
    private weak var model: AppModel?
    private let source: any MicrophoneAccessSource
    private let tell: @MainActor (Bool) -> Void

    /// `tell` hands the guest its microphone: on only while the switch shows
    /// on, and never while access is out of reach.
    init(model: AppModel, source: any MicrophoneAccessSource, tell: @escaping @MainActor (Bool) -> Void) {
        self.model = model
        self.source = source
        self.tell = tell
        model.microphoneState = source.state
    }

    /// The switch changed, and the model has stored the choice.
    func setOn(_ on: Bool) {
        let access = source.state
        model?.microphoneState = access
        guard on else {
            tell(false)
            return
        }
        switch access {
        case .granted:
            tell(true)
        case .denied:
            tell(false)
        case .notAsked:
            // The guest hears nothing until the person has answered iOS's
            // question, which is asked now.
            tell(false)
            source.request { [weak self] answer in self?.answered(answer) }
        }
    }

    /// Looks at the permission again: at launch and whenever Andarium becomes
    /// active, since the person may have changed it in Settings.
    func refresh() {
        answered(source.state)
    }

    /// A run starts: the guest follows the switch as it shows now.
    func guestStarting() {
        tell(model?.microphoneShownOn ?? false)
    }

    /// iOS's answer, or Andarium's access as the person left it in Settings:
    /// the model keeps the switch, and the guest follows it.
    private func answered(_ state: MicrophoneState) {
        guard let model else { return }
        model.microphoneAccessChanged(state)
        tell(model.microphoneShownOn)
    }
}
