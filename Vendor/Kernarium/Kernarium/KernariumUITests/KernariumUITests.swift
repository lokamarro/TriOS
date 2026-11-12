// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class KernariumUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    @MainActor
    func testSettingsSheetShowsLaunchConfigurationControls() throws {
        let app = testApplication()
        app.launch()

        let settingsButton = app.buttons["settingsButton"]
        XCTAssertTrue(settingsButton.waitForExistence(timeout: 10))
        settingsButton.tap()

        XCTAssertTrue(
            app.steppers["vCPUCountStepper"].waitForExistence(timeout: 5)
        )
        XCTAssertTrue(app.steppers["guestMemoryStepper"].exists)
        XCTAssertTrue(app.steppers["overlaySizeStepper"].exists)
        XCTAssertTrue(app.switches["retinaModeToggle"].exists)
        XCTAssertTrue(app.switches["autoResizeGuestToggle"].exists)
        XCTAssertTrue(app.buttons["settingsSaveButton"].exists)

        // A form lists only the rows near the screen, so a window too short
        // for all of them scrolls to the picker at the end.
        let picker = app.descendants(matching: .any)["rootfsPicker"]
        for _ in 0..<6 where !picker.exists {
            app.swipeUp()
        }
        XCTAssertTrue(picker.exists)
    }

    @MainActor
    func testSpecialKeyTrayExpandsAndCollapses() throws {
        let app = testApplication()
        app.launch()

        let trayButton = app.buttons["specialKeysButton"]
        XCTAssertTrue(trayButton.waitForExistence(timeout: 10))
        XCTAssertEqual(trayButton.frame.width, 32, accuracy: 1)
        XCTAssertEqual(trayButton.frame.height, 32, accuracy: 1)
        XCTAssertFalse(app.buttons["guestPowerButton"].exists)

        trayButton.tap()
        for identifier in [
            "guestPowerButton",
            "guestVolumeUpButton",
            "guestVolumeDownButton",
            "guestEscapeButton",
            "guestTabButton",
            "guestHomeButton",
        ] {
            XCTAssertTrue(app.buttons[identifier].waitForExistence(timeout: 2))
        }

        trayButton.tap()
        XCTAssertTrue(
            app.buttons["guestPowerButton"].waitForNonExistence(timeout: 2)
        )
    }

    @MainActor
    func testDisplayModeDefaultsToFramebufferAndToggles() throws {
        let app = testApplication()
        app.launch()

        let displayModeButton = app.buttons["displayModeButton"]
        XCTAssertTrue(displayModeButton.waitForExistence(timeout: 10))
        XCTAssertEqual(displayModeButton.label, "Show serial console")

        displayModeButton.tap()
        XCTAssertEqual(displayModeButton.label, "Show framebuffer")

        displayModeButton.tap()
        XCTAssertEqual(displayModeButton.label, "Show serial console")
    }

    /// Tapping the serial console raises the software keyboard with the
    /// terminal's key bar, which carries the keys the keyboard lacks.
    /// Returning to the framebuffer dismisses the keyboard. The bytes those
    /// keys produce are checked by the hosted SerialConsoleTests.
    @MainActor
    func testSerialConsoleRaisesTheKeyboardAndItsKeyBar() throws {
        let app = testApplication()
        app.launch()

        let displayModeButton = app.buttons["displayModeButton"]
        XCTAssertTrue(displayModeButton.waitForExistence(timeout: 10))
        displayModeButton.tap()
        let console = app.descendants(matching: .any)["serialConsoleView"]
        XCTAssertTrue(console.waitForExistence(timeout: 5))
        console.tap()

        let keyboard = app.keyboards.firstMatch
        XCTAssertTrue(
            keyboard.waitForExistence(timeout: 10),
            "the console did not raise the software keyboard"
        )
        app.typeText("uname -a\n")
        // The software keyboard has a Tab key of its own, so the keys are
        // looked up in the key bar's scroll view alone.
        let keyBar = app.scrollViews.containing(
            NSPredicate(format: "label == %@", "Escape")
        ).firstMatch
        for key in [
            "Escape", "Tab", "Up Arrow", "Down Arrow", "Left Arrow",
            "Right Arrow", "Control",
        ] {
            let button = keyBar.buttons[key]
            XCTAssertTrue(button.exists, "the key bar has no \(key) key")
            button.tap()
        }
        app.typeText("c")
        attachScreenshot(of: app, named: "serial-console-keyboard")

        displayModeButton.tap()
        XCTAssertTrue(
            keyboard.waitForNonExistence(timeout: 10),
            "leaving the console did not dismiss the keyboard"
        )
    }

    /// In a window, iPadOS draws its window controls over the content's
    /// top-left corner, where the guest's own status bar starts. The guest
    /// view has to begin below them wherever the window sits and however big
    /// it is. Full Screen Apps has no controls, and SpringBoard then shows
    /// none, so there is nothing to clear.
    @MainActor
    func testGuestViewClearsTheWindowControls() throws {
        let app = testApplication()
        app.launch()

        let guest = app.descendants(matching: .any)["framebufferView"]
        XCTAssertTrue(guest.waitForExistence(timeout: 10))
        let springboard = XCUIApplication(
            bundleIdentifier: "com.apple.springboard"
        )
        let controls = springboard.descendants(matching: .any)[
            "window-controls:com.imxieyi.Kernarium"
        ]
        guard controls.waitForExistence(timeout: 5) else {
            throw XCTSkip("the window has no windowing controls to clear")
        }
        XCTAssertFalse(
            guest.frame.intersects(controls.frame),
            "the guest view \(guest.frame) lies under the window controls " +
                "\(controls.frame)"
        )
        attachScreenshot(of: app, named: "guest-below-window-controls")
    }

    /// Where iPadOS draws no window controls over the content, in Full
    /// Screen Apps and in a window zoomed to the screen, the guest view
    /// needs no margin and begins at the top of the safe area, 32 points
    /// down on an iPad mini. The margin of a window (75 points there) must
    /// not outlive the window: Multitasking switching a running application
    /// to Full Screen Apps once left it in place. The switch itself is
    /// checked on a device through the automation host; see
    /// docs/testing.md.
    @MainActor
    func testGuestViewKeepsNoMarginWithoutWindowControls() throws {
        let app = testApplication()
        app.launch()

        let guest = app.descendants(matching: .any)["framebufferView"]
        XCTAssertTrue(guest.waitForExistence(timeout: 10))
        let springboard = XCUIApplication(
            bundleIdentifier: "com.apple.springboard"
        )
        let controls = springboard.descendants(matching: .any)[
            "window-controls:com.imxieyi.Kernarium"
        ]
        if controls.waitForExistence(timeout: 5) {
            throw XCTSkip(
                "the window has controls; testGuestViewClearsTheWindowControls checks it"
            )
        }
        let window = app.windows.firstMatch
        XCTAssertLessThanOrEqual(
            guest.frame.minY - window.frame.minY,
            40,
            "the guest view \(guest.frame) keeps a margin below the top of " +
                "the window \(window.frame) with no window controls"
        )
        attachScreenshot(of: app, named: "guest-without-window-controls")
    }

    @MainActor
    private func attachScreenshot(of app: XCUIApplication, named name: String) {
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    private func testApplication() -> XCUIApplication {
        let app = XCUIApplication()
        // UI tests exercise the app shell, not a multi-gigabyte guest boot.
        app.launchEnvironment["KERNARIUM_ROOTFS"] =
            "kernarium-ui-tests-missing.erofs"
        return app
    }
}
