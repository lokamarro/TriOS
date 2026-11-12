// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import Foundation
import GuestHostCore
import MetalKit
import SwiftUI

struct ContentView: View {
    @StateObject private var serialConsole = SerialConsoleController()
    @ObservedObject private var launcher = EmulatorLauncher.shared
    @ObservedObject private var backgroundSession = BackgroundSession.shared
    @ObservedObject private var backgroundJob = BackgroundJob.shared
    @ObservedObject private var spacePause = GuestSpacePause.shared
    /// Whether the running guest chose the keep-running mode, which is what
    /// offers the button that starts its job.
    @State private var keepRunningChosen = false
    @State private var guestViewMode = GuestViewMode.defaultMode
    /// Whether the framebuffer holds the keyboard. Only the framebuffer
    /// mode sets it; the serial console's keyboard is its terminal's focus.
    @State private var framebufferKeyboardVisible = false
    @State private var specialKeysExpanded = false
    @State private var settingsPresented = false
    /// Whether the window scene fills the screen. Until the view is in a
    /// scene, it is taken to be a window, which is the system's own default.
    @State private var sceneCoversScreen = false
    @Namespace private var floatingButtonNamespace

    var body: some View {
        ZStack(alignment: .trailing) {
            ZStack {
                GuestPictureInPictureAnchor()
                    .allowsHitTesting(false)
                    .accessibilityHidden(true)

                GuestDisplayView(
                    keyboardVisible: $framebufferKeyboardVisible,
                    retinaMode: EmulatorLauncher.shared.retinaMode
                ) { view, geometry in
                    EmulatorLauncher.shared.start(view: view, geometry: geometry)
                }
                .opacity(guestViewMode == .framebuffer ? 1 : 0)
                .allowsHitTesting(guestViewMode == .framebuffer)
                .accessibilityHidden(guestViewMode != .framebuffer)
                .accessibilityIdentifier("framebufferView")

                SerialConsoleView(
                    controller: serialConsole,
                    isVisible: guestViewMode == .serialConsole
                )
                    .opacity(guestViewMode == .serialConsole ? 1 : 0)
                    .allowsHitTesting(guestViewMode == .serialConsole)
                    .accessibilityHidden(guestViewMode != .serialConsole)

                if let shortage = launcher.startShortage {
                    StartShortageNotice(shortage: shortage) {
                        launcher.retryStart()
                    }
                    .padding()
                } else if case let .ended(result) = launcher.phase {
                    GuestEndedNotice(
                        end: GuestRunEnd(
                            result: result,
                            requested: launcher.stopRequested
                        )
                    )
                        .padding()
                } else if let warning = launcher.gpuBudgetWarning {
                    GPUBudgetWarningNotice(warning: warning) {
                        launcher.dismissGPUBudgetWarning()
                    }
                    .padding()
                } else if launcher.restarting {
                    GuestRestartingBadge()
                        .padding()
                        .frame(maxHeight: .infinity, alignment: .top)
                } else if let restart = backgroundSession.restart {
                    GuestRestartedNotice(restart: restart) {
                        backgroundSession.dismissRestartNotice()
                    }
                    .padding()
                    .frame(maxHeight: .infinity, alignment: .top)
                }
            }
            // In a window, iPadOS draws its window controls over the top-left
            // corner of the content, which would cover the guest's own top-left
            // (Android's status bar). Pushing the guest below them is the
            // system's job: it knows where the controls are for the window's
            // placement. A scene that covers the whole screen has none over
            // its content, and the system's corner offset can outlive the
            // window it described, so the offset applies only to a scene
            // smaller than the screen.
            .containerCornerOffset(
                SceneCoverage.cornerOffsetEdges(coversScreen: sceneCoversScreen),
                sizeToFit: true
            )
            floatingButtons
        }
        .ignoresSafeArea(.keyboard)
        .overlay {
            // Over the guest and its controls: the guest is not running.
            if let pause = spacePause.pause {
                SpacePauseOverlay(
                    pause: pause,
                    resume: { spacePause.resume() },
                    shutDown: { spacePause.shutDown() },
                    checkAgain: { spacePause.checkAgain() }
                )
            }
        }
        .animation(
            .easeInOut(duration: 0.2), value: spacePause.pause != nil
        )
        .background {
            SceneCoverageReader { sceneCoversScreen = $0 }
        }
        .sheet(isPresented: $settingsPresented) {
            EmulatorSettingsView()
        }
        .onAppear {
            serialConsole.startObserving()
            let window = GuestPictureInPicture.shared
            window.wanted = {
                // A guest paused for space is not shown in a window.
                !GuestSpacePause.shared.holdsGuest &&
                    EmulatorLauncher.shared.runningGuestName.map {
                        BackgroundSettings().mode(forGuest: $0)
                    } == .pictureInPicture
            }
            GuestPersistence.shared.startObserving(
                guest: { EmulatorLauncher.shared.runningGuestName },
                pictureInPicture: window
            )
        }
        .onDisappear {
            serialConsole.stopObserving()
            GuestPersistence.shared.stopObserving()
            GuestPictureInPicture.shared.guestRunningChanged(false)
            EmulatorLauncher.shared.stop()
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: UIApplication.didBecomeActiveNotification
            )
        ) { _ in
            MicrophoneSetting.shared.refresh()
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: UserDefaults.didChangeNotification
            )
        ) { _ in
            GuestPictureInPicture.shared.refresh()
            refreshKeepRunning()
        }
        .onChange(of: spacePause.pause != nil) { _, paused in
            // The screen over the guest says why it is paused, and a sheet
            // or a keyboard would cover it.
            guard paused else { return }
            settingsPresented = false
            framebufferKeyboardVisible = false
            serialConsole.resignFirstResponder()
        }
        .onChange(of: launcher.phase) { _, phase in
            GuestPictureInPicture.shared.guestRunningChanged(phase == .running)
            refreshKeepRunning()
            if phase == .running { startJobIfAutomationAsked() }
            // Nothing is left to type into, and the keyboard would cover
            // the notice.
            guard case .ended = phase else { return }
            framebufferKeyboardVisible = false
            serialConsole.resignFirstResponder()
        }
    }

    @ViewBuilder
    private var floatingButtons: some View {
        GlassEffectContainer(spacing: FloatingControlMetrics.spacing) {
            floatingButtonsContent
        }
    }

    private var floatingButtonsContent: some View {
        VStack(alignment: .trailing, spacing: FloatingControlMetrics.spacing) {
            settingsButton
            if keepRunningChosen && launcher.phase == .running {
                keepRunningButton
            }
            displayModeButton
            keyboardButton
            specialKeysRow
        }
    }

    /// Starts, or stops, the job that keeps the guest running once the user
    /// leaves Kernarium. iOS wants such a job to follow the user's own action,
    /// so it never starts by itself.
    private var keepRunningButton: some View {
        floatingIconButton(
            systemName: backgroundJob.isRunning ? "stop.circle" : "timer",
            accessibilityLabel: keepRunningLabel,
            identifier: "backgroundJobButton"
        ) {
            if backgroundJob.state == .idle {
                startKeepRunningJob()
            } else {
                backgroundJob.cancel()
            }
        }
    }

    /// Submits the job for the running guest, as the button does.
    private func startKeepRunningJob() {
        guard let guest = launcher.runningGuestName else { return }
        backgroundJob.start(
            limitMinutes: BackgroundSettings().limitMinutes(forGuest: guest),
            guestName: (guest as NSString).deletingPathExtension
        )
    }

    /// An automation launch can ask for the job as the guest starts, the one
    /// request iOS accepts while the application is in front, so a driver
    /// without the UI automation host can reach a guest that runs behind it.
    /// Only a runtime built for automation answers yes, and only a guest in
    /// the keep-running mode takes a job.
    private func startJobIfAutomationAsked() {
        guard BackgroundJob.startsWithGuest(
            asked: kernarium_runtime_loader_automation_job_at_start() != 0,
            mode: keepRunningChosen ? .keepRunning : .off,
            state: backgroundJob.state
        ) else { return }
        print("Kernarium: automation starts the background job")
        startKeepRunningJob()
    }

    private var keepRunningLabel: String {
        switch backgroundJob.state {
        case .idle: "Keep the guest running in the background"
        case .submitted: "Starting to keep the guest running"
        case .running: "Stop keeping the guest running"
        }
    }

    private func refreshKeepRunning() {
        let mode = launcher.runningGuestName.map {
            BackgroundSettings().mode(forGuest: $0)
        } ?? .off
        keepRunningChosen = mode == .keepRunning
        backgroundJob.reconcile(mode: mode)
    }

    private var displayModeButton: some View {
        floatingIconButton(
            systemName: guestViewMode == .framebuffer ? "terminal" : "display",
            accessibilityLabel: guestViewMode == .framebuffer
                ? "Show serial console"
                : "Show framebuffer",
            identifier: "displayModeButton",
            action: toggleGuestViewMode
        )
    }

    private var settingsButton: some View {
        floatingIconButton(
            systemName: "gearshape",
            accessibilityLabel: "Emulator settings",
            identifier: "settingsButton"
        ) {
            settingsPresented = true
        }
    }

    /// Whether the view on screen holds the keyboard.
    private var keyboardVisible: Bool {
        switch guestViewMode {
        case .framebuffer: framebufferKeyboardVisible
        case .serialConsole: serialConsole.isFocused
        }
    }

    private var keyboardButton: some View {
        floatingIconButton(
            systemName: keyboardVisible
                ? "keyboard.chevron.compact.down"
                : "keyboard",
            accessibilityLabel: keyboardVisible
                ? "Hide keyboard"
                : "Show keyboard",
            identifier: "keyboardButton",
            action: toggleKeyboard
        )
    }

    private var specialKeysRow: some View {
        HStack(spacing: FloatingControlMetrics.spacing) {
            if specialKeysExpanded {
                ForEach(GuestSpecialKey.allCases) { key in
                    specialKeyButton(key)
                }
                .transition(.move(edge: .trailing).combined(with: .opacity))
            }
            floatingIconButton(
                systemName: specialKeysExpanded
                    ? "chevron.right"
                    : "chevron.left",
                accessibilityLabel: specialKeysExpanded
                    ? "Hide special keys"
                    : "Show special keys",
                identifier: "specialKeysButton"
            ) {
                withAnimation {
                    specialKeysExpanded.toggle()
                }
            }
        }
    }

    private func specialKeyButton(_ key: GuestSpecialKey) -> some View {
        floatingIconButton(
            systemName: key.systemName,
            accessibilityLabel: key.accessibilityLabel,
            identifier: key.accessibilityIdentifier
        ) {
            key.press()
        }
    }

    @ViewBuilder
    private func floatingIconButton(
        systemName: String,
        accessibilityLabel: String,
        identifier: String,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            floatingIcon(systemName: systemName)
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .circle)
        .glassEffectID(identifier, in: floatingButtonNamespace)
        .accessibilityLabel(accessibilityLabel)
        .accessibilityIdentifier(identifier)
    }

    private func floatingIcon(systemName: String) -> some View {
        Image(systemName: systemName)
            .frame(
                width: FloatingControlMetrics.buttonSide,
                height: FloatingControlMetrics.buttonSide
            )
            .contentShape(Circle())
    }

    /// Raises or puts away the keyboard of the view on screen, so typed
    /// text reaches what the user is looking at.
    private func toggleKeyboard() {
        switch guestViewMode {
        case .framebuffer:
            framebufferKeyboardVisible.toggle()
        case .serialConsole:
            if serialConsole.isFocused {
                serialConsole.resignFirstResponder()
            } else {
                serialConsole.becomeFirstResponder()
            }
        }
    }

    /// A mode switch puts the keyboard away: the view that held it is
    /// hidden, and no hidden view keeps keyboard focus.
    private func toggleGuestViewMode() {
        switch guestViewMode {
        case .framebuffer:
            framebufferKeyboardVisible = false
            guestViewMode = .serialConsole
        case .serialConsole:
            serialConsole.resignFirstResponder()
            guestViewMode = .framebuffer
        }
    }
}

