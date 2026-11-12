// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// Test support, not a product: an application that takes the audio session
/// from the application in front, so that a run can show what Kernarium does
/// when another application interrupts its audio. A person never needs to
/// touch it; `scripts/audio-interrupter.py` launches and ends it.
@main
struct AudioInterrupterApp: App {
    @State private var interrupter = Interrupter(
        configuration: .init(environment: ProcessInfo.processInfo.environment))

    var body: some Scene {
        WindowGroup {
            VStack(spacing: 12) {
                Text("Audio interrupter")
                    .font(.title)
                Text("Test support for Kernarium. It plays digital silence.")
                    .font(.footnote)
            }
            .padding()
            .task { interrupter.start() }
        }
    }
}
