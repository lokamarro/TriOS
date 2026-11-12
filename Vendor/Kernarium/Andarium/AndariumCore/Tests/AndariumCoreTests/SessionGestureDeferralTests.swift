// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

// Which screen edges a run defers.
struct SessionGestureDeferralTests {
    private let screen = CGSize(width: 834, height: 1210)

    private func deferral(_ phase: Phase = .running(.normal), size: CGSize? = nil) -> ScreenEdges? {
        SessionGestureDeferral(phase: phase, sceneSize: size ?? screen, screenSize: screen).deferredEdges
    }

    @Test func aFullScreenNormalRunDefersAllFourEdges() {
        #expect(deferral() == [.top, .left, .bottom, .right])
    }

    @Test func aPhoneSizedScreenDefersAllFourEdgesToo() {
        let phone = CGSize(width: 402, height: 874)
        let edges = SessionGestureDeferral(phase: .running(.normal), sceneSize: phone, screenSize: phone)
        #expect(edges.deferredEdges == .all)
    }

    @Test func screensOtherThanANormalRunKeepTheirOwnPreference() {
        for phase in [Phase.home, .booting(.normal), .booting(.recovery), .running(.recovery), .failed] {
            #expect(deferral(phase) == nil)
        }
    }

    @Test func aWindowKeepsItsOwnPreference() {
        #expect(deferral(size: CGSize(width: 600, height: 900)) == nil)
        #expect(deferral(size: CGSize(width: 834, height: 900)) == nil)
    }

    @Test func edgesShareUIRectEdgeRawValues() {
        // UIRectEdge: top 1, left 2, bottom 4, right 8.
        #expect(ScreenEdges.top.rawValue == 1)
        #expect(ScreenEdges.left.rawValue == 2)
        #expect(ScreenEdges.bottom.rawValue == 4)
        #expect(ScreenEdges.right.rawValue == 8)
        #expect(ScreenEdges.all.rawValue == 15)
    }
}