enum GuestViewMode {
    case framebuffer
    case serialConsole

    static let defaultMode: Self = .framebuffer
}

/// How this launch's guest run ended, as the application reports it.
struct GuestRunEnd: Equatable {
    let result: Int32
    /// The application stopped the run, as Shut down on the screen over a
    /// guest paused for space does. An Android run the host stops returns an
    /// error status by contract, because only the guest's own power-off
    /// leaves a process another run may start in, and the person who asked for
    /// the stop did not meet an error.
    var requested = false

    private var shutDown: Bool { result == 0 || requested }

    /// The runtime stopped a guest that restarted itself again and again
    /// within seconds of booting. A guest that restarts once in a while is
    /// booted again by the runtime and never reaches this notice.
    var restartedInALoop: Bool {
        result == Int32(KERNARIUM_RUNTIME_RESULT_RESTART_LOOP)
    }

    var title: String {
        if restartedInALoop { return "Guest kept restarting" }
        return shutDown ? "Guest shut down" : "Guest stopped with an error"
    }

    var message: String {
        let relaunch = "Relaunch Kernarium to start it again."
        if restartedInALoop {
            return "The guest restarted itself again and again within " +
                "seconds of booting, so Kernarium stopped booting it. " +
                relaunch
        }
        return shutDown
            ? relaunch
            : "The runtime returned \(result). \(relaunch)"
    }

