// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Security

/// Answers adbd's AUTH token with a signature of the host's key.
public protocol AdbSigner: Sendable {
    /// The signature adbd verifies: `RSA_verify(NID_sha1, token, 20, signature)`,
    /// that is PKCS#1 v1.5 over the SHA-1 `DigestInfo` of the 20 token bytes
    /// taken as the digest itself.
    func sign(token: Data) throws -> Data
}

/// Signs with a private RSA key held by Security.framework.
public struct SecKeySigner: AdbSigner, @unchecked Sendable {
    public static let algorithm = SecKeyAlgorithm.rsaSignatureDigestPKCS1v15SHA1

    private let key: SecKey

    public init(privateKey: SecKey) {
        key = privateKey
    }

    public func sign(token: Data) throws -> Data {
        guard SecKeyIsAlgorithmSupported(key, .sign, Self.algorithm) else {
            throw AdbError.protocolViolation("the key cannot sign for adb")
        }
        var error: Unmanaged<CFError>?
        guard let signature = SecKeyCreateSignature(key, Self.algorithm, token as CFData, &error) else {
            throw AdbError.io(Int32(clamping: (error?.takeRetainedValue()).map { CFErrorGetCode($0) } ?? -1))
        }
        return signature as Data
    }
}
