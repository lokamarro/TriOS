// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The same name vectors as the runtime's `kernarium-guest-time-zone` test
/// (`tests/unit/kernarium/guest_time_zone_tests.cpp`), and every zone Foundation
/// knows is accepted.
struct GuestTimeZoneTests {
    @Test func theNamesOfTheIanaDatabaseAreGivenAsTheyAre() {
        for name in [
            "UTC", "GMT", "Asia/Tokyo", "Europe/London", "EST5EDT", "GB-Eire", "W-SU", "Etc/GMT+5", "Etc/GMT-14",
            "GMT+0800", "America/Port-au-Prince", "Africa/Dar_es_Salaam", "America/Argentina/Buenos_Aires",
            "America/Indiana/Tell_City", "Asia/Ho_Chi_Minh", "Antarctica/DumontDUrville",
            "America/Argentina/ComodRivadavia", "a", "A0",
        ] {
            #expect(GuestTimeZone.bootName(name) == name, "\(name)")
        }
    }

    @Test func theLimitIsSixtyFourCharacters() {
        #expect(GuestTimeZone.bootName(String(repeating: "A", count: 64)) != nil)
        #expect(GuestTimeZone.bootName(String(repeating: "A", count: 65)) == nil)
        #expect(GuestTimeZone.bootName("") == nil)
    }

    @Test func whatCouldBeAnotherKernelArgumentIsRefused() {
        for name in [
            "Asia/Tokyo x", " Asia/Tokyo", "Asia/Tokyo ", "Asia\tTokyo", "Asia/Tokyo\n", "Asia/\"Tokyo\"",
            "Asia/Tokyo,UTC", "Asia/Tokyo:UTC", "Asia/Tokyo=1", "Asia\\Tokyo", "Asia/To'kyo", "a b",
            "Asia\u{0}Tokyo",
        ] {
            #expect(GuestTimeZone.bootName(name) == nil, "\(name.debugDescription)")
        }
    }

    @Test func whatIsNotANameIsRefused() {
        for name in [
            "/", "/Asia/Tokyo", "Asia/Tokyo/", "Asia//Tokyo", "//", "../etc/passwd", "Asia/../Tokyo", "Asia/.",
            ".", "Asia.Tokyo", "_Asia/Tokyo", "Asia/_Tokyo", "-Asia", "+5", "Asia/-Tokyo", "Asia/+Tokyo",
            "Zürich", "Europe/Zürich", "東京", "Asia/東京",
        ] {
            #expect(GuestTimeZone.bootName(name) == nil, "\(name.debugDescription)")
        }
    }

    @Test func everyZoneTheHostKnowsIsAccepted() {
        for name in TimeZone.knownTimeZoneIdentifiers {
            #expect(GuestTimeZone.bootName(name) == name, "\(name)")
        }
    }

    @Test func theCurrentZoneIsTheNameOfTheZonePassed() throws {
        for name in ["Asia/Tokyo", "America/Los_Angeles", "Pacific/Kiritimati", "UTC"] {
            let zone = try #require(TimeZone(identifier: name))
            #expect(GuestTimeZone.current(of: zone) == zone.identifier)
        }
        // A zone of a fixed offset has the name GMT+0900, which is a valid
        // name the guest may or may not know.
        let fixed = try #require(TimeZone(secondsFromGMT: 9 * 3600))
        #expect(GuestTimeZone.current(of: fixed) == fixed.identifier)
    }

    @Test func theDefaultIsTheLiveSystemZone() {
        #expect(GuestTimeZone.current() == TimeZone.autoupdatingCurrent.identifier)
    }
}
