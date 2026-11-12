// SPDX-License-Identifier: GPL-2.0-or-later

import Testing
@testable import AndariumCore

struct FormatsTests {
    private func bytes(mib: Double) -> UInt64 { UInt64(mib * Double(Limits.bytesPerMiB)) }
    private func bytes(gib: Double) -> UInt64 { UInt64(gib * Double(Limits.bytesPerGiB)) }

    @Test func sizesUseTheUnitThatReadsBest() {
        #expect(Formats.size(bytes: 0) == "0 B")
        #expect(Formats.size(bytes: 512) == "512 B")
        #expect(Formats.size(bytes: 1536) == "1.5 KiB")
        #expect(Formats.size(bytes: bytes(mib: 0.48)) == "492 KiB")
        #expect(Formats.size(bytes: bytes(mib: 480)) == "480 MiB")
        #expect(Formats.size(bytes: bytes(gib: 0.98)) == "1004 MiB")
        #expect(Formats.size(bytes: 2 * Limits.bytesPerGiB) == "2 GiB")
        #expect(Formats.size(bytes: bytes(gib: 1.5)) == "1.5 GiB")
        #expect(Formats.size(bytes: bytes(gib: 12.6)) == "12.6 GiB")
        #expect(Formats.size(bytes: bytes(gib: 118.3)) == "118 GiB")
        #expect(Formats.size(bytes: bytes(gib: 1.82 * 1024)) == "1.82 TiB")
    }

    /// A size keeps three significant digits, so a unit's last values round
    /// up into the next unit and never state "1024 MiB".
    @Test func aProcessorCountTakesTheSingularForOne() {
        #expect(Formats.vcpus(1) == "1 vCPU")
        #expect(Formats.vcpus(0) == "0 vCPUs")
        #expect(Formats.vcpus(2) == "2 vCPUs")
        #expect(Formats.vcpus(8) == "8 vCPUs")
    }

    @Test func aSizeRoundingToTheNextUnitMovesUp() {
        #expect(Formats.size(bytes: bytes(mib: 1023.4)) == "1023 MiB")
        #expect(Formats.size(bytes: bytes(mib: 1023.6)) == "1 GiB")
        #expect(Formats.size(bytes: bytes(mib: 9.996)) == "10 MiB")
        #expect(Formats.size(bytes: bytes(mib: 99.96)) == "100 MiB")
        #expect(Formats.size(bytes: bytes(gib: 1023.7)) == "1 TiB")
    }

    @Test func aSizeIsTheSameWhicheverUnitItIsGivenIn() {
        #expect(Formats.size(mib: 8 * 1024) == Formats.size(gib: 8))
        #expect(Formats.size(mib: 1536) == Formats.size(bytes: bytes(gib: 1.5)))
        #expect(Formats.size(gib: 8) == "8 GiB")
        #expect(Formats.size(gib: 64) == "64 GiB")
        #expect(Formats.size(mib: 0) == "0 MiB")
    }

