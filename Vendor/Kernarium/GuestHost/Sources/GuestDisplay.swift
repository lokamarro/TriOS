// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import Foundation
import MetalKit
import SwiftUI
import UIKit

struct GuestDisplayGeometry {
    let pixelSize: CGSize
    let dotsPerInch: UInt32
}

struct GuestDisplayView: UIViewControllerRepresentable {
    @Binding var keyboardVisible: Bool
    let retinaMode: Bool
    /// Told each time touches begin on the display, after their contacts have
    /// gone to the guest. An application that draws controls over the display
    /// uses it to put them away when a touch lands on the guest instead.
    var touchDidBegin: (@MainActor () -> Void)?
    let viewDidLayout: @MainActor (
        GuestMetalView,
        GuestDisplayGeometry
    ) -> Void

    func makeUIViewController(context: Context) -> GuestDisplayViewController {
        let view = GuestMetalView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.viewDidLayout = viewDidLayout
        view.touchDidBegin = touchDidBegin
        view.retinaMode = retinaMode
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.autoResizeDrawable = false
        view.isMultipleTouchEnabled = true
        return GuestDisplayViewController(metalView: view)
    }

    func updateUIViewController(
        _ controller: GuestDisplayViewController,
        context: Context
    ) {
        let view = controller.metalView
        view.touchDidBegin = touchDidBegin
        view.retinaMode = retinaMode
        view.setKeyboardVisible(keyboardVisible)
        view.reportLayout()
    }

    static func dismantleUIViewController(
        _ controller: GuestDisplayViewController,
        coordinator: ()
    ) {
        controller.metalView.cancelTrackedTouches()
        controller.metalView.finishHostResize()
        controller.metalView.resignFirstResponder()
    }
}



final class GuestDisplayViewController: UIViewController {
    let metalView: GuestMetalView
    private var resizeHold = GuestResizeHold()
    private var sceneGeometryObservation: NSKeyValueObservation?

    init(metalView: GuestMetalView) {
        self.metalView = metalView
        super.init(nibName: nil, bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func loadView() {
        view = metalView
    }

    override func viewWillTransition(
        to size: CGSize,
        with coordinator: UIViewControllerTransitionCoordinator
    ) {
        let (token, action) = resizeHold.transitionWillBegin()
        perform(action)
        super.viewWillTransition(to: size, with: coordinator)
        coordinator.animate(alongsideTransition: nil) { [weak self] _ in
            guard let self else { return }
            self.perform(self.resizeHold.transitionDidComplete(token: token))
        }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        guard sceneGeometryObservation == nil,
              let scene = view.window?.windowScene else { return }
        // An interactive window resize reports no transition of its own to
        // hold across, so the scene's geometry says when the gesture runs.
        sceneGeometryObservation = scene.observe(
            \.effectiveGeometry, options: [.initial, .new]
        ) { [weak self] scene, _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.perform(self.resizeHold.interactiveResizeChanged(
                    scene.effectiveGeometry.isInteractivelyResizing
                ))
            }
        }
    }

    private func perform(_ action: GuestResizeHold.Action) {
        switch action {
        case .none: break
        case .begin: metalView.beginHostResize()
        case .finish: metalView.finishHostResize()
        }
    }
}



private extension Notification.Name {
    static let guestDisplayResizeWillBegin = Notification.Name(
        "org.linux-um-darwin.guest-display-resize-will-begin"
    )
    static let guestDisplayResizeDidEnd = Notification.Name(
        "org.linux-um-darwin.guest-display-resize-did-end"
    )
}

final class GuestMetalView: MTKView, UIKeyInput {
    var viewDidLayout: (@MainActor (
        GuestMetalView,
        GuestDisplayGeometry
    ) -> Void)?
    var touchDidBegin: (@MainActor () -> Void)?
    var retinaMode = false
    @objc private(set) var umlGuestResizeInProgress = false
    private var touchContacts = GuestTouchContacts<ObjectIdentifier>()
    private var guestContentSize: CGSize?

    override var canBecomeFirstResponder: Bool { true }
    // While the guest has the keyboard focus, three fingers are the guest's
    // touches, not UIKit's undo, redo, copy and paste gestures.
    override var editingInteractionConfiguration: UIEditingInteractionConfiguration { .none }
    var hasText: Bool { true }
    var keyboardType: UIKeyboardType = .asciiCapable
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var smartDashesType: UITextSmartDashesType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no

    override func layoutSubviews() {
        super.layoutSubviews()
        reportLayout()
    }

