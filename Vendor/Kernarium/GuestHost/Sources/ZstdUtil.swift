// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import LibZstd

/// Streaming Zstandard decompression for files owned by the application.
///
/// The operation is synchronous. Progress callbacks run on the calling thread,
/// so callers performing large decompressions should invoke this API from a
/// worker task or queue. The destination is replaced only after the complete
/// stream has decoded successfully.
final class ZstdUtil {
    struct DecompressionProgress: Sendable {
        let compressedBytesConsumed: UInt64
        let compressedBytesTotal: UInt64
        let decompressedBytesWritten: UInt64
        let sparseBytesSkipped: UInt64

        var fractionCompleted: Double {
            guard compressedBytesTotal != 0 else { return 0 }
            return min(
                1,
                Double(compressedBytesConsumed) /
                    Double(compressedBytesTotal)
            )
        }
    }

    enum DecompressionError: LocalizedError {
        case invalidFileURL(URL)
        case sourceIsNotRegularFile(URL)
        case truncatedFrame
        case decoderStalled
        case outputTooLarge
        case zstd(String)

        var errorDescription: String? {
            switch self {
            case .invalidFileURL(let url):
                return "Zstandard input and output must be file URLs: \(url)"
            case .sourceIsNotRegularFile(let url):
                return "Zstandard input is not a regular file: \(url.path)"
            case .truncatedFrame:
                return "The Zstandard input ended before its frame completed."
            case .decoderStalled:
                return "The Zstandard decoder made no forward progress."
            case .outputTooLarge:
                return "The decompressed file exceeds the supported file size."
            case .zstd(let message):
                return "Zstandard decompression failed: \(message)"
            }
        }
    }

    typealias ProgressHandler = (DecompressionProgress) -> Void

    nonisolated private static let streamBufferSize = 1024 * 1024
    nonisolated private static let sparseBlockSize = 64 * 1024

    private struct OutputState {
        var logicalOffset: off_t = 0
        var pendingHoleOffset: off_t?
        var pendingHoleLength: off_t = 0
        var sparseBytesSkipped: UInt64 = 0

        nonisolated init() {}
    }

    private init() {}

