// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// A page of Info that a navigation path or a launch can open directly.
enum InfoPage: Hashable {
    case acknowledgements
    case license(String)

    /// The pages to push, in order, to land on this one.
    var trail: [InfoPage] {
        switch self {
        case .acknowledgements: [.acknowledgements]
        case .license: [.acknowledgements, self]
        }
    }
}

/// The projects Andarium carries and their licenses: the bundled resource
/// `Acknowledgements.json`, which `Andarium/scripts/acknowledgements.py`
/// generates from the verified dependency sources and the kernel's license
/// files. A test keeps it complete against what the app embeds.
nonisolated struct Acknowledgements: Decodable, Sendable {
    struct Group: Decodable, Identifiable, Sendable {
        let id: String
        let title: String
    }

    /// A license text shared by projects.
    struct License: Decodable, Sendable {
        let title: String
        let text: String
    }

    /// A text a project carries itself, with a heading when it has several.
    struct Notice: Decodable, Sendable {
        let title: String?
        let text: String
    }

    struct Project: Decodable, Identifiable, Hashable, Sendable {
        let id: String
        let group: String
        let name: String
        /// The holders, without the symbol or "Copyright".
        let copyright: String
        /// How the license reads in a list ("MIT License").
        let license: String
        /// Its SPDX expression.
        let spdx: String
        /// Identifiers into `Acknowledgements.licenses`.
        let licenses: [String]
        let texts: [Notice]

        static func == (lhs: Project, rhs: Project) -> Bool { lhs.id == rhs.id }
        func hash(into hasher: inout Hasher) { hasher.combine(id) }
    }

    /// One heading and text on a project's license page.
    struct Block: Identifiable, Sendable {
        let id: Int
        let title: String?
        let text: String
    }

    let groups: [Group]
    let licenses: [String: License]
    let projects: [Project]

    func projects(in group: Group) -> [Project] {
        projects.filter { $0.group == group.id }
    }

    func project(_ id: String) -> Project? {
        projects.first { $0.id == id }
    }

    /// The texts of a project's page, its own first, then the shared ones.
    func blocks(of project: Project) -> [Block] {
        var blocks = project.texts.map { (title: $0.title, text: $0.text) }
        for id in project.licenses {
            if let license = licenses[id] { blocks.append((title: license.title, text: license.text)) }
        }
        return blocks.enumerated().map { Block(id: $0.offset, title: $0.element.title, text: $0.element.text) }
    }

    static let empty = Acknowledgements(groups: [], licenses: [:], projects: [])

    /// The app's own, read once.
    static let bundled = load()

    static func decode(_ data: Data) throws -> Acknowledgements {
        try JSONDecoder().decode(Acknowledgements.self, from: data)
    }

    /// The acknowledgements in a bundle; empty if the resource is missing.
    static func load(from bundle: Bundle = .main) -> Acknowledgements {
        guard let url = bundle.url(forResource: "Acknowledgements", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let decoded = try? decode(data)
        else {
            assertionFailure("Acknowledgements.json is missing or unreadable")
            return .empty
        }
        return decoded
    }
}

/// The list of projects, grouped, ending with the line about names and marks.
struct AcknowledgementsView: View {
    private let acknowledgements = Acknowledgements.bundled

    var body: some View {
        List {
            ForEach(acknowledgements.groups) { group in
                let projects = acknowledgements.projects(in: group)
                Section {
                    ForEach(projects) { project in
                        NavigationLink(value: InfoPage.license(project.id)) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(project.name)
                                Text(project.license)
                                    .font(.footnote)
                                    .foregroundStyle(.secondary)
                            }
                            .padding(.vertical, 2)
                        }
                    }
                } header: {
                    Text(group.title)
                } footer: {
                    if group.id == acknowledgements.groups.last?.id {
                        Text("Android, LineageOS, Linux and the other names and marks here belong to their owners. They identify the software built into Andarium and imply no endorsement.")
                            .padding(.top, 8)
                    }
                }
            }
        }
        .navigationTitle("Acknowledgements")
        .navigationBarTitleDisplayMode(.inline)
    }
}

/// One project: who holds the copyright, the license, and its full text.
struct LicenseView: View {
    let projectID: String
    private let acknowledgements = Acknowledgements.bundled

    var body: some View {
        ScrollView {
            if let project = acknowledgements.project(projectID) {
                VStack(alignment: .leading, spacing: 22) {
                    summary(project)
                    ForEach(acknowledgements.blocks(of: project)) { block in
                        VStack(alignment: .leading, spacing: 10) {
                            if let title = block.title {
                                Text(title).font(.headline)
                            }
                            ForEach(Array(paragraphs(block.text).enumerated()), id: \.offset) { _, paragraph in
                                Text(verbatim: paragraph)
                                    .font(.footnote)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                    }
                }
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 20)
                .padding(.vertical, 16)
            } else {
                ContentUnavailableView("No license", systemImage: "doc.text")
            }
        }
        .background(Color(.systemGroupedBackground))
        .navigationTitle(acknowledgements.project(projectID)?.name ?? "License")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func summary(_ project: Acknowledgements.Project) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            field("Copyright", "© \(project.copyright)")
            field("License", project.license)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(.secondarySystemGroupedBackground), in: .rect(cornerRadius: 20))
    }

    private func field(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(.footnote)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.subheadline)
        }
    }

    private func paragraphs(_ text: String) -> [String] {
        text.components(separatedBy: "\n\n")
    }
}
