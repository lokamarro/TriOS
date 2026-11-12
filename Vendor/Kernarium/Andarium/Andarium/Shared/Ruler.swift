// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// A ruler with a cyan needle: the whole range as ticks, the chosen value
/// under the needle. Drag along it or tap a tick. Limits are drawn on the
/// ruler itself: ticks past a recommendation turn orange, ticks past a second,
/// harder limit turn red, and ticks below a floor are hatched and out of reach.
struct Ruler: View {
    @Binding var index: Int
    let count: Int
    /// Positions below this one cannot be chosen.
    var floor: Int = 0
    /// Every this many positions a tick is long and labelled.
    var majorEvery: Int = 1
    var majorOffset: Int = 0
    /// Draw a short tick only every this many positions.
    var minorEvery: Int = 1
    let label: (Int) -> String
    /// What VoiceOver reads for a position, where its label leaves out a unit.
    var valueText: ((Int) -> String)?
    /// Positions at and above this one are past the recommended value.
    var warnFrom: Int?
    /// Positions at and above this one are past a harder limit than `warnFrom`.
    var severeFrom: Int?
    var accessibilityName: String = ""

    @State private var dragging = false
    /// A drag that starts mostly vertical belongs to the scroll view.
    @State private var horizontal: Bool?

    private let inset: CGFloat = 12
    private let track: CGFloat = 30

    var body: some View {
        GeometryReader { geo in
            let step = (geo.size.width - inset * 2) / CGFloat(max(count - 1, 1))
            let x = { (i: Int) -> CGFloat in inset + CGFloat(i) * step }
            ZStack(alignment: .topLeading) {
                Capsule()
                    .fill(Color.primary.opacity(0.06))
                    .frame(width: geo.size.width, height: track)
                if let warnFrom, warnFrom < count {
                    let end = severeFrom.map { min($0, count) } ?? count
                    let endsRuler = end >= count
                    UnevenRoundedRectangle(
                        topLeadingRadius: 4, bottomLeadingRadius: 4,
                        bottomTrailingRadius: endsRuler ? track / 2 : 4, topTrailingRadius: endsRuler ? track / 2 : 4
                    )
                        .fill(Color.orange.opacity(0.14))
                        .frame(width: (endsRuler ? geo.size.width : x(end) - step / 2) - x(warnFrom) + step / 2, height: track)
                        .offset(x: x(warnFrom) - step / 2)
                }
                if let severeFrom, severeFrom < count {
                    UnevenRoundedRectangle(topLeadingRadius: 4, bottomLeadingRadius: 4, bottomTrailingRadius: track / 2, topTrailingRadius: track / 2)
                        .fill(Color.red.opacity(0.14))
                        .frame(width: geo.size.width - x(severeFrom) + step / 2, height: track)
                        .offset(x: x(severeFrom) - step / 2)
                }
                if floor > 0 {
                    let shape = UnevenRoundedRectangle(topLeadingRadius: track / 2, bottomLeadingRadius: track / 2, bottomTrailingRadius: 4, topTrailingRadius: 4)
                    shape
                        .fill(Color.primary.opacity(0.07))
                        .overlay(
                            HatchPattern()
                                .stroke(Color.primary.opacity(0.14), lineWidth: 1)
                                .clipShape(shape)
                        )
                        .frame(width: x(floor) - step / 2, height: track)
                }
                ForEach(0..<count, id: \.self) { i in
                    let major = (i - majorOffset) % majorEvery == 0
                    let warn = warnFrom.map { i >= $0 } ?? false
                    let severe = severeFrom.map { i >= $0 } ?? false
                    let below = i < floor
                    if major || (i - majorOffset) % minorEvery == 0 {
                        Capsule()
                            .fill(tickColor(major: major, warn: warn, severe: severe, below: below))
                            .frame(width: 2, height: major ? 14 : 8)
                            .position(x: x(i), y: track / 2)
                    }
                    if major {
                        Text(label(i))
                            .font(.system(size: 12, weight: i == index ? .bold : .medium, design: .rounded).monospacedDigit())
                            .foregroundStyle(labelStyle(selected: i == index, warn: warn, severe: severe, below: below))
                            .fixedSize()
                            .position(x: x(i), y: track + 13)
                    }
                }
                Capsule()
                    .fill(Color.accentColor)
                    .frame(width: 6, height: dragging ? 40 : 36)
                    .overlay(Capsule().strokeBorder(.white.opacity(0.95), lineWidth: 1.5))
                    .shadow(color: Color.accentColor.opacity(0.45), radius: dragging ? 6 : 3, y: 1)
                    .position(x: x(index), y: track / 2)
                    .animation(.snappy(duration: 0.22), value: index)
            }
            .contentShape(Rectangle())
            .onTapGesture(coordinateSpace: .local) { location in
                set(at: location.x, step: step)
            }
            .simultaneousGesture(
                DragGesture(minimumDistance: 6, coordinateSpace: .local)
                    .onChanged { value in
                        if horizontal == nil {
                            horizontal = abs(value.translation.width) > abs(value.translation.height)
                        }
                        guard horizontal == true else { return }
                        dragging = true
                        set(at: value.location.x, step: step)
                    }
                    .onEnded { _ in
                        dragging = false
                        horizontal = nil
                    }
            )
        }
        .frame(height: 52)
        .sensoryFeedback(.selection, trigger: index)
        .accessibilityElement()
        .accessibilityLabel(accessibilityName)
        .accessibilityValue((valueText ?? label)(index))
        .accessibilityAdjustableAction { direction in
            switch direction {
            case .increment: index = min(count - 1, index + 1)
            case .decrement: index = max(floor, index - 1)
            @unknown default: break
            }
        }
    }

