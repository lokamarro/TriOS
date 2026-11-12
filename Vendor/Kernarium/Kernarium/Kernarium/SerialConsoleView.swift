// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import Foundation
import GhosttyTerminal
import SwiftUI
import UIKit

/// The guest serial console, rendered by Ghostty over a host-managed byte
/// stream: guest output is fed to the terminal, and the bytes the terminal
/// produces (encoded keys, pastes, and replies to the guest's queries) are
/// written back to the guest. No process is spawned, and the console has no
/// window size of its own, so terminal resizes are not forwarded.
@MainActor
final class SerialConsoleController: ObservableObject {
    /// Receives the bytes the terminal sends to the guest. Ghostty calls it
    /// on its own I/O thread, in the order the input was produced.
    typealias InputSink = @Sendable (Data) -> Void

    /// The terminal configuration belongs to the application. No Ghostty
    /// configuration file of the user's is read, and the guest can neither
    /// read nor write the host clipboard through OSC 52.
    static let terminalConfiguration = TerminalConfiguration { builder in
        builder.withFontSize(12)
        builder.withCursorStyle(.block)
        builder.withCursorStyleBlink(true)
        builder.withCustom("clipboard-read", "deny")
        builder.withCustom("clipboard-write", "deny")
        // Ghostty's 50 MB default keeps about 49,000 lines of a full-screen
        // iPad console, and history stays uncompressed while output streams.
        // 10 MB still keeps about 9,300 such lines, and more on a narrower
        // screen.
        builder.withCustom("scrollback-limit-bytes", "10000000")
    }

    /// One colour scheme regardless of the system appearance.
    static let terminalTheme = TerminalTheme(light: .afterglow, dark: .afterglow)

    let session: InMemoryTerminalSession
    let terminal: TerminalController
    private let surfaceOptions: TerminalSurfaceOptions
    private weak var terminalView: TerminalView?
    /// Output that arrived while no terminal surface existed. The session
    /// itself would keep only the newest 1 MiB of it.
    private var pendingOutput = Data()
    private var isObserving = false
    /// Whether the terminal has keyboard focus, and with it the key bar and
    /// any software keyboard. A tap on the terminal changes it as well as
    /// the application's keyboard button.
    @Published private(set) var isFocused = false

    convenience init() {
        self.init(input: { SerialConsoleController.sendToGuest($0) })
        startObserving()
    }

    /// Creates a console whose input goes to `input` and which does not
    /// observe the runtime until `startObserving()` is called.
    init(input: @escaping InputSink) {
        session = InMemoryTerminalSession(write: input, resize: { _ in })
        terminal = TerminalController(
            configuration: Self.terminalConfiguration,
            theme: Self.terminalTheme
        )
        surfaceOptions = TerminalSurfaceOptions(backend: .inMemory(session))
    }

    func startObserving() {
        guard !isObserving else { return }
        isObserving = true
        // The loader delivers every chunk on the main queue, in the order the
        // guest wrote it; anything else is a broken contract, not a race.
        kernarium_runtime_loader_set_console_output_handler {
            @Sendable [weak self] data in
            MainActor.assumeIsolated {
                self?.receive(data)
            }
        }
    }

    func stopObserving() {
        guard isObserving else { return }
        isObserving = false
        kernarium_runtime_loader_set_console_output_handler(nil)
    }

    func makeTerminalView() -> TerminalView {
        let view = TerminalView(frame: .zero)
        attach(view)
        return view
    }

    func attach(_ view: TerminalView) {
        terminalView = view
        view.delegate = self
        // The options come first: a controller installed on a view that
        // still carries the default options would build a surface that
        // executes a shell.
        view.configuration = surfaceOptions
        view.controller = terminal
    }

    func detach(_ view: TerminalView) {
        _ = view.resignFirstResponder()
        view.delegate = nil
        if terminalView === view {
            terminalView = nil
        }
    }

    func becomeFirstResponder() {
        _ = terminalView?.acquireProgrammaticFocus()
    }

    func resignFirstResponder() {
        _ = terminalView?.resignFirstResponder()
    }

    func receive(_ data: Data) {
        guard !data.isEmpty else { return }
        guard terminalView?.surface != nil else {
            pendingOutput.append(data)
            return
        }
        session.receive(data)
    }

    nonisolated static func sendToGuest(_ data: Data) {
        let result = data.withUnsafeBytes { buffer -> Int32 in
            guard let baseAddress = buffer.baseAddress else { return 0 }
            return kernarium_runtime_loader_send_console_input(
                baseAddress,
                buffer.count
            )
        }
        if result != 0 {
            print("Kernarium: serial console input failed: \(result)")
        }
    }
}

extension SerialConsoleController: TerminalSurfaceLifecycleDelegate {
    func terminalDidAttachSurface(_: TerminalSurface) {
        guard !pendingOutput.isEmpty else { return }
        session.receive(pendingOutput)
        pendingOutput = Data()
    }

    func terminalDidDetachSurface() {}
}

extension SerialConsoleController: TerminalSurfaceOpenURLDelegate {
    /// Links in guest output are not opened on the host.
    func terminalDidRequestOpenURL(_: String, kind _: TerminalOpenURLKind) {}
}

