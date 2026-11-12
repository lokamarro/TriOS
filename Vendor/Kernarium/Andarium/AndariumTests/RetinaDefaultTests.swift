// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

@MainActor
struct RetinaDefaultTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func model() -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        return AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
    }

    @Test func nothingIsKeptUntilThePersonChooses() {
        let model = model()
        #expect(!model.retinaChosen)
        #expect(model.displayOptions == DisplayOptions())
        // A launch that states Retina mode (a harness) is not a choice.
        model.retina = true
        #expect(model.displayOptions == DisplayOptions())
    }

    @Test func aChoiceOnHomeIsKeptWhetherOnOrOff() {
        let model = model()
        model.chooseRetina(false)
        #expect(!model.retina)
        #expect(model.displayOptions == DisplayOptions(chosenRetina: false))
        model.chooseRetina(true)
        #expect(model.retina)
        #expect(model.displayOptions == DisplayOptions(chosenRetina: true))
    }

    @Test func aKeptChoiceBeatsTheDefaultOnTheNextLaunch() {
        // The launch reads the kept options with the system's limit: a Mac
        // (no limit) turns it on by default, and an off choice still wins.
        let kept = model().displayOptionsAfterChoosing(false)
        #expect(!kept.retina(memoryLimitBytes: 0))
        #expect(!kept.retina(memoryLimitBytes: 8 << 30))
        #expect(DisplayOptions().retina(memoryLimitBytes: 8 << 30))
        #expect(!DisplayOptions().retina(memoryLimitBytes: 2867 << 20))
    }
}

private extension AppModel {
    func displayOptionsAfterChoosing(_ on: Bool) -> DisplayOptions {
        chooseRetina(on)
        return displayOptions
    }
}
