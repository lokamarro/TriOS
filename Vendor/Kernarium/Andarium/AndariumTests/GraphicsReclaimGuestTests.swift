// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The graphics memory reading, the activity manager's process list and the
/// end of a cached app on a real guest. Runs alone with
/// ANDARIUM_GRAPHICS_RECLAIM_TESTS=1 and
/// ANDARIUM_GUEST_INSTANCE=graphics-reclaim, on a Mac (Designed for iPad) or
/// an iPad, with an isolated guest. It reads the budget through the runtime,
/// parses what `dumpsys activity lru` prints, which the policy's exactness
/// rests on, and ends one cached app the guest started itself, named by a
/// holder the test makes up, through the shell the application uses.
@MainActor
struct GraphicsReclaimGuestTests {
    @Test(.timeLimit(.minutes(25)))
    func theBudgetAndTheProcessListAreReadFromARunningGuest() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_GRAPHICS_RECLAIM_TESTS"] == "1" else { return }
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "graphics-reclaim")
        try await withAndroidGuest { model, operations in
            let driver = try #require(model.driver)

            let reading = try #require(await driver.readGraphicsMemory(minimumIdle: .zero, minimumBytes: 0))
            print(
                "GRAPHICS_RECLAIM: budget \(reading.budgetBytes >> 20) MiB, usage \(reading.usageBytes >> 20) MiB, "
                    + "contexts \(reading.contextBytes >> 20) MiB, holders \(reading.holders.map { "\($0.pid):\($0.name):\($0.bytes >> 20)" })")
            #expect(reading.budgetBytes > 0, "the host gave the guest no graphics budget")
            #expect(reading.usageBytes > 0)
            #expect(reading.holders.count <= 8)
            #expect(reading.holders.map(\.bytes) == reading.holders.map(\.bytes).sorted(by: >), "not largest first")
            #expect(reading.holders.allSatisfy { $0.name.utf8.count <= 15 })

            // The filters hide what they exclude and nothing else.
            let large = try #require(
                await driver.readGraphicsMemory(minimumIdle: .zero, minimumBytes: reading.budgetBytes))
            #expect(large.holders.isEmpty, "a holder larger than the budget was listed")
            #expect(large.budgetBytes == reading.budgetBytes)
            let quiet = try #require(await driver.readGraphicsMemory(minimumIdle: .seconds(86400), minimumBytes: 0))
            #expect(quiet.holders.isEmpty, "a holder quiet for a day on a guest that has just booted")

            // The process list the policy reads parses on the real guest.
            // Android starts its apps for a while after it reports the boot done.
            var list = try await operations.shell(AndroidProcessList.command, limit: .seconds(60))
            var processes = AndroidProcessList.parse(list.text)
            let listDeadline = ContinuousClock.now + .seconds(180)
            while ContinuousClock.now < listDeadline,
                processes.count <= 10 || !processes.contains(where: { $0.standing == .cached && $0.uid.hasPrefix("u0a") })
            {
                try await Task.sleep(for: .seconds(5))
                list = try await operations.shell(AndroidProcessList.command, limit: .seconds(60))
                processes = AndroidProcessList.parse(list.text)
            }
            #expect(list.exitCode == 0)
            print("GRAPHICS_RECLAIM: \(processes.count) processes; standings "
                + "\(processes.filter { $0.standing == .cached }.count) cached, "
                + "\(processes.filter { $0.standing == .previous }.count) previous")
            #expect(processes.count > 10, "the process list did not parse: \(list.text.prefix(400))")
            #expect(processes.contains { $0.name == "system" }, "system_server is not in the parsed list")
            #expect(processes.contains { $0.standing == .kept })
            #expect(processes.allSatisfy { $0.pid > 0 && !$0.label.isEmpty && !$0.state.isEmpty })

            // A cached app ends through the application's own shell. The
            // holder is made up from the process list, which a kernel that
            // names the creating process gives the real one.
            let victim = try #require(
                processes.first { $0.standing == .cached && $0.uid.hasPrefix("u0a") && $0.user == 0 },
                "the guest has no cached app to end: \(list.text.prefix(600))")
            let holder = GraphicsHolder(
                pid: victim.pid, bytes: GraphicsReclaim.minimumHolderBytes, idle: .seconds(60),
                name: String(victim.name.suffix(15)))
            let made = GraphicsMemoryReading(
                budgetBytes: reading.budgetBytes, usageBytes: reading.budgetBytes,
                contextBytes: reading.budgetBytes, refusals: 0, holders: [holder])
            let result = await driver.reclaimGraphicsMemory(made, using: GraphicsReclaim())
            print("GRAPHICS_RECLAIM: ended \(victim.name) (\(victim.pid)): \(result)")
            #expect(result == .ended(package: victim.package, pid: victim.pid, bytes: holder.bytes))
            var gone = false
            for _ in 0..<10 where !gone {
                try await Task.sleep(for: .seconds(1))
                let after = try await operations.shell(AndroidProcessList.command, limit: .seconds(60))
                gone = !AndroidProcessList.parse(after.text).contains { $0.pid == victim.pid }
            }
            #expect(gone, "\(victim.name) (\(victim.pid)) was asked to end and is still there")

            // Without the kernel's process naming no holder has a pid, and
            // nothing is ended.
            if reading.holders.allSatisfy({ $0.pid == 0 }) {
                var policy = GraphicsReclaim()
                let now = ContinuousClock.now
                let first = policy.observe(reading, at: now)
                let second = policy.observe(reading, at: now.advanced(by: .seconds(2)))
                #expect(!first && !second)
            }
        }
    }
}
