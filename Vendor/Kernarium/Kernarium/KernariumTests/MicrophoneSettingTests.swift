// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// The Microphone setting is off by default, and only the user turning it on
/// can make iOS ask for access. Everything here runs over fakes: nothing opens
/// the microphone or can show the system's prompt.
@MainActor
struct MicrophoneSettingTests {
    private final class FakePermission: MicrophonePermission, @unchecked Sendable {
        var access: MicrophoneAccess
        private(set) var requests = 0
        private var answers: [@Sendable (MicrophoneAccess) -> Void] = []

        init(_ access: MicrophoneAccess) { self.access = access }

        func request(_ answered: @escaping @Sendable (MicrophoneAccess) -> Void) {
            requests += 1
            answers.append(answered)
        }

        /// The user's answer to the prompt that is showing.
        func answer(_ answer: MicrophoneAccess) {
            access = answer
            let pending = answers
            answers = []
            for answered in pending { answered(answer) }
        }
    }

    private final class Runtime {
        private(set) var told: [Bool] = []
        func tell(_ enabled: Bool) { told.append(enabled) }
    }

    private func scratchDefaults() -> UserDefaults {
        let name = "microphone-setting-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defaults.removePersistentDomain(forName: name)
        return defaults
    }

    private func setting(
        _ permission: FakePermission,
        _ runtime: Runtime,
        defaults: UserDefaults? = nil
    ) -> MicrophoneSetting {
        MicrophoneSetting(
            defaults: defaults ?? scratchDefaults(),
            permission: permission,
            tellRuntime: runtime.tell
        )
    }

    @Test
    func theSettingIsOffAndStartingAGuestWithItOffTouchesNothing() {
        let permission = FakePermission(.notAsked)
        let runtime = Runtime()
        let microphone = setting(permission, runtime)
        #expect(!microphone.isOn)
        microphone.restoreForGuest()
        microphone.refresh()
        #expect(permission.requests == 0)
        #expect(runtime.told.isEmpty)
    }

    @Test
    func turningItOnAsksOnceAndTheAnswerReachesTheRuntime() async throws {
        let permission = FakePermission(.notAsked)
        let runtime = Runtime()
        let microphone = setting(permission, runtime)
        microphone.setOn(true)
        #expect(microphone.isOn)
        #expect(permission.requests == 1)
        // The guest records silence until the user answers.
        #expect(runtime.told == [true])
        microphone.setOn(true)
        #expect(permission.requests == 1)

        permission.answer(.granted)
        #expect(try await eventually { microphone.access == .granted })
        #expect(runtime.told == [true, true])
    }

    /// The system's answer comes from its own queue, possibly after the
    /// setting is gone; it must reach nothing then.
    @Test
    func anAnswerThatArrivesAfterTheSettingIsGoneReachesNothing() async throws {
        let permission = FakePermission(.notAsked)
        let runtime = Runtime()
        var microphone: MicrophoneSetting? = setting(permission, runtime)
        microphone?.setOn(true)
        microphone = nil
        permission.answer(.granted)
        try await Task.sleep(for: .milliseconds(300))
        #expect(runtime.told == [true])
    }

    @Test
    func aDeniedAnswerLeavesTheSettingOnAndSaysSo() async throws {
        let permission = FakePermission(.notAsked)
        let runtime = Runtime()
        let microphone = setting(permission, runtime)
        microphone.setOn(true)
        permission.answer(.denied)
        #expect(try await eventually { microphone.access == .denied })
        #expect(microphone.isOn)
        #expect(
            MicrophoneSetting.explanation(isOn: true, access: .denied)
                .contains("not allowed")
        )
    }

    @Test
    func anAnswerThatIsAlreadyGivenIsNeverAskedForAgain() {
        for answer in [MicrophoneAccess.granted, .denied] {
            let permission = FakePermission(answer)
            let runtime = Runtime()
            let microphone = setting(permission, runtime)
            microphone.setOn(true)
            microphone.setOn(false)
            microphone.setOn(true)
            #expect(permission.requests == 0)
            #expect(microphone.access == answer)
        }
    }

    @Test
    func turningItOffTellsTheRuntimeAndStoresTheChoice() {
        let defaults = scratchDefaults()
        let permission = FakePermission(.granted)
        let runtime = Runtime()
        let microphone = setting(permission, runtime, defaults: defaults)
        microphone.setOn(true)
        #expect(defaults.bool(forKey: MicrophoneSetting.defaultsKey))
        microphone.setOn(false)
        #expect(!defaults.bool(forKey: MicrophoneSetting.defaultsKey))
        #expect(runtime.told == [true, false])
        #expect(permission.requests == 0)
    }

    /// A launch that finds the setting on carries it into the guest, and when
    /// the user has never answered it asks nothing: the guest records silence
    /// until the user turns the setting off and on again.
    @Test
    func aStoredOnSettingFollowsTheGuestWithoutAsking() {
        let defaults = scratchDefaults()
        defaults.set(true, forKey: MicrophoneSetting.defaultsKey)
        for access in [MicrophoneAccess.notAsked, .denied, .granted] {
            let permission = FakePermission(access)
            let runtime = Runtime()
            let microphone = setting(permission, runtime, defaults: defaults)
            #expect(microphone.isOn)
            microphone.restoreForGuest()
            microphone.refresh()
            #expect(runtime.told == [true, true])
            #expect(permission.requests == 0)
        }
    }

    @Test
    func aChangeMadeInTheSystemSettingsIsPickedUpWhenTheAppReturns() {
        let permission = FakePermission(.granted)
        let runtime = Runtime()
        let microphone = setting(permission, runtime)
        microphone.setOn(true)
        permission.access = .denied
        microphone.refresh()
        #expect(microphone.access == .denied)
        #expect(permission.requests == 0)
    }

    /// The whole sentence of each state, so that a concatenation that
    /// drops its tail is caught.
    @Test
    func theExplanationSaysEverythingForEachState() {
        #expect(
            MicrophoneSetting.explanation(isOn: false, access: .granted) ==
                "Off. Programs in the guest record silence, and Kernarium " +
                "does not use the microphone. This applies at once."
        )
        #expect(
            MicrophoneSetting.explanation(isOn: true, access: .granted) ==
                "On. Programs in the guest can record from this device's " +
                "microphone, and iOS shows its recording indicator while " +
                "they do. This applies at once."
        )
        #expect(
            MicrophoneSetting.explanation(isOn: true, access: .notAsked) ==
                "Waiting for your answer to iOS's request for microphone " +
                "access. Until then the guest records silence."
        )
        #expect(
            MicrophoneSetting.explanation(isOn: true, access: .denied) ==
                "iOS has not allowed Kernarium to use the microphone, so the " +
                "guest records silence. Allow it for Kernarium in Settings."
        )
        // A setting that is off says so whatever the system's answer is.
        for access in [MicrophoneAccess.notAsked, .denied, .granted] {
            #expect(
                MicrophoneSetting.explanation(isOn: false, access: access)
                    .hasSuffix("This applies at once.")
            )
        }
    }
}