    /// The host's free space, the guest's data and capacity and the system
    /// are all binary: a figure the host reports in bytes is the figure in
    /// gibibytes, not in the decimal gigabytes iOS Settings uses.
    @Test func hostAndGuestSizesShareOneUnit() {
        #expect(Formats.size(bytes: 1_500_000_000) == "1.4 GiB")
        let guest = Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8, usedBytes: Limits.bytesPerGiB)
        #expect(guest.effectiveCapacityBytes == 8 * Limits.bytesPerGiB)
        #expect(Formats.size(bytes: guest.effectiveCapacityBytes) == "8 GiB")
        #expect(Formats.size(bytes: guest.usedBytes) == "1 GiB")
        var growing = guest
        growing.setCapacity(24)
        #expect(growing.effectiveCapacityBytes == 24 * Limits.bytesPerGiB)
        #expect(Formats.size(bytes: growing.effectiveCapacityBytes) == "24 GiB")
    }

    @Test func theGuestsFreeSpaceIsWhatIsLeftOfItsCapacity() {
        let guest = Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8, usedBytes: bytes(gib: 5.4))
        #expect(Formats.size(bytes: guest.freeBytes) == "2.6 GiB")
        var full = guest
        full.usedBytes = 9 * Limits.bytesPerGiB
        #expect(full.freeBytes == 0)
        #expect(Formats.size(bytes: full.freeBytes) == "0 B")
    }

    /// Home states these two figures.
    @Test func theReservesAreStatedInGibibytes() {
        #expect(Formats.size(bytes: Limits.firstStartReserveBytes) == "3 GiB")
        #expect(Formats.size(bytes: Limits.startReserveBytes) == "1.5 GiB")
    }

    @Test func memoryShowsQuarterStepsWithoutRounding() {
        #expect(Formats.size(mib: 512) == "512 MiB")
        #expect(Formats.size(mib: 576) == "576 MiB")
        #expect(Formats.size(mib: 1024) == "1 GiB")
        #expect(Formats.size(mib: 1280) == "1.25 GiB")
        #expect(Formats.size(mib: 1536) == "1.5 GiB")
        #expect(Formats.size(mib: 1792) == "1.75 GiB")
        #expect(Formats.size(mib: 3840) == "3.75 GiB")
        #expect(Formats.size(mib: 4096) == "4 GiB")
    }

    @Test func aRulerLabelIsTheCountOfGibibytes() {
        #expect(Formats.gibibytes(mib: 512) == "0.5")
        #expect(Formats.gibibytes(mib: 1024) == "1")
        #expect(Formats.gibibytes(mib: 1280) == "1.25")
        #expect(Formats.gibibytes(mib: 4096) == "4")
    }

    @Test func elapsedTimePadsSeconds() {
        #expect(Formats.elapsed(0) == "0:00")
        #expect(Formats.elapsed(74) == "1:14")
        #expect(Formats.elapsed(3605) == "60:05")
    }
}

struct MemoryPolicyTests {
    /// A host whose memory limit lets the bound fall as vCPUs are added: 1.5 GiB
    /// at four.
    let limited = MemoryPolicy(maximumsMiB: [2048, 1920, 1728, 1536])
    /// A host with no limit to speak of (a Mac).
    let roomy = MemoryPolicy(maximumsMiB: Array(repeating: 4096, count: 8))

    private func host(_ memory: MemoryPolicy, processors: Int) -> HostDevice {
        HostDevice(name: "iPad", processorCount: processors, memory: memory, freeStorageBytes: 40_000_000_000)
    }

    /// A ten-core device offers the eight vCPUs the memory budget is measured
    /// for: a new guest takes eight, and the memory policy has eight rows and
    /// the ruler eight positions.
    @Test func aHostWithMoreCoresThanTheLimitOffersTheLimit() {
        var asked: [Int] = []
        let memory = MemoryPolicy(processorCount: 8) { count in
            asked.append(count)
            return 4096
        }
        let device = HostDevice(
            name: "iPad", processorCount: 8, memory: memory, freeStorageBytes: 40_000_000_000
        )
        #expect(asked == Array(1...8))
        #expect(device.processorCount == 8)
        #expect(Guest.fresh(on: device).cores == 8)
        // The overrides the application builds from the same limit reach
        // eight and no further.
        var overrides = LaunchOverrides(processorCount: device.processorCount) {
            $0 == .vCPUCount ? "8" : nil
        }
        #expect(overrides.applied(to: Guest.fresh(on: device), on: device).cores == 8)
        overrides.vCPUCount = 10
        #expect(overrides.applied(to: Guest.fresh(on: device), on: device).cores == 8)
    }

    /// A guest stored with more vCPUs than the host now offers (it was made
    /// when the limit was higher, or on a larger device) comes down to the
    /// limit on load, and its memory to the bound of that count.
    @Test func aStoredCountAboveTheLimitComesDownOnLoad() {
        let device = HostDevice(
            name: "Mac", processorCount: 8,
            memory: MemoryPolicy(maximumsMiB: [4096, 4096, 4096, 4096, 4096, 4096, 3840, 3584]),
            freeStorageBytes: 40_000_000_000
        )
        let stored = Guest(cores: 10, memoryMiB: 4096, capacityGiB: 8)
        let loaded = LaunchOverrides().applied(to: stored, on: device)
        #expect(loaded.cores == 8 && loaded.memoryMiB == 3584)
        // A count within the limit is kept, and so is a stored zero's floor of one.
        #expect(LaunchOverrides().applied(to: Guest(cores: 4, memoryMiB: 2048, capacityGiB: 8), on: device).cores == 4)
        #expect(LaunchOverrides().applied(to: Guest(cores: 0, memoryMiB: 2048, capacityGiB: 8), on: device).cores == 1)
    }

