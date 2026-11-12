// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Security

/// The guest's adb host key: one RSA-2048 key in the Keychain, this device
/// only. The Secure Enclave holds only P-256 keys and adb needs RSA, so the
/// key is a software key the Keychain protects. Its public half goes to the
/// guest in each boot request, where adbd takes it as authorized for that
/// boot; its private half signs adbd's AUTH tokens. It is made before the
/// first start that has none, and Erase guest deletes it. A harness
/// launch's isolated instance has a key of its own.
nonisolated struct AdbKeyStore {
    static let defaultTag = "com.imxieyi.andarium.adb-key"
    static let label = "Andarium debugging key"

    /// The key and the line adbd is given for it.
    nonisolated struct Identity: @unchecked Sendable {
        var privateKey: SecKey
        /// The public key as adb writes it, base64, without a comment.
        var publicKeyLine: String

        var signer: SecKeySigner { SecKeySigner(privateKey: privateKey) }
    }

    nonisolated enum Failure: Error, Equatable {
        case keychain(OSStatus)
        case noPublicKey
    }

    /// The application tag that names the key: a test uses its own.
    let tag: String

    init(tag: String = AdbKeyStore.defaultTag) {
        self.tag = tag
    }

    #if !APP_STORE
    /// The key of the guest a launch runs. The ordinary guest keeps the
    /// default tag; an isolated instance is tagged with its name, so erasing
    /// one never deletes the ordinary guest's key. `base` is for tests. An
    /// App Store build has no isolated instance.
    init(instance: String?, base: String = AdbKeyStore.defaultTag) {
        self.init(tag: instance.map { "\(base).instance.\($0)" } ?? base)
    }
    #endif

    private var tagData: Data { Data(tag.utf8) }

    /// The key as it is stored, or nil when there is none.
    func find() throws -> SecKey? {
        let query: [String: Any] = baseQuery().merging([
            kSecReturnRef as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]) { _, new in new }
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        switch status {
        case errSecSuccess:
            guard let item, CFGetTypeID(item) == SecKeyGetTypeID() else { throw Failure.noPublicKey }
            return unsafeDowncast(item, to: SecKey.self)
        case errSecItemNotFound:
            return nil
        default:
            throw Failure.keychain(status)
        }
    }

    /// Finding the key and making it are one step: two callers that find none
    /// would each make one, and the guest would be given a key the client
    /// does not sign with.
    private static let creation = NSLock()

    /// The key, made first when there is none. It blocks for the key's
    /// generation, so call it away from the main thread.
    func loadOrCreate() throws -> Identity {
        Self.creation.lock()
        defer { Self.creation.unlock() }
        let key = try find() ?? create()
        return try Identity(privateKey: key, publicKeyLine: Self.line(for: key))
    }

    private func create() throws -> SecKey {
        let attributes: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
            kSecAttrKeySizeInBits as String: 2048,
            kSecUseDataProtectionKeychain as String: true,
            kSecPrivateKeyAttrs as String: [
                kSecAttrIsPermanent as String: true,
                kSecAttrApplicationTag as String: tagData,
                kSecAttrLabel as String: Self.label,
                kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
            ] as [String: Any],
        ]
        var error: Unmanaged<CFError>?
        guard let key = SecKeyCreateRandomKey(attributes as CFDictionary, &error) else {
            let code = error.map { OSStatus(truncatingIfNeeded: CFErrorGetCode($0.takeRetainedValue())) } ?? errSecInternalError
            throw Failure.keychain(code)
        }
        return key
    }

    /// Deletes the key. A key that is not there is no failure.
    @discardableResult
    func delete() -> Bool {
        let status = SecItemDelete(baseQuery() as CFDictionary)
        return status == errSecSuccess || status == errSecItemNotFound
    }

    private func baseQuery() -> [String: Any] {
        [
            kSecClass as String: kSecClassKey,
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
            kSecAttrKeyClass as String: kSecAttrKeyClassPrivate,
            kSecAttrApplicationTag as String: tagData,
            kSecAttrSynchronizable as String: false,
            kSecUseDataProtectionKeychain as String: true,
        ]
    }

    static func line(for privateKey: SecKey) throws -> String {
        guard let publicKey = SecKeyCopyPublicKey(privateKey) else { throw Failure.noPublicKey }
        var error: Unmanaged<CFError>?
        guard let der = SecKeyCopyExternalRepresentation(publicKey, &error) as Data? else {
            throw Failure.noPublicKey
        }
        return try AdbPublicKey.encode(pkcs1: der)
    }
}