    private func set(at x: CGFloat, step: CGFloat) {
        let raw = Int(((x - inset) / step).rounded())
        let clamped = min(count - 1, max(floor, raw))
        if clamped != index { index = clamped }
    }

    private func tickColor(major: Bool, warn: Bool, severe: Bool, below: Bool) -> Color {
        if below { return Color.primary.opacity(0.14) }
        if severe { return Color.red.opacity(major ? 0.95 : 0.6) }
        if warn { return Color.orange.opacity(major ? 0.95 : 0.6) }
        return Color.primary.opacity(major ? 0.45 : 0.22)
    }

    private func labelStyle(selected: Bool, warn: Bool, severe: Bool, below: Bool) -> AnyShapeStyle {
        if selected { return AnyShapeStyle(.primary) }
        if below { return AnyShapeStyle(Color.primary.opacity(0.25)) }
        if severe { return AnyShapeStyle(Color.red) }
        if warn { return AnyShapeStyle(Color.orange) }
        return AnyShapeStyle(.secondary)
    }
}

/// Diagonal hatching for the part of a ruler that cannot be chosen.
nonisolated struct HatchPattern: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        var x = rect.minX - rect.height
        while x < rect.maxX {
            path.move(to: CGPoint(x: x, y: rect.maxY))
            path.addLine(to: CGPoint(x: x + rect.height, y: rect.minY))
            x += 7
        }
        return path
    }
}

/// Processor, memory and graphics memory, the three rows of the System sheet.
/// The first two have their value on the right and a ruler beneath, and keep
/// the limits of the host: the processor count stops at the vCPUs the device
/// offers, and memory runs from 1.5 GiB to the bound of the chosen vCPU count,
/// with the bound said beneath it. The third is the estimate of the guest's
/// graphics memory (VRAM) for the chosen memory and vCPUs, which the host
/// reads from the runtime (`MemoryPolicy.graphicsMemoryMiB`); it is left out
/// where the host gives none.
struct SystemControls: View {
    @Binding var guest: Guest
    let host: HostDevice

    var body: some View {
        VStack(spacing: 0) {
            processor
            Divider().padding(.leading, 58)
            memory
            if let vram = host.memory.graphicsMemoryMiB(forMemory: guest.memoryMiB, vCPUs: guest.cores) {
                Divider().padding(.leading, 58)
                GraphicsMemoryRow(
                    estimateMiB: vram, warns: host.memory.risk(guest.memoryMiB, vCPUs: guest.cores) != .none
                )
            }
        }
        .glassEffect(.regular, in: .rect(cornerRadius: 30))
    }