    var systemName: String {
        if restartedInALoop { return "arrow.trianglehead.2.clockwise.rotate.90" }
        return shutDown ? "power" : "exclamationmark.triangle"
    }
}

/// Shown over the guest views once the run has ended. The guest's last frame
/// and console stay readable behind it, and the floating controls still
/// switch between them.
private struct GuestEndedNotice: View {
    let end: GuestRunEnd

    var body: some View {
        GuestNoticeCard(
            systemName: end.systemName,
            title: end.title,
            message: end.message
        )
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("guestEndedNotice")
    }
}

/// Shown over the guest views when the device has too little free space for
/// the guest to start. Nothing started; Check again tries once more.
private struct StartShortageNotice: View {
    let shortage: StartSpace
    let checkAgain: () -> Void

    var body: some View {
        VStack(spacing: 14) {
            GuestNoticeCard(
                systemName: "externaldrive.badge.exclamationmark",
                title: StartSpace.title,
                message: shortage.message(device: EmulatorLauncher.deviceName)
                    + " " + shortage.advice(device: EmulatorLauncher.deviceName),
                messageStyle: .primary
            )
            Button("Check again", action: checkAgain)
                .buttonStyle(.borderedProminent)
                .accessibilityIdentifier("startShortageCheckAgainButton")
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("startShortageNotice")
    }
}

/// Shown over the guest views while a guest that restarted itself boots
/// again. It takes no input and goes by itself when the new guest's console
/// answers.
private struct GuestRestartingBadge: View {
    var body: some View {
        HStack(spacing: 8) {
            ProgressView()
            Text("Restarting")
                .font(.subheadline)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .glassEffect(.regular, in: .capsule)
        .allowsHitTesting(false)
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("guestRestartingBadge")
    }
}

/// The warning shown at launch when the application's memory limit leaves the
/// host GPU less than its 384 MiB floor once the guest, the gadget cache and
/// the launch's allowances are counted. The guest boots anyway, with the
/// floor, and the system is likely to end the application.
struct GPUBudgetWarning: Equatable {
    let limitMiB: Int
    let guestMemoryMiB: Int
    let availableMiB: Int
    let floorMiB: Int

