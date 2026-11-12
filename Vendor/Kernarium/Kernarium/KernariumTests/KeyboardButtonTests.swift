// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import GhosttyTerminal
import MetalKit
import Testing
import UIKit
@testable import Kernarium

/// The floating keyboard button of the application's own window. It raises
/// the keyboard for the view on screen, so typed text reaches what the user
/// is looking at: the framebuffer, or the serial console's terminal. The
/// floating buttons are pressed through accessibility, as VoiceOver presses
/// them. These tests belong to the serialized console suite: keyboard focus
/// is one per application.
extension SerialConsoleTests {
    /// The application's window and the controls in it.
    @MainActor
    fileprivate struct Application {
        let window: UIWindow
        let terminal: TerminalView

        /// The accessibility element carrying `identifier`.
        func element(_ identifier: String) -> NSObject? {
            accessibilityElement(identifier, in: window)
        }

        func label(of identifier: String) -> String? {
            element(identifier)?.accessibilityLabel
        }

        func press(_ identifier: String) throws {
            let button = try #require(
                element(identifier),
                "the application shows no \(identifier)"
            )
            #expect(button.accessibilityActivate(), "\(identifier) did not act")
        }

        /// The views in the window that hold keyboard focus.
        var firstResponders: [UIView] {
            func walk(_ view: UIView) -> [UIView] {
                (view.isFirstResponder ? [view] : []) +
                    view.subviews.flatMap(walk)
            }
            return walk(window)
        }
    }

    fileprivate func application() throws -> Application {
        for window in try windowScene().windows
        where window.windowLevel == .normal {
            if let terminal = terminalView(
                in: window,
                identifier: SerialConsoleView.accessibilityIdentifier
            ) {
                return Application(window: window, terminal: terminal)
            }
        }
        throw ApplicationMissing()
    }

    fileprivate struct ApplicationMissing: Error {}

    /// Back to the framebuffer, the application's first mode, with no
    /// keyboard up.
    fileprivate func restore(_ app: Application) async throws {
        if app.label(of: "displayModeButton") == "Show framebuffer" {
            try app.press("displayModeButton")
            #expect(
                try await eventually {
                    app.label(of: "displayModeButton") == "Show serial console"
                }
            )
        }
        if app.label(of: "keyboardButton") == "Hide keyboard" {
            try app.press("keyboardButton")
        }
        #expect(try await eventually { app.firstResponders.isEmpty })
    }

    @Test(.timeLimit(.minutes(1)))
    func theKeyboardButtonFocusesTheViewOnScreen() async throws {
        let app = try application()
        try await restore(app)
        do {
            try await pressTheKeyboardButton(in: app)
        } catch {
            try? await restore(app)
            throw error
        }
        try await restore(app)
    }

    fileprivate func pressTheKeyboardButton(in app: Application) async throws {
        var responders = ""
        func focused(_ view: UIView?) -> Bool {
            let found = app.firstResponders
            responders = found.map { "\(type(of: $0))" }.description
            if let view { return found.count == 1 && found[0] === view }
            return found.isEmpty
        }

        // The framebuffer takes the keyboard as it always has.
        try app.press("keyboardButton")
        #expect(
            try await eventually {
                app.firstResponders.count == 1 &&
                    app.firstResponders[0] is MTKView
            },
            "the framebuffer did not take the keyboard: \(app.firstResponders)"
        )
        #expect(app.label(of: "keyboardButton") == "Hide keyboard")

        // A mode switch puts the keyboard away; the view that had it is
        // hidden.
        try app.press("displayModeButton")
        #expect(
            try await eventually {
                app.label(of: "displayModeButton") == "Show framebuffer"
            }
        )
        #expect(
            try await eventually { focused(nil) },
            "a hidden view kept the keyboard: \(responders)"
        )
        #expect(app.label(of: "keyboardButton") == "Show keyboard")

        // On the console, the button focuses the console's terminal, which
        // raises its keyboard and key bar, and puts them away again.
        try app.press("keyboardButton")
        #expect(
            try await eventually { focused(app.terminal) },
            "the console's terminal did not take the keyboard: \(responders)"
        )
        #expect(
            try await eventually {
                app.label(of: "keyboardButton") == "Hide keyboard"
            }
        )
        try app.press("keyboardButton")
        #expect(
            try await eventually { focused(nil) },
            "the console kept the keyboard: \(responders)"
        )
        #expect(
            try await eventually {
                app.label(of: "keyboardButton") == "Show keyboard"
            }
        )

        // Focus taken on the console itself shows on the button, and a
        // switch back to the framebuffer puts that keyboard away too.
        #expect(app.terminal.becomeFirstResponder())
        #expect(
            try await eventually {
                app.label(of: "keyboardButton") == "Hide keyboard"
            },
            "the button does not show the console's keyboard"
        )
        try app.press("displayModeButton")
        #expect(
            try await eventually {
                app.label(of: "displayModeButton") == "Show serial console"
            }
        )
        #expect(
            try await eventually { focused(nil) },
            "a hidden view kept the keyboard: \(responders)"
        )
        #expect(app.label(of: "keyboardButton") == "Show keyboard")
    }
}