    @Test func aNewGuestTakesTheHostsVCPULimit() {
        let six = host(roomy, processors: 6)
        #expect(six.processorCount == 6)
        #expect(Guest.fresh(on: six).cores == 6)
        // A count below one is one.
        #expect(host(roomy, processors: 0).processorCount == 1)
    }

    @Test func thePickerRunsFrom1536ToTheBoundOfTheVCPUCountInSteps64() {
        #expect(Limits.minimumMemoryMiB == 1536)
        let two = limited.stops(vCPUs: 2)
        #expect(two.first == 1536 && two.last == 1920)
        #expect(two.count == (1920 - 1536) / 64 + 1)
        #expect(two.allSatisfy(MemoryPolicy.runtimeAccepts))
        #expect(limited.stops(vCPUs: 1).last == 2048)
        // The bound at four vCPUs is the minimum: one position.
        #expect(limited.stops(vCPUs: 4) == [1536])
        #expect(roomy.stops(vCPUs: 8).count == (4096 - 1536) / 64 + 1)
        #expect(roomy.stops(vCPUs: 8).first == 1536)
        // No position of any picker is below the minimum, whatever the count.
        for policy in [limited, roomy] {
            for count in 1...4 {
                #expect(policy.stops(vCPUs: count).allSatisfy { $0 >= Limits.minimumMemoryMiB })
            }
        }
        // Added vCPUs never raise the bound.
        let bounds = (1...4).map { limited.maximumMiB(vCPUs: $0) }
        #expect(bounds == bounds.sorted(by: >))
    }

    /// A device whose memory limit leaves less than the minimum offers the
    /// bound alone: the picker is not empty and offers no size the host cannot
    /// honour, as the default is never above the bound.
    @Test func aBoundBelowTheMinimumIsTheOnlyPosition() {
        let small = MemoryPolicy(maximumsMiB: [960, 896])
        #expect(small.minimumMiB(vCPUs: 1) == 960)
        #expect(small.stops(vCPUs: 1) == [960])
        #expect(small.stops(vCPUs: 2) == [896])
        #expect(small.clamped(512, vCPUs: 1) == 960)
        #expect(small.clamped(2048, vCPUs: 2) == 896)
        #expect(limited.minimumMiB(vCPUs: 1) == 1536)
        #expect(roomy.minimumMiB(vCPUs: 8) == 1536)
    }

    @Test func theBoundsComeOntoTheRuntimesStepsAndRange() {
        let policy = MemoryPolicy(maximumsMiB: [100, 5000, 1700])
        #expect(policy.maximumMiB(vCPUs: 1) == 512)
        #expect(policy.maximumMiB(vCPUs: 2) == 4096)
        #expect(policy.maximumMiB(vCPUs: 3) == 1664)
        // A count above the host's takes the last bound; zero counts as one.
        #expect(policy.maximumMiB(vCPUs: 9) == 1664)
        #expect(policy.maximumMiB(vCPUs: 0) == 512)
        #expect(MemoryPolicy(maximumsMiB: []).maximumMiB(vCPUs: 1) == 4096)
    }

    @Test func theLoaderBoundIsReadOncePerVCPUCount() {
        var asked: [Int] = []
        let policy = MemoryPolicy(processorCount: 3) { count in
            asked.append(count)
            return 4096 - count * 128
        }
        #expect(asked == [1, 2, 3])
        #expect(policy.maximumMiB(vCPUs: 2) == 3840)
    }

