// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_decoder.hpp"
#include "h264_stream.hpp"
#include "hevc_stream.hpp"

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace uml::platform {
namespace {

void checked(OSStatus status, const char* operation)
{
    if (status != noErr)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(status));
}

template<class T> class CFObject {
public:
    CFObject() = default;
    // Adopts a reference a CF "Copy" function already retained for us.
    explicit CFObject(T value) : value_(value) {}
    ~CFObject() { reset(); }
    CFObject(const CFObject&) = delete;
    CFObject& operator=(const CFObject&) = delete;
    T* address() { reset(); return &value_; }
    T get() const { return value_; }
    void reset() { if (value_) CFRelease(value_); value_ = nullptr; }
private:
    T value_ {};
};

/* The largest picture, per axis, that each hardware decoder opens. The
 * capability list states it and a session is refused above it, so what the
 * guest is told and what the host accepts cannot differ. The hardware limits a
 * dimension and not the area: an M1 Ultra opens H.264 up to 4096 on either
 * axis (4096 x 4096 and 2304 x 4096 decode, 4112 in either direction is
 * refused) and HEVC at least to 8192 x 8192, where it stops being measured.
 * A level's frame-size limit is not used: level 5.2 allows 36864 macroblocks
 * and the hardware opens 65536. iOS has not been measured. */
struct SizeLimit {
    std::uint32_t width;
    std::uint32_t height;
};
constexpr SizeLimit decodeSizeLimit(VideoCodec codec) noexcept
{
    switch (codec) {
    case VideoCodec::h264: return {4096, 4096};
    case VideoCodec::hevc: return {8192, 8192};
    case VideoCodec::jpeg: case VideoCodec::vp9: return {4096, 2304};
    }
    return {};
}

bool high10Available()
{
#if TARGET_OS_OSX
    // The existing Virgl decoder excludes macOS: a session-only probe accepts
    // High10 headers there even though hardware rejects representative frames.
    return false;
#else
    static const bool available = [] {
        // Same profile-specific sequence as the maintained Virgl backend.
        const std::uint8_t sps[] = {0x67, 0x6e, 0x00, 0x0a, 0xa6, 0xcd, 0x94, 0x42,
            0x6c, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x1e, 0x03,
            0xc4, 0x89, 0x65, 0x80};
        const std::uint8_t pps[] = {0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0};
        const std::uint8_t* data[] = {sps, pps};
        const std::size_t sizes[] = {sizeof(sps), sizeof(pps)};
        CFObject<CMVideoFormatDescriptionRef> description;
        CFObject<VTDecompressionSessionRef> session;
        CFObject<CFTypeRef> hardware;
        if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
            2, data, sizes, 4, description.address()) != noErr) return false;
        NSDictionary* specification = @{(__bridge NSString*)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES};
        NSDictionary* attributes = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)};
        if (VTDecompressionSessionCreate(kCFAllocatorDefault, description.get(),
            (__bridge CFDictionaryRef)specification, (__bridge CFDictionaryRef)attributes,
            nullptr, session.address()) != noErr) return false;
        const bool result = VTSessionCopyProperty(session.get(),
            kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
            kCFAllocatorDefault, hardware.address()) == noErr && hardware.get() == kCFBooleanTrue;
        VTDecompressionSessionInvalidate(session.get());
        return result;
    }();
    return available;
#endif
}

/* VP9 is a supplemental VideoToolbox decoder: `VTIsHardwareDecodeSupported`
 * answers no until the process asks for it, which is what Chromium and FFmpeg
 * do before probing. Asking once is harmless where no such decoder exists.
 */
bool vp9Available()
{
    static const bool available = [] {
        VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
        return bool(VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9));
    }();
    return available;
}

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint32_t read(unsigned count)
    {
        std::uint32_t value = 0;
        while (count-- > 0) {
            if (position_ >= bytes_.size() * 8) throw std::runtime_error("truncated VP9 header");
            value = (value << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1);
            ++position_;
        }
        return value;
    }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ {};
};

struct Vp9KeyFrame {
    VideoFormat format;
    unsigned profile;
    unsigned colorSpace;   // VP9 CS_* as the uncompressed header states it
    bool fullRange;
};

/* VP9 carries no parameter sets: a key frame's uncompressed header is the only
 * place the profile, bit depth and frame size appear, so the format is read
 * from the stream rather than configured ahead of it. Returns nothing for a
 * frame that cannot change the format, and the caller keeps the current one.
 */
std::optional<Vp9KeyFrame> vp9KeyFrame(std::span<const std::uint8_t> bytes)
{
    BitReader reader(bytes);
    if (reader.read(2) != 2) throw std::runtime_error("invalid VP9 frame marker");
    const unsigned low = reader.read(1);
    const unsigned high = reader.read(1);
    const unsigned profile = (high << 1) | low;
    if (profile == 3) reader.read(1);
    if (reader.read(1) != 0) return std::nullopt;   // show_existing_frame
    const bool key = reader.read(1) == 0;
    reader.read(1);                                 // show_frame
    reader.read(1);                                 // error_resilient_mode
    if (!key) return std::nullopt;
    if (reader.read(24) != 0x498342) throw std::runtime_error("invalid VP9 frame sync code");
    unsigned bitDepth = 8;
    if (profile >= 2) bitDepth = reader.read(1) != 0 ? 12 : 10;
    unsigned colorSpace = 0, colorRange = 0;
    colorSpace = reader.read(3);
    if (colorSpace != 7 /* SRGB */) {
        colorRange = reader.read(1);
        if (profile == 1 || profile == 3) {
            if (reader.read(1) != 1 || reader.read(1) != 1)
                throw std::runtime_error("VP9 decoder requires 4:2:0 chroma");
            reader.read(1);
        }
    } else {
        throw std::runtime_error("VP9 4:4:4 is not a supported decoder input");
    }
    const std::uint32_t width = reader.read(16) + 1;
    const std::uint32_t height = reader.read(16) + 1;
    return Vp9KeyFrame {{width, height, bitDepth}, profile, colorSpace, colorRange != 0};
}

