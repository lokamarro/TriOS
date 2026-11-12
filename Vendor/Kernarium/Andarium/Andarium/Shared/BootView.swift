// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The boot, from Andarium's start to the guest's `sys.boot_completed`: a
/// status line with a live dot, what is starting and the elapsed time; the
/// title; and the steps in one panel, with no bar or percentage of the whole
/// (only the disk write, which knows its length, says one). First start,
/// update, recovery and restart are variants of the same screen. A launch
/// has one run, so there is no Cancel.
///
/// A normal boot lays the guest's display out from the first frame and shows
/// it dimmed behind this screen, so the guest is seen starting. The dim is a
/// plain translucent fill and the panels are flat fills too, not materials or
/// glass: the display costs the presentation pass it costs when it is
/// visible, and the screen asks the render server for no blur of it.
/// Recovery keeps an opaque screen.
struct BootView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.colorScheme) private var colorScheme
    let kind: BootKind

    @State private var stage = 0
    @State private var startedAt = Date()

    struct Step {
        let title: String
        let detail: String
        let seconds: Double
    }

    var body: some View {
        let guest = model.guest ?? Guest.fresh(on: model.host)
        let steps = shownSteps(guest: guest)
        let frozen = model.frozenBootStage.map { min($0, steps.count - 1) }
        let current = frozen ?? stage
        VStack(spacing: 0) {
            statusLine(frozen: frozen != nil)
            ScrollView {
                content(guest: guest, system: model.system, steps: steps, current: current)
                    .containerRelativeFrame(.vertical, alignment: .center) { length, _ in length }
            }
            .scrollBounceBehavior(.basedOnSize)
        }
        .background { backdrop }
        .environment(\.colorScheme, dimmed ? .dark : colorScheme)
        .statusBarHidden(true)
        .task(id: kind) {
            guard frozen == nil else { return }
            if model.driver != nil {
                await followRuntime(stepCount: steps.count)
                return
            }
            for (index, step) in steps.enumerated() {
                withAnimation(.snappy) { stage = index }
                try? await Task.sleep(for: .seconds(step.seconds))
                if Task.isCancelled { return }
            }
            withAnimation(.snappy) { stage = steps.count }
            try? await Task.sleep(for: .seconds(0.4))
            model.finishBoot(kind)
        }
    }

    /// The guest's display shows through, dimmed, on every boot but Recovery's.
    private var dimmed: Bool { kind != .recovery }

    @ViewBuilder
    private var backdrop: some View {
        if dimmed {
            Color.black.opacity(0.64).ignoresSafeArea()
        } else {
            Color(.systemGroupedBackground).ignoresSafeArea()
        }
    }

    /// The steps, with the first saying what the disk write is doing while
    /// one is under way.
    private func shownSteps(guest: Guest) -> [Step] {
        var steps = Self.steps(kind, guest: guest, system: model.system)
        if model.preparingDisk {
            let detail = model.diskProgress.map { "Preparing \u{00B7} \(Int(($0 * 100).rounded()))%" }
                ?? "Preparing"
            steps[0] = Step(title: steps[0].title, detail: detail, seconds: 0)
        }
        return steps
    }

    /// The device application: the boot is as far as the guest's console
    /// says, and ends when the guest has something to show.
    private func followRuntime(stepCount: Int) async {
        while !Task.isCancelled && !model.bootTracker.isReady {
            let live = min(model.liveBootStage(), stepCount - 1)
            if live != stage { withAnimation(.snappy) { stage = live } }
            try? await Task.sleep(for: .milliseconds(200))
        }
        if Task.isCancelled { return }
        withAnimation(.snappy) { stage = stepCount }
        try? await Task.sleep(for: .seconds(0.4))
        if Task.isCancelled { return }
        model.finishBoot(kind)
    }

    // MARK: Parts

    private func statusLine(frozen: Bool) -> some View {
        HStack(spacing: 8) {
            PulsingDot()
            Text(Self.status(kind))
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(Color.accentColor)
            Spacer()
            TimelineView(.periodic(from: startedAt, by: 1)) { timeline in
                let seconds = frozen ? model.frozenElapsed : Int(timeline.date.timeIntervalSince(startedAt))
                Text(Formats.elapsed(seconds))
                    .font(.system(.subheadline, design: .rounded).weight(.medium))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
            }
        }
        .padding(.horizontal, 24)
        .frame(height: 44)
        .frame(maxWidth: 640)
        .frame(maxWidth: .infinity)
    }

    private func content(guest: Guest, system: SystemInfo, steps: [Step], current: Int) -> some View {
        VStack(alignment: .leading, spacing: 18) {
            Text(kind == .recovery ? "Recovery" : "Andarium")
                .font(.largeTitle.weight(.bold))
                .padding(.horizontal, 4)

            VStack(spacing: 0) {
                ForEach(Array(steps.enumerated()), id: \.offset) { index, step in
                    if index > 0 { Divider().padding(.leading, 54) }
                    stepRow(step, state: index < current ? .done : (index == current ? .active : .pending))
                }
            }
            .padding(.vertical, 4)
            .modifier(BootPanel(dimmed: dimmed, radius: 26))

            if let note = Self.note(kind, from: model.bootFrom, system: system) {
                Label {
                    Text(note)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } icon: {
                    Image(systemName: Self.noteSymbol(kind))
                        .foregroundStyle(Color.accentColor)
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
                .modifier(BootPanel(dimmed: dimmed, radius: 22))
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 20)
        .frame(maxWidth: 520)
        .frame(maxWidth: .infinity)
    }

    private enum StepState { case done, active, pending }

    private func stepRow(_ step: Step, state: StepState) -> some View {
        HStack(alignment: .top, spacing: 14) {
            ZStack {
                switch state {
                case .done:
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(Color.accentColor)
                case .active:
                    ProgressView()
                        .controlSize(.small)
                case .pending:
                    Image(systemName: "circle")
                        .foregroundStyle(.tertiary)
                }
            }
            .font(.title3)
            .frame(width: 24, height: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(step.title)
                    .font(.body.weight(.semibold))
                    .foregroundStyle(state == .pending ? AnyShapeStyle(.tertiary) : AnyShapeStyle(.primary))
                Text(step.detail)
                    .font(.subheadline)
                    .monospacedDigit()
                    .foregroundStyle(state == .active ? AnyShapeStyle(Color.accentColor) : (state == .done ? AnyShapeStyle(.secondary) : AnyShapeStyle(.tertiary)))
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    // MARK: Content per kind

    static func status(_ kind: BootKind) -> String {
        switch kind {
        case .normal: "Starting"
        case .firstStart: "First start"
        case .upgrade: "Updating"
        case .recovery: "Starting recovery"
        case .restart: "Restarting"
        }
    }

    /// The symbol of the note a first start or an update shows; no other
    /// boot has one.
    static func noteSymbol(_ kind: BootKind) -> String {
        kind == .firstStart ? "sparkles" : "arrow.triangle.2.circlepath"
    }

    static func note(_ kind: BootKind, from: String?, system: SystemInfo) -> String? {
        switch kind {
        case .firstStart:
            "The first-run setup may take a few minutes."
        case .upgrade:
            "Updating \(from.map { "from \(ReleaseLabel.revision(of: $0)) " } ?? "")to \(system.releaseRevision). Apps and files are kept."
        case .normal, .restart, .recovery:
            nil
        }
    }

    static func steps(_ kind: BootKind, guest: Guest, system info: SystemInfo) -> [Step] {
        let system = Step(title: "System image", detail: "\(info.releaseRevision) · read-only", seconds: 0.7)
        let kernel = Step(title: "Linux kernel", detail: "\(info.kernel.replacingOccurrences(of: "Linux ", with: "")) · \(Formats.vcpus(guest.cores)) · \(Formats.size(mib: guest.memoryMiB)) RAM", seconds: 0.9)
        let used = "\(Formats.size(bytes: guest.usedBytes)) of \(Formats.size(gib: guest.capacityGiB))"
        switch kind {
        case .recovery:
            return [system, kernel,
                    Step(title: "Storage", detail: used, seconds: 0.6),
                    Step(title: "Recovery", detail: "Loading recovery", seconds: 1.4)]
        case .firstStart:
            return [system, kernel,
                    Step(title: "Storage", detail: "Preparing \(Formats.size(gib: guest.effectiveCapacityGiB)) for first use", seconds: 0.9),
                    Step(title: "Startup", detail: "Setting up for the first time", seconds: 4.0)]
        case .upgrade:
            return [system, kernel,
                    Step(title: "Storage", detail: used, seconds: 0.7),
                    Step(title: "Startup", detail: "Updating apps for \(info.releaseRevision)", seconds: 3.6)]
        case .normal, .restart:
            let storage = guest.pendingCapacityGiB.map { "Growing from \(Formats.size(gib: guest.capacityGiB)) to \(Formats.size(gib: $0))" } ?? used
            return [system, kernel,
                    Step(title: "Storage", detail: storage, seconds: guest.pendingCapacityGiB == nil ? 0.6 : 1.2),
                    Step(title: "Startup", detail: "Starting services", seconds: 1.8)]
        }
    }
}

/// The boot screen's panels: a flat translucent fill over the dimmed
/// display, which needs no backdrop sampling, and glass over Recovery's
/// opaque screen.
private struct BootPanel: ViewModifier {
    let dimmed: Bool
    let radius: CGFloat

    func body(content: Content) -> some View {
        if dimmed {
            content.background(
                Color(.secondarySystemGroupedBackground).opacity(0.78),
                in: .rect(cornerRadius: radius, style: .continuous)
            )
        } else {
            content.glassEffect(.regular, in: .rect(cornerRadius: radius))
        }
    }
}

/// Overall boot progress: a cyan capsule on a quiet track.
struct ProgressBar: View {
    let value: Double
    var height: CGFloat = 8

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Color(.systemFill))
                Capsule()
                    .fill(Color.accentColor)
                    .frame(width: max(height, geo.size.width * value))
            }
        }
        .frame(height: height)
        .animation(.snappy, value: value)
        .accessibilityElement()
        .accessibilityLabel("Progress")
        .accessibilityValue("\(Int((value * 100).rounded())) percent")
    }
}

/// The live dot on the status line.
struct PulsingDot: View {
    @State private var dim = false

    var body: some View {
        Circle()
            .fill(Color.accentColor)
            .frame(width: 8, height: 8)
            .opacity(dim ? 0.35 : 1)
            .animation(.easeInOut(duration: 0.9).repeatForever(autoreverses: true), value: dim)
            .onAppear { dim = true }
            .accessibilityHidden(true)
    }
}