    /// The bounds and defaults the shared budget rule
    /// (`runtime/guest_memory_budget.hpp`, `maximum_guest_memory_mib` and
    /// `default_guest_memory_mib`) gives at 1 through 8 vCPUs for a process
    /// memory limit. The bound counts none of the launch allowances; the default
    /// counts the conservative set (64 MiB for the application, 320 for the rest
    /// of the engine, 149 of headroom), takes the result down to 0.5 GiB, holds
    /// it within 1.5 to 4 GiB and never above the bound (the safe size); where
    /// the bound is 4096 the default is also the most that leaves the host GPU
    /// a gibibyte. `AndariumTests` asks the loader for the same rows, so these
    /// stay the rule's.
    static let defaultFixtures: [(limitMiB: Int, bounds: [Int], defaults: [Int], safe: [Int])] = [
        // Below 1.5 GiB the bound wins: the default is never above what the picker offers.
        (1536, [960, 960, 896, 896, 896, 896, 896, 896], [960, 960, 896, 896, 896, 896, 896, 896],
            [960, 960, 896, 896, 896, 896, 896, 896]),
        (2048, [1472, 1472, 1408, 1408, 1344, 1344, 1344, 1344], [1472, 1472, 1408, 1408, 1344, 1344, 1344, 1344],
            [1472, 1472, 1408, 1408, 1344, 1344, 1344, 1344]),
        // The lower clamp: the conservative 1216 to 1024 MiB come up to 1536, under the bound.
        (2304, [1728, 1728, 1664, 1664, 1600, 1600, 1600, 1600], Array(repeating: 1536, count: 8),
            Array(repeating: 1536, count: 8)),
        // An iPad mini 6's 2867 MiB: 1.5 GiB at every count, 2 GiB still on the picker.
        (2867, [2304, 2304, 2240, 2240, 2176, 2176, 2176, 2176], Array(repeating: 1536, count: 8),
            Array(repeating: 1536, count: 8)),
        (3584, [3008, 3008, 2944, 2944, 2880, 2880, 2880, 2880], Array(repeating: 2048, count: 8),
            Array(repeating: 2048, count: 8)),
        (4096, [3520, 3520, 3456, 3456, 3392, 3392, 3392, 3392], Array(repeating: 2560, count: 8),
            Array(repeating: 2560, count: 8)),
        // A limit that offers 4096 at one to three vCPUs only: the default keeps the GPU a gibibyte at every count,
        // 2560 where the safe size is 3584 at one and two and 3072 after.
        (4700, [4096, 4096, 4096, 4032, 4032, 4032, 3968, 3968], Array(repeating: 2560, count: 8),
            [3584, 3584, 3072, 3072, 3072, 3072, 3072, 3072]),
        // A limit that offers 4096 everywhere: 3584 MiB would leave the GPU 699 to 871 MiB, so a new guest gets
        // 3072 and the safe size stays at 3584.
        (5120, Array(repeating: 4096, count: 8), Array(repeating: 3072, count: 8), Array(repeating: 3584, count: 8)),
        // The upper clamp: a limit that leaves the whole range, and a Mac with none.
        (6144, Array(repeating: 4096, count: 8), Array(repeating: 4096, count: 8), Array(repeating: 4096, count: 8)),
        (0, Array(repeating: 4096, count: 8), Array(repeating: 4096, count: 8), Array(repeating: 4096, count: 8)),
    ]

    @Test(arguments: [1, 4, 6, 8])
    func aNewGuestTakesEveryCoreAndTheRuntimesDefault(cores: Int) {
        for fixture in Self.defaultFixtures {
            let policy = MemoryPolicy(
                maximumsMiB: fixture.bounds, defaultsMiB: fixture.defaults, safeMiB: fixture.safe
            )
            let device = host(policy, processors: cores)
            let guest = Guest.fresh(on: device)
            #expect(guest.cores == cores)
            #expect(guest.memoryMiB == fixture.defaults[cores - 1], "limit \(fixture.limitMiB) MiB")
            #expect(guest.memoryMiB == policy.defaultMemoryMiB(vCPUs: cores))
            // Never above the bound, on the runtime's steps, and whole half
            // gibibytes wherever the bound did not take the default.
            #expect(guest.memoryMiB <= policy.maximumMiB(vCPUs: cores))
            #expect(MemoryPolicy.runtimeAccepts(guest.memoryMiB))
            if guest.memoryMiB < policy.maximumMiB(vCPUs: cores) {
                #expect(guest.memoryMiB % 512 == 0)
            }
            if policy.maximumMiB(vCPUs: cores) >= Limits.defaultMemoryRangeMiB.lowerBound {
                #expect(Limits.defaultMemoryRangeMiB.contains(guest.memoryMiB))
            }
            // A new guest is not one the application warns about, and the
            // safe size is never under the default.
            #expect(!policy.isAboveDefault(guest.memoryMiB, vCPUs: cores))
            #expect(!policy.isAboveSafe(guest.memoryMiB, vCPUs: cores))
            #expect(policy.safeMemoryMiB(vCPUs: cores) == fixture.safe[cores - 1])
            #expect(policy.safeMemoryMiB(vCPUs: cores) >= guest.memoryMiB)
        }
    }