/* VP9 states its color space in the frame header rather than in a parameter
 * set, so the configuration record has to carry it across or the decoder
 * resolves an unstated description by frame size and a stream that means
 * BT.601 at HD size is converted with the wrong matrix. The values are the
 * ISO/IEC 23001-8 code points the record is defined in. */
struct Vp9ColorDescription { std::uint8_t primaries, transfer, matrix; };

Vp9ColorDescription vp9ColorDescription(unsigned colorSpace)
{
    switch (colorSpace) {
    case 1: return {5, 6, 6};    // CS_BT_601
    case 2: return {1, 1, 1};    // CS_BT_709
    case 3: return {6, 6, 6};    // CS_SMPTE_170
    case 4: return {7, 7, 7};    // CS_SMPTE_240
    case 5: return {9, 14, 9};   // CS_BT_2020
    default: return {2, 2, 2};   // unstated; the decoder resolves it
    }
}

// The VP9 codec configuration box a CMVideoFormatDescription needs, in the
// same shape Chromium and FFmpeg build it.
NSData* vp9Configuration(unsigned profile, unsigned bitDepth, unsigned colorSpace, bool fullRange)
{
    const auto color = vp9ColorDescription(colorSpace);
    const std::uint8_t atom[12] = {
        1, 0, 0, 0,                                 // version and flags
        std::uint8_t(profile),
        41,                                         // level 4.1; advisory only
        // bit depth, 4:2:0 colocated chroma, and the stream's range
        std::uint8_t((bitDepth << 4) | (1u << 1) | (fullRange ? 1u : 0u)),
        color.primaries, color.transfer, color.matrix,
        0, 0,                                       // codecInitializationDataSize
    };
    return [NSData dataWithBytes:atom length:sizeof(atom)];
}

std::vector<std::span<const std::uint8_t>> annexB(std::span<const std::uint8_t> bytes)
{
    std::vector<std::span<const std::uint8_t>> units;
    const auto prefix = [bytes](std::size_t p) -> std::size_t {
        if (p + 3 <= bytes.size() && bytes[p] == 0 && bytes[p + 1] == 0) {
            if (bytes[p + 2] == 1) return 3;
            if (p + 4 <= bytes.size() && bytes[p + 2] == 0 && bytes[p + 3] == 1) return 4;
        }
        return 0;
    };
    std::size_t begin = bytes.size();
    for (std::size_t i = 0; i < bytes.size();) {
        const auto length = prefix(i);
        if (!length) { ++i; continue; }
        if (begin < i) units.push_back(bytes.subspan(begin, i - begin));
        begin = i + length;
        i = begin;
    }
    if (begin < bytes.size()) units.push_back(bytes.subspan(begin));
    if (units.empty()) throw std::runtime_error("video packet has no Annex B units");
    return units;
}

VideoFormat jpegFormat(std::span<const std::uint8_t> data)
{
    if (data.size() < 4 || data[0] != 0xff || data[1] != 0xd8)
        throw std::runtime_error("invalid JPEG start marker");
    for (std::size_t p = 2; p + 4 <= data.size();) {
        if (data[p++] != 0xff) throw std::runtime_error("invalid JPEG marker");
        while (p < data.size() && data[p] == 0xff) ++p;
        if (p >= data.size()) break;
        const auto marker = data[p++];
        if (marker == 0xd9 || marker == 0xda) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (p + 2 > data.size()) break;
        const std::size_t length = (std::size_t(data[p]) << 8) | data[p + 1];
        if (length < 2 || length > data.size() - p) break;
        if (marker == 0xc0) {
            if (length < 17 || data[p + 2] != 8 || data[p + 7] != 3 ||
                data[p + 9] != 0x22 || data[p + 12] != 0x11 || data[p + 15] != 0x11)
                throw std::runtime_error("JPEG decoder requires baseline 8-bit 4:2:0");
            return {std::uint32_t(data[p + 5]) * 256 + data[p + 6],
                    std::uint32_t(data[p + 3]) * 256 + data[p + 4], 8};
        }
        p += length;
    }
    throw std::runtime_error("JPEG baseline frame header is missing");
}

/* The rectangle of a decoded picture the stream shows, when that is less than
 * the whole of it. */
struct Crop {
    std::uint32_t x {};
    std::uint32_t y {};
    std::uint32_t width {};
    std::uint32_t height {};
    bool set {};
};

/* One picture on its way through the hardware: what the caller said about it,
 * and the frames the hardware's callback made of it. The callback runs before
 * the wait that follows each picture returns, so the picture lives on the
 * stack of the call that submitted it. */
struct Picture {
    std::uint64_t timestamp {};
    Crop crop;
    // The caller wants 8-bit planes whatever the depth of the stream.
    bool narrow {};
    std::vector<DecodedVideoFrame> frames;
};

/* The decoded surface as the planes or pixels a caller takes, cut to the
 * picture the stream shows. */
