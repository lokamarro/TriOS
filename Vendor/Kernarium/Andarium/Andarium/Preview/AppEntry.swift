// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import TipKit
import UIKit

/// The Simulator preview's entry. `-screen <name>` opens any screen or state
/// directly with mock data, so a simulator driven only by `simctl` reaches
/// every flow. The arguments are the keys `fromArguments()` reads.
struct AppEntry {
    var screen: String
    /// Boot screens animate and continue into the session instead of holding.
    var play: Bool
    var lowSpace: Bool
    var resetTips: Bool
    /// The boot step a frozen boot screen shows, from 0; the screen's own
    /// when absent.
    var bootStage: Int?
    /// The Microphone switch asks the Simulator's own permission instead of
    /// taking the access as given, so `simctl privacy` and the system's
    /// alert drive it as they drive the device's.
    var systemMicrophone: Bool
    /// How many CPU readings the stats overlay and Settings' Stats rows
    /// start with, and hold: 0 is a session before its first reading. Absent,
    /// the mock readings run on.
    var statsReadings: Int?
    /// Graphics memory in use, in MiB, held with `statsReadings`: a figure
    /// near the budget turns its bar orange.
    var statsVramMiB: Int?
    /// The mock device: "iphone-18-pro-max" or "ipad-mini-6"; absent, the
    /// idiom's own mock.
    var device: String?
    /// The guest's memory, in MiB, instead of the default a new guest gets.
    var memoryMiB: Int?

    static func fromArguments() -> AppEntry {
        let defaults = UserDefaults.standard
        return AppEntry(
            screen: defaults.string(forKey: "screen") ?? "home",
            play: defaults.bool(forKey: "play"),
            lowSpace: defaults.bool(forKey: "lowSpace"),
            resetTips: defaults.bool(forKey: "resetTips"),
            bootStage: defaults.string(forKey: "bootStage").flatMap { Int($0) },
            systemMicrophone: defaults.bool(forKey: "systemMicrophone"),
            statsReadings: defaults.string(forKey: "statsReadings").flatMap { Int($0) },
            statsVramMiB: defaults.string(forKey: "statsVramMiB").flatMap { Int($0) },
            device: defaults.string(forKey: "device"),
            memoryMiB: defaults.string(forKey: "memoryMiB").flatMap { Int($0) }
        )
    }

    /// Where Home scrolls to for a screen that shows a lower section.
    private static let scrollTargets: [String: String] = [
        "home-options": "erase", "home-recovery-erase": "erase", "start-recovery": "erase", "erase": "erase",
        "home-new-recovery": "erase",
        "home-display": "display",
        "home-capabilities": "capabilities", "home-mic-on": "capabilities", "home-mic-denied": "capabilities",
        "home-mic-alert": "capabilities", "home-gyroscope-unavailable": "capabilities",
        "home-backup": "backup",
    ]

    /// Where Settings scrolls to for a screen that shows a lower section.
    private static let settingsScrolls: [String: String] = [
        "settings-stats": "stats",
    ]

    /// The page of Info a screen opens on.
    private static let infoPages: [String: InfoPage] = [
        "info-acknowledgements": .acknowledgements,
        "info-license": .license("mesa"),
        "info-license-gpl": .license("linux"),
    ]

    var hints: ScreenHints {
        ScreenHints(
            scrollTarget: Self.scrollTargets[screen],
            infoPage: Self.infoPages[screen]
        )
    }

    /// The side-keys tip. A screenshot of the tip shows it whatever the
    /// datastore says; other session screenshots leave it out.
    func configureTips() {
        if resetTips { try? Tips.resetDatastore() }
        if screen == "session-tip" {
            Tips.showAllTipsForTesting()
        } else if screen.hasPrefix("session") || screen.hasPrefix("settings") || screen.hasPrefix("recovery") || screen.hasPrefix("boot")
            || screen.hasPrefix("space-pause") || Self.operationScreens.contains(screen) {
            Tips.hideAllTipsForTesting()
        }
        try? Tips.configure([.displayFrequency(.immediate)])
    }

