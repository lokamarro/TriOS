// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The only screen before a run: the guest's storage, processors and memory,
/// its options and Start. Without a guest it offers to set one up.
struct HomeView: View {
    @Environment(AppModel.self) private var model
    let hints: ScreenHints

    var body: some View {
        @Bindable var model = model
        NavigationStack {
            GeometryReader { geo in
                let wide = geo.size.width >= 700
                Group {
                    if model.guest != nil {
                        ScrollViewReader { proxy in
                            ScrollView {
                                if wide && geo.size.width > geo.size.height {
                                    wideLayout
                                } else if wide {
                                    tallLayout
                                } else {
                                    narrowLayout
                                }
                            }
                            .onAppear {
                                if let target = hints.scrollTarget {
                                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                                        proxy.scrollTo(target, anchor: .bottom)
                                    }
                                }
                            }
                        }
                        .safeAreaBar(edge: .bottom) { startBar(wide: wide) }
                    } else {
                        WelcomeView()
                    }
                }
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Andarium")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        model.sheet = .info
                    } label: {
                        Label("Info", systemImage: "info")
                    }
                }
            }
        }
        .sheet(item: $model.sheet) { kind in
            switch kind {
            case .create: NewGuestSheet()
            case .resize: ResizeSheet()
            case .system: SystemSheet()
            case .info: InfoSheet(page: hints.infoPage)
            }
        }
        .microphoneAccessAlert()
        .alert("Erase guest?", isPresented: $model.confirmingErase) {
            Button("Erase guest", role: .destructive) { model.erase() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Everything the guest has stored is deleted. This can\u{2019}t be undone.")
        }
        .alert(StartSpace.title, isPresented: $model.startBlocked) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(StartSpace(neededBytes: Limits.startReserveBytes, freeBytes: model.host.freeStorageBytes)
                .message(device: model.host.name))
        }
    }

    // MARK: Layouts

    private var narrowLayout: some View {
        VStack(spacing: 22) {
            notices
            SystemSection()
            guestSections
            RecoveryErasePanel()
                .id("erase")
        }
        .padding(.horizontal, 16)
        .padding(.top, 4)
        .padding(.bottom, 20)
    }

    /// iPad in portrait or a tall window: the controls across the top, the
    /// options and Recovery and erase beneath.
    private var tallLayout: some View {
        VStack(spacing: 28) {
            notices
            SystemSection()
            guestSections
            RecoveryErasePanel()
                .id("erase")
        }
        .frame(maxWidth: 680)
        .padding(.horizontal, 28)
        .padding(.top, 12)
        .padding(.bottom, 20)
        .frame(maxWidth: .infinity)
    }

    private var wideLayout: some View {
        HStack(alignment: .top, spacing: 24) {
            VStack(spacing: 22) {
                notices
                SystemSection()
            }
            .frame(maxWidth: 520)
            VStack(spacing: 22) {
                guestSections
                RecoveryErasePanel()
                    .id("erase")
            }
            .frame(maxWidth: 400)
        }
        .padding(.horizontal, 28)
        .padding(.top, 8)
        .padding(.bottom, 20)
        .frame(maxWidth: .infinity)
    }

    /// Display, Capabilities and Backup: what the guest shows and may use.
    @ViewBuilder
    private var guestSections: some View {
        DisplaySection()
        CapabilitiesSection()
        BackupSection()
    }

    @ViewBuilder
    private var notices: some View {
        @Bindable var model = model
        if !model.host.runsGuestGraphics {
            NoticeCard(
                symbol: "cpu", color: .orange,
                title: DeviceRequirement.title,
                text: DeviceRequirement.explanation(deviceName: model.host.name)
            )
        }
        if model.lowSpace {
            NoticeCard(
                symbol: "externaldrive.badge.exclamationmark", color: .orange,
                title: "\(Formats.size(bytes: model.host.freeStorageBytes)) free on this \(model.host.name)",
                text: "The guest needs \(Formats.size(bytes: Limits.startReserveBytes)) free to start. Free up space in Settings › General › \(model.host.name) Storage."
            )
        }
        if let guest = model.guest, model.host.memory.isAboveSafe(guest.memoryMiB, vCPUs: guest.cores) {
            NoticeCard(
                symbol: "exclamationmark.triangle.fill", color: .orange,
                title: MemoryWarning.title,
                text: MemoryWarning.explanation(
                    memoryMiB: guest.memoryMiB,
                    safeMiB: model.host.memory.safeMemoryMiB(vCPUs: guest.cores),
                    deviceName: model.host.name
                )
            )
        }
        switch model.plan {
        case .update(_, let release):
            NoticeCard(
                symbol: "arrow.down.circle.fill", color: .accentColor,
                title: "System image updated",
                text: "Andarium system image is updated to \(ReleaseLabel.revision(of: release)). The next start finishes the update and takes a little longer."
            )
        case .unfinishedUpdate(let release, let canRetry, let canRestore):
            UnfinishedUpdateCard(release: release, canRetry: canRetry, canRestore: canRestore)
        case .blocked(let reason):
            NoticeCard(
                symbol: "exclamationmark.triangle.fill", color: .orange,
                title: reason.title, text: reason.explanation
            )
        case .firstStart, .normal:
            EmptyView()
        }
        if let notice = model.notice {
            switch notice {
            case .closedInBackground:
                NoticeCard(
                    symbol: "moon.zzz.fill", color: .secondary,
                    title: "iOS closed Andarium",
                    text: "The guest was shut down in the background.",
                    onClose: { withAnimation { model.notice = nil } }
                )
            case .sessionEnded(let ended):
                NoticeCard(
                    symbol: "exclamationmark.triangle.fill", color: .orange,
                    title: ended.title, text: ended.text,
                    onClose: { withAnimation { model.notice = nil } }
                )
            }
        }
    }

    // MARK: Start

    /// The main action always starts Android normally.
    private func startBar(wide: Bool) -> some View {
        StartButton()
            .frame(maxWidth: wide ? 520 : .infinity)
            .padding(.horizontal, 16)
            .padding(.top, 14)
            .padding(.bottom, 6)
            .frame(maxWidth: .infinity)
            .background {
                // Content fades out under the button instead of showing through.
                LinearGradient(
                    stops: [
                        .init(color: Color(.systemGroupedBackground).opacity(0), location: 0),
                        .init(color: Color(.systemGroupedBackground).opacity(0.92), location: 0.45),
                        .init(color: Color(.systemGroupedBackground), location: 1),
                    ],
                    startPoint: .top, endPoint: .bottom
                )
                .ignoresSafeArea(edges: .bottom)
                .allowsHitTesting(false)
            }
    }
}

