// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// The preview has no guest display: its session screens draw mock screens.
/// While a normal boot shows, a fixture frame stands in for what the guest
/// draws under the dimmed boot screen.
struct GuestLayer: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if case .booting(let kind) = model.phase, kind != .recovery {
            // The image is an overlay so its size never proposes one to the
            // screens above it.
            Color.black
                .overlay {
                    Image("GuestWallpaper")
                        .resizable()
                        .scaledToFill()
                }
                .clipped()
                .ignoresSafeArea()
                .accessibilityHidden(true)
        }
    }
}