    private var processor: some View {
        ControlRow(
            title: "Processor", symbol: "cpu",
            value: Formats.vcpus(guest.cores)
        ) {
            Ruler(
                index: Binding(get: { guest.cores - 1 }, set: { guest.setCores($0 + 1, on: host) }),
                count: host.processorCount,
                label: { "\($0 + 1)" },
                accessibilityName: "Processor"
            )
        } note: {
            EmptyView()
        }
    }

    private var memory: some View {
        // The picker follows the vCPU count live: more vCPUs hold more of the
        // process's memory, so the top of the ruler falls as the count rises.
        let stops = host.memory.stops(vCPUs: guest.cores)
        // Whole gibibytes are the long, labelled ticks: the first one is this
        // many steps above the bottom of the ruler.
        let bottom = stops.first ?? Limits.minimumMemoryMiB
        let steps = (1024 - bottom % 1024) % 1024 / Limits.memoryStepMiB
        // Sizes past what a new guest gets turn orange, and past the size
        // where iOS is likely to close Andarium red where that is a higher
        // one; the line beneath says why while the chosen size is one of
        // them, the worse risk alone. Both sizes follow the vCPU count as the
        // top does.
        let defaultMiB = host.memory.defaultMemoryMiB(vCPUs: guest.cores)
        let safeMiB = host.memory.safeMemoryMiB(vCPUs: guest.cores)
        let distinct = safeMiB > defaultMiB
        let risk = host.memory.risk(guest.memoryMiB, vCPUs: guest.cores)
        return ControlRow(title: "Memory", symbol: "memorychip", value: Formats.size(mib: guest.memoryMiB)) {
            Ruler(
                index: Binding(
                    get: { stops.lastIndex(where: { $0 <= guest.memoryMiB }) ?? 0 },
                    set: { guest.memoryMiB = stops[min(max($0, 0), stops.count - 1)] }
                ),
                count: stops.count,
                majorEvery: 1024 / Limits.memoryStepMiB,
                majorOffset: steps,
                minorEvery: 256 / Limits.memoryStepMiB,
                label: { Formats.gibibytes(mib: stops[$0]) },
                valueText: { Formats.size(mib: stops[$0]) },
                warnFrom: stops.firstIndex { $0 > defaultMiB },
                severeFrom: distinct ? stops.firstIndex { $0 > safeMiB } : nil,
                accessibilityName: "Memory"
            )
        } note: {
            switch risk {
            case .closing:
                Label(MemoryWarning.note(safeMiB: safeMiB), systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(distinct ? Color.red : Color.orange)
            case .graphics:
                Label(GraphicsMemoryWarning.note(defaultMiB: defaultMiB), systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
            case .none:
                EmptyView()
            }
        }
    }
}

/// The third row of the System sheet: the estimate of the guest's graphics
/// memory, orange while the chosen memory is above what a new guest gets, with
/// the note that it is not part of the RAM.
struct GraphicsMemoryRow: View {
    let estimateMiB: Int
    let warns: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ControlHeader(
                title: GraphicsMemoryWarning.title, symbol: "cube", value: "\u{2248} " + Formats.size(mib: estimateMiB),
                valueColor: warns ? .orange : .primary
            )
            Text(GraphicsMemoryWarning.separateNote)
                .font(.footnote)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 16)
        .padding(.top, 14)
        .padding(.bottom, 12)
    }
}

/// A new guest's storage in one glass panel: its capacity on a ruler, with
/// the host's free space beneath it.
struct StoragePanel: View {
    @Binding var guest: Guest
    let host: HostDevice
    /// The sizes the ruler offers (`AppModel.capacityStopsGiB`).
    let stops: [Int]