DecodedVideoFrame copyPicture(CVImageBufferRef image, const Picture& picture, std::uint64_t timestamp)
{
    const auto pixelFormat = CVPixelBufferGetPixelFormatType(image);
    const bool packed = pixelFormat == kCVPixelFormatType_32BGRA;
    const bool tenBit = pixelFormat == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
                        pixelFormat == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    const bool fullRange = pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
                           pixelFormat == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    if (!packed && !tenBit && pixelFormat != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
        pixelFormat != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
        throw std::runtime_error("unexpected hardware video pixel format");
    if (CVPixelBufferGetPlaneCount(image) != (packed ? 0u : 2u))
        throw std::runtime_error("invalid hardware video planes");
    const std::uint32_t surfaceWidth = std::uint32_t(CVPixelBufferGetWidth(image));
    const std::uint32_t surfaceHeight = std::uint32_t(CVPixelBufferGetHeight(image));
    const Crop crop = picture.crop.set ? picture.crop : Crop {0, 0, surfaceWidth, surfaceHeight, true};
    if (!crop.width || !crop.height || crop.x > surfaceWidth || crop.width > surfaceWidth - crop.x ||
        crop.y > surfaceHeight || crop.height > surfaceHeight - crop.y)
        throw std::runtime_error("hardware video surface is smaller than the picture it carries");
    DecodedVideoFrame frame;
    frame.layout = packed ? VideoPixelLayout::packedBgra : VideoPixelLayout::planarYuv;
    const bool narrowed = tenBit && picture.narrow;
    frame.format = {crop.width, crop.height, tenBit && !narrowed ? 10u : 8u};
    /* Read back the description the hardware itself applied, so a
     * caller that converts this frame reaches the same picture the
     * hardware would have packed. A stream that states nothing is
     * resolved by VideoToolbox, not guessed again here. */
    frame.color.fullRange = fullRange;
    CFObject<CFTypeRef> matrixValue {CVBufferCopyAttachment(image, kCVImageBufferYCbCrMatrixKey, nullptr)};
    if (const auto matrix = matrixValue.get()) {
        if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4) ||
            CFEqual(matrix, kCVImageBufferYCbCrMatrix_SMPTE_240M_1995))
            frame.color.matrix = VideoColorMatrix::bt601;
        else if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_2020))
            frame.color.matrix = VideoColorMatrix::bt2020;
        else frame.color.matrix = VideoColorMatrix::bt709;
    } else {
        // The same rule VideoToolbox applies to an unstated matrix.
        frame.color.matrix = surfaceHeight > 576 ? VideoColorMatrix::bt709 : VideoColorMatrix::bt601;
    }
    frame.timestamp = timestamp;
    checked(CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly), "map decoded video");
    struct Unlock {
        CVImageBufferRef image;
        ~Unlock() { CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly); }
    } unlock {image};
    if (packed) {
        const std::size_t rowBytes = std::size_t(crop.width) * 4;
        const std::size_t offset = std::size_t(crop.x) * 4;
        const auto stride = CVPixelBufferGetBytesPerRow(image);
        const auto* source = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(image));
        if (!source || stride < offset + rowBytes) throw std::runtime_error("truncated hardware video surface");
        frame.pixels.resize(rowBytes * crop.height);
        for (std::size_t row = 0; row < crop.height; ++row)
            std::memcpy(frame.pixels.data() + row * rowBytes, source + (crop.y + row) * stride + offset, rowBytes);
        return frame;
    }
    const std::size_t sample = tenBit ? 2 : 1;
    const std::size_t outputSample = narrowed ? 1 : sample;
    for (unsigned plane = 0; plane < 2; ++plane) {
        /* The chroma plane holds one interleaved pair for every two luma
         * samples each way, and a window offset counts whole pairs. */
        const std::size_t width = plane ? ((crop.width + 1) / 2) * 2 : crop.width;
        const std::size_t height = plane ? (crop.height + 1) / 2 : crop.height;
        const std::size_t firstColumn = plane ? (crop.x / 2) * 2 : crop.x;
        const std::size_t firstRow = plane ? crop.y / 2 : crop.y;
        const std::size_t rowBytes = width * sample;
        const std::size_t offset = firstColumn * sample;
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(image, plane);
        const auto* source = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, plane));
        if (!source || stride < offset + rowBytes || CVPixelBufferGetHeightOfPlane(image, plane) < firstRow + height)
            throw std::runtime_error("truncated hardware video surface");
        auto& destination = plane ? frame.chroma : frame.luma;
        destination.resize(width * outputSample * height);
        for (std::size_t row = 0; row < height; ++row) {
            const auto* in = source + (firstRow + row) * stride + offset;
            auto* out = destination.data() + row * width * outputSample;
            if (!narrowed) {
                std::memcpy(out, in, rowBytes);
                continue;
            }
            /* A 10-bit sample sits in the high bits of a little-endian pair.
             * Rounding it to 8 bits here, rather than asking the hardware for
             * 8-bit output, keeps the chroma exact: the hardware's own
             * conversion moves the colors. */
            for (std::size_t column = 0; column < width; ++column) {
                const unsigned value = unsigned(in[2 * column]) | (unsigned(in[2 * column + 1]) << 8);
                out[column] = std::uint8_t(std::min(255u, (value + 0x80u) >> 8));
            }
        }
    }
    return frame;
}

