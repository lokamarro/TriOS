// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Setting up the one guest: its storage and backups. A new guest takes every
/// vCPU the device offers and the default memory (`Guest.fresh`), which the
/// System section of Home can change afterwards.
struct NewGuestSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var draft: Guest?

    private var draftBinding: Binding<Guest> {
        Binding(
            get: { draft ?? Guest.fresh(on: model.host) },
            set: { draft = $0 }
        )
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 22) {
                    if !model.canCreate {
                        NoticeCard(
                            symbol: "externaldrive.badge.exclamationmark", color: .orange,
                            title: "Not enough free space",
                            text: "Setting up needs \(Formats.size(bytes: model.createRequirementBytes)) free for the guest's setup. This \(model.host.name) has \(Formats.size(bytes: model.host.freeStorageBytes)). Free up space in Settings › General › \(model.host.name) Storage."
                        )
                    }
                    StoragePanel(guest: draftBinding, host: model.host, stops: model.capacityStopsGiB)
                    HomeSection(title: "Backup") {
                        BackupOption(isOn: Binding(
                            get: { draftBinding.wrappedValue.includeInBackups },
                            set: { draftBinding.wrappedValue.includeInBackups = $0 }
                        ))
                    } footer: {
                        BackupNote(host: model.host)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 24)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("New guest")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", role: .cancel) { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") {
                        model.create(draft ?? Guest.fresh(on: model.host))
                        dismiss()
                    }
                    .fontWeight(.semibold)
                    .disabled(!model.canCreate)
                }
            }
        }
    }
}

/// Edit on Home's System section: the guest's vCPUs and memory on their
/// rulers, with the limits of the device. Save applies them; they take effect
/// when the guest next starts.
struct SystemSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var draft: Guest?

    private var current: Guest { model.guest ?? Guest.fresh(on: model.host) }

    private var draftBinding: Binding<Guest> {
        Binding(get: { draft ?? current }, set: { draft = $0 })
    }

    private var changed: Bool {
        guard let draft else { return false }
        return draft.cores != current.cores || draft.memoryMiB != current.memoryMiB
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                SystemControls(guest: draftBinding, host: model.host)
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 24)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("System")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", role: .cancel) { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        if let draft {
                            model.guest?.cores = draft.cores
                            model.guest?.memoryMiB = draft.memoryMiB
                        }
                        dismiss()
                    }
                    .fontWeight(.semibold)
                    .disabled(!changed)
                }
            }
        }
        .presentationDetents([.height(470)])
        .presentationSizing(FixedSheetSizing(width: 540, height: 480))
        .onAppear { draft = model.guest }
    }
}

/// The app, the system it carries and the notices for what is in it. Read-only.
struct InfoSheet: View {
    /// The project's page on GitHub.
    static let projectURL = URL(string: "https://github.com/imxieyi/Kernarium")!

    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var path: [InfoPage]

    init(page: InfoPage?) {
        _path = State(initialValue: page?.trail ?? [])
    }

    var body: some View {
        NavigationStack(path: $path) {
            List {
                Section {
                    HStack(spacing: 16) {
                        StackMark(lit: true, width: 62)
                            .frame(width: 80)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Andarium").font(.title3.weight(.semibold))
                            Text("Version \(model.system.appVersion)")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .padding(.vertical, 6)
                }
                Section("System info") {
                    LabeledContent("Build", value: model.system.buildLabel)
                    LabeledContent("Android version", value: model.system.version)
                    if let patch = model.system.securityPatch {
                        LabeledContent("Security patch", value: patch)
                    }
                    LabeledContent("Kernel", value: model.system.kernel)
                    LabeledContent("Architecture", value: model.system.architecture)
                    if !model.system.abis.isEmpty {
                        LabeledContent("Supported architectures", value: model.system.abis.joined(separator: ", "))
                    }
                }
                Section {
                    Link(destination: Self.projectURL) {
                        HStack(spacing: 12) {
                            IconTile(asset: "GitHubMark")
                                .accessibilityHidden(true)
                            Text("Project on GitHub")
                            Spacer()
                            Image(systemName: "arrow.up.right")
                                .font(.footnote.weight(.semibold))
                                .foregroundStyle(.secondary)
                                .accessibilityHidden(true)
                        }
                    }
                    NavigationLink(value: InfoPage.acknowledgements) {
                        HStack(spacing: 12) {
                            IconTile(symbol: "doc.text.fill")
                                .accessibilityHidden(true)
                            Text("Acknowledgements")
                        }
                    }
                }
            }
            .navigationTitle("Info")
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(for: InfoPage.self) { page in
                switch page {
                case .acknowledgements: AcknowledgementsView()
                case .license(let id): LicenseView(projectID: id)
                }
            }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}
