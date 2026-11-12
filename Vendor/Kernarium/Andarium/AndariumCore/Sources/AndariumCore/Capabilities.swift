// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the runtime and storage behind the screens can do. A screen shows a
/// control only when the capability behind it exists, so the application
/// never offers what it cannot do. The Simulator preview has all of them; the
/// device application has the ones its runtime provides.
public struct Capabilities: Equatable, Sendable {
    /// A computer can reach the guest with ADB over the network.
    public var adbOverNetwork: Bool
    /// An app picked in Files (an .apk) can be installed into the guest.
    public var apkInstall: Bool
    /// The guest's bug report can be made and shared.
    public var bugReport: Bool
    /// The session can show the guest's own statistics.
    public var statistics: Bool

    public init(adbOverNetwork: Bool, apkInstall: Bool, bugReport: Bool, statistics: Bool) {
        self.adbOverNetwork = adbOverNetwork
        self.apkInstall = apkInstall
        self.bugReport = bugReport
        self.statistics = statistics
    }

    /// Everything the mock runtime pretends to do.
    public static let preview = Capabilities(
        adbOverNetwork: true, apkInstall: true, bugReport: true, statistics: true
    )

    /// The device application: the session offers an app install, ADB over
    /// the network and a bug report, all through the runtime's adb channel,
    /// and the statistics from the runtime's own readings.
    public static let device = Capabilities(
        adbOverNetwork: true,
        apkInstall: true,
        bugReport: true,
        statistics: true
    )
}