class AppleVideoDecoder final : public VideoDecoder {
public:
    explicit AppleVideoDecoder(VideoCodec codec) : codec_(codec) {}
    ~AppleVideoDecoder() override
    {
        if (session_.get()) {
            VTDecompressionSessionWaitForAsynchronousFrames(session_.get());
            VTDecompressionSessionInvalidate(session_.get());
        }
    }

    void submit(std::span<const std::uint8_t> bytes, std::uint64_t timestamp) override
    {
        if (codec_ == VideoCodec::hevc) {
            readHevc(bytes, timestamp, true);
            return;
        }
        if (codec_ == VideoCodec::h264) {
            readH264(bytes, timestamp, true);
            return;
        }
        const auto packet = prepare(bytes);
        if (packet.empty()) return;
        if (!session_.get()) throw std::runtime_error("video picture precedes its parameter sets");
        Picture picture {timestamp, {}, layout_ == VideoPixelLayout::planar8Bit, {}};
        decode(packet, picture);
        for (auto& frame : picture.frames) emit(std::move(frame));
    }

    /* Everything `submit` does before the picture reaches the hardware:
     * parse the packet's headers, adopt what they declare, and build or
     * rebuild the decompression session when that changed or when a random
     * access point lets the requested layout take effect for free. A caller
     * that needs the picture size before it can choose a layout runs this
     * alone, then submits the same packet, which repeats the parse without
     * repeating its effect. */
    void inspect(std::span<const std::uint8_t> bytes) override
    {
        if (codec_ == VideoCodec::hevc) readHevc(bytes, 0, false);
        else if (codec_ == VideoCodec::h264) readH264(bytes, 0, false);
        else prepare(bytes);
    }

    void drain() override
    {
        finishSession();
        releaseWaiting();
    }

    /* A seek: nothing decoded so far is shown and no reference survives, but
     * the parameter sets and the format they described do, because a client
     * that seeks does not send them again. The session is built anew from them
     * with the first picture that follows. */
    void discard() override
    {
        if (session_.get()) VTDecompressionSessionWaitForAsynchronousFrames(session_.get());
        {
            std::lock_guard lock(mutex_);
            frames_.clear();
            callbackError_.clear();
        }
        waiting_.clear();
        order_.clear();
        hevcOrder_.reset();
        h264Order_.reset();
        if (session_.get()) VTDecompressionSessionInvalidate(session_.get());
        session_.reset();
    }

    std::vector<DecodedVideoFrame> takeFrames() override
    {
        std::lock_guard lock(mutex_);
        std::vector<DecodedVideoFrame> result;
        result.swap(frames_);
        return result;
    }
    VideoFormat format() const noexcept override { return format_; }
    unsigned reorderDepth() const noexcept override
    {
        if (!format_.width) return 0;
        if (codec_ == VideoCodec::hevc) return hevcActive_.maxNumReorder;
        if (codec_ == VideoCodec::h264) return h264Active_.reorderFrames();
        return 0;
    }

    /* A client negotiates its capture layout only after the decoder has seen
     * the stream's headers, which is the middle of a group of pictures. A
     * decompression session cannot change its output layout, and restarting
     * one there throws away the reference frames the next picture needs, which
     * the hardware reports as kVTVideoDecoderBadDataErr. The request is
     * therefore recorded and applied at the next random access point, where a
     * restart costs nothing; until then the decoder keeps producing its
     * current layout and the caller converts, which every frame states. */
    void requestOutputLayout(VideoPixelLayout layout) override
    {
        if (layout == layout_) return;
        layout_ = layout;
        if (!description_.get()) return;
        if (!session_.get()) configure();
    }

private:
    // What the session itself produces. Narrowing a 10-bit picture to 8 bits
    // is done as it is copied, so asking for it needs no new session.
    [[nodiscard]] VideoPixelLayout wantedLayout() const noexcept
    {
        return layout_ == VideoPixelLayout::packedBgra ? VideoPixelLayout::packedBgra : VideoPixelLayout::planarYuv;
    }