    /// Nil unless the runtime found the budget short of its floor.
    init?(plan: kernarium_runtime_gpu_budget) {
        guard plan.short_of_floor != 0 else { return nil }
        func mebibytes(_ bytes: UInt64) -> Int {
            Int(clamping: bytes / (1 << 20))
        }
        limitMiB = mebibytes(plan.process_limit_bytes)
        guestMemoryMiB = mebibytes(plan.guest_memory_bytes)
        availableMiB = mebibytes(plan.available_bytes)
        // The runtime clamps to the floor unless half the guest is less.
        floorMiB = mebibytes(plan.budget_bytes)
    }

    var title: String { "Memory may run short" }

    var message: String {
        "The system lets Kernarium use \(limitMiB) MiB. After the guest's " +
            "\(guestMemoryMiB) MiB and what the emulator keeps beside it, " +
            "\(availableMiB) MiB are left for graphics, and graphics are " +
            "given \(floorMiB) MiB at least. The guest starts anyway, and " +
            "the system is likely to end Kernarium. Giving the guest less " +
            "memory in Emulator settings avoids it."
    }

    var systemName: String { "exclamationmark.triangle" }
}

/// Shown over the guest views from the launch until dismissed. The guest boots
/// behind it.
private struct GPUBudgetWarningNotice: View {
    let warning: GPUBudgetWarning
    let dismiss: () -> Void

    var body: some View {
        Button(action: dismiss) {
            GuestNoticeCard(
                systemName: warning.systemName,
                title: warning.title,
                message: warning.message,
                messageStyle: .primary,
                footer: "Tap to dismiss"
            )
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("gpuBudgetWarningNotice")
    }
}

/// Shown over the guest views when the previous launch ended in the
/// background and this launch's guest booted again. It stays until dismissed:
/// the guest's boot is under way behind it, and what was running before is
/// gone.
private struct GuestRestartedNotice: View {
    let restart: GuestRestart
    let dismiss: () -> Void

    var body: some View {
        Button(action: dismiss) {
            GuestNoticeCard(
                systemName: restart.systemName,
                title: restart.title,
                message: restart.message,
                messageStyle: .primary,
                footer: "Tap to dismiss"
            )
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("guestRestartedNotice")
    }
}

/// The card both notices share: an icon, a headline and a message, with an
/// optional line beneath.
private struct GuestNoticeCard: View {
    let systemName: String
    let title: String
    let message: String
    var messageStyle = HierarchicalShapeStyle.secondary
    var footer: String?

    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: systemName)
                .font(.title)
            Text(title)
                .font(.headline)
            Text(message)
                .font(.subheadline)
                .foregroundStyle(messageStyle)
                .multilineTextAlignment(.center)
            if let footer {
                Text(footer)
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(20)
        .frame(maxWidth: 320)
        .glassEffect(.regular, in: .rect(cornerRadius: 20))
    }
}

enum FloatingControlMetrics {
    static let buttonSide: CGFloat = 32
    static let spacing: CGFloat = 8
}

enum GuestSpecialKey: UInt16, CaseIterable, Identifiable {
    case power = 116
    case volumeUp = 115
    case volumeDown = 114
    case escape = 1
    case tab = 15
    case home = 102