/// The main action starts Android normally. Recovery lives in the Recovery
/// and Erase section.
struct StartButton: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Button {
            model.start(.normal)
        } label: {
            Label(title, systemImage: "play.fill")
                .frame(maxWidth: .infinity)
                .frame(height: 52)
                .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .font(.body.weight(.semibold))
        .foregroundStyle(.white)
        .background(Capsule().fill(Color.accentColor))
        .glassEffect(.regular.tint(Color.accentColor).interactive(), in: .capsule)
        .opacity(blocked ? 0.4 : 1)
        .disabled(blocked)
    }

    private var blocked: Bool { !model.plan.allowsStart || !model.host.runsGuestGraphics }

    private var title: String {
        guard model.guest != nil, model.plan.allowsStart else { return "Start" }
        switch model.bootKind(.normal) {
        case .firstStart: return "Start and set up"
        case .upgrade: return "Update and start"
        default: return "Start"
        }
    }
}

/// The rounded square that marks every row: a white glyph on the accent. The
/// glyph is an SF Symbol, or an image of the asset catalog that is already
/// white (it is shown as it is, never tinted).
struct IconTile: View {
    private enum Glyph {
        case symbol(String)
        case asset(String)
    }

    private let glyph: Glyph
    let color: Color

    init(symbol: String, color: Color = .accentColor) {
        glyph = .symbol(symbol)
        self.color = color
    }

    init(asset: String, color: Color = .accentColor) {
        glyph = .asset(asset)
        self.color = color
    }

    var body: some View {
        glyphView
            .frame(width: 30, height: 30)
            .background(color.gradient, in: .rect(cornerRadius: 8, style: .continuous))
    }

    @ViewBuilder private var glyphView: some View {
        switch glyph {
        case .symbol(let name):
            Image(systemName: name)
                .font(.system(size: 14, weight: .semibold))
                .foregroundStyle(.white)
        case .asset(let name):
            Image(name)
                .resizable()
                .scaledToFit()
                .frame(width: 16, height: 16)
        }
    }
}

