// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import SwiftUI
import UIKit

struct EmulatorSettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var microphone = MicrophoneSetting.shared

    @State private var configuration: EmulatorConfiguration
    @State private var backgroundMode: BackgroundMode
    @State private var backgroundLimitMinutes: Int

    private let maximumVCPUCount: Int
    private let defaults: UserDefaults
    private let rootfsFilenames: [String]
    private let documentsDirectory: URL

    /// The most guest RAM the memory limit leaves at the chosen vCPU count.
    private var maximumGuestMemoryMiB: Int {
        configuration.maximumGuestMemoryMiB(
            vCPUCount: configuration.vCPUCount
        )
    }

    private var androidBundleSelected: Bool {
        RootfsImage.selection(
            named: configuration.rootfsFilename,
            in: documentsDirectory
        )?.flavor == .android
    }

    init(
        defaults: UserDefaults = .standard,
        physicalProcessorCount: Int = EmulatorConfiguration.physicalProcessorCount,
        documentsDirectory: URL = RootfsImage.documentsDirectory
    ) {
        let loaded = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: physicalProcessorCount,
            documentsDirectory: documentsDirectory
        )
        self.maximumVCPUCount = EmulatorConfiguration.maximumVCPUCount(
            physicalProcessorCount: physicalProcessorCount
        )
        self.defaults = defaults
        self.documentsDirectory = documentsDirectory
        let availableRootfsFilenames = RootfsImage.availableFilenames(
            in: documentsDirectory
        )
        self.rootfsFilenames = availableRootfsFilenames.contains(
            loaded.rootfsFilename
        ) ? availableRootfsFilenames : [loaded.rootfsFilename] +
            availableRootfsFilenames
        _configuration = State(initialValue: loaded)
        let background = BackgroundSettings(defaults: defaults)
        _backgroundMode = State(
            initialValue: background.mode(forGuest: loaded.rootfsFilename)
        )
        _backgroundLimitMinutes = State(
            initialValue: background.limitMinutes(
                forGuest: loaded.rootfsFilename
            )
        )
    }

    /// The window is offered only where iOS can show it; a guest that already
    /// chose it keeps showing as chosen.
    private var offeredBackgroundModes: [BackgroundMode] {
        BackgroundMode.allCases.filter {
            $0 != .pictureInPicture || $0 == backgroundMode ||
                GuestPictureInPicture.isSupported
        }
    }

    /// The times on offer, with the one a guest already has even if it is not
    /// among them.
    private var offeredLimitMinutes: [Int] {
        Set(BackgroundSettings.limitChoicesMinutes + [backgroundLimitMinutes])
            .sorted()
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Hardware") {
                    Toggle("Retina mode", isOn: $configuration.retinaMode)
                        .accessibilityIdentifier("retinaModeToggle")

                    Toggle(
                        "Automatically resize guest display",
                        isOn: $configuration.autoResizeGuest
                    )
                    .accessibilityIdentifier("autoResizeGuestToggle")

                    Text(
                        "Controls whether settled view-size changes resize " +
                            "the guest. When off, the guest keeps its " +
                            "resolution and is fitted to the view. A " +
                            "guest-selected resolution pauses automatic " +
                            "resizing until the guest restarts."
                    )
                    .font(.footnote)
                    .foregroundStyle(.secondary)

                    Stepper(
                        "vCPUs: \(configuration.vCPUCount)",
                        value: $configuration.vCPUCount,
                        in: 1...maximumVCPUCount
                    )
                    .accessibilityIdentifier("vCPUCountStepper")
                    .onChange(of: configuration.vCPUCount) { oldCount, _ in
                        configuration.vCPUCountChanged(from: oldCount)
                    }

                    Stepper(
                        "Guest RAM: \(configuration.guestMemoryMiB) MiB",
                        value: $configuration.guestMemoryMiB,
                        in: EmulatorConfiguration.guestMemoryRangeMiB
                            .lowerBound...maximumGuestMemoryMiB,
                        step: EmulatorConfiguration.guestMemoryStepMiB
                    )
                    .accessibilityIdentifier("guestMemoryStepper")

                    if maximumGuestMemoryMiB <
                        EmulatorConfiguration.guestMemoryRangeMiB.upperBound {
                        Text(
                            "With \(configuration.vCPUCount) \(configuration.vCPUCount == 1 ? "vCPU" : "vCPUs") the " +
                                "system leaves room for at most " +
                                "\(maximumGuestMemoryMiB) MiB of guest RAM " +
                                "beside graphics and the emulator. More " +
                                "vCPUs lower it."
                        )
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .accessibilityIdentifier("guestMemoryLimitNote")
                    }

                    Stepper(
                        "Overlay FS: \(configuration.overlaySizeGiB) GiB",
                        value: $configuration.overlaySizeGiB,
                        in: EmulatorConfiguration.overlaySizeRangeGiB
                    )
                    .accessibilityIdentifier("overlaySizeStepper")
                }

                // Unlike the rest of the form this applies at once, whatever
                // Save or Cancel does: turning it on is when iOS asks for
                // access (see MicrophoneSetting).
                Section("Microphone") {
                    Toggle(
                        "Microphone",
                        isOn: Binding(
                            get: { microphone.isOn },
                            set: { microphone.setOn($0) }
                        )
                    )
                    .accessibilityIdentifier("microphoneToggle")

                    Text(
                        MicrophoneSetting.explanation(
                            isOn: microphone.isOn,
                            access: microphone.access
                        )
                    )
                    .font(.footnote)
                    .foregroundStyle(.secondary)

                    if microphone.isOn, microphone.access == .denied {
                        Button("Open Settings") {
                            if let url = URL(
                                string: UIApplication.openSettingsURLString
                            ) {
                                UIApplication.shared.open(url)
                            }
                        }
                        .accessibilityIdentifier("microphoneOpenSettingsButton")
                    }
                }

                Section("Storage") {
                    Picker(
                        "Root filesystem",
                        selection: $configuration.rootfsFilename
                    ) {
                        ForEach(rootfsFilenames, id: \.self) { filename in
                            Text(filename).tag(filename)
                        }
                    }
                    .accessibilityIdentifier("rootfsPicker")

                    if androidBundleSelected {
                        Stepper(
                            "Android disk: \(configuration.androidDiskSizeGiB) GiB",
                            value: $configuration.androidDiskSizeGiB,
                            in: EmulatorConfiguration.androidDiskSizeRangeGiB
                        )
                        .accessibilityIdentifier("androidDiskSizeStepper")

                        Picker(
                            "Android boot",
                            selection: $configuration.androidBootMode
                        ) {
                            ForEach(AndroidBootMode.allCases) { mode in
                                Text(mode.title).tag(mode)
                            }
                        }
                        .accessibilityIdentifier("androidBootModePicker")
                    }

                    Text(
                        "EROFS images and .\(GuestBundle.pathExtension) " +
                        "guest bundles are loaded from Documents."
                    )
                }

                Section("Background") {
                    Picker(
                        "When Kernarium is in the background",
                        selection: $backgroundMode
                    ) {
                        ForEach(offeredBackgroundModes) { mode in
                            Text(mode.title).tag(mode)
                        }
                    }
                    .accessibilityIdentifier("backgroundModePicker")

                    if backgroundMode == .keepRunning {
                        Picker(
                            "Keep running for",
                            selection: $backgroundLimitMinutes
                        ) {
                            ForEach(offeredLimitMinutes, id: \.self) { minutes in
                                Text(BackgroundSettings.limitTitle(
                                    minutes: minutes
                                ))
                                .tag(minutes)
                            }
                        }
                        .accessibilityIdentifier("backgroundLimitPicker")
                    }

                    Text(backgroundMode.explanation)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                .onChange(of: configuration.rootfsFilename) { _, filename in
                    let background = BackgroundSettings(defaults: defaults)
                    backgroundMode = background.mode(forGuest: filename)
                    backgroundLimitMinutes = background.limitMinutes(
                        forGuest: filename
                    )
                }

                Section {
                    Text("Changes take effect the next time the app launches.")
                }
            }
            .navigationTitle("Emulator settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        dismiss()
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        save()
                    }
                    .accessibilityIdentifier("settingsSaveButton")
                }
            }
        }
    }

    private func save() {
        configuration.save(defaults: defaults)
        let background = BackgroundSettings(defaults: defaults)
        background.setMode(
            backgroundMode,
            forGuest: configuration.rootfsFilename
        )
        background.setLimitMinutes(
            backgroundLimitMinutes,
            forGuest: configuration.rootfsFilename
        )
        dismiss()
    }
}