extension SerialConsoleController: TerminalSurfaceFocusDelegate {
    /// Focus is the console's keyboard: the application's keyboard button
    /// follows it, and the console makes room for keyboard UI only while it
    /// has focus. UIKit does not report every focus change as a keyboard
    /// change.
    func terminalDidChangeFocus(_: Bool) {
        // Ghostty reports a loss of focus even when resigning failed.
        let focused = terminalView?.isFirstResponder ?? false
        if isFocused != focused {
            isFocused = focused
        }
        terminalView?.superview?.setNeedsLayout()
    }
}

/// How much of the console the keyboard's docked parts cover. The
/// application's content ignores the keyboard, so the console makes room for
/// it itself. Everything is in screen coordinates.
enum SerialConsoleKeyboardInset {
    /// The height at the bottom of `host` covered by keyboard UI docked at the
    /// bottom of the screen: the key bar, and a docked software keyboard with
    /// the bar on top of it. A floating keyboard, and a bar riding on one, is
    /// not docked. It overlaps the console wherever the user puts it, as it
    /// does every application, and leaves the rows alone.
    static func bottomInset(
        host: CGRect,
        screen: CGRect,
        obstructions: [CGRect]
    ) -> CGFloat {
        let tops = obstructions.filter { frame in
            !frame.isNull && !frame.isEmpty &&
                frame.maxY >= screen.maxY - 1 &&
                frame.minX < host.maxX && frame.maxX > host.minX
        }.map(\.minY)
        guard let top = tops.min() else { return 0 }
        return min(max(0, host.maxY - top), host.height)
    }
}

/// The console's terminal, sized to end where the keyboard's docked parts
/// begin, so its last row stays visible. The key bar is the terminal's input
/// accessory: it sits on a docked keyboard, and at the bottom of the screen
/// when the software keyboard floats or a hardware keyboard replaces it.
/// Keyboard notifications report a floating keyboard but not the bar below
/// it, so the bar is measured where it is.
final class SerialConsoleHostView: UIView {
    let terminal: TerminalView
    /// The keyboard's frame as last reported, in screen coordinates.
    private var keyboardFrame: CGRect?

    init(terminal: TerminalView) {
        self.terminal = terminal
        super.init(frame: .zero)
        addSubview(terminal)
        for name in [
            UIResponder.keyboardWillChangeFrameNotification,
            UIResponder.keyboardDidChangeFrameNotification,
            UIResponder.keyboardDidShowNotification,
            UIResponder.keyboardDidHideNotification,
        ] {
            NotificationCenter.default.addObserver(
                self,
                selector: #selector(keyboardDidMove(_:)),
                name: name,
                object: nil
            )
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    @objc private func keyboardDidMove(_ notification: Notification) {
        if let frame = notification.userInfo?[
            UIResponder.keyboardFrameEndUserInfoKey
        ] as? CGRect {
            keyboardFrame = frame
        }
        setNeedsLayout()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        setNeedsLayout()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let frame = CGRect(
            x: 0,
            y: 0,
            width: bounds.width,
            height: max(0, bounds.height - coveredHeight())
        )
        // Each change resizes the terminal's grid.
        if terminal.frame != frame {
            terminal.frame = frame
        }
    }

    /// Only a focused terminal has the key bar and a keyboard of its own.
    /// Any other keyboard UI overlaps the console; so does the console's own
    /// on its way out, which UIKit may still report where it was: a hardware
    /// keyboard's bar leaves with focus, reported at its last frame and with
    /// no hide notification.
    private func coveredHeight() -> CGFloat {
        guard let window, terminal.isFirstResponder else { return 0 }
        let space = window.screen.coordinateSpace
        var obstructions: [CGRect] = []
        if let keyboardFrame {
            obstructions.append(keyboardFrame)
        }
        if let bar = terminal.inputAccessoryView,
           bar.window != nil, !bar.isHidden {
            obstructions.append(bar.convert(bar.bounds, to: space))
        }
        return SerialConsoleKeyboardInset.bottomInset(
            host: convert(bounds, to: space),
            screen: space.bounds,
            obstructions: obstructions
        )
    }
}

struct SerialConsoleView: UIViewRepresentable {
    /// Carried by the terminal itself, the element that takes keyboard focus.
    static let accessibilityIdentifier = "serialConsoleView"

    let controller: SerialConsoleController
    /// A hidden console keeps parsing guest output but stops drawing.
    let isVisible: Bool

    func makeUIView(context: Context) -> SerialConsoleHostView {
        let view = SerialConsoleHostView(terminal: controller.makeTerminalView())
        view.terminal.accessibilityIdentifier = Self.accessibilityIdentifier
        view.terminal.setSurfaceVisible(isVisible)
        return view
    }

    func updateUIView(_ view: SerialConsoleHostView, context: Context) {
        controller.attach(view.terminal)
        view.terminal.setSurfaceVisible(isVisible)
    }

    static func dismantleUIView(
        _ view: SerialConsoleHostView,
        coordinator: ()
    ) {
        (view.terminal.delegate as? SerialConsoleController)?.detach(view.terminal)
    }
}