/// First launch: there is no guest yet.
struct WelcomeView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 20)
            StackMark(lit: false, width: 150)
                .padding(.bottom, 34)
            Text("No guest yet")
                .font(.title2.weight(.bold))
            if !model.host.runsGuestGraphics {
                NoticeCard(
                    symbol: "cpu", color: .orange,
                    title: DeviceRequirement.title,
                    text: DeviceRequirement.explanation(deviceName: model.host.name)
                )
                .frame(maxWidth: 400)
                .padding(.top, 20)
            }
            Button {
                model.sheet = .create
            } label: {
                Text("Set up guest")
                    .font(.body.weight(.semibold))
                    .frame(width: 220, height: 50)
            }
            .buttonStyle(.glassProminent)
            .disabled(!model.host.runsGuestGraphics)
            .padding(.top, 28)
            Spacer(minLength: 20)
            Label("\(Formats.size(bytes: model.host.freeStorageBytes)) free on this \(model.host.name)", systemImage: "internaldrive")
                .font(.footnote)
                .foregroundStyle(model.canCreate ? Color.secondary : Color.orange)
                .padding(.bottom, 12)
        }
        .padding(.horizontal, 24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// A dismissible message above the controls.
struct NoticeCard: View {
    let symbol: String
    let color: Color
    let title: String
    let text: String
    var onClose: (() -> Void)?

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol)
                .font(.title3)
                .foregroundStyle(color)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline.weight(.semibold))
                Text(text)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
            if let onClose {
                Button(action: onClose) {
                    Image(systemName: "xmark")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .frame(width: 28, height: 28)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Dismiss")
            }
        }
        .padding(14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }
}

/// An update that did not reach its end. Start tries it again; the one
/// button, Restore, puts the data back to what the update started from.
struct UnfinishedUpdateCard: View {
    @Environment(AppModel.self) private var model
    let release: String
    let canRetry: Bool
    let canRestore: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "exclamationmark.arrow.triangle.2.circlepath")
                    .font(.title3)
                    .foregroundStyle(Color.orange)
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Andarium's update didn't finish").font(.subheadline.weight(.semibold))
                    Text(text)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            if canRestore {
                Button("Restore") { model.restoreUpdate() }
                    .buttonStyle(.glass)
                    .controlSize(.regular)
                    .accessibilityIdentifier("home-update-restore")
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }

    private var text: String {
        let revision = ReleaseLabel.revision(of: release)
        return switch (canRetry, canRestore) {
        case (true, true):
            "The start that updates to \(revision) did not reach its end. Start tries it again, or Restore puts back your data from before the update."
        case (true, false):
            "The start that updates to \(revision) did not reach its end. Start tries it again, or erase the guest to start over."
        case (false, true):
            "This app version carries a different release than the update that did not finish. Restore puts back your data from before it, or erase the guest to start over."
        case (false, false):
            "The start that updates to \(revision) did not reach its end, and nothing from before it is left to put back. Erase the guest to start over."
        }
    }
}

/// Recovery and erasing the guest share one section. Its title is not
/// "Restore", the name of the unfinished-update card's button.
struct RecoveryErasePanel: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Recovery and erase")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.secondary)
                .padding(.leading, 18)
            VStack(spacing: 0) {
                Button {
                    model.start(.recovery)
                } label: {
                    HStack(spacing: 12) {
                        IconTile(symbol: "wrench.adjustable.fill", color: .gray)
                        Text("Start in recovery")
                        Spacer()
                        Image(systemName: "chevron.right")
                            .font(.footnote.weight(.semibold))
                            .foregroundStyle(.tertiary)
                    }
                    .padding(.horizontal, 16)
                    .frame(minHeight: 54)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(!model.plan.offersRecovery || !model.host.runsGuestGraphics)
                Divider().padding(.leading, 58)
                Button {
                    model.confirmingErase = true
                } label: {
                    HStack(spacing: 12) {
                        IconTile(symbol: "trash.fill", color: .red)
                        Text("Erase guest…")
                            .foregroundStyle(.red)
                        Spacer()
                        if let guest = model.guest {
                            Text(Formats.size(bytes: guest.usedBytes))
                                .foregroundStyle(.secondary)
                        }
                    }
                    .padding(.horizontal, 16)
                    .frame(minHeight: 54)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
            .glassEffect(.regular.interactive(), in: .rect(cornerRadius: 26))
        }
    }
}
