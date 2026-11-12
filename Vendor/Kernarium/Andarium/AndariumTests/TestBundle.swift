// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import TipKit
@testable import Andarium

/// The hosted test bundle's principal class (`NSPrincipalClass`), which XCTest
/// makes once when it loads the bundle, before any test runs.
///
/// It fixes the side keys' tip for the whole run, whatever the device's tip
/// store says: TipKit applies a testing override only to a tip that has not
/// shown yet in the process and keeps it for the process, so otherwise the
/// first suite to put the keys on screen would decide for every later one.
/// The tip shows, as on a first launch. A harness launch that only holds the
/// host for its guest (`AutomationHoldTests`) hides it instead, as a person
/// who has used the keys sees the session, so no popover sits over a guest
/// that is being measured.
@objc(AndariumTestBundle)
final class AndariumTestBundle: NSObject {
    nonisolated override init() {
        super.init()
        if ProcessInfo.processInfo.environment["ANDARIUM_HOLD_SECONDS"] != nil {
            Tips.hideTipsForTesting([SideKeysTip.self])
        } else {
            Tips.showTipsForTesting([SideKeysTip.self])
        }
    }
}