    /// Decompresses one or more concatenated Zstandard frames.
    ///
    /// When `sparse` is true, completely zero 64 KiB output blocks are emitted
    /// as filesystem holes. Existing destinations remain unchanged on failure.
    nonisolated static func decompress(
        from sourceURL: URL,
        to destinationURL: URL,
        sparse: Bool = false,
        progress: ProgressHandler? = nil
    ) throws {
        guard sourceURL.isFileURL else {
            throw DecompressionError.invalidFileURL(sourceURL)
        }
        guard destinationURL.isFileURL else {
            throw DecompressionError.invalidFileURL(destinationURL)
        }

        let sourceDescriptor = sourceURL.path.withCString {
            Darwin.open($0, O_RDONLY | O_CLOEXEC)
        }
        guard sourceDescriptor >= 0 else {
            throw posixError(operation: "open input", url: sourceURL)
        }
        defer { _ = Darwin.close(sourceDescriptor) }

        var sourceStatus = stat()
        guard Darwin.fstat(sourceDescriptor, &sourceStatus) == 0 else {
            throw posixError(operation: "inspect input", url: sourceURL)
        }
        guard sourceStatus.st_mode & S_IFMT == S_IFREG else {
            throw DecompressionError.sourceIsNotRegularFile(sourceURL)
        }
        guard sourceStatus.st_size >= 0 else {
            throw DecompressionError.outputTooLarge
        }
        let compressedBytesTotal = UInt64(sourceStatus.st_size)

        let temporaryURL = destinationURL
            .deletingLastPathComponent()
            .appendingPathComponent(
                ".\(destinationURL.lastPathComponent)." +
                    "\(UUID().uuidString).zstd.tmp"
            )
        var destinationDescriptor = temporaryURL.path.withCString {
            Darwin.open(
                $0,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                S_IRUSR | S_IWUSR
            )
        }
        guard destinationDescriptor >= 0 else {
            throw posixError(operation: "create output", url: temporaryURL)
        }
        var published = false
        defer {
            if destinationDescriptor >= 0 {
                _ = Darwin.close(destinationDescriptor)
            }
            if !published {
                temporaryURL.path.withCString { _ = Darwin.unlink($0) }
            }
        }

        guard let stream = ZSTD_createDStream() else {
            throw DecompressionError.zstd("cannot allocate decoder")
        }
        defer { _ = ZSTD_freeDStream(stream) }
        try checkZstd(ZSTD_initDStream(stream))

        var inputStorage = [UInt8](
            repeating: 0,
            count: max(streamBufferSize, Int(ZSTD_DStreamInSize()))
        )
        var outputStorage = [UInt8](
            repeating: 0,
            count: max(streamBufferSize, Int(ZSTD_DStreamOutSize()))
        )
        var compressedBytesConsumed: UInt64 = 0
        var decompressedBytesWritten: UInt64 = 0
        var outputState = OutputState()
        var finalFrameRemaining = 1
        var completedFrame = false

        progress?(
            DecompressionProgress(
                compressedBytesConsumed: 0,
                compressedBytesTotal: compressedBytesTotal,
                decompressedBytesWritten: 0,
                sparseBytesSkipped: 0
            )
        )

        while true {
            let inputCount = try inputStorage.withUnsafeMutableBytes {
                try read(
                    descriptor: sourceDescriptor,
                    buffer: $0,
                    sourceURL: sourceURL
                )
            }
            if inputCount == 0 {
                break
            }
            let compressedBase = compressedBytesConsumed

            try inputStorage.withUnsafeBytes { inputBytes in
                var input = ZSTD_inBuffer(
                    src: inputBytes.baseAddress,
                    size: inputCount,
                    pos: 0
                )
                var flushOutput = false
                repeat {
                    let previousInputPosition = input.pos
                    var produced = 0
                    var outputWasFull = false
                    finalFrameRemaining = try outputStorage
                        .withUnsafeMutableBytes { outputBytes in
                            var output = ZSTD_outBuffer(
                                dst: outputBytes.baseAddress,
                                size: outputBytes.count,
                                pos: 0
                            )
                            let result = ZSTD_decompressStream(
                                stream,
                                &output,
                                &input
                            )
                            try checkZstd(result)
                            produced = output.pos
                            outputWasFull = output.pos == output.size
                            return Int(result)
                        }
                    if finalFrameRemaining == 0 {
                        completedFrame = true
                    }
                    if produced != 0 {
                        let (newSize, overflow) = decompressedBytesWritten
                            .addingReportingOverflow(UInt64(produced))
                        guard !overflow, newSize <= UInt64(Int64.max) else {
                            throw DecompressionError.outputTooLarge
                        }
                        try outputStorage.withUnsafeBytes { outputBytes in
                            try write(
                                descriptor: destinationDescriptor,
                                buffer: UnsafeRawBufferPointer(
                                    start: outputBytes.baseAddress,
                                    count: produced
                                ),
                                sparse: sparse,
                                state: &outputState,
                                destinationURL: temporaryURL
                            )
                        }
                        decompressedBytesWritten = newSize
                        progress?(
                            DecompressionProgress(
                                compressedBytesConsumed: compressedBase +
                                    UInt64(input.pos),
                                compressedBytesTotal: compressedBytesTotal,
                                decompressedBytesWritten:
                                    decompressedBytesWritten,
                                sparseBytesSkipped:
                                    outputState.sparseBytesSkipped
                            )
                        )
                    }
                    flushOutput = outputWasFull && finalFrameRemaining > 0
                    let decoderStalled =
                        input.pos == previousInputPosition && produced == 0 &&
                        (input.pos < input.size || flushOutput)
                    if decoderStalled {
                        throw DecompressionError.decoderStalled
                    }
                } while input.pos < input.size || flushOutput
            }
            compressedBytesConsumed += UInt64(inputCount)
        }

        guard completedFrame, finalFrameRemaining == 0 else {
            throw DecompressionError.truncatedFrame
        }
        guard Darwin.ftruncate(
            destinationDescriptor,
            off_t(decompressedBytesWritten)
        ) == 0 else {
            throw posixError(operation: "size output", url: temporaryURL)
        }
        if sparse {
            try punchPendingHole(
                descriptor: destinationDescriptor,
                state: &outputState,
                destinationURL: temporaryURL
            )
        }
        guard Darwin.close(destinationDescriptor) == 0 else {
            destinationDescriptor = -1
            throw posixError(operation: "close output", url: temporaryURL)
        }
        destinationDescriptor = -1

        let renameStatus = temporaryURL.path.withCString { temporaryPath in
            destinationURL.path.withCString { destinationPath in
                Darwin.rename(temporaryPath, destinationPath)
            }
        }
        guard renameStatus == 0 else {
            throw posixError(operation: "publish output", url: destinationURL)
        }
        published = true
        progress?(
            DecompressionProgress(
                compressedBytesConsumed: compressedBytesTotal,
                compressedBytesTotal: compressedBytesTotal,
                decompressedBytesWritten: decompressedBytesWritten,
                sparseBytesSkipped: outputState.sparseBytesSkipped
            )
        )
    }