    static void checkPacketSize(std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty() || bytes.size() > 8 * 1024 * 1024)
            throw std::runtime_error("video packet size is outside the decoder limit");
    }

    std::vector<std::uint8_t> prepare(std::span<const std::uint8_t> bytes)
    {
        checkPacketSize(bytes);
        std::vector<std::uint8_t> packet;
        bool changed = false;
        // A point where restarting the session loses nothing, because the
        // picture that follows needs no earlier frame.
        bool randomAccess = false;
        if (codec_ == VideoCodec::jpeg) {
            const auto next = jpegFormat(bytes);
            changed = next != format_;
            format_ = next;
            randomAccess = true;   // every JPEG frame stands alone
            packet.assign(bytes.begin(), bytes.end());
        } else if (codec_ == VideoCodec::vp9) {
            // A superframe reaches the decoder whole; its first frame's
            // uncompressed header is the one that can resize the stream.
            if (const auto key = vp9KeyFrame(bytes)) {
                if (key->profile != 0) throw std::runtime_error("unsupported VP9 profile");
                changed = key->format != format_ || key->colorSpace != vp9ColorSpace_ ||
                          key->fullRange != vp9FullRange_;
                format_ = key->format;
                vp9ColorSpace_ = key->colorSpace;
                vp9FullRange_ = key->fullRange;
                randomAccess = true;
            } else if (!description_.get()) {
                throw std::runtime_error("VP9 stream does not start with a key frame");
            }
            packet.assign(bytes.begin(), bytes.end());
        }
        if (changed || !description_.get() || !session_.get() ||
            (randomAccess && wantedLayout() != sessionLayout_))
            configure();
        return packet;
    }

    /* An HEVC packet: its parameter sets are kept, and each picture it carries
     * is decoded on its own, so that the order pictures are shown in can be
     * worked out per picture. With `decodePictures` unset only the headers are
     * read. */
    struct HevcPicture {
        hevc::SliceStart start;
        std::vector<std::uint8_t> packet;
        bool open {};
    };

    void readHevc(std::span<const std::uint8_t> bytes, std::uint64_t timestamp, bool decodePictures)
    {
        checkPacketSize(bytes);
        HevcPicture current;
        bool sized = false;
        const auto finish = [&] {
            if (!current.open) return;
            current.open = false;
            if (decodePictures) {
                decodeHevc(current, timestamp);
            } else if (!sized) {
                adoptHevc(current.start.spsId);
                sized = true;
            }
        };
        for (const auto unit : annexB(bytes)) {
            // A decoder of the base layer ignores the other layers.
            if (unit.size() < 2 || hevc::layerId(unit) != 0) continue;
            const unsigned type = hevc::nalType(unit);
            if (type >= hevc::nalVps && type <= hevc::nalPps) {
                // Pictures before a parameter set are decoded with the sets they were coded for.
                finish();
                switch (hevcSets_.store(unit)) {
                case hevc::ParameterSets::Result::rejected: throw std::runtime_error("invalid HEVC parameter set");
                case hevc::ParameterSets::Result::changed: hevcChanged_ = true; break;
                case hevc::ParameterSets::Result::unchanged: break;
                }
                // A stream that states its parameter sets and no picture is
                // still described by the last SPS it stated.
                if (type == hevc::nalSps) adoptHevc(hevc::parseSps(unit)->id);
                continue;
            }
            if (type == hevc::nalEndOfSequence || type == hevc::nalEndOfBitstream) {
                finish();
                if (decodePictures) {
                    hevcOrder_.endSequence();
                    releaseWaiting();
                }
                continue;
            }
            if (!hevc::isPicture(type)) continue;
            const auto start = hevc::parseSliceStart(unit, hevcSets_);
            if (!start) throw std::runtime_error("video picture precedes its parameter sets");
            if (start->firstInPicture) {
                finish();
                current = {*start, {}, true};
            } else if (!current.open) {
                throw std::runtime_error("HEVC slice segment without its first segment");
            }
            const auto size = std::uint32_t(unit.size());
            current.packet.insert(current.packet.end(), {std::uint8_t(size >> 24), std::uint8_t(size >> 16),
                                                         std::uint8_t(size >> 8), std::uint8_t(size)});
            current.packet.insert(current.packet.end(), unit.begin(), unit.end());
        }
        finish();
    }

    // The size and depth of the pictures an SPS describes are the stream's.
    void adoptHevc(unsigned spsId)
    {
        const auto* sps = hevcSets_.sps(spsId);
        if (!sps) return;
        if ((sps->profile != 1 && sps->profile != 2) || sps->chromaFormat != 1 ||
            (sps->bitDepth != 8 && sps->bitDepth != 10))
            throw std::runtime_error("unsupported HEVC profile");
        format_ = {sps->visibleWidth(), sps->visibleHeight(), sps->bitDepth};
        hevcActive_ = *sps;
    }

    void decodeHevc(const HevcPicture& picture, std::uint64_t timestamp)
    {
        adoptHevc(picture.start.spsId);
        const auto sps = *hevcSets_.sps(picture.start.spsId);
        const auto verdict = hevcOrder_.next(picture.start, sps);
        // Nothing decoded before a new sequence is shown after a picture of it.
        if (verdict.startsSequence) releaseWaiting();
        if (!verdict.decode) return;
        const bool randomAccess = hevc::isIrap(picture.start.type);
        if (hevcChanged_ || !description_.get() || !session_.get() ||
            (randomAccess && wantedLayout() != sessionLayout_))
            configure();
        if (!session_.get()) throw std::runtime_error("video picture precedes its parameter sets");
        Picture decoded {timestamp, {}, layout_ == VideoPixelLayout::planar8Bit, {}};
        // VideoToolbox honors only part of the conformance window, so the
        // session decodes the whole coded picture and the copy is cut here.
        if (sps.hasWindow)
            decoded.crop = {sps.window.left, sps.window.top, sps.visibleWidth(), sps.visibleHeight(), true};
        decode(picture.packet, decoded);
        if (!verdict.output) return;   // decoded because later pictures refer to it
        std::vector<std::uint64_t> released;
        for (auto& frame : decoded.frames) {
            const auto id = ++sequence_;
            waiting_.emplace(id, std::move(frame));
            order_.add(id, verdict.poc, sps.maxNumReorder, sps.maxLatencyIncreasePlus1, released);
        }
        release(released);
    }

    /* An H.264 packet: its parameter sets are kept, and each picture it carries
     * is decoded on its own, so that the order pictures are shown in can be
     * worked out per picture. With `decodePictures` unset only the headers are
     * read. */
    struct H264Picture {
        h264::SliceHeader first;
        std::vector<std::uint8_t> packet;
        bool open {};
    };

    void readH264(std::span<const std::uint8_t> bytes, std::uint64_t timestamp, bool decodePictures)
    {
        checkPacketSize(bytes);
        H264Picture current;
        bool sized = false;
        const auto finish = [&] {
            if (!current.open) return;
            current.open = false;
            if (decodePictures) {
                decodeH264(current, timestamp);
            } else if (!sized) {
                adoptH264(current.first.spsId);
                sized = true;
            }
        };
        for (const auto unit : annexB(bytes)) {
            if (unit.size() < 2) continue;
            const unsigned type = h264::nalType(unit);
            if (type == h264::nalSps || type == h264::nalPps) {
                // Pictures before a parameter set are decoded with the sets they were coded for.
                finish();
                switch (h264Sets_.store(unit)) {
                case h264::ParameterSets::Result::rejected: throw std::runtime_error("invalid H.264 parameter set");
                case h264::ParameterSets::Result::changed: h264Changed_ = true; break;
                case h264::ParameterSets::Result::unchanged: break;
                }
                // A stream that states its parameter sets and no picture is
                // still described by the last SPS it stated.
                if (type == h264::nalSps) adoptH264(h264::parseSps(unit)->id);
                continue;
            }
            if (!h264::isPicture(type)) {
                // These begin an access unit: the picture before them is complete.
                if (type == h264::nalSei || type == h264::nalAud || type == h264::nalEndOfSequence ||
                    type == h264::nalEndOfStream)
                    finish();
                continue;
            }
            const auto slice = h264::parseSliceHeader(unit, h264Sets_);
            if (!slice) throw std::runtime_error("video picture precedes its parameter sets");
            // A redundant slice repeats a primary one the decoder already has.
            if (slice->redundantPicCnt) continue;
            if (current.open && h264::startsPicture(current.first, *slice)) finish();
            if (!current.open) current = {*slice, {}, true};
            const auto size = std::uint32_t(unit.size());
            current.packet.insert(current.packet.end(), {std::uint8_t(size >> 24), std::uint8_t(size >> 16),
                                                         std::uint8_t(size >> 8), std::uint8_t(size)});
            current.packet.insert(current.packet.end(), unit.begin(), unit.end());
        }
        finish();
    }

    // The size and depth of the pictures an SPS describes are the stream's.
    void adoptH264(unsigned spsId)
    {
        const auto* sps = h264Sets_.sps(spsId);
        if (!sps) return;
        if ((sps->profile != 66 && sps->profile != 77 && sps->profile != 100 && sps->profile != 110) ||
            sps->chromaFormat != 1 || (sps->bitDepth != 8 && sps->bitDepth != 10))
            throw std::runtime_error("unsupported H.264 profile");
        format_ = {sps->visibleWidth(), sps->visibleHeight(), sps->bitDepth};
        h264Active_ = *sps;
    }

    void decodeH264(const H264Picture& picture, std::uint64_t timestamp)
    {
        adoptH264(picture.first.spsId);
        const auto sps = *h264Sets_.sps(picture.first.spsId);
        const bool randomAccess = picture.first.idr();
        if (h264Changed_ || !description_.get() || !session_.get() ||
            (randomAccess && wantedLayout() != sessionLayout_))
            configure();
        if (!session_.get()) throw std::runtime_error("video picture precedes its parameter sets");
        Picture decoded {timestamp, {}, layout_ == VideoPixelLayout::planar8Bit, {}};
        // The session decodes the whole coded picture and the copy is cut here.
        if (sps.hasWindow)
            decoded.crop = {sps.window.left, sps.window.top, sps.visibleWidth(), sps.visibleHeight(), true};
        decode(picture.packet, decoded);
        std::vector<std::uint64_t> released;
        // A picture the hardware returned no frame for is still a picture of
        // the stream: the buffer's account of what refers to what needs it.
        if (decoded.frames.empty()) h264Order_.add(0, picture.first, sps, released);
        for (auto& frame : decoded.frames) {
            const auto id = ++sequence_;
            waiting_.emplace(id, std::move(frame));
            h264Order_.add(id, picture.first, sps, released);
        }
        release(released);
    }

    void release(const std::vector<std::uint64_t>& ids)
    {
        for (const auto id : ids) {
            const auto found = waiting_.find(id);
            if (found == waiting_.end()) continue;
            emit(std::move(found->second));
            waiting_.erase(found);
        }
    }

    // Every picture that waits for a lower count to be shown leaves now.
    void releaseWaiting()
    {
        std::vector<std::uint64_t> released;
        if (codec_ == VideoCodec::hevc) order_.flush(released);
        if (codec_ == VideoCodec::h264) h264Order_.flush(released);
        release(released);
    }

    /* The hardware returns every picture it has been given. Nothing is held
     * back inside it, and what the stream's order holds back is not its to
     * return, so this changes nothing that is waiting. */
    void finishSession()
    {
        if (!session_.get()) return;
        checked(VTDecompressionSessionFinishDelayedFrames(session_.get()), "drain video decoder");
        checked(VTDecompressionSessionWaitForAsynchronousFrames(session_.get()), "wait for video drain");
        checkCallback();
    }

    void emit(DecodedVideoFrame frame)
    {
        std::lock_guard lock(mutex_);
        if (frames_.size() >= 32) throw std::runtime_error("decoded video queue is full");
        frames_.push_back(std::move(frame));
    }

    void decode(const std::vector<std::uint8_t>& packet, Picture& picture)
    {
        CFObject<CMBlockBufferRef> block;
        checked(CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, packet.size(),
            kCFAllocatorDefault, nullptr, 0, packet.size(), 0, block.address()), "allocate video packet");
        checked(CMBlockBufferReplaceDataBytes(packet.data(), block.get(), 0, packet.size()), "copy video packet");
        CFObject<CMSampleBufferRef> sample;
        const std::size_t size = packet.size();
        const CMSampleTimingInfo timing {kCMTimeInvalid, CMTimeMake(std::int64_t(++sequence_), 1000), kCMTimeInvalid};
        checked(CMSampleBufferCreateReady(kCFAllocatorDefault, block.get(), description_.get(),
            1, 1, &timing, 1, &size, sample.address()), "create video sample");
        /* The picture is the callback's to fill until the session has
         * returned everything it was given, even when decoding fails. */
        struct Settle {
            VTDecompressionSessionRef session;
            ~Settle() { VTDecompressionSessionWaitForAsynchronousFrames(session); }
        } settle {session_.get()};
        /* No temporal processing: the hardware returns each picture as it
         * decodes it, and the order pictures are shown in is worked out here,
         * from the stream, not from timestamps a client may not set. */
        checked(VTDecompressionSessionDecodeFrame(session_.get(), sample.get(), 0, &picture, nullptr),
                "decode video packet");
        checked(VTDecompressionSessionWaitForAsynchronousFrames(session_.get()), "wait for decoded video");
        checkCallback();
    }

    void configure()
    {
        if (codec_ == VideoCodec::h264 && !h264Sets_.complete()) return;
        if (codec_ == VideoCodec::hevc && !hevcSets_.complete()) return;
        /* Building a session anew does not end the stream: what waits for the
         * pictures before it keeps waiting. */
        finishSession();
        if (session_.get()) VTDecompressionSessionInvalidate(session_.get());
        session_.reset();
        hevcChanged_ = false;
        h264Changed_ = false;
        if (codec_ == VideoCodec::jpeg) {
            checked(CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_JPEG,
                int(format_.width), int(format_.height), nullptr, description_.address()), "describe JPEG");
        } else if (codec_ == VideoCodec::vp9) {
            NSDictionary* extensions = @{
                (__bridge NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms:
                    @{@"vpcC": vp9Configuration(0, format_.bitDepth, vp9ColorSpace_, vp9FullRange_)},
                (__bridge NSString*)kCMFormatDescriptionExtension_FullRangeVideo: @(vp9FullRange_),
            };
            checked(CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_VP9,
                int(format_.width), int(format_.height), (__bridge CFDictionaryRef)extensions,
                description_.address()), "describe VP9");
        } else if (codec_ == VideoCodec::h264) {
            if (h264Active_.profile == 110 && !high10Available())
                throw std::runtime_error("H.264 High10 hardware is unavailable on this host");
            const auto units = h264Sets_.descriptionUnits();
            std::vector<const std::uint8_t*> data;
            std::vector<std::size_t> sizes;
            for (const auto& unit : units) { data.push_back(unit.data()); sizes.push_back(unit.size()); }
            checked(CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
                units.size(), data.data(), sizes.data(), 4, description_.address()), "describe H.264");
        } else {
            const auto units = hevcSets_.descriptionUnits();
            std::vector<const std::uint8_t*> data;
            std::vector<std::size_t> sizes;
            for (const auto& unit : units) { data.push_back(unit.data()); sizes.push_back(unit.size()); }
            checked(CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault,
                units.size(), data.data(), sizes.data(), 4, nullptr, description_.address()), "describe HEVC");
        }
        /* What the hardware decodes is the coded picture: the window an HEVC
         * or H.264 stream states is cut from it afterwards. */
        const auto codedWidth = codec_ == VideoCodec::hevc ? hevcActive_.width
            : codec_ == VideoCodec::h264 ? h264Active_.width() : format_.width;
        const auto codedHeight = codec_ == VideoCodec::hevc ? hevcActive_.height
            : codec_ == VideoCodec::h264 ? h264Active_.height() : format_.height;
        const auto limit = decodeSizeLimit(codec_);
        if (!codedWidth || !codedHeight || codedWidth > limit.width || codedHeight > limit.height)
            throw std::runtime_error("video dimensions exceed decoder capabilities");
        /* A stream that states full range is decoded to full range: asking
         * for video range would have the hardware squeeze every sample. */
        bool fullRange = false;
        if (codec_ == VideoCodec::vp9) {
            fullRange = vp9FullRange_;
        } else if (codec_ != VideoCodec::jpeg) {
            const auto value = CMFormatDescriptionGetExtension(description_.get(),
                                                               kCMFormatDescriptionExtension_FullRangeVideo);
            fullRange = value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(CFBooleanRef(value));
        }
        const OSType planarFormat = format_.bitDepth <= 8
            ? (fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
            : (fullRange ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
        NSDictionary* specification = @{(__bridge NSString*)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES};
        VTDecompressionOutputCallbackRecord callback {output, this};
        const auto create = [&](OSType pixelFormat) {
            NSDictionary* attributes = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(pixelFormat),
                                        (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}};
            return VTDecompressionSessionCreate(kCFAllocatorDefault, description_.get(),
                (__bridge CFDictionaryRef)specification, (__bridge CFDictionaryRef)attributes,
                &callback, session_.address());
        };
        /* The hardware converts to packed BGRA as part of decoding, which
         * is why a client that draws through an RGB surface asks for it.
         * A decoder that will not emit it still decodes, in its own
         * layout, and the frame reports which layout it carries. */
        sessionLayout_ = wantedLayout();
        if (sessionLayout_ == VideoPixelLayout::packedBgra && create(kCVPixelFormatType_32BGRA) != noErr)
            session_.reset();
        if (!session_.get()) {
            sessionLayout_ = VideoPixelLayout::planarYuv;
            checked(create(planarFormat), "create hardware video decoder");
        }
        CFObject<CFTypeRef> hardware;
        const auto status = VTSessionCopyProperty(session_.get(),
            kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
            kCFAllocatorDefault, hardware.address());
        // Apple's JPEG decoder honors the required-hardware creation flag but
        // does not implement this property, as in the existing Virgl backend.
        if (!((codec_ == VideoCodec::jpeg || codec_ == VideoCodec::vp9) &&
              status == kVTPropertyNotSupportedErr)) {
            checked(status, "verify hardware video decoder");
            if (hardware.get() != kCFBooleanTrue)
                throw std::runtime_error("video decoder did not select hardware");
        }
    }

    static void output(void* context, void* source, OSStatus status, VTDecodeInfoFlags,
                       CVImageBufferRef image, CMTime, CMTime) noexcept
    {
        auto& self = *static_cast<AppleVideoDecoder*>(context);
        auto& picture = *static_cast<Picture*>(source);
        std::lock_guard lock(self.mutex_);
        try {
            checked(status, "hardware video output");
            if (!image) return;
            picture.frames.push_back(copyPicture(image, picture, picture.timestamp));
        } catch (const std::exception& error) {
            self.callbackError_ = error.what();
        } catch (...) {
            self.callbackError_ = "hardware video output failed";
        }
    }
    void checkCallback()
    {
        std::lock_guard lock(mutex_);
        if (!callbackError_.empty()) throw std::runtime_error(callbackError_);
    }

    VideoCodec codec_;
    VideoFormat format_;
    VideoPixelLayout layout_ {VideoPixelLayout::planarYuv};
    VideoPixelLayout sessionLayout_ {VideoPixelLayout::planarYuv};
    unsigned vp9ColorSpace_ {};
    bool vp9FullRange_ {};
    // H.264: the stream's parameter sets, the SPS in use, and the order its
    // frames are shown in.
    h264::ParameterSets h264Sets_;
    h264::Sps h264Active_;
    bool h264Changed_ {};
    h264::OutputOrder h264Order_;
    // HEVC: the stream's parameter sets, the one in use, and the order its
    // pictures are shown in. Decoded pictures of either codec wait here, by
    // number, for the pictures that precede them.
    hevc::ParameterSets hevcSets_;
    hevc::Sps hevcActive_;
    bool hevcChanged_ {};
    hevc::PictureOrder hevcOrder_;
    hevc::ReorderBuffer order_;
    std::map<hevc::ReorderBuffer::Id, DecodedVideoFrame> waiting_;
    std::uint64_t sequence_ {};
    CFObject<CMVideoFormatDescriptionRef> description_;
    CFObject<VTDecompressionSessionRef> session_;
    std::mutex mutex_;
    std::vector<DecodedVideoFrame> frames_;
    std::string callbackError_;
};

} // namespace

