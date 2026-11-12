// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore

/// What iOS says about Andarium's microphone, through the access wrapper the
/// two applications share (`GuestHost`).
struct SystemMicrophoneSource: MicrophoneAccessSource {
    private let permission = SystemMicrophonePermission()

    var state: MicrophoneState { Self.state(permission.access) }

    func request(_ answered: @escaping @MainActor @Sendable (MicrophoneState) -> Void) {
        permission.request { answer in
            Task { @MainActor in answered(Self.state(answer)) }
        }
    }

    private static func state(_ access: MicrophoneAccess) -> MicrophoneState {
        switch access {
        case .notAsked: .notAsked
        case .denied: .denied
        case .granted: .granted
        }
    }
}
