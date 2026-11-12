// SPDX-License-Identifier: GPL-2.0-or-later

import AVFAudio

/// How far the system lets the application use the microphone.
enum MicrophoneAccess: Equatable {
    /// The system has never asked the user.
    case notAsked
    case denied
    case granted
}

/// The system's microphone permission. Reading `access` never shows a prompt;
/// `request` is the one call that can, and only while the user has not
/// answered before.
protocol MicrophonePermission {
    var access: MicrophoneAccess { get }
    func request(_ answered: @escaping @Sendable (MicrophoneAccess) -> Void)
}

nonisolated struct SystemMicrophonePermission: MicrophonePermission {
    var access: MicrophoneAccess {
        switch AVAudioApplication.shared.recordPermission {
        case .granted: .granted
        case .undetermined: .notAsked
        default: .denied
        }
    }

    func request(_ answered: @escaping @Sendable (MicrophoneAccess) -> Void) {
        AVAudioApplication.requestRecordPermission { granted in
            answered(granted ? .granted : .denied)
        }
    }
}