    var id: Self { self }
    var code: UInt16 { rawValue }

    /// Presses and releases the key.
    func press() {
        kernarium_runtime_loader_send_key(code, 1)
        kernarium_runtime_loader_send_key(code, 0)
    }

    var systemName: String {
        switch self {
        case .power: "power"
        case .volumeUp: "speaker.wave.3"
        case .volumeDown: "speaker.wave.1"
        case .escape: "escape"
        case .tab: "arrow.right.to.line"
        case .home: "house"
        }
    }

    var accessibilityLabel: String {
        switch self {
        case .power: "Power"
        case .volumeUp: "Volume up"
        case .volumeDown: "Volume down"
        case .escape: "Escape"
        case .tab: "Tab"
        case .home: "Home"
        }
    }

    var accessibilityIdentifier: String {
        switch self {
        case .power: "guestPowerButton"
        case .volumeUp: "guestVolumeUpButton"
        case .volumeDown: "guestVolumeDownButton"
        case .escape: "guestEscapeButton"
        case .tab: "guestTabButton"
        case .home: "guestHomeButton"
        }
    }
}

enum OverlayDisk {
    static let filename = "kernarium-overlay.ext4"

    static func byteCount(forSizeGiB sizeGiB: Int) throws -> UInt64 {
        guard EmulatorConfiguration.overlaySizeRangeGiB.contains(sizeGiB) else {
            throw CocoaError(.fileWriteInvalidFileName)
        }
        return UInt64(sizeGiB) * 1024 * 1024 * 1024
    }

    static func recreate(
        sizeGiB: Int,
        in directory: URL = FileManager.default.temporaryDirectory
    ) throws -> URL {
        let manager = FileManager.default
        let disk = directory.appendingPathComponent(filename)
        let requestedByteCount = try byteCount(forSizeGiB: sizeGiB)
        if manager.fileExists(atPath: disk.path) {
            try manager.removeItem(at: disk)
        }
        guard manager.createFile(atPath: disk.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        do {
            let handle = try FileHandle(forWritingTo: disk)
            defer { try? handle.close() }
            try handle.truncate(atOffset: requestedByteCount)
        } catch {
            try? manager.removeItem(at: disk)
            throw error
        }
        return disk
    }
}

enum ThermalSamplePhase: String {
    case start
    case change
    case finish
}

enum ThermalStateReport {
    static func name(for state: ProcessInfo.ThermalState) -> String {
        switch state {
        case .nominal:
            return "nominal"
        case .fair:
            return "fair"
        case .serious:
            return "serious"
        case .critical:
            return "critical"
        @unknown default:
            return "unknown"
        }
    }

    static func line(
        phase: ThermalSamplePhase,
        state: ProcessInfo.ThermalState,
        uptime: TimeInterval
    ) -> String {
        let uptimeText = String(
            format: "%.3f",
            locale: Locale(identifier: "en_US_POSIX"),
            uptime
        )
        return "Kernarium thermal state: phase=\(phase.rawValue) " +
            "state=\(name(for: state)) uptime=\(uptimeText)"
    }
}

/// The paths one boot needs, in the order the boot request declares them.
/// They are held as Swift strings and only become C pointers for the duration
/// of the run, which spans the entire guest lifetime.
private struct PreparedBoot: Sendable {
    private let paths: [String?]
    let androidBootMode: AndroidBootMode
    /// The data disk's capacity: the runtime creates a missing disk this size
    /// and grows a smaller one to it. 0 leaves an existing disk alone.
    let androidDataGiB: UInt32

    init(rootfs: URL, overlay: URL) {
        paths = [rootfs.path, overlay.path, nil, nil]
        androidBootMode = .automatic
        androidDataGiB = 0
    }

    init(
        bundle: GuestBundle,
        androidBootMode: AndroidBootMode,
        androidDataGiB: UInt32
    ) {
        paths = [
            nil,
            nil,
            bundle.dataDisk.path,
            bundle.url.path,
        ]
        self.androidBootMode = androidBootMode
        self.androidDataGiB = androidDataGiB
    }