    @Test func theDefaultIsTheRuntimesAndFollowsTheVCPUCount() {
        // 4096 MiB limit: the bound falls from 3520 at one vCPU to 3392 at six,
        // and the default from 2560 to 2048 where the runtime says so.
        let policy = MemoryPolicy(
            maximumsMiB: [3520, 3520, 3456, 3456, 3392, 3392, 3392, 3392],
            defaultsMiB: [2560, 2560, 2560, 2048, 2048, 2048, 2048, 2048]
        )
        #expect(policy.defaultMemoryMiB(vCPUs: 1) == 2560)
        #expect(policy.defaultMemoryMiB(vCPUs: 4) == 2048)
        #expect(policy.defaultMemoryMiB(vCPUs: 8) == 2048)
        // A count above the host's takes the last default; zero counts as one.
        #expect(policy.defaultMemoryMiB(vCPUs: 12) == 2048)
        #expect(policy.defaultMemoryMiB(vCPUs: 0) == 2560)
        // Like a bound, a default comes onto the runtime's steps and range.
        let loose = MemoryPolicy(maximumsMiB: [3520, 3520], defaultsMiB: [2570, 100])
        #expect(loose.defaultMemoryMiB(vCPUs: 1) == 2560)
        #expect(loose.defaultMemoryMiB(vCPUs: 2) == 512)
        // A count without a default takes its bound, and so does a policy that
        // states bounds alone.
        #expect(MemoryPolicy(maximumsMiB: [3520, 3456], defaultsMiB: [2560]).defaultMemoryMiB(vCPUs: 2) == 3456)
        #expect(MemoryPolicy(maximumsMiB: [3520, 3456]).defaultMemoryMiB(vCPUs: 1) == 3520)
        #expect(MemoryPolicy(maximumsMiB: []).defaultMemoryMiB(vCPUs: 1) == 4096)
    }

    @Test func theLoaderDefaultIsReadOncePerVCPUCount() {
        var askedBound: [Int] = []
        var askedDefault: [Int] = []
        let policy = MemoryPolicy(
            processorCount: 3,
            bound: { count in askedBound.append(count); return 4096 - count * 128 },
            defaultMemory: { count in askedDefault.append(count); return 2048 - count * 64 }
        )
        #expect(askedBound == [1, 2, 3] && askedDefault == [1, 2, 3])
        #expect(policy.maximumMiB(vCPUs: 2) == 3840)
        #expect(policy.defaultMemoryMiB(vCPUs: 2) == 1920)
    }

    /// The default threshold: a guest has more than a new one gets. Where the
    /// default is the safe size, as in these fixtures, it is also the line
    /// Home and the memory picker warn about, so a size and a count give one
    /// answer.
    @Test func aGuestAboveTheDefaultIsTheOneTheApplicationWarnsAbout() {
        // An iPad mini 6: 1.5 GiB at every count, 2 GiB still selectable.
        let mini = MemoryPolicy(
            maximumsMiB: [2304, 2304, 2240, 2240, 2176, 2176], defaultsMiB: Array(repeating: 1536, count: 6)
        )
        #expect(!mini.isAboveDefault(1536, vCPUs: 6))
        #expect(!mini.isAboveDefault(512, vCPUs: 6))
        #expect(mini.isAboveDefault(1600, vCPUs: 6))
        #expect(mini.isAboveDefault(2048, vCPUs: 6), "a guest made at 2 GiB is warned about")
        #expect(mini.isAboveDefault(2176, vCPUs: 6))
        // The threshold is the default itself, step by step.
        for count in 1...6 {
            let line = mini.defaultMemoryMiB(vCPUs: count)
            #expect(!mini.isAboveDefault(line, vCPUs: count))
            #expect(mini.isAboveDefault(line + Limits.memoryStepMiB, vCPUs: count))
        }
        // It follows the vCPU count live: the same memory is above the default
        // at one count and not at another when the default falls with the count.
        let falling = MemoryPolicy(
            maximumsMiB: [3520, 3520, 3456, 3456], defaultsMiB: [2560, 2560, 2048, 2048]
        )
        #expect(!falling.isAboveDefault(2304, vCPUs: 2))
        #expect(falling.isAboveDefault(2304, vCPUs: 3))
        #expect(!falling.isAboveDefault(2048, vCPUs: 3))
        // A count above the host's takes the last default; zero counts as one.
        #expect(falling.isAboveDefault(2304, vCPUs: 9))
        #expect(!falling.isAboveDefault(2304, vCPUs: 0))
        // A Mac and bounds alone: no size is above the default.
        #expect(!roomy.isAboveDefault(4096, vCPUs: 8))
        #expect(!limited.isAboveDefault(limited.maximumMiB(vCPUs: 1), vCPUs: 1))
        // Raising vCPUs takes memory down to the bound but not necessarily
        // below the default, so the warning stays while the guest is above it.
        let device = host(mini, processors: 6)
        var guest = Guest(cores: 1, memoryMiB: 2304, capacityGiB: 8)
        #expect(mini.isAboveDefault(guest.memoryMiB, vCPUs: guest.cores))
        guest.setCores(6, on: device)
        #expect(guest.memoryMiB == 2176)
        #expect(mini.isAboveDefault(guest.memoryMiB, vCPUs: guest.cores))
        guest.memoryMiB = 1536
        #expect(!mini.isAboveDefault(guest.memoryMiB, vCPUs: guest.cores))
    }