    var body: some View {
        ControlRow(title: "Storage", symbol: "internaldrive", value: Formats.size(gib: guest.capacityGiB)) {
            Ruler(
                index: Binding(
                    get: { stops.firstIndex { $0 >= guest.capacityGiB } ?? stops.count - 1 },
                    set: { guest.capacityGiB = stops[min(max($0, 0), stops.count - 1)] }
                ),
                count: stops.count,
                majorEvery: 4,
                majorOffset: 3,
                label: { "\(stops[$0])" },
                valueText: { Formats.size(gib: stops[$0]) },
                accessibilityName: "Storage"
            )
        } note: {
            Text("\(Formats.size(bytes: host.freeStorageBytes)) free on this \(host.name)")
                .foregroundStyle(.secondary)
        }
        .glassEffect(.regular, in: .rect(cornerRadius: 30))
    }
}

/// An existing guest's storage, the second row of Home's Guest system
/// section: what the guest holds of its capacity, as words and on a bar, a
/// pending growth or the largest size as a note under the bar, and Resize.
struct StorageRow: View {
    let guest: Guest
    /// The sizes Resize offers (`AppModel.capacityStopsGiB`).
    let stops: [Int]
    var resize: () -> Void = {}

    /// Storage only grows, so at the largest size Resize has nothing to offer.
    private var atMaximum: Bool {
        guest.capacityGiB >= (stops.last ?? Limits.capacityRangeGiB.upperBound)
    }

    var body: some View {
        SystemRow(
            symbol: "internaldrive", title: "Storage",
            subtitle: "\(Formats.size(bytes: guest.usedBytes)) of \(Formats.size(gib: guest.capacityGiB))",
            button: SystemRowButton(
                title: "Resize", identifier: "home-storage-resize", isEnabled: !atMaximum, action: resize
            )
        ) {
            UsageBar(
                used: guest.usedGiB, capacity: Double(guest.capacityGiB),
                pending: guest.pendingCapacityGiB.map(Double.init), height: 8
            )
            note
        }
    }

    @ViewBuilder
    private var note: some View {
        if let pending = guest.pendingCapacityGiB {
            Label("Grows to \(Formats.size(gib: pending)) at the next start", systemImage: "arrow.up.circle.fill")
                .font(.footnote)
                .foregroundStyle(Color.accentColor)
        } else if atMaximum {
            Text("\(Formats.size(gib: stops.last ?? guest.capacityGiB)) is the largest size")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }
}

/// The icon tile, title and value that head every control row.
struct ControlHeader: View {
    let title: String
    let symbol: String
    let value: String
    var valueColor: Color = .primary

    var body: some View {
        HStack(spacing: 12) {
            IconTile(symbol: symbol)
            Text(title)
            Spacer(minLength: 8)
            Text(value)
                .font(.system(size: 20, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .foregroundStyle(valueColor)
                .contentTransition(.numericText())
                .animation(.snappy, value: value)
        }
    }
}

/// A control row: header, ruler, and a line about the limits.
struct ControlRow<Dial: View, Note: View>: View {
    let title: String
    let symbol: String
    let value: String
    var valueColor: Color = .primary
    @ViewBuilder var dial: Dial
    @ViewBuilder var note: Note

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ControlHeader(title: title, symbol: symbol, value: value, valueColor: valueColor)
            dial
                .padding(.top, 2)
            note
                .font(.footnote)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 16)
        .padding(.top, 14)
        .padding(.bottom, 12)
    }
}

/// What Android has written out of its capacity; a pending growth is drawn
/// as a dashed outline to the new size.
struct UsageBar: View {
    let used: Double
    let capacity: Double
    var pending: Double?
    var height: CGFloat = 10

    var body: some View {
        GeometryReader { geo in
            let total = max(capacity, pending ?? 0)
            let usedWidth = max(8, geo.size.width * used / total)
            let capWidth = geo.size.width * capacity / total
            ZStack(alignment: .leading) {
                if pending != nil {
                    Capsule()
                        .strokeBorder(Color.accentColor.opacity(0.6), style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
                } else {
                    Capsule().fill(Color(.tertiarySystemFill))
                }
                Capsule().fill(Color(.systemFill)).frame(width: capWidth)
                if used > 0 {
                    Capsule().fill(Color.accentColor).frame(width: usedWidth)
                }
            }
        }
        .frame(height: height)
        .accessibilityHidden(true)
    }
}

/// Resize opens on the same ruler, from 4 GiB in steps of 4. Storage only
/// grows: the sizes below the current one are hatched and out of reach, and
/// growth applies at the next start.
struct ResizeSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var capacity = 16

