// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

#if !APP_STORE

/// The names a harness launch gives `LaunchOverrides` settings in the
/// environment (`Andarium/README.md#automation`). The `KERNARIUM_` names mean
/// what they mean in Kernarium; the `ANDARIUM_` ones are Andarium's own. An
/// App Store build compiles this file out, so it reads no launch setting.
extension LaunchOverrides {
    static func environmentName(_ setting: Setting) -> String {
        switch setting {
        case .vCPUCount: "KERNARIUM_VCPU_COUNT"
        case .memoryMiB: "KERNARIUM_GUEST_MEMORY_MIB"
        case .diskGiB: "KERNARIUM_ANDROID_DISK_SIZE_GIB"
        case .retina: "KERNARIUM_RETINA_MODE"
        case .start: "ANDARIUM_START"
        case .bootMode: "KERNARIUM_ANDROID_BOOT_MODE"
        case .erase: "ANDARIUM_ERASE"
        case .stats: "ANDARIUM_STATS"
        case .instance: "ANDARIUM_GUEST_INSTANCE"
        }
    }

    init(environment: [String: String], processorCount: Int) {
        self.init(processorCount: processorCount) { environment[Self.environmentName($0)] }
    }
}

extension AppEntry {
    /// The file in an isolated guest instance's directory whose number, in
    /// mebibytes, stands for the device's free space while it exists, so that
    /// a harness launch drives the run's low-space pause without filling the
    /// disk. Only an isolated instance reads it, and an App Store build
    /// compiles it out.
    static let harnessFreeSpaceFile = "free-space-mib"

    static func harnessFreeSpace(
        in directory: URL, fallback: @escaping @Sendable () -> UInt64
    ) -> @Sendable () -> UInt64 {
        let file = directory.appendingPathComponent(harnessFreeSpaceFile)
        return {
            guard let text = try? String(contentsOf: file, encoding: .utf8),
                let mebibytes = UInt64(text.trimmingCharacters(in: .whitespacesAndNewlines))
            else { return fallback() }
            return mebibytes * Limits.bytesPerMiB
        }
    }

    /// The file in an isolated instance's directory that answers the screen
    /// over a guest paused for space as the person would: `resume`,
    /// `shutdown` or `check`. The harness has no finger, so the application
    /// takes the answer when the screen offers it, and removes the file.
    static let harnessSpaceAnswerFile = "space-answer"

    @MainActor
    static func answerSpaceScreenFromHarness(model: AppModel, directory: URL) {
        let file = directory.appendingPathComponent(harnessSpaceAnswerFile)
        Task { @MainActor [weak model] in
            while !Task.isCancelled, let model {
                try? await Task.sleep(for: .seconds(1))
                guard let text = try? String(contentsOf: file, encoding: .utf8),
                    let stage = model.spacePause?.stage, model.spacePause?.working == nil
                else { continue }
                let answer = text.trimmingCharacters(in: .whitespacesAndNewlines)
                switch (answer, stage) {
                case ("resume", .paused): model.resumeAfterSpacePause()
                case ("shutdown", .paused), ("shutdown", .notPaused): model.shutDownAfterSpacePause()
                case ("check", .notPaused): model.checkSpaceAfterPauseFailed()
                default: continue
                }
                print("Andarium: the harness answered the space screen: \(answer)")
                try? FileManager.default.removeItem(at: file)
            }
        }
    }
}

#endif