    /// The two thresholds. An iPhone 18 Pro Max-like device: 4 GiB offered at
    /// every count, a new guest at 3 GiB (the most that keeps a gibibyte for
    /// the GPU) and iOS likely to close Andarium above 3.5 GiB.
    private let phone = MemoryPolicy(
        maximumsMiB: Array(repeating: 4096, count: 8), defaultsMiB: Array(repeating: 3072, count: 8),
        safeMiB: Array(repeating: 3584, count: 8)
    )

    @Test func theGraphicsThresholdIsTheDefaultAndTheClosingOneIsTheSafeSize() {
        for count in 1...8 {
            #expect(phone.defaultMemoryMiB(vCPUs: count) == 3072)
            #expect(phone.safeMemoryMiB(vCPUs: count) == 3584)
            // At the default and below: nothing to warn about.
            #expect(!phone.isAboveDefault(3072, vCPUs: count) && !phone.isAboveSafe(3072, vCPUs: count))
            #expect(phone.risk(3072, vCPUs: count) == .none)
            #expect(phone.risk(1536, vCPUs: count) == .none)
            // One step above the default: the graphics warning only.
            #expect(phone.isAboveDefault(3136, vCPUs: count) && !phone.isAboveSafe(3136, vCPUs: count))
            #expect(phone.risk(3136, vCPUs: count) == .graphics)
            #expect(phone.risk(3584, vCPUs: count) == .graphics, "the safe size itself is not a crash risk")
            // Above the safe size: iOS is likely to close Andarium, which is the worse risk.
            #expect(phone.isAboveSafe(3648, vCPUs: count))
            #expect(phone.risk(3648, vCPUs: count) == .closing)
            #expect(phone.risk(4096, vCPUs: count) == .closing)
        }
    }

    @Test func theHomeNoticeKeepsMeaningAboveTheSafeSizeAlone() {
        // Between the thresholds Home says nothing (it asks isAboveSafe), only
        // the sheet warns (it asks risk, then isAboveDefault).
        #expect(!phone.isAboveSafe(3584, vCPUs: 8))
        #expect(phone.isAboveDefault(3584, vCPUs: 8))
        // Where the default is the safe size, as on an iPad mini 6 or with no
        // second bound, the two coincide and the graphics warning never shows
        // on its own.
        let mini = MemoryPolicy(
            maximumsMiB: [2304, 2304, 2240, 2240, 2176, 2176], defaultsMiB: Array(repeating: 1536, count: 6)
        )
        for count in 1...6 {
            #expect(mini.defaultMemoryMiB(vCPUs: count) == mini.safeMemoryMiB(vCPUs: count))
            for mib in stride(from: 512, through: 2304, by: Limits.memoryStepMiB) {
                #expect(mini.isAboveDefault(mib, vCPUs: count) == mini.isAboveSafe(mib, vCPUs: count))
                #expect(mini.risk(mib, vCPUs: count) != .graphics)
            }
        }
    }

    @Test func theSafeSizeIsHeldBetweenTheDefaultAndTheBound() {
        // Stepped down like the others, never under the default or over the bound.
        let policy = MemoryPolicy(
            maximumsMiB: [3520, 3520, 3456], defaultsMiB: [2560, 2560, 2560], safeMiB: [2570, 2000, 9000]
        )
        #expect(policy.safeMemoryMiB(vCPUs: 1) == 2560)
        #expect(policy.safeMemoryMiB(vCPUs: 2) == 2560, "not under the default")
        #expect(policy.safeMemoryMiB(vCPUs: 3) == 3456, "not over the bound")
        // A count without a safe size takes its default; so does a policy without any.
        #expect(MemoryPolicy(maximumsMiB: [3520, 3456], defaultsMiB: [2560, 2048]).safeMemoryMiB(vCPUs: 2) == 2048)
        #expect(MemoryPolicy(maximumsMiB: [3520, 3456]).safeMemoryMiB(vCPUs: 1) == 3520)
        // A count above the host's takes the last one; zero counts as one.
        #expect(phone.safeMemoryMiB(vCPUs: 12) == 3584)
        #expect(phone.safeMemoryMiB(vCPUs: 0) == 3584)
    }

