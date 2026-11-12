// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
import UniformTypeIdentifiers

@testable import AndariumCore

struct InstallRefusalTests {
    @Test func aSingleApkIsTaken() {
        for name in ["Signal-7.21.4.apk", "A.APK", "my app.Apk"] {
            #expect(InstallRefusal(fileName: name) == nil, "\(name)")
        }
    }

    @Test func aFileThatIsNotAnAppIsTurnedAway() {
        for name in ["Trip photos.zip", "notes.txt", "apk", "app.apk.zip", ""] {
            #expect(InstallRefusal(fileName: name) == .notApk, "\(name)")
        }
    }

    @Test func anAppInSeveralFilesIsTurnedAwayWithItsOwnWords() {
        for name in ["app.apks", "Trip Planner.XAPK", "app.apkm"] {
            #expect(InstallRefusal(fileName: name) == .bundle, "\(name)")
        }
        #expect(InstallRefusal.notApk.message == "This isn\u{2019}t an app. Choose a file that ends in .apk.")
        #expect(InstallRefusal.bundle.message
            == "Apps that come in several files can\u{2019}t be installed yet. Choose a single .apk file.")
    }

    /// The type the system gives a file name, as the picker's rows have it.
    private func type(of name: String) -> UTType? {
        UTType(filenameExtension: URL(fileURLWithPath: name).pathExtension)
    }

    @Test func thePickerOffersOnlyTheTypeOfAnApk() {
        let picker = InstallRefusal.pickerType
        // Not everything: an unfiltered picker is what this type replaces.
        #expect(picker != .item && picker != .data && picker != .archive)
        for name in ["a.apk", "A.APK", "my app.Apk"] {
            #expect(type(of: name)?.conforms(to: picker) == true, "\(name)")
        }
        // The system or an application that declares .apk changes the
        // identifier, never the agreement between the picker and the files.
        #expect(UTType(filenameExtension: "apk", conformingTo: .data) == picker)
    }

    @Test func thePickerDimsEveryOtherKindOfFile() {
        let picker = InstallRefusal.pickerType
        for name in ["a.apks", "a.apkm", "a.zip", "a.txt", "a.jar", "a.ipa", "a.json", "a.pdf", "a.png"] {
            #expect(type(of: name)?.conforms(to: picker) != true, "\(name)")
        }
        // A file with no extension has no type to match, and the two plain
        // types do not conform to the apk type.
        #expect(type(of: "noextension") == nil)
        #expect(!UTType.data.conforms(to: picker) && !UTType.item.conforms(to: picker))
    }

    @Test func theFilterAndTheRefusalAgreeOnWhatIsAnApk() {
        for name in ["a.apk", "A.APK", "my app.Apk"] {
            #expect(InstallRefusal(fileName: name) == nil && type(of: name)?.conforms(to: InstallRefusal.pickerType) == true)
        }
        // A bundle that a newer system also calls an apk gets past the
        // picker and is refused with its own words.
        #expect(InstallRefusal(fileName: "app.xapk") == .bundle)
    }
}
