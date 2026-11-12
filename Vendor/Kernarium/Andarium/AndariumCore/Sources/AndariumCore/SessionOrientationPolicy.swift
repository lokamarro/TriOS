// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

public struct SessionOrientationPolicy: Equatable, Sendable {
    public let requestInterfaceLock: Bool
    public let hostDrivesGuestRotation: Bool

    public init(phase: Phase, isPhone: Bool, sceneSize: CGSize, screenSize: CGSize, interfaceLocked: Bool) {
        let session: Bool = switch phase {
        case .booting, .running: true
        case .home, .failed: false
        }
        let fullScreen = abs(sceneSize.width - screenSize.width) < 0.5
            && abs(sceneSize.height - screenSize.height) < 0.5
        requestInterfaceLock = session && !isPhone && fullScreen
        // iPhone is always portrait. On iPad a full-screen-sized scene can
        // still lose its lock, for example when another scene obscures it.
        hostDrivesGuestRotation = session && !isPhone && !(fullScreen && interfaceLocked)
    }
}