    var body: some View {
        let guest = model.guest ?? Guest.fresh(on: model.host)
        let stops = model.capacityStopsGiB
        NavigationStack {
            VStack(spacing: 16) {
                VStack(spacing: 2) {
                    Text(Formats.size(gib: capacity))
                        .font(.system(size: 50, weight: .semibold, design: .rounded))
                        .monospacedDigit()
                        .contentTransition(.numericText(value: Double(capacity)))
                    Text(capacity == guest.capacityGiB ? "Current size" : "From \(Formats.size(gib: guest.capacityGiB))")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
                .padding(.top, 4)
                Ruler(
                    index: Binding(
                        get: { stops.firstIndex { $0 >= capacity } ?? stops.count - 1 },
                        set: { value in
                            withAnimation(.snappy) { capacity = stops[min(max(value, 0), stops.count - 1)] }
                        }
                    ),
                    count: stops.count,
                    // A size an earlier version allowed, off the 4 GiB steps, has
                    // the next step up as its first choice.
                    floor: stops.firstIndex { $0 >= guest.capacityGiB } ?? stops.count - 1,
                    majorEvery: 4,
                    majorOffset: 3,
                    label: { "\(stops[$0])" },
                    valueText: { Formats.size(gib: stops[$0]) },
                    accessibilityName: "Storage"
                )
                .padding(.horizontal, 6)
                UsageBar(used: guest.usedGiB, capacity: Double(capacity))
                    .padding(.horizontal, 18)
                status(guest)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 18)
            .navigationTitle("Resize storage")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", systemImage: "xmark", role: .cancel) { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Resize", systemImage: "checkmark") {
                        model.guest?.setCapacity(capacity, maximum: model.releaseMaximumGiB)
                        dismiss()
                    }
                    .disabled(capacity == guest.effectiveCapacityGiB)
                }
            }
        }
        .presentationDetents([.height(470)])
        .presentationSizing(FixedSheetSizing(width: 540, height: 480))
        .onAppear {
            capacity = model.resizePreset ?? guest.effectiveCapacityGiB
        }
    }

    @ViewBuilder
    private func status(_ guest: Guest) -> some View {
        if capacity > guest.capacityGiB {
            line("arrow.up.circle.fill", .accentColor,
                 "Grows to \(Formats.size(gib: capacity)) at the next start", nil)
        } else if let pending = guest.pendingCapacityGiB {
            line("arrow.uturn.backward.circle.fill", .secondary,
                 "Stays at \(Formats.size(gib: guest.capacityGiB))",
                 "The growth to \(Formats.size(gib: pending)) is cancelled.")
        } else {
            line("internaldrive", .secondary,
                 "\(Formats.size(bytes: guest.usedBytes)) used of \(Formats.size(gib: guest.capacityGiB))",
                 "Storage can only grow. A larger size applies at the next start.")
        }
    }

    private func line(_ symbol: String, _ tint: Color, _ title: String, _ detail: String?) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol)
                .font(.title3)
                .foregroundStyle(tint)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline.weight(.semibold))
                if let detail {
                    Text(detail)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(14)
        .background(Color(.secondarySystemGroupedBackground), in: .rect(cornerRadius: 20, style: .continuous))
    }
}

/// A form-width sheet of a set size on iPad. On iPhone the detents decide.
nonisolated struct FixedSheetSizing: PresentationSizing {
    let width: CGFloat
    let height: CGFloat

    func proposedSize(for root: PresentationSizingRoot, context: PresentationSizingContext) -> ProposedViewSize {
        ProposedViewSize(width: width, height: height)
    }
}