    func withCPaths<Result>(
        _ body: ([UnsafePointer<CChar>?]) -> Result
    ) -> Result {
        let buffers = paths.map { path in
            path.map { ContiguousArray($0.utf8CString) }
        }
        var pointers = [UnsafePointer<CChar>?](
            repeating: nil, count: buffers.count
        )
        func bind(_ index: Int) -> Result {
            guard index < buffers.count else { return body(pointers) }
            guard let buffer = buffers[index] else { return bind(index + 1) }
            return buffer.withUnsafeBufferPointer { raw in
                pointers[index] = raw.baseAddress
                defer { pointers[index] = nil }
                return bind(index + 1)
            }
        }
        return bind(0)
    }
}

/// A, E_rest and S of the host GPU's budget, in MiB, as a launch states them.
struct GPUBudgetParameters: Equatable {
    var applicationMiB: UInt32 = 0
    var engineMiB: UInt32 = 0
    var headroomMiB: UInt32 = 0

    init(configuration: EmulatorConfiguration) {
        applicationMiB = UInt32(clamping: configuration.gpuBudgetApplicationMiB)
        engineMiB = UInt32(clamping: configuration.gpuBudgetEngineMiB)
        headroomMiB = UInt32(clamping: configuration.gpuBudgetHeadroomMiB)
    }
}

private func runGuest(
    view: GuestMetalView,
    boot: PreparedBoot,
    width: UInt32,
    height: UInt32,
    dotsPerInch: UInt32,
    guestMemoryMiB: UInt32,
    vCPUCount: UInt32,
    autoResizeGuest: Bool,
    gpuBudget: GPUBudgetParameters
) -> Int32 {
    boot.withCPaths { paths in
        var request = kernarium_runtime_boot()
        request.struct_size = UInt32(MemoryLayout<kernarium_runtime_boot>.size)
        request.width = width
        request.height = height
        request.dots_per_inch = dotsPerInch
        request.guest_memory_mib = guestMemoryMiB
        request.guest_processor_count = vCPUCount
        request.android_boot_mode = boot.androidBootMode.rawValue
        request.auto_resize_guest = autoResizeGuest ? 1 : 0
        request.gpu_budget_application_mib = gpuBudget.applicationMiB
        request.gpu_budget_engine_mib = gpuBudget.engineMiB
        request.gpu_budget_headroom_mib = gpuBudget.headroomMiB
        request.rootfs_path = paths[0]
        request.overlay_path = paths[1]
        request.android_data_disk_path = paths[2]
        request.android_release_path = paths[3]
        request.android_data_gib = boot.androidDataGiB
        return withUnsafePointer(to: &request) { pointer in
            kernarium_runtime_loader_run(view, pointer)
        }
    }
}

/// Where this launch's guest is. A launch makes one run, and the runtime boots
/// a guest that restarts itself again inside it, so the phase stays `running`
/// across restarts. Once the run returns the guest is over: it powered off, was
/// stopped, failed, or kept restarting, and the application can only ask to be
/// relaunched.
enum GuestRunPhase: Equatable {
    /// No run has started. A failure before the run begins, such as a
    /// missing rootfs, leaves the launch here, and a later layout retries.
    case idle
    case running
    /// The run returned `result`; nothing starts another in this process.
    case ended(result: Int32)
}

@MainActor
final class EmulatorLauncher: ObservableObject, SpaceRun {
    static let shared: EmulatorLauncher = EmulatorLauncher(
        configuration: .load(), space: { GuestSpacePause.shared }
    )

    private let configuration: EmulatorConfiguration
    /// The job that keeps the guest running in the background, which ends
    /// with the run.
    private let job: BackgroundJob
    @Published private(set) var phase = GuestRunPhase.idle
    /// Whether the guest is between two boots.
    @Published private(set) var restarting = false
    /// Set at launch when the application's memory limit leaves the host GPU
    /// less than its floor; the guest boots regardless.
    @Published private(set) var gpuBudgetWarning: GPUBudgetWarning?
    /// Set while the device has too little free space for the guest to start.
    /// Nothing started: the guest starts once there is room and the person
    /// asks again.
    @Published private(set) var startShortage: StartSpace?
    private var restartIndicator = GuestRestartIndicator()
    /// The name the running guest's own settings are stored under.
    private(set) var runningGuestName: String?
    private(set) var runningGuestIsAndroid = false
    /// The application asked the run to stop, which is how a run the person
    /// ended ends.
    private(set) var stopRequested = false
    private var thermalObserver: NSObjectProtocol?
    /// The view and geometry of a start the free space refused, for asking
    /// again.
    private var refusedStart: (GuestMetalView, GuestDisplayGeometry)?
    private let freeSpace: @Sendable () -> UInt64
    /// What pauses the guest when the device runs short of space while it
    /// runs; the unit tests' launchers have none.
    private let space: (() -> GuestSpacePause)?

    var retinaMode: Bool { configuration.retinaMode }

