// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import UIKit

@main
final class AndariumApp: UIResponder, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions options: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        // Mesa reads its disk cache's location and limit from the process
        // environment when the runtime thread creates the guest's screen.
        // Set them here, before any scene or runtime exists.
        MesaShaderCache.configureBeforeFirstUse()
        return true
    }

    func application(
        _ application: UIApplication, configurationForConnecting session: UISceneSession,
        options: UIScene.ConnectionOptions
    ) -> UISceneConfiguration {
        let configuration = UISceneConfiguration(name: "Andarium", sessionRole: session.role)
        configuration.delegateClass = AndariumSceneDelegate.self
        return configuration
    }
}

/// The entry, model and runtime of the process. A scene shows them and owns
/// only its window: UIKit disconnects a scene it releases and connects one
/// again, and the runtime holds a guest that a new scene cannot be handed.
@MainActor
final class AndariumProcess {
    static let shared = AndariumProcess()

    let entry: AppEntry
    let model: AppModel

    private init() {
        entry = AppEntry.fromArguments()
        entry.configureTips()
        // A report staged for sharing by a process that ended is of no use.
        // This is the process's launch: a test that builds a model does not
        // sweep the folder a running test stages its files in.
        BugReportStaging.sweep()
        model = entry.makeModel()
    }
}

final class AndariumSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    /// The process's model, which this scene shows.
    private(set) var model: AppModel?

    func windowScene(_ windowScene: UIWindowScene, didUpdateEffectiveGeometry previousGeometry: UIWindowScene.Geometry) {
        (window?.rootViewController as? SceneGeometryObserver)?.sceneGeometryDidChange()
    }

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let scene = scene as? UIWindowScene else { return }
        connect(scene)
    }

    func connect(_ scene: UIWindowScene) {
        guard window == nil else { return }
        let process = AndariumProcess.shared
        let entry = process.entry
        let model = process.model
        self.model = model
        let window = UIWindow(windowScene: scene)
        window.rootViewController = AndariumHostingController(
            rootView: RootView(entry: entry).environment(model), model: model)
        self.window = window
        window.makeKeyAndVisible()
    }
}

/// Clean power-off returns to Home; a failed run waits for a fresh process.
struct RootView: View {
    @Environment(AppModel.self) private var model
    let entry: AppEntry

    var body: some View {
        @Bindable var model = model
        ZStack {
            // The guest's display lives under the screens of the boot and the
            // run, so it is one view from the first frame to the last.
            GuestLayer()
            Group {
                switch model.phase {
                case .home:
                    HomeView(hints: entry.hints)
                case .booting(let kind):
                    BootView(kind: kind)
                        .id(kind)
                case .running(let mode):
                    SessionView(mode: mode)
                case .failed:
                    EndView()
                }
            }
            .transition(.opacity)
            if let busy = model.busy {
                BusyOverlay(state: busy)
            }
            // Over everything the session shows, a Settings sheet aside: the
            // model closes it when the guest pauses.
            if let pause = model.spacePause {
                LowSpaceOverlay(
                    pause: pause, resume: { model.resumeAfterSpacePause() },
                    shutDown: { model.shutDownAfterSpacePause() },
                    checkAgain: { model.checkSpaceAfterPauseFailed() })
            }
        }
        // Toggles take the jade accent too, not the system green.
        .tint(Color.accentColor)
        .animation(.easeInOut(duration: 0.4), value: model.phase)
        .animation(.easeInOut(duration: 0.2), value: model.busy != nil)
        .animation(.easeInOut(duration: 0.2), value: model.spacePause != nil)
        .alert(
            model.alert?.title ?? "",
            isPresented: Binding(get: { model.alert != nil }, set: { if !$0 { model.alert = nil } }),
            presenting: model.alert
        ) { _ in
            Button("OK", role: .cancel) {}
        } message: { alert in
            Text(alert.message)
        }
        #if !APP_STORE
        .task {
            // The launch's automation work happens once per model: a scene
            // that connects again must not create, erase or start the guest
            // a second time.
            guard !model.launchApplied else { return }
            model.launchApplied = true
            await entry.applyDeferred(to: model)
        }
        #endif
    }
}
