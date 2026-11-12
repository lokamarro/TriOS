// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

// @main désactivé par TriOS
struct KernariumApp: App {
    init() {
        // Before any view: the display and the terminal compile Metal
        // pipelines as they are created, and that reads the shader cache.
        MetalShaderCache.removeIfBloatedBeforeFirstUse()
        // Mesa would otherwise keep a disk cache of its own below
        // $HOME/.cache, which a device container does not let it create.
        // Give it a bounded home in the application's cache directory, here
        // and on a Mac alike.
        MesaShaderCache.configureBeforeFirstUse()
        // The host GL provider selects its driver through the environment
        // when it creates its display, on a runtime thread during boot. The
        // terminal's libghostty copied the environment at its first
        // initialization, so a variable added after that leaves its copy
        // pointing at freed memory and the next terminal configuration
        // crashes. Every process variable is set here, before any view.
        // The Simulator has no Metal 4 device, so its provider is Mesa's
        // llvmpipe rasterizer, which Mesa selects when software rendering
        // is forced.
        #if targetEnvironment(simulator)
        setenv("LIBGL_ALWAYS_SOFTWARE", "true", 1)
        #else
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1)
        #endif
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
