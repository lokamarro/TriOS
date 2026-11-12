// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Testing

@testable import Andarium

#if !APP_STORE

/// The environment names a harness launch uses are the application's, and an
/// App Store build has none (`Andarium/README.md#automation`).
struct LaunchEnvironmentTests {
    @Test func everySettingHasItsOwnName() {
        let names = LaunchOverrides.Setting.allCases.map(LaunchOverrides.environmentName)
        #expect(Set(names).count == LaunchOverrides.Setting.allCases.count)
    }

    @Test func readsTheNamesOfKernariumAndAndarium() {
        let overrides = LaunchOverrides(
            environment: [
                "KERNARIUM_VCPU_COUNT": "2",
                "KERNARIUM_GUEST_MEMORY_MIB": "1536",
                "KERNARIUM_ANDROID_DISK_SIZE_GIB": "32",
                "KERNARIUM_RETINA_MODE": "1",
                "ANDARIUM_START": "recovery",
                "ANDARIUM_ERASE": "1",
                "ANDARIUM_STATS": "1",
                "ANDARIUM_GUEST_INSTANCE": "legacy-test",
            ],
            processorCount: 6
        )
        #expect(overrides.vCPUCount == 2)
        #expect(overrides.memoryMiB == 1536)
        #expect(overrides.capacityGiB == 32)
        #expect(overrides.retina == true)
        #expect(overrides.start == .recovery)
        #expect(overrides.erase)
        #expect(overrides.statistics == true)
        #expect(overrides.instance == "legacy-test")
    }

    @Test func isolatedGuestsHaveSeparateStorage() {
        let ordinary = AppEntry.guestDirectory()
        let isolated = AppEntry.guestDirectory(instance: "legacy-test")
        #expect(ordinary.lastPathComponent == "Guest")
        #expect(isolated.lastPathComponent == "legacy-test")
        #expect(isolated.deletingLastPathComponent().lastPathComponent == "TestGuests")
        #expect(isolated != ordinary)
    }

    @Test func kernariumsBootModeAsksForARecoveryStartOnly() {
        #expect(LaunchOverrides(environment: ["KERNARIUM_ANDROID_BOOT_MODE": "recovery"], processorCount: 4).start == .recovery)
        #expect(LaunchOverrides(environment: ["KERNARIUM_ANDROID_BOOT_MODE": "normal"], processorCount: 4).start == nil)
        #expect(LaunchOverrides(environment: [:], processorCount: 4).isEmpty)
    }
}

#endif
