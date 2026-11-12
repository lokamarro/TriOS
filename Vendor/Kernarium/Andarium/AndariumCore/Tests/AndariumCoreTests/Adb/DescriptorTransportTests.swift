// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing

@testable import AndariumCore

@Suite(.timeLimit(.minutes(2)))
struct DescriptorTransportTests {
    /// The runtime ends a stream the guest closed with `shutdown(SHUT_WR)` on
    /// the application's end of a socket pair, right after the last bytes.
    /// A reader that goes back to sleep at that moment must still hear it.
    @Test func aReaderHearsTheEndTheOtherSideAnnouncesRightAfterItsLastBytes() {
        for round in 0..<4000 {
            var pair: [Int32] = [0, 0]
            #expect(socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0)
            let reader = DescriptorTransport(descriptor: pair[0])
            let outcome = DispatchSemaphore(value: 0)
            let result = DataBox()
            let thread = Thread {
                do {
                    let head = try reader.readExactly(3)
                    _ = try reader.readExactly(1)
                    result.set(head)
                } catch AdbError.guestGone {
                    result.set(Data("gone".utf8))
                } catch {
                    result.set(Data("other".utf8))
                }
                outcome.signal()
            }
            thread.start()
            _ = Darwin.write(pair[1], "bye", 3)
            if round % 3 == 0 { usleep(UInt32(round % 7)) }
            _ = Darwin.shutdown(pair[1], SHUT_WR)
            let heard = outcome.wait(timeout: .now() + 5) == .success
            #expect(heard, "round \(round): the reader never heard the end")
            if !heard {
                reader.shutdown()
                _ = outcome.wait(timeout: .now() + 5)
                Darwin.close(pair[1])
                return
            }
            #expect(result.value == Data("gone".utf8))
            Darwin.close(pair[1])
        }
    }

    @Test func shuttingDownFromAnotherThreadWakesAReaderAtOnce() {
        var pair: [Int32] = [0, 0]
        #expect(socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0)
        let reader = DescriptorTransport(descriptor: pair[0])
        let outcome = DispatchSemaphore(value: 0)
        let result = DataBox()
        let thread = Thread {
            do {
                _ = try reader.readExactly(1)
                result.set(Data("data".utf8))
            } catch AdbError.guestGone {
                result.set(Data("gone".utf8))
            } catch {
                result.set(Data("other".utf8))
            }
            outcome.signal()
        }
        thread.start()
        Thread.sleep(forTimeInterval: 0.2)
        reader.shutdown()
        #expect(outcome.wait(timeout: .now() + 5) == .success)
        #expect(result.value == Data("gone".utf8))
        Darwin.close(pair[1])
    }

    /// Darwin's `poll` never wakes for a descriptor that is closed under it,
    /// so a shutdown whose wake the system does not deliver must still end
    /// the wait. A pipe stands for that descriptor: `shutdown` cannot reach
    /// it, and the transport is left to see its own shutdown.
    @Test func aShutdownTheSystemDoesNotDeliverStillEndsAWait() {
        var ends: [Int32] = [-1, -1]
        #expect(pipe(&ends) == 0)
        let reader = DescriptorTransport(descriptor: ends[0])
        let outcome = DispatchSemaphore(value: 0)
        let result = DataBox()
        let thread = Thread {
            do {
                _ = try reader.readExactly(1)
                result.set(Data("data".utf8))
            } catch AdbError.guestGone {
                result.set(Data("gone".utf8))
            } catch {
                result.set(Data("other".utf8))
            }
            outcome.signal()
        }
        thread.start()
        Thread.sleep(forTimeInterval: 0.2)
        reader.shutdown()
        let ended = outcome.wait(timeout: .now() + 5) == .success
        #expect(ended)
        #expect(result.value == Data("gone".utf8))
        if !ended {
            // Let the reader go before its descriptor is released.
            _ = Darwin.write(ends[1], "x", 1)
            _ = outcome.wait(timeout: .now() + 5)
        }
        Darwin.close(ends[1])
    }
}
