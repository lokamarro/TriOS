// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The public key in the form adbd reads (AOSP `libcrypto_utils`,
/// `android_pubkey_encode`): a 524-byte little-endian structure, base64 coded.
///
/// ```text
/// u32 len = 64            number of 32-bit words in the modulus
/// u32 n0inv               -1 / n[0] mod 2^32
/// u32 n[64]               the modulus
/// u32 rr[64]              2^4096 mod n
/// u32 e                   the public exponent, 65537
/// ```
public enum AdbPublicKey {
    public static let wordCount = 64
    public static let structureSize = 4 + 4 + 4 * 64 + 4 * 64 + 4

    public enum Failure: Error, Equatable, Sendable {
        /// Not a PKCS#1 `RSAPublicKey`.
        case notPKCS1
        /// adb keys are RSA-2048.
        case wrongSize(bits: Int)
        /// adb keys have the exponent 65537.
        case wrongExponent
    }

    /// The base64 text for a PKCS#1 `RSAPublicKey` DER, which is what
    /// `SecKeyCopyExternalRepresentation` gives for an RSA public key.
    public static func encode(pkcs1: Data) throws -> String {
        try structure(pkcs1: pkcs1).base64EncodedString()
    }

    /// The 524-byte structure.
    public static func structure(pkcs1: Data) throws -> Data {
        let parts = try parse(pkcs1)
        guard parts.modulus.count == 4 * wordCount else {
            throw Failure.wrongSize(bits: parts.modulus.count * 8)
        }
        guard parts.exponent == 65537 else { throw Failure.wrongExponent }

        // Little-endian words from the big-endian modulus.
        var modulus = [UInt32](repeating: 0, count: wordCount)
        for index in 0..<wordCount {
            let offset = parts.modulus.count - 4 * (index + 1)
            var word: UInt32 = 0
            for byte in 0..<4 { word = word << 8 | UInt32(parts.modulus[offset + byte]) }
            modulus[index] = word
        }
        guard modulus[0] & 1 == 1, modulus[wordCount - 1] >> 31 == 1 else {
            throw Failure.wrongSize(bits: parts.modulus.count * 8 - 1)
        }

        var data = Data(capacity: structureSize)
        data.appendLittleEndian(UInt32(wordCount))
        data.appendLittleEndian(negatedInverse(of: modulus[0]))
        for word in modulus { data.appendLittleEndian(word) }
        for word in rr(modulus: modulus) { data.appendLittleEndian(word) }
        data.appendLittleEndian(UInt32(65537))
        return data
    }

    /// -n^-1 mod 2^32 for an odd `n`, by Newton's iteration: each step doubles
    /// the correct low bits, and n is its own inverse mod 8.
    static func negatedInverse(of n: UInt32) -> UInt32 {
        var inverse = n
        for _ in 0..<5 { inverse = inverse &* (2 &- n &* inverse) }
        return 0 &- inverse
    }

    /// 2^(32 * 64 * 2) mod n, by 4096 modular doublings of 1: no big-number
    /// library is needed for a number this size.
    static func rr(modulus n: [UInt32]) -> [UInt32] {
        var value = [UInt32](repeating: 0, count: n.count)
        value[0] = 1
        for _ in 0..<(32 * n.count * 2) {
            // value = 2 * value, remembering the bit shifted out.
            var carry: UInt32 = 0
            for index in 0..<n.count {
                let next = value[index] >> 31
                value[index] = value[index] << 1 | carry
                carry = next
            }
            if carry == 1 || !isLess(value, n) { subtract(&value, n) }
        }
        return value
    }

    private static func isLess(_ a: [UInt32], _ b: [UInt32]) -> Bool {
        for index in stride(from: a.count - 1, through: 0, by: -1) where a[index] != b[index] {
            return a[index] < b[index]
        }
        return false
    }

    /// a -= b, ignoring the borrow out of the top word (a carry already
    /// accounts for it).
    private static func subtract(_ a: inout [UInt32], _ b: [UInt32]) {
        var borrow: UInt64 = 0
        for index in 0..<a.count {
            let difference = UInt64(a[index]) &- UInt64(b[index]) &- borrow
            a[index] = UInt32(truncatingIfNeeded: difference)
            borrow = difference >> 63
        }
    }

    // MARK: DER

    /// `SEQUENCE { INTEGER modulus, INTEGER publicExponent }`.
    private static func parse(_ der: Data) throws -> (modulus: [UInt8], exponent: UInt64) {
        var reader = DERReader(Array(der))
        let sequence = try reader.read(tag: 0x30)
        var inner = DERReader(sequence)
        var modulus = try inner.read(tag: 0x02)
        let exponentBytes = try inner.read(tag: 0x02)
        guard inner.isAtEnd, reader.isAtEnd, exponentBytes.count <= 8 else { throw Failure.notPKCS1 }
        while modulus.first == 0 { modulus.removeFirst() }
        let exponent = exponentBytes.reduce(UInt64(0)) { $0 << 8 | UInt64($1) }
        return (modulus, exponent)
    }

    private struct DERReader {
        let bytes: [UInt8]
        var position = 0

        init(_ bytes: [UInt8]) {
            self.bytes = bytes
        }

        var isAtEnd: Bool { position == bytes.count }

        mutating func read(tag: UInt8) throws -> [UInt8] {
            guard position < bytes.count, bytes[position] == tag else { throw Failure.notPKCS1 }
            position += 1
            guard position < bytes.count else { throw Failure.notPKCS1 }
            var length = Int(bytes[position])
            position += 1
            if length & 0x80 != 0 {
                let count = length & 0x7F
                guard (1...3).contains(count), position + count <= bytes.count else { throw Failure.notPKCS1 }
                length = 0
                for _ in 0..<count {
                    length = length << 8 | Int(bytes[position])
                    position += 1
                }
            }
            guard position + length <= bytes.count else { throw Failure.notPKCS1 }
            defer { position += length }
            return Array(bytes[position..<(position + length)])
        }
    }
}