    nonisolated private static func read(
        descriptor: Int32,
        buffer: UnsafeMutableRawBufferPointer,
        sourceURL: URL
    ) throws -> Int {
        while true {
            let result = Darwin.read(
                descriptor,
                buffer.baseAddress,
                buffer.count
            )
            if result >= 0 {
                return result
            }
            if errno != EINTR {
                throw posixError(operation: "read input", url: sourceURL)
            }
        }
    }

    nonisolated private static func write(
        descriptor: Int32,
        buffer: UnsafeRawBufferPointer,
        sparse: Bool,
        state: inout OutputState,
        destinationURL: URL
    ) throws {
        guard sparse else {
            try writeAll(
                descriptor: descriptor,
                buffer: buffer,
                destinationURL: destinationURL
            )
            state.logicalOffset += off_t(buffer.count)
            return
        }

        var offset = 0
        while offset < buffer.count {
            let count = min(sparseBlockSize, buffer.count - offset)
            let block = UnsafeRawBufferPointer(
                start: buffer.baseAddress?.advanced(by: offset),
                count: count
            )
            if block.allSatisfy({ $0 == 0 }) {
                if state.pendingHoleOffset == nil {
                    state.pendingHoleOffset = state.logicalOffset
                }
                guard Darwin.lseek(descriptor, off_t(count), SEEK_CUR) >= 0 else {
                    throw posixError(
                        operation: "create sparse output hole",
                        url: destinationURL
                    )
                }
                state.pendingHoleLength += off_t(count)
                state.sparseBytesSkipped += UInt64(count)
            } else {
                try writeAll(
                    descriptor: descriptor,
                    buffer: block,
                    destinationURL: destinationURL
                )
                try punchPendingHole(
                    descriptor: descriptor,
                    state: &state,
                    destinationURL: destinationURL
                )
            }
            offset += count
            state.logicalOffset += off_t(count)
        }
    }

    nonisolated private static func punchPendingHole(
        descriptor: Int32,
        state: inout OutputState,
        destinationURL: URL
    ) throws {
        guard let offset = state.pendingHoleOffset else { return }
        // F_PUNCHHOLE takes whole filesystem blocks. A hole is only as aligned
        // as the output chunks before it, so one that begins or ends inside a
        // block leaves that block allocated, which reads as zeros either way.
        var filesystem = statfs()
        let block = Darwin.fstatfs(descriptor, &filesystem) == 0 &&
            filesystem.f_bsize > 0 ? off_t(filesystem.f_bsize) : 4096
        let start = (offset + block - 1) / block * block
        let end = (offset + state.pendingHoleLength) / block * block
        if end > start {
            var hole = fpunchhole_t(
                fp_flags: 0,
                reserved: 0,
                fp_offset: start,
                fp_length: end - start
            )
            guard Darwin.fcntl(descriptor, F_PUNCHHOLE, &hole) == 0 else {
                throw posixError(
                    operation: "deallocate sparse output range",
                    url: destinationURL
                )
            }
        }
        state.pendingHoleOffset = nil
        state.pendingHoleLength = 0
    }

    nonisolated private static func writeAll(
        descriptor: Int32,
        buffer: UnsafeRawBufferPointer,
        destinationURL: URL
    ) throws {
        var offset = 0
        while offset < buffer.count {
            let result = Darwin.write(
                descriptor,
                buffer.baseAddress?.advanced(by: offset),
                buffer.count - offset
            )
            if result > 0 {
                offset += result
            } else if result < 0 && errno == EINTR {
                continue
            } else {
                throw posixError(operation: "write output", url: destinationURL)
            }
        }
    }

    nonisolated private static func checkZstd(_ result: Int) throws {
        guard ZSTD_isError(result) == 0 else {
            throw DecompressionError.zstd(
                String(cString: ZSTD_getErrorName(result))
            )
        }
    }

    nonisolated private static func posixError(
        operation: String,
        url: URL
    ) -> NSError {
        let code = errno
        return NSError(
            domain: NSPOSIXErrorDomain,
            code: Int(code),
            userInfo: [
                NSLocalizedDescriptionKey:
                    "Cannot \(operation) at \(url.path): " +
                    String(cString: strerror(code)),
                NSURLErrorKey: url,
            ]
        )
    }
}
