// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import AndariumCore

struct CapabilitiesTests {
    @Test func theDeviceOffersWhatTheRuntimeDoes() {
        let device = Capabilities.device
        #expect(device.bugReport && device.apkInstall && device.adbOverNetwork)
        // The statistics are the runtime's own readings.
        #expect(device.statistics)
    }

    @Test func thePreviewPretendsToDoEverything() {
        let preview = Capabilities.preview
        #expect(preview.bugReport && preview.apkInstall && preview.adbOverNetwork)
        #expect(preview.statistics)
    }
}
