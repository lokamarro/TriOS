// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// Covers the screens while a long task runs, so nothing else is asked of the
/// guest's files meanwhile: a glass card with what is happening and how far.
/// When the task is over and has a result, the card stays and says how it
/// ended until the person closes it.
struct BusyOverlay: View {
    let state: BusyState

    var body: some View {
        ZStack {
            Color.black.opacity(0.32).ignoresSafeArea()
            VStack(alignment: .leading, spacing: 14) {
                if let result = state.result {
                    ended(result)
                } else {
                    running
                }
            }
            .padding(20)
            .frame(maxWidth: 360)
            .glassEffect(.regular, in: .rect(cornerRadius: 26))
            .padding(24)
        }
        .transition(.opacity)
        .animation(.easeInOut(duration: 0.2), value: state.cancel != nil)
        .accessibilityElement(children: .contain)
    }

    @ViewBuilder
    private var running: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(state.title)
                .font(.title3.weight(.bold))
            Spacer()
            if let progress = state.progress {
                Text("\(Int((progress * 100).rounded()))%")
                    .font(.system(.body, design: .rounded).weight(.semibold))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
            }
        }
        ProgressBar(value: state.progress ?? 0)
        Text(state.detail)
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        if let cancel = state.cancel {
            Button("Cancel", role: .cancel, action: cancel.action)
                .buttonStyle(.bordered)
                .frame(maxWidth: .infinity, alignment: .trailing)
                .transition(.opacity)
        }
    }

    @ViewBuilder
    private func ended(_ result: BusyResult) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Image(systemName: result.failed ? "exclamationmark.triangle.fill" : "checkmark.circle.fill")
                .font(.title3)
                .foregroundStyle(result.failed ? Color.red : Color.accentColor)
                .accessibilityHidden(true)
            Text(state.title)
                .font(.title3.weight(.bold))
        }
        Text(state.detail)
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        Button(result.failed ? "OK" : "Done", action: result.dismiss.action)
            .buttonStyle(.borderedProminent)
            .frame(maxWidth: .infinity, alignment: .trailing)
            .accessibilityIdentifier("busy-result-dismiss")
    }
}