std::vector<VideoDecodeCapability> videoDecodeCapabilities()
{
    std::vector<VideoDecodeCapability> result;
    if (VTIsHardwareDecodeSupported(kCMVideoCodecType_H264)) {
        // Level 5.2 is the last whose frames the hardware opens in full: it
        // decodes a level 5.2, 6.0 or 6.2 stream of 4096 x 2304 alike, and
        // refuses the 8192 x 4320 frame a level 6 stream may carry. The
        // platform test decodes the level 5.2 frame. Its size range is wider
        // than the level's frame area, because the hardware limits a
        // dimension, not the area (see decodeSizeLimit).
        const auto h264 = decodeSizeLimit(VideoCodec::h264);
        for (auto profile : {VideoProfile::h264Baseline, VideoProfile::h264ConstrainedBaseline,
                             VideoProfile::h264Main, VideoProfile::h264High,
                             VideoProfile::h264ConstrainedHigh})
            result.push_back({VideoCodec::h264, profile, h264.width, h264.height, 52, 8});
        if (high10Available())
            result.push_back({VideoCodec::h264, VideoProfile::h264High10, h264.width, h264.height, 51, 10});
    }
    if (VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC)) {
        const auto hevc = decodeSizeLimit(VideoCodec::hevc);
        result.push_back({VideoCodec::hevc, VideoProfile::hevcMain, hevc.width, hevc.height, 186, 8});
        result.push_back({VideoCodec::hevc, VideoProfile::hevcMain10, hevc.width, hevc.height, 186, 10});
    }
    if (VTIsHardwareDecodeSupported(kCMVideoCodecType_JPEG))
        result.push_back({VideoCodec::jpeg, VideoProfile::jpegBaseline,
                          decodeSizeLimit(VideoCodec::jpeg).width, decodeSizeLimit(VideoCodec::jpeg).height, 0, 8});
    if (vp9Available())
        result.push_back({VideoCodec::vp9, VideoProfile::vp9Profile0,
                          decodeSizeLimit(VideoCodec::vp9).width, decodeSizeLimit(VideoCodec::vp9).height, 0, 8});
    return result;
}

std::unique_ptr<VideoDecoder> makeVideoDecoder(VideoCodec codec)
{
    const auto capabilities = videoDecodeCapabilities();
    if (std::none_of(capabilities.begin(), capabilities.end(),
                    [codec](const auto& cap) { return cap.codec == codec; }))
        throw std::runtime_error("requested video hardware decoder is unavailable");
    return std::make_unique<AppleVideoDecoder>(codec);
}

} // namespace uml::platform
