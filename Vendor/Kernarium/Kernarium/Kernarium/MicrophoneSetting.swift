// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import Foundation

/// The user's Microphone setting: whether programs in the guest may record
/// from the device's microphone. It is off until the user turns it on in
/// Emulator settings, and while it is off, or iOS has not granted access, the
/// guest records silence and nothing opens the microphone.
///
/// The setting applies at once, not at the next launch like the rest of the
/// emulator settings, because turning it on is also the moment iOS asks for
/// access. That question is asked here, as the direct result of the user's
/// switch and nowhere else: the runtime is only told the setting and never
/// asks, and a launch that finds the setting on carries it into the guest
/// without asking again.
@MainActor
final class MicrophoneSetting: ObservableObject {
    static let shared = MicrophoneSetting()
    static let defaultsKey = "emulator.microphone"

    @Published private(set) var isOn: Bool
    @Published private(set) var access: MicrophoneAccess

    private let defaults: UserDefaults
    private let permission: any MicrophonePermission
    private let tellRuntime: (Bool) -> Void

    /// - Parameter tellRuntime: hands the setting to the guest's runtime.
    ///   Nothing is handed over until the setting is on or the user turns it
    ///   off.
    init(
        defaults: UserDefaults = .standard,
        permission: any MicrophonePermission = SystemMicrophonePermission(),
        tellRuntime: @escaping (Bool) -> Void = { enabled in
            _ = kernarium_runtime_loader_set_microphone(enabled ? 1 : 0)
        }
    ) {
        self.defaults = defaults
        self.permission = permission
        self.tellRuntime = tellRuntime
        isOn = defaults.bool(forKey: Self.defaultsKey)
        access = permission.access
    }

    /// The user turned the setting on or off.
    func setOn(_ on: Bool) {
        guard on != isOn else { return }
        isOn = on
        defaults.set(on, forKey: Self.defaultsKey)
        access = permission.access
        if on, access == .notAsked {
            // The system answers on its own queue. The hop back to the main
            // actor holds its own weak reference to the setting, taken when
            // the hop is made, instead of reading the callback's variable
            // from concurrently-executing code.
            permission.request { [weak self] answer in
                Task { @MainActor [weak self] in self?.answered(answer) }
            }
        }
        tellRuntime(on)
    }

    /// A guest is about to run: carries a setting that is on into its runtime.
    /// An off setting leaves the runtime untouched.
    func restoreForGuest() {
        guard isOn else { return }
        access = permission.access
        tellRuntime(true)
    }

    /// The application became active again: the user may have changed the
    /// permission in the system's Settings, and a guest that was recording
    /// when the microphone was out of reach tries again.
    func refresh() {
        let current = permission.access
        if access != current { access = current }
        if isOn { tellRuntime(true) }
    }

    private func answered(_ answer: MicrophoneAccess) {
        access = answer
        if isOn { tellRuntime(true) }
    }

    /// What the settings screen says about the setting.
    static func explanation(isOn: Bool, access: MicrophoneAccess) -> String {
        guard isOn else {
            return "Off. Programs in the guest record silence, and Kernarium " +
                "does not use the microphone. This applies at once."
        }
        switch access {
        case .granted:
            return "On. Programs in the guest can record from this device's " +
                "microphone, and iOS shows its recording indicator while " +
                "they do. This applies at once."
        case .notAsked:
            return "Waiting for your answer to iOS's request for microphone " +
                "access. Until then the guest records silence."
        case .denied:
            return "iOS has not allowed Kernarium to use the microphone, so the " +
                "guest records silence. Allow it for Kernarium in Settings."
        }
    }
}