    func makeModel() -> AppModel {
        CaptureState.begin(screen: screen)
        let lowSpace = lowSpace || ["create-low-space", "start-low-space", "home-low-space"].contains(screen)
        let host = PreviewData.host(lowSpace: lowSpace, device: device)
        var guest: Guest? = PreviewData.sampleGuest(on: host)
        if let memoryMiB { guest?.memoryMiB = memoryMiB }
        if ["welcome", "create", "create-low-space"].contains(screen) { guest = nil }
        let model = AppModel(
            guest: guest, host: host, system: PreviewData.system, capabilities: .preview,
            createRequirementBytes: PreviewData.createRequirementBytes
        )
        let freeze = !play

        func boot(_ kind: BootKind, stage: Int, seconds: Int) {
            model.frozenBootStage = freeze ? (bootStage ?? stage) : nil
            model.frozenElapsed = seconds
            model.phase = .booting(kind)
        }

        switch screen {
        case "home-new", "home-new-recovery":
            model.guest = Guest.fresh(on: host)
            model.plan = .firstStart
        case "home-grow":
            model.guest?.pendingCapacityGiB = 24
        case "resize":
            model.resizePreset = 24
        case "home-max":
            model.guest?.capacityGiB = Limits.capacityRangeGiB.upperBound
        case "home-update":
            model.plan = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "home-update-failed":
            model.plan = .unfinishedUpdate(to: PreviewData.systemRelease, canRetry: true, canRestore: true)
            model.planAfterRestore = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "home-update-failed-restore":
            model.plan = .unfinishedUpdate(to: PreviewData.systemRelease, canRetry: false, canRestore: true)
            model.planAfterRestore = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "home-blocked":
            model.plan = .blocked(.olderAndroid)
        case "home-closed":
            model.notice = .closedInBackground
        case "home-ended":
            model.notice = .sessionEnded(SessionEndNotice(
                title: "Android's kernel panicked",
                text: "Android stopped with a kernel panic: Attempted to kill init! exitcode=0x0000000b."))
        case "home-memory-warning", "system-memory-warning":
            model.guest = PreviewData.highMemoryGuest(on: host)
        case "home-options", "home-display", "home-capabilities", "home-backup", "system-edit":
            break
        case "home-mic-on":
            model.guest?.microphone = true
            model.microphoneState = .granted
        case "home-mic-denied", "home-mic-alert":
            model.microphoneState = .denied
        case "home-gyroscope-unavailable":
            model.motionSensorsAvailable = false
        case "busy-create":
            model.busy = BusyState(title: "Creating guest", detail: "Setting up Andarium with 16 GiB.", progress: 0.42)
        case "busy-erase":
            model.busy = BusyState(title: "Erasing guest", detail: "Deleting everything the guest stored.", progress: 0.65)
        case "boot":
            model.guest?.pendingCapacityGiB = 24
            boot(.normal, stage: 3, seconds: 9)
        case "boot-single-vcpu":
            model.guest?.cores = 1
            boot(.normal, stage: 3, seconds: 9)
        case "boot-first":
            model.guest = model.guest?.erased
            model.plan = .firstStart
            boot(.firstStart, stage: 3, seconds: 74)
        case "boot-upgrade":
            model.bootFrom = PreviewData.previousSystemRelease
            boot(.upgrade, stage: 3, seconds: 51)
        case "boot-recovery":
            boot(.recovery, stage: 3, seconds: 7)
        case "boot-restart":
            boot(.restart, stage: 3, seconds: 8)
        case "session", "session-tip", "session-open", "session-keyboard", "session-stats",
             "session-power", "session-setup", "session-off", "session-left", "session-volume",
             "session-graphics-notice":
            model.session.showStats = screen == "session-stats"
            model.session.sideKeysOpen = ["session-open", "session-off", "session-left", "session-volume"].contains(screen)
            model.sessionPreset = SessionPreset(
                volumePanel: screen == "session-volume",
                powerMenu: screen == "session-power",
                leftEdge: ["session-left", "session-volume"].contains(screen),
                guestScreen: {
                    switch screen {
                    case "session-setup": .setup
                    case "session-off": .off
                    case "session-keyboard": .search
                    default: .home
                    }
                }()
            )
            model.phase = .running(.normal)
            if screen == "session-graphics-notice" { model.previewGraphicsNotice() }
        case "settings", "settings-adb-off", "settings-adb-no-address", "settings-adb", "settings-stats",
             "install-apk", "install-done", "install-failed", "install-not-apk", "install-bundle", "bug-report":
            let session = model.session
            switch screen {
            case "settings-adb":
                session.adb = true
                session.adbAddress = SessionState.previewAddress
            case "settings-adb-no-address":
                session.adb = true
            case "settings-adb-off":
                session.usbDebugging = false
            default:
                break
            }
            // The guest operations play on the full-screen overlay with
            // Settings already gone.
            let operation = Self.operationScreens.contains(screen)
            model.sessionPreset = SessionPreset(
                settings: !operation,
                settingsLarge: Self.settingsScrolls.keys.contains(screen) || screen == "settings",
                settingsScroll: Self.settingsScrolls[screen]
            )
            model.phase = .running(.normal)
            // The real operation lasts a few seconds, too short for a
            // screenshot to catch: hold its overlay part way unless `-play`.
            // A result is what the real operation leaves up, so it always
            // shows as it is.
            if operation && (freeze || Self.resultScreens.contains(screen)) {
                model.busy = Self.operationOverlay(for: screen, model: model)
            }
        case "space-pause", "space-pause-short", "space-pause-shutting-down", "space-pause-pausing",
            "space-pause-running":
            model.phase = .running(.normal)
            model.spacePause = switch screen {
            case "space-pause-short": SpacePause(freeBytes: 1_200 * Limits.bytesPerMiB, stillShort: true)
            case "space-pause-shutting-down": SpacePause(freeBytes: 820 * Limits.bytesPerMiB, working: .shuttingDown)
            case "space-pause-pausing": SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .pausing)
            case "space-pause-running": SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .notPaused)
            default: SpacePause(freeBytes: 820 * Limits.bytesPerMiB)
            }
        case "recovery", "recovery-open":
            model.session.sideKeysOpen = screen == "recovery-open"
            model.phase = .running(.recovery)
        case "failed":
            // The failure path a real run takes: a crash after 42 minutes.
            model.fail()
        default:
            break
        }
        if let statsReadings {
            model.session.readout = Readout(
                previewReadings: statsReadings, memoryMiB: model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound
            )
            if let statsVramMiB { model.session.readout.vramUsedBytes = UInt64(statsVramMiB) * Limits.bytesPerMiB }
        }
        if systemMicrophone { useSystemMicrophone(in: model) }
        return model
    }

    /// The screens that show a guest operation on the full-screen overlay.
    private static let operationScreens: Set<String> = [
        "bug-report", "install-apk", "install-done", "install-failed", "install-not-apk", "install-bundle",
    ]
    /// The ones that show how an install ended.
    private static let resultScreens: Set<String> = [
        "install-done", "install-failed", "install-not-apk", "install-bundle",
    ]

    /// A guest operation's overlay as it looks part way, or at its end: what
    /// `createBugReport` and `installPicked` show, held still.
    private static func operationOverlay(for screen: String, model: AppModel) -> BusyState {
        let cancel = BusyCancel {}
        let dismiss = BusyCancel { model.busy = nil }
        let file = PreviewData.sampleApkName
        switch screen {
        case "bug-report":
            return BusyState(
                title: "Creating bug report",
                detail: "The guest is collecting its logs and state. This can take several minutes.",
                progress: 0.6, cancel: cancel
            )
        case "install-done":
            return BusyState(
                title: "App installed", detail: "\(file) is installed in the guest.",
                result: BusyResult(failed: false, dismiss: dismiss)
            )
        case "install-failed":
            return BusyState(
                title: AppModel.installFailure,
                detail: "The guest couldn\u{2019}t install it: this app has no version for arm64 devices.",
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        case "install-not-apk":
            return BusyState(
                title: AppModel.installFailure, detail: InstallRefusal.notApk.message,
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        case "install-bundle":
            return BusyState(
                title: AppModel.installFailure, detail: InstallRefusal.bundle.message,
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        default:
            return BusyState(
                title: "Installing app",
                detail: "\(file) \u{00B7} \(Formats.size(bytes: PreviewData.sampleApkSizeBytes))",
                progress: 0.45, cancel: cancel
            )
        }
    }

    /// The device's microphone conversation against the Simulator's
    /// permission, which is read again whenever the app becomes active.
    private func useSystemMicrophone(in model: AppModel) {
        model.microphone = MicrophoneController(model: model, source: SimulatorMicrophoneSource()) { _ in }
        NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { _ in
            Task { @MainActor in model.microphone?.refresh() }
        }
    }

    /// Sheets and dialogs open after the first frame so SwiftUI presents them
    /// the way a tap would.
    @MainActor
    func applyDeferred(to model: AppModel) async {
        try? await Task.sleep(for: .milliseconds(450))
        switch screen {
        case "create", "create-low-space":
            model.sheet = .create
        case "resize":
            model.sheet = .resize
        case "system-edit", "system-memory-warning":
            model.sheet = .system
        case "home-mic-alert":
            model.setMicrophone(true)
        case "erase":
            model.confirmingErase = true
        case "info", "info-acknowledgements", "info-license", "info-license-gpl":
            model.sheet = .info
        case "start-low-space":
            model.start(.normal)
        case "alert-error":
            model.alert = AppAlert(
                title: "Settings not saved",
                message: "The file couldn\u{2019}t be saved because there is no space left on the device."
            )
        case "bug-report" where play:
            try? await Task.sleep(for: .milliseconds(450))
            model.createBugReport()
        case "install-apk" where play:
            try? await Task.sleep(for: .milliseconds(450))
            model.installPicked(PreviewData.sampleApk())
        default:
            break
        }
    }
}