    func reportLayout() {
        guard let screen = window?.screen else { return }
        let scale = retinaMode ? screen.nativeScale : 1
        guard bounds.width > 0, bounds.height > 0,
              scale.isFinite, scale > 0 else { return }
        // SwiftUI supplies safe-area-excluded point bounds. Point-resolution
        // mode maps them directly to guest pixels; Retina mode converts them
        // to physical backing pixels, including fractional native scales.
        let pixelSize = CGSize(
            width: (bounds.width * scale).rounded(.down),
            height: (bounds.height * scale).rounded(.down)
        )
        guard pixelSize.width >= 1, pixelSize.height >= 1 else { return }
        drawableSize = pixelSize
        let dotsPerInch = UInt32(
            (CGFloat(160) * scale).rounded()
        )
        viewDidLayout?(
            self,
            GuestDisplayGeometry(
                pixelSize: pixelSize,
                dotsPerInch: dotsPerInch
            )
        )
    }

    func beginHostResize() {
        guard !umlGuestResizeInProgress else { return }
        umlGuestResizeInProgress = true
        NotificationCenter.default.post(
            name: .guestDisplayResizeWillBegin,
            object: self
        )
    }

    func finishHostResize() {
        guard umlGuestResizeInProgress else { return }
        reportLayout()
        umlGuestResizeInProgress = false
        NotificationCenter.default.post(
            name: .guestDisplayResizeDidEnd,
            object: self
        )
    }

    @objc(umlSetGuestDisplayWidth:height:)
    func setGuestDisplay(width: UInt32, height: UInt32) {
        cancelTrackedTouches()
        guard width > 0, height > 0 else {
            guestContentSize = nil
            return
        }
        guestContentSize = CGSize(
            width: CGFloat(width),
            height: CGFloat(height)
        )
    }

    func setKeyboardVisible(_ visible: Bool) {
        if visible {
            becomeFirstResponder()
        } else {
            resignFirstResponder()
        }
    }

    func insertText(_ text: String) {
        for character in text {
            guard let stroke = LinuxKeyboard.stroke(for: character) else {
                print("Kernarium: unsupported keyboard character \(character)")
                continue
            }
            if stroke.shifted {
                sendKey(code: LinuxKeyboard.leftShift, pressed: true)
            }
            sendKey(code: stroke.code, pressed: true)
            sendKey(code: stroke.code, pressed: false)
            if stroke.shifted {
                sendKey(code: LinuxKeyboard.leftShift, pressed: false)
            }
        }
    }

    func deleteBackward() {
        sendKey(code: LinuxKeyboard.backspace, pressed: true)
        sendKey(code: LinuxKeyboard.backspace, pressed: false)
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesBegan(touches, with: event)
        for touch in touches {
            sendTouchEvents(touchContacts.begin(
                ObjectIdentifier(touch),
                at: touch.location(in: self),
                geometry: touchGeometry()
            ))
        }
        touchDidBegin?()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesMoved(touches, with: event)
        for touch in touches {
            let coalesced = (event?.coalescedTouches(for: touch) ?? []).map {
                $0.location(in: self)
            }
            sendTouchEvents(touchContacts.move(
                ObjectIdentifier(touch),
                through: GuestTouchSamples.ordered(
                    coalesced: coalesced,
                    current: touch.location(in: self)
                ),
                geometry: touchGeometry()
            ))
        }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesEnded(touches, with: event)
        for touch in touches {
            sendTouchEvents(touchContacts.end(
                ObjectIdentifier(touch),
                at: touch.location(in: self),
                geometry: touchGeometry()
            ))
        }
    }

    override func touchesCancelled(
        _ touches: Set<UITouch>,
        with event: UIEvent?
    ) {
        super.touchesCancelled(touches, with: event)
        for touch in touches {
            sendTouchEvents(touchContacts.cancel(ObjectIdentifier(touch)))
        }
    }

    private func touchGeometry() -> GuestTouchGeometry? {
        GuestTouchGeometry(
            coordinateSpace: .touchscreen,
            boundsSize: bounds.size,
            drawableSize: drawableSize,
            contentSize: guestContentSize ?? drawableSize
        )
    }

    private func sendTouchEvents(_ events: [GuestTouchContactEvent]) {
        for contact in events {
            if contact.event.cancelled {
                kernarium_runtime_loader_cancel_touch(contact.slot)
                continue
            }
            kernarium_runtime_loader_send_touch(
                contact.slot,
                contact.event.x,
                contact.event.y,
                contact.event.pressed ? 1 : 0
            )
        }
    }

    func cancelTrackedTouches() {
        sendTouchEvents(touchContacts.cancelAll())
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { cancelTrackedTouches() }
    }

    private func sendKey(code: UInt16, pressed: Bool) {
        kernarium_runtime_loader_send_key(code, pressed ? 1 : 0)
    }
}

