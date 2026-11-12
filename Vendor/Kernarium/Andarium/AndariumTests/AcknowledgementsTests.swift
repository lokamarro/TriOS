// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Andarium

/// The Acknowledgements resource the app bundles reads, groups every project
/// and carries a license text for each. `tests/tooling/andarium` keeps it
/// complete against what the app embeds.
struct AcknowledgementsTests {
    private let acknowledgements = Acknowledgements.load()

    @Test func theBundledResourceDecodes() {
        #expect(acknowledgements.projects.count >= 15)
        #expect(!acknowledgements.groups.isEmpty)
    }

    @Test func everyProjectHasAGroupAndAText() throws {
        let groups = Set(acknowledgements.groups.map(\.id))
        for project in acknowledgements.projects {
            #expect(groups.contains(project.group), "\(project.name) has no group")
            #expect(!project.copyright.isEmpty && !project.license.isEmpty && !project.spdx.isEmpty)
            let blocks = acknowledgements.blocks(of: project)
            #expect(!blocks.isEmpty, "\(project.name) has no license text")
            #expect(blocks.allSatisfy { !$0.text.isEmpty })
        }
    }

    @Test func everyGroupListsItsProjectsOnce() {
        let listed = acknowledgements.groups.flatMap { acknowledgements.projects(in: $0) }.map(\.id)
        #expect(listed.sorted() == acknowledgements.projects.map(\.id).sorted())
    }

    @Test func theSystemIsFirstAndUserModeLinuxIsNotListed() {
        #expect(acknowledgements.projects.first?.name == "Android Open Source Project")
        #expect(!acknowledgements.projects.contains { $0.name == "User-Mode Linux" })
        #expect(acknowledgements.project("linux")?.spdx == "GPL-2.0-only WITH Linux-syscall-note")
    }

    @Test func aLicensePageOpensOnTheListAboveIt() {
        #expect(InfoPage.license("mesa").trail == [.acknowledgements, .license("mesa")])
        #expect(InfoPage.acknowledgements.trail == [.acknowledgements])
    }
}
