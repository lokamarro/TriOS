// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AVFAudio

/// The Simulator's own microphone permission, for `-systemMicrophone YES`: the
/// Microphone switch then runs the device's conversation with the system
/// (`MicrophoneController`) against the Simulator's alert and
/// `simctl privacy`, instead of taking the access as given.
struct SimulatorMicrophoneSource: MicrophoneAccessSource {
    var state: MicrophoneState {
        switch AVAudioApplication.shared.recordPermission {
        case .granted: .granted
        case .undetermined: .notAsked
        default: .denied
        }
    }

    func request(_ answered: @escaping @MainActor @Sendable (MicrophoneState) -> Void) {
        AVAudioApplication.requestRecordPermission { granted in
            Task { @MainActor in answered(granted ? .granted : .denied) }
        }
    }
}