    /// The unit tests make launchers of their own, with a job of their own.
    init(
        configuration: EmulatorConfiguration,
        job: BackgroundJob? = nil,
        freeSpace: @escaping @Sendable () -> UInt64 = {
            SpaceHarness.current.freeSpace()
        },
        space: (() -> GuestSpacePause)? = nil
    ) {
        self.configuration = configuration
        self.job = job ?? .shared
        self.freeSpace = freeSpace
        self.space = space
    }

    // MARK: SpaceRun

    var isActive: Bool { phase == .running && !restarting }

    var guestName: String { runningGuestIsAndroid ? "Android" : "Linux" }

    fileprivate func start(
        view: GuestMetalView,
        geometry: GuestDisplayGeometry
    ) {
        guard phase == .idle else { return }
        guard let rootfsFilename = RootfsImage.selectedFilename(
            configuredFilename: configuration.rootfsFilename
        ) else {
            print("Kernarium: invalid rootfs filename")
            return
        }
        guard let selection = RootfsImage.selection(named: rootfsFilename) else {
            print("Kernarium: Documents/\(rootfsFilename) is missing")
            return
        }
        if let shortage = Self.startShortage(
            for: selection, freeBytes: freeSpace()
        ) {
            // Every layout of the guest view asks to start while the run is
            // idle, so a refusal is said, and published, once.
            if startShortage == nil {
                print(
                    "Kernarium: not enough free space to start: " +
                        "\(shortage.message(device: Self.deviceName))"
                )
            }
            refusedStart = (view, geometry)
            if startShortage != shortage { startShortage = shortage }
            return
        }
        refusedStart = nil
        if startShortage != nil { startShortage = nil }
        runningGuestName = rootfsFilename
        runningGuestIsAndroid = selection.flavor == .android
        guard kernarium_runtime_loader_select(
            selection.flavor.rawValue
        ) == 0 else {
            print("Kernarium: cannot load the runtime")
            return
        }
        // A guest that restarts itself is booted again inside the run; the
        // runtime says when, so the application can show it and apply what
        // belongs to a kernel to the new one.
        // The loader delivers each event on the main queue.
        kernarium_runtime_loader_set_restart_handler {
            @Sendable event, restartCount in
            guard let restart = GuestRestartEvent(
                event: event, restartCount: restartCount
            ) else { return }
            MainActor.assumeIsolated {
                EmulatorLauncher.shared.guestRestarted(restart)
            }
        }
        // A Microphone setting the user turned on earlier follows the guest;
        // a setting that is off leaves the microphone alone.
        MicrophoneSetting.shared.restoreForGuest()
        let boot: PreparedBoot
        switch selection {
        case let .image(rootfs, _):
            guard let prepared = prepareImageBoot(rootfs: rootfs) else { return }
            boot = prepared
        case let .bundle(bundle):
            guard let prepared = prepareBundleBoot(bundle: bundle) else { return }
            boot = prepared
        }
        // The budget the runtime will give the host GPU is evaluated before
        // the guest boots, so a launch that cannot afford it says so while it
        // starts; it is not refused.
        let gpuBudget = GPUBudgetParameters(configuration: configuration)
        var plan = kernarium_runtime_gpu_budget()
        plan.struct_size = UInt32(
            MemoryLayout<kernarium_runtime_gpu_budget>.size
        )
        if kernarium_runtime_loader_plan_gpu_budget(
            UInt32(configuration.guestMemoryMiB),
            UInt32(configuration.vCPUCount),
            gpuBudget.applicationMiB,
            gpuBudget.engineMiB,
            gpuBudget.headroomMiB,
            &plan
        ) == 0 {
            gpuBudgetWarning = GPUBudgetWarning(plan: plan)
        }
        phase = .running
        space?().runStarted()
        startThermalObservation()
        let size = GuestInputCoordinateSpace.boundedDisplaySize(
            geometry.pixelSize
        )
        let width = UInt32(size.width.rounded(.down))
        let height = UInt32(size.height.rounded(.down))
        view.setGuestDisplay(width: width, height: height)
        UIApplication.shared.isIdleTimerDisabled = true
        let guestMemoryMiB = UInt32(configuration.guestMemoryMiB)
        let vCPUCount = UInt32(configuration.vCPUCount)
        let dotsPerInch = geometry.dotsPerInch
        let autoResizeGuest = configuration.autoResizeGuest
        // The run occupies its thread for the guest's whole life and the GPU
        // service reaches the main queue while it starts, so it must not be the
        // main thread. Own the thread outright rather than depending on where a
        // task happens to be scheduled, and keep it off the cooperative pool it
        // would otherwise block for hours. It inherits the main thread's
        // SIGALRM block, which the runtime then reasserts for itself.
        let worker = Thread {
            let result = runGuest(
                view: view,
                boot: boot,
                width: width,
                height: height,
                dotsPerInch: dotsPerInch,
                guestMemoryMiB: guestMemoryMiB,
                vCPUCount: vCPUCount,
                autoResizeGuest: autoResizeGuest,
                gpuBudget: gpuBudget
            )
            Task { @MainActor in
                EmulatorLauncher.shared.finished(result: result)
            }
        }
        worker.name = "uml-launcher"
        worker.start()
    }