    @Test func moreVCPUsDoNotRaiseTheThresholds() {
        let falling = MemoryPolicy(
            processorCount: 4,
            bound: { 4096 - ($0 - 1) * 64 },
            defaultMemory: { 3072 - ($0 - 1) * 64 },
            safeMemory: { 3584 - ($0 - 1) * 64 }
        )
        for count in 1..<4 {
            #expect(falling.defaultMemoryMiB(vCPUs: count + 1) <= falling.defaultMemoryMiB(vCPUs: count))
            #expect(falling.safeMemoryMiB(vCPUs: count + 1) <= falling.safeMemoryMiB(vCPUs: count))
        }
        // The thresholds follow the count live: 3.5 GiB is a crash risk at four vCPUs, not at one.
        #expect(falling.risk(3456, vCPUs: 1) == .graphics)
        #expect(falling.risk(3456, vCPUs: 4) == .closing)
    }

    @Test func theLoaderIsAskedOncePerCountForTheSafeSizeAndOncePerSizeForGraphics() {
        var safeAsked: [Int] = []
        var graphicsAsked = 0
        let policy = MemoryPolicy(
            processorCount: 3,
            bound: { _ in 4096 }, defaultMemory: { _ in 2048 },
            safeMemory: { safeAsked.append($0); return 3072 },
            graphicsMemory: { memory, count in graphicsAsked += 1; return memory / 2 - count }
        )
        #expect(safeAsked == [1, 2, 3])
        #expect(graphicsAsked == 3 * 57, "57 sizes of the runtime's range at 3 counts")
        #expect(policy.safeMemoryMiB(vCPUs: 2) == 3072)
        // The estimate is the table's, for the size and count asked.
        #expect(policy.graphicsMemoryMiB(forMemory: 2048, vCPUs: 1) == 1023)
        #expect(policy.graphicsMemoryMiB(forMemory: 4096, vCPUs: 3) == 2045)
        #expect(policy.graphicsMemoryMiB(forMemory: 512, vCPUs: 2) == 254)
        // A size between steps takes the step below; one outside the range, its edge.
        #expect(policy.graphicsMemoryMiB(forMemory: 2080, vCPUs: 1) == 1023)
        #expect(policy.graphicsMemoryMiB(forMemory: 100, vCPUs: 1) == 255)
        #expect(policy.graphicsMemoryMiB(forMemory: 9999, vCPUs: 1) == 2047)
        // A count above the host's takes the last row; zero counts as one.
        #expect(policy.graphicsMemoryMiB(forMemory: 2048, vCPUs: 9) == 1021)
        #expect(policy.graphicsMemoryMiB(forMemory: 2048, vCPUs: 0) == 1023)
        // A policy that is not given the host's answer has none.
        #expect(phone.graphicsMemoryMiB(forMemory: 2048, vCPUs: 1) == nil)
        #expect(MemoryPolicy(maximumsMiB: [4096]).graphicsMemoryMiB(forMemory: 2048, vCPUs: 1) == nil)
    }

    @Test func theGraphicsWordsSayItIsSeparateAndWhereGamesRunShort() {
        #expect(GraphicsMemoryWarning.separateNote.contains("separate") && GraphicsMemoryWarning.separateNote.contains("RAM"))
        #expect(GraphicsMemoryWarning.note(defaultMiB: 3072) == "Above 3 GiB, games may run out of graphics memory.")
    }

    @Test func theMemoryWarningNamesTheSizesAndWhatHappens() {
        let home = MemoryWarning.explanation(memoryMiB: 2048, safeMiB: 1536, deviceName: "iPad")
        #expect(home.contains("2 GiB") && home.contains("1.5 GiB") && home.contains("this iPad"))
        #expect(home.contains("Andarium") && home.contains("Edit"))
        let note = MemoryWarning.note(safeMiB: 1536)
        #expect(note == "iOS is likely to close Andarium above 1.5 GiB.")
    }

