// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
@preconcurrency import Security
import Testing

@testable import AndariumCore

/// The key encoder against moduli and the text `adb keygen` wrote for them
/// (AdbKeyVectors.swift).
struct AdbPublicKeyTests {
    /// PKCS#1 `RSAPublicKey` DER for a big-endian modulus and an exponent.
    private func der(modulus: [UInt8], exponent: [UInt8] = [1, 0, 1]) -> Data {
        func integer(_ bytes: [UInt8]) -> [UInt8] {
            var body = bytes
            if body.first.map({ $0 & 0x80 != 0 }) ?? false { body.insert(0, at: 0) }
            return [0x02] + length(body.count) + body
        }
        func length(_ count: Int) -> [UInt8] {
            count < 128 ? [UInt8(count)] : count < 256 ? [0x81, UInt8(count)] : [0x82, UInt8(count >> 8), UInt8(count & 0xFF)]
        }
        let body = integer(modulus) + integer(exponent)
        return Data([0x30] + length(body.count) + body)
    }

    private func bytes(hex: String) -> [UInt8] {
        stride(from: 0, to: hex.count, by: 2).map {
            let start = hex.index(hex.startIndex, offsetBy: $0)
            return UInt8(hex[start..<hex.index(start, offsetBy: 2)], radix: 16)!
        }
    }

    @Test func eachVectorMatchesWhatAdbKeygenWrote() throws {
        #expect(AdbKeyVectors.all.count >= 3)
        for vector in AdbKeyVectors.all {
            let key = der(modulus: bytes(hex: vector.modulusHex))
            #expect(try AdbPublicKey.encode(pkcs1: key) == vector.base64)
        }
    }

    @Test func theStructureHasTheDocumentedLayout() throws {
        let vector = try #require(AdbKeyVectors.all.first)
        let modulus = bytes(hex: vector.modulusHex)
        let structure = try AdbPublicKey.structure(pkcs1: der(modulus: modulus))
        #expect(structure.count == 524 && AdbPublicKey.structureSize == 524)
        #expect(structure.littleEndian32(at: 0) == 64)
        let n0 = structure.littleEndian32(at: 8)
        // n0inv * n[0] = -1 mod 2^32
        #expect(structure.littleEndian32(at: 4) &* n0 == UInt32.max)
        // The modulus follows, least significant word first.
        var low = UInt32(0)
        for byte in modulus.suffix(4) { low = low << 8 | UInt32(byte) }
        #expect(n0 == low)
        #expect(structure.littleEndian32(at: 520) == 65537)
        #expect(structure.count == 4 + 4 + 256 + 256 + 4)
        // The base64 text is 700 characters.
        #expect(try AdbPublicKey.encode(pkcs1: der(modulus: modulus)).count == 700)
    }

    @Test func rrIsTwoToThePowerOfTwiceTheBitsModN() {
        // Two words: the same loop, checked against a modular doubling in
        // 128-bit arithmetic. The second modulus has its top bit set, which
        // exercises the carry out of the top word.
        for n in [[UInt32(0xFFFF_FFFB), 0x0000_0003], [0xFFFF_FFC5, 0xFFFF_FFFF], [0x8000_0001, 0x8000_0000]] {
            let modulus = (UInt64(n[1]) << 32) | UInt64(n[0])
            var expected = UInt64(1)
            for _ in 0..<(32 * n.count * 2) {
                let (high, low) = expected.multipliedFullWidth(by: 2)
                expected = modulus.dividingFullWidth((high, low)).remainder
            }
            let rr = AdbPublicKey.rr(modulus: n)
            #expect((UInt64(rr[1]) << 32) | UInt64(rr[0]) == expected)
        }
    }

    @Test func theNegatedInverseHoldsForManyOddNumbers() {
        for n in stride(from: UInt32(1), to: UInt32.max - 1_000_000, by: 987_654_321) where n & 1 == 1 {
            #expect(AdbPublicKey.negatedInverse(of: n) &* n == UInt32.max)
        }
        for n: UInt32 in [1, 3, 0xFFFF_FFFF, 0x8000_0001] {
            #expect(AdbPublicKey.negatedInverse(of: n) &* n == UInt32.max)
        }
    }

    @Test func anExponentOtherThan65537IsRefused() throws {
        let modulus = bytes(hex: try #require(AdbKeyVectors.all.first).modulusHex)
        #expect(throws: AdbPublicKey.Failure.wrongExponent) {
            try AdbPublicKey.encode(pkcs1: der(modulus: modulus, exponent: [3]))
        }
    }

    @Test func aKeyOfAnotherSizeIsRefused() {
        var small = [UInt8](repeating: 0xAB, count: 128)
        small[127] |= 1
        #expect(throws: AdbPublicKey.Failure.wrongSize(bits: 1024)) {
            try AdbPublicKey.encode(pkcs1: der(modulus: small))
        }
    }

    @Test func somethingThatIsNotPKCS1IsRefused() {
        #expect(throws: AdbPublicKey.Failure.notPKCS1) { try AdbPublicKey.encode(pkcs1: Data()) }
        #expect(throws: AdbPublicKey.Failure.notPKCS1) { try AdbPublicKey.encode(pkcs1: Data([0x30, 0x03, 0x02, 0x01])) }
        #expect(throws: AdbPublicKey.Failure.notPKCS1) {
            try AdbPublicKey.encode(pkcs1: Data(repeating: 0x04, count: 300))
        }
    }

    @Test func aKeySecurityMakesEncodesAndItsSignaturesVerify() throws {
        let attributes: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA, kSecAttrKeySizeInBits as String: 2048,
        ]
        var error: Unmanaged<CFError>?
        let key = try #require(SecKeyCreateRandomKey(attributes as CFDictionary, &error))
        let publicKey = try #require(SecKeyCopyPublicKey(key))
        let external = try #require(SecKeyCopyExternalRepresentation(publicKey, &error) as Data?)
        // Security gives the PKCS#1 form, which is what the encoder reads.
        let line = try AdbPublicKey.encode(pkcs1: external)
        #expect(line.count == 700)
        let decoded = try #require(Data(base64Encoded: line))
        #expect(decoded.count == 524)
        // The modulus in the structure is the key's: the first signature check passes.
        let token = Data((0..<20).map { UInt8($0) })
        let signature = try SecKeySigner(privateKey: key).sign(token: token)
        #expect(SecKeyVerifySignature(
            publicKey, SecKeySigner.algorithm, token as CFData, signature as CFData, nil))
    }
}