    /// A plain image boots read-only under a disposable overlay disk, which is
    /// recreated every launch and holds no state worth keeping.
    private func prepareImageBoot(rootfs: URL) -> PreparedBoot? {
        if rootfs.pathExtension.lowercased() == "ext4" {
            do {
                try FileManager.default.setAttributes(
                    [.posixPermissions: 0o444],
                    ofItemAtPath: rootfs.path
                )
            } catch {
                print("Kernarium: cannot seal the ext4 rootfs read-only")
                return nil
            }
        }
        do {
            let overlay = try OverlayDisk.recreate(
                sizeGiB: configuration.overlaySizeGiB
            )
            return PreparedBoot(rootfs: rootfs, overlay: overlay)
        } catch {
            print(
                "Kernarium: cannot create " +
                "\(configuration.overlaySizeGiB) GiB overlay disk: \(error)"
            )
            return nil
        }
    }

    /// A bundle keeps its writable data disk beside its payload, so guest
    /// state survives the app being closed and reopened. The runtime creates
    /// and grows that disk.
    private func prepareBundleBoot(bundle: GuestBundle) -> PreparedBoot? {
        guard FileManager.default.isWritableFile(atPath: bundle.url.path) else {
            print("Kernarium: \(bundle.name) is not writable")
            return nil
        }
        return PreparedBoot(
            bundle: bundle,
            androidBootMode: configuration.androidBootMode,
            androidDataGiB: UInt32(configuration.androidDiskSizeGiB)
        )
    }

    fileprivate func guestRestarted(_ event: GuestRestartEvent) {
        guard phase == .running else { return }
        restartIndicator.apply(event)
        restarting = restartIndicator.restarting
        print("Kernarium: \(event)")
        if case .booted = event {
            // What the policy applied to the old kernel belongs to a kernel.
            GuestPersistence.shared.background?.guestDidRestart()
        }
    }

    func finished(result: Int32) {
        space?().runEnded()
        stopThermalObservation()
        restartIndicator.runEnded()
        restarting = false
        phase = .ended(result: result)
        UIApplication.shared.isIdleTimerDisabled = false
        print("Kernarium: runtime returned \(result)")
        // The job keeps a guest running in the background. A guest whose run
        // is over has nothing left to keep, whichever way it ended, and a job
        // that went on would hold the application alive for no reason.
        job.guestRunEnded(result: result)
    }

    func stop() {
        stopRequested = true
        kernarium_runtime_loader_request_stop()
    }

    /// What a start lacks in free space, nil when there is enough. A guest's
    /// disk grows as it writes: a start needs room for its first writes, and
    /// a bundle that has no data disk yet needs the first start's.
    static func startShortage(
        for selection: GuestSelection, freeBytes: UInt64
    ) -> StartSpace? {
        let firstStart: Bool
        if case let .bundle(bundle) = selection {
            firstStart = !FileManager.default.fileExists(
                atPath: bundle.dataDisk.path
            )
        } else {
            firstStart = false
        }
        return StartSpace(freeBytes: freeBytes, firstStart: firstStart)
    }

    /// What the screen calls the device: "iPad", "iPhone".
    static var deviceName: String { UIDevice.current.model }

    /// Check again, over a start the free space refused: the guest starts
    /// when there is room now.
    func retryStart() {
        guard let (view, geometry) = refusedStart else { return }
        start(view: view, geometry: geometry)
    }

    func dismissGPUBudgetWarning() {
        gpuBudgetWarning = nil
    }

    private func reportThermalState(phase: ThermalSamplePhase) {
        print(
            ThermalStateReport.line(
                phase: phase,
                state: ProcessInfo.processInfo.thermalState,
                uptime: ProcessInfo.processInfo.systemUptime
            )
        )
    }

    private func startThermalObservation() {
        reportThermalState(phase: .start)
        thermalObserver = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification,
            object: nil,
            queue: .main
        ) { _ in
            Task { @MainActor in
                EmulatorLauncher.shared.reportThermalState(phase: .change)
            }
        }
    }

    private func stopThermalObservation() {
        reportThermalState(phase: .finish)
        if let thermalObserver {
            NotificationCenter.default.removeObserver(thermalObserver)
            self.thermalObserver = nil
        }
    }
}