    @Test func aStoredSizeIsTakenWithinTheBoundOnLoad() {
        let device = host(limited, processors: 4)
        // Above the bound of its vCPU count: down to the bound.
        #expect(Guest(cores: 4, memoryMiB: 3072, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 1536)
        // The same size is allowed with fewer vCPUs.
        #expect(Guest(cores: 1, memoryMiB: 2048, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 2048)
        // Off the steps: the nearest step, then the bound.
        #expect(Guest(cores: 2, memoryMiB: 1700, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 1728)
        // Below the minimum: raised to it.
        #expect(Guest(cores: 2, memoryMiB: 100, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 1536)
        #expect(Guest(cores: 2, memoryMiB: 1024, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 1536)
        #expect(Guest(cores: 1, memoryMiB: 1472, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 1536)
        #expect(Guest(cores: 1, memoryMiB: 1536, capacityGiB: 8).memoryWithinBound(on: device).memoryMiB == 1536)
        // A size is raised only to what the host can honour: a bound below the
        // minimum wins.
        let small = host(MemoryPolicy(maximumsMiB: [960]), processors: 1)
        #expect(Guest(cores: 1, memoryMiB: 512, capacityGiB: 8).memoryWithinBound(on: small).memoryMiB == 960)
    }

    /// A guest stored by an earlier version below the minimum is raised when
    /// the application loads it, on the path the application takes.
    @Test func aStoredGuestBelowTheMinimumIsRaisedWhenLoaded() {
        let device = host(roomy, processors: 4)
        let stored = Guest(cores: 4, memoryMiB: 1024, capacityGiB: 8)
        let loaded = LaunchOverrides().applied(to: stored, on: device)
        #expect(loaded.memoryMiB == 1536)
        #expect(loaded.cores == 4 && loaded.capacityGiB == 8)
    }

    @Test func raisingVCPUsBringsMemoryDownInStepsAndLoweringLeavesIt() {
        let device = host(limited, processors: 4)
        var guest = Guest(cores: 1, memoryMiB: 2048, capacityGiB: 8)
        guest.setCores(2, on: device)
        #expect(guest.cores == 2 && guest.memoryMiB == 1920)
        guest.setCores(3, on: device)
        #expect(guest.memoryMiB == 1728)
        guest.setCores(4, on: device)
        #expect(guest.memoryMiB == 1536)
        #expect(guest.memoryMiB % Limits.memoryStepMiB == 0)
        // Lowering does not give memory back.
        guest.setCores(1, on: device)
        #expect(guest.cores == 1 && guest.memoryMiB == 1536)
        // Raising with memory already below the new bound leaves it too.
        var small = Guest(cores: 1, memoryMiB: 1536, capacityGiB: 8)
        small.setCores(2, on: host(roomy, processors: 4))
        #expect(small.memoryMiB == 1536)
    }

    @Test func anAutomationLaunchStatesItsSizeUnclamped() {
        let device = host(limited, processors: 4)
        var overrides = LaunchOverrides()
        overrides.memoryMiB = 4096
        overrides.vCPUCount = 4
        let stated = overrides.applied(to: Guest.fresh(on: device), on: device)
        #expect(stated.cores == 4 && stated.memoryMiB == 4096)
        // A launch that states only vCPUs takes the stored size within the
        // bound of the count it states.
        var onlyCPUs = LaunchOverrides()
        onlyCPUs.vCPUCount = 4
        let stored = Guest(cores: 1, memoryMiB: 2048, capacityGiB: 8)
        #expect(onlyCPUs.applied(to: stored, on: device).memoryMiB == 1536)
        // And a launch that states nothing clamps what is stored.
        let big = Guest(cores: 4, memoryMiB: 3072, capacityGiB: 8)
        #expect(LaunchOverrides().applied(to: big, on: device).memoryMiB == 1536)
    }

    @Test func theRuntimeAcceptsEvery64MiBInItsRange() {
        #expect(MemoryPolicy.runtimeAccepts(1536))
        #expect(MemoryPolicy.runtimeAccepts(512))
        #expect(MemoryPolicy.runtimeAccepts(4096))
        #expect(!MemoryPolicy.runtimeAccepts(1500))
        #expect(!MemoryPolicy.runtimeAccepts(448))
        #expect(!MemoryPolicy.runtimeAccepts(4160))
    }
}
