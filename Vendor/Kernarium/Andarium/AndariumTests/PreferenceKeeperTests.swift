// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// What the person sets is kept across launches, and a value the launch
/// decided for its own run leaves what the person stored alone. Each test
/// keeps into a defaults database of its own and awaits the write an observed
/// change queues, so nothing waits on a clock.
@MainActor
struct PreferenceKeeperTests {
    private func model() -> AppModel {
        AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
    }

    private func withDefaults(_ body: (UserDefaults) async throws -> Void) async rethrows {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        try await body(defaults)
    }

    /// Waits for the write the change just made has queued.
    private func written(by keeper: PreferenceKeeper) async throws {
        let write = try #require(keeper.pending, "the change queued no write")
        await write.value
    }

    @Test func aFreshModelKeepsTheStatisticsOffAndTheDisplayWithoutAChoice() {
        let model = model()
        #expect(model.showStatsToKeep == false)
        #expect(model.displayOptionsToKeep == DisplayOptions())
        model.session.showStats = true
        #expect(model.showStatsToKeep == true)
    }

    @Test func aValueTheLaunchDecidedIsNotKept() {
        let model = model()
        model.session.showStats = true
        model.statsDecidedByLaunch = true
        #expect(model.showStatsToKeep == nil)

        model.chooseRetina(true)
        #expect(model.displayOptionsToKeep == DisplayOptions(chosenRetina: true))
        model.retinaStatedByLaunch = true
        #expect(model.displayOptionsToKeep == nil)
    }

    @Test func theStatisticsToggleIsWrittenWhenItChanges() async throws {
        try await withDefaults { defaults in
            let model = model()
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.session.showStats = true
            try await written(by: keeper)
            #expect(StatsPreference(defaults: defaults).isOn)

            // The write re-armed the observation.
            model.session.showStats = false
            try await written(by: keeper)
            #expect(!StatsPreference(defaults: defaults).isOn)
        }
    }

    @Test func aChoiceOfRetinaIsWrittenWhenMade() async throws {
        try await withDefaults { defaults in
            let model = model()
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.chooseRetina(false)
            try await written(by: keeper)
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenRetina: false))
            model.chooseRetina(true)
            try await written(by: keeper)
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenRetina: true))
        }
    }

    @Test func aLaunchThatDecidedTheStatisticsWritesNone() async {
        await withDefaults { defaults in
            StatsPreference(defaults: defaults).keep(true)
            let model = model()
            model.statsDecidedByLaunch = true
            model.session.showStats = false
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.session.showStats = true
            model.session.showStats = false
            await keeper.pending?.value
            keeper.keep()
            #expect(StatsPreference(defaults: defaults).isOn)
        }
    }

    @Test func aLaunchThatStatedRetinaLeavesTheStoredChoice() async {
        await withDefaults { defaults in
            DisplayOptionsStore.save(DisplayOptions(chosenRetina: false), to: defaults)
            // The launch states Retina mode over a stored choice of off.
            let model = model()
            model.retina = true
            model.retinaChosen = true
            model.retinaStatedByLaunch = true
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            // What the window does afterwards, and any change a write follows.
            model.hostDrivesGuestRotation = true
            model.retina = true
            model.session.showStats = true
            await keeper.pending?.value
            keeper.keep()
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenRetina: false))
        }
    }
}
