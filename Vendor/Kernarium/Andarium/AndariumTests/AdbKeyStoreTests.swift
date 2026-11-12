// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
@preconcurrency import Security
import Testing
@testable import Andarium

/// The guest's adb host key as the Keychain keeps it.
struct AdbKeyStoreTests {
    private func store() -> AdbKeyStore {
        AdbKeyStore(tag: "com.imxieyi.andarium.adb-key.test-\(UUID().uuidString)")
    }

    @Test func aKeyIsMadeOnceFoundAgainAndNewAfterTheDelete() throws {
        let store = store()
        defer { store.delete() }
        #expect(try store.find() == nil)
        let first = try store.loadOrCreate()
        // 524 bytes of adb's structure, in base64.
        #expect(first.publicKeyLine.count == 700)
        #expect(Data(base64Encoded: first.publicKeyLine)?.count == 524)
        #expect(try store.loadOrCreate().publicKeyLine == first.publicKeyLine)
        #expect(store.delete())
        #expect(try store.find() == nil)
        #expect(try store.loadOrCreate().publicKeyLine != first.publicKeyLine)
        // Deleting what is not there is no failure.
        #expect(store.delete() && store.delete())
    }

    /// The start that prepares and anything else that asks for the key at the
    /// same time must end up with the one key, or the guest would be given a
    /// key the client does not sign with.
    @Test func callersAtTheSameTimeGetTheSameKey() throws {
        let store = store()
        defer { store.delete() }
        let lines = LineBox()
        let group = DispatchGroup()
        for _ in 0..<8 {
            group.enter()
            Thread.detachNewThread {
                lines.add((try? store.loadOrCreate().publicKeyLine) ?? "failed")
                group.leave()
            }
        }
        group.wait()
        #expect(lines.all.count == 8)
        #expect(Set(lines.all).count == 1)
        #expect(!lines.all.contains("failed"))
    }

    /// A test that waits for a guest the application started must not make a
    /// key of its own: an automation launch that erases first deletes the key
    /// the test would have made, and the guest is given another.
    @Test func theTestsReadTheKeyAgainAndNeverMakeIt() throws {
        let store = store()
        defer { store.delete() }
        #expect(try store.existingIdentity() == nil)
        #expect(try store.find() == nil, "reading the key must not make one")
        let first = try store.loadOrCreate()
        #expect(try store.existingIdentity()?.publicKeyLine == first.publicKeyLine)
        // Erase guest deletes the key and the next start makes another.
        #expect(store.delete())
        #expect(try store.existingIdentity() == nil)
        let second = try store.loadOrCreate()
        #expect(try store.existingIdentity()?.publicKeyLine == second.publicKeyLine)
        #expect(second.publicKeyLine != first.publicKeyLine)
    }

    #if !APP_STORE
    /// Erasing an isolated instance must never delete the ordinary guest's
    /// key. The tests use a base tag of their own, never the application's.
    @Test func anIsolatedInstanceHasItsOwnKeyAndItsEraseLeavesTheOthers() throws {
        let base = "com.imxieyi.andarium.adb-key.test-\(UUID().uuidString)"
        let ordinary = AdbKeyStore(instance: nil, base: base)
        let first = AdbKeyStore(instance: "retina-memory", base: base)
        let second = AdbKeyStore(instance: "shutdown_flow-2", base: base)
        defer { [ordinary, first, second].forEach { $0.delete() } }
        #expect(AdbKeyStore(instance: nil).tag == AdbKeyStore.defaultTag)
        #expect(Set([ordinary.tag, first.tag, second.tag]).count == 3)
        let lines = try [ordinary, first, second].map { try $0.loadOrCreate().publicKeyLine }
        #expect(Set(lines).count == 3)
        #expect(first.delete())
        #expect(try first.find() == nil)
        #expect(try ordinary.loadOrCreate().publicKeyLine == lines[0])
        #expect(try second.loadOrCreate().publicKeyLine == lines[2])
        // An instance finds its own key again, and a new one after its erase.
        #expect(try first.loadOrCreate().publicKeyLine != lines[1])
    }
    #endif

    @Test func theKeyIsRSA2048ThisDeviceOnlyAndNotSynchronized() throws {
        let store = store()
        defer { store.delete() }
        _ = try store.loadOrCreate()
        let query: [String: Any] = [
            kSecClass as String: kSecClassKey,
            kSecAttrApplicationTag as String: Data(store.tag.utf8),
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
            kSecAttrKeyClass as String: kSecAttrKeyClassPrivate,
            kSecAttrSynchronizable as String: false,
            kSecUseDataProtectionKeychain as String: true,
            kSecReturnAttributes as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        #expect(SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess)
        let attributes = try #require(item as? [String: Any])
        #expect(attributes[kSecAttrKeySizeInBits as String] as? Int == 2048)
        #expect(attributes[kSecAttrLabel as String] as? String == AdbKeyStore.label)
        #expect(attributes[kSecAttrAccessible as String] as? String
            == kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly as String)
        // 0 or false: not in iCloud Keychain.
        #expect((attributes[kSecAttrSynchronizable as String] as? Bool ?? false) == false)
    }

    @Test func theKeySignsATokenTheWayAdbdVerifiesIt() throws {
        let store = store()
        defer { store.delete() }
        let identity = try store.loadOrCreate()
        let token = Data((0..<20).map { UInt8($0 * 11 + 5) })
        let signature = try identity.signer.sign(token: token)
        let publicKey = try #require(SecKeyCopyPublicKey(identity.privateKey))
        #expect(SecKeyVerifySignature(
            publicKey, .rsaSignatureDigestPKCS1v15SHA1, token as CFData, signature as CFData, nil))
    }

    @Test func theLineGoesInTheBootRequestAsTheRuntimeAcceptsIt() throws {
        let store = store()
        defer { store.delete() }
        let line = try store.loadOrCreate().publicKeyLine
        // 1 through 1024 characters of A-Z a-z 0-9 + / =.
        #expect((1...1024).contains(line.count))
        #expect(line.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "+/=".contains($0)) })
    }
}

private nonisolated final class LineBox: @unchecked Sendable {
    private let lock = NSLock()
    private var lines: [String] = []
    var all: [String] { lock.withLock { lines } }
    func add(_ line: String) { lock.withLock { lines.append(line) } }
}

extension AdbKeyStore {
    /// The key as it is now, or nil when there is none; never makes one.
    func existingIdentity() throws -> Identity? {
        guard let key = try find() else { return nil }
        return try Identity(privateKey: key, publicKeyLine: Self.line(for: key))
    }
}
