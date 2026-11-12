// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import UniformTypeIdentifiers

/// A file Install APK turns away before anything is sent to the guest. The
/// words are the ones the screen shows.
public enum InstallRefusal: Equatable, Sendable {
    /// The file is not an app package.
    case notApk
    /// The file is an app in several parts (.apks, .xapk, .apkm), which the
    /// install does not take.
    case bundle

    /// nil for a file the install takes: one that ends in .apk.
    public init?(fileName: String) {
        let name = fileName.lowercased()
        if name.hasSuffix(".apk") { return nil }
        if [".apks", ".xapk", ".apkm"].contains(where: { name.hasSuffix($0) }) {
            self = .bundle
        } else {
            self = .notApk
        }
    }

    public var message: String {
        switch self {
        case .notApk: "This isn\u{2019}t an app. Choose a file that ends in .apk."
        case .bundle: "Apps that come in several files can\u{2019}t be installed yet. Choose a single .apk file."
        }
    }
}

extension InstallRefusal {
    /// The one type the file picker offers: what the system calls a file
    /// that ends in .apk. No Info.plist declaration is needed. With none, the
    /// type is the dynamic one every .apk file also gets; if the system or
    /// another application declares .apk, the call returns that type and the
    /// files carry it too, so the picker and the files always agree. Other
    /// extensions (.apks, .xapk, .apkm, .zip, none) are dimmed, and
    /// `init(fileName:)` still refuses a mislabelled file.
    public static var pickerType: UTType {
        UTType(filenameExtension: "apk", conformingTo: .data) ?? .data
    }
}
