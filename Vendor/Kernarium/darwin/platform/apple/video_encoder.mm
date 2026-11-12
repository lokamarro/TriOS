// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_encoder.hpp"
#include "video_encoder_bitstream.hpp"
#include "video_encoder_internal.hpp"
#include "video_encoder_levels.hpp"
#include "descriptor.hpp"
#if UML_VIDEO_ENCODER_TESTING
#include "video_encoder_testing.hpp"
#endif

#import <Accelerate/Accelerate.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/* The Apple backend of the platform video encoder.
 *
 * Every raw frame is written once, by the caller, into a pixel buffer from the
 * session's own pool, so the hardware reads memory it can map without a second
 * copy. The coded sample VideoToolbox returns is converted to Annex B on its
 * output thread and queued for the caller, who is woken through a descriptor.
 *
 * Nothing here holds the state lock across a VideoToolbox call. The output
 * callback runs on a VideoToolbox thread and takes the lock only to look up the
 * frame it answers and to queue the result; the caller's thread takes it for
 * the same short sections. */

namespace uml::platform::apple_encoder {
namespace {

using bitstream::Bytes;

constexpr CMTimeScale kTimescale = 2'400'000;   // one frame of every common rate is a whole number of ticks
constexpr unsigned kMaximumInFlight = 4;
constexpr unsigned kMaximumInFlightReordered = 6;   // VideoToolbox holds up to three frames to reorder
constexpr unsigned kPoolSlack = 4;                  // VideoToolbox releases a buffer a moment after its output
constexpr std::uint64_t kDefaultBitrate = 2'000'000;
constexpr unsigned kDefaultQuality = 50;
constexpr unsigned kDefaultJpegQuality = 85;
constexpr unsigned kDrainWaitMilliseconds = 5000;
constexpr std::int32_t kNoKeyFrameInterval = std::numeric_limits<std::int32_t>::max();

template<class T> class CFRef {
public:
    CFRef() = default;
    // Adopts a reference a CF "Create" or "Copy" function already retained.
    static CFRef adopt(T value) noexcept
    {
        CFRef reference;
        reference.value_ = value;
        return reference;
    }
    static CFRef retain(T value)
    {
        CFRef reference;
        reference.value_ = value ? static_cast<T>(const_cast<void*>(CFRetain(value))) : nullptr;
        return reference;
    }
    CFRef(const CFRef& other) : value_(other.value_ ? static_cast<T>(const_cast<void*>(CFRetain(other.value_))) : nullptr) {}
    CFRef(CFRef&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    CFRef& operator=(CFRef other) noexcept
    {
        std::swap(value_, other.value_);
        return *this;
    }
    ~CFRef() { reset(); }
    [[nodiscard]] T get() const noexcept { return value_; }
    [[nodiscard]] T* address() { reset(); return &value_; }
    void reset() noexcept
    {
        if (value_) CFRelease(value_);
        value_ = nullptr;
    }
    explicit operator bool() const noexcept { return value_ != nullptr; }
private:
    T value_ {};
};

bool sessionLoss(OSStatus status) noexcept
{
    return status == kVTInvalidSessionErr || status == kVTVideoEncoderMalfunctionErr ||
           status == kVTVideoEncoderNotAvailableNowErr;
}

// ---- The tables that map interface values to CoreVideo and VideoToolbox.

struct LayoutInfo {
    OSType limitedRange;
    OSType fullRange;
    unsigned planes;
    // The caller's bytes are R, G, B, A and CoreVideo has no buffer of that
    // order it can back with an IOSurface, so the buffer is BGRA and the
    // encoder swaps red and blue in place when the frame is submitted.
    bool swapsRedBlue;
    // One plane of 8-bit RGB. VideoToolbox converts it to YCbCr itself, and
    // always to video range; a stream that asks for full range is converted
    // here instead.
    bool packedRgb;
};

LayoutInfo layoutInfo(RawVideoLayout layout) noexcept
{
    switch (layout) {
    case RawVideoLayout::nv12: return {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 2, false, false};
    case RawVideoLayout::i420: return {kCVPixelFormatType_420YpCbCr8Planar, kCVPixelFormatType_420YpCbCr8PlanarFullRange, 3, false, false};
    case RawVideoLayout::p010: return {kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr10BiPlanarFullRange, 2, false, false};
    case RawVideoLayout::nv16: return {kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_422YpCbCr8BiPlanarFullRange, 2, false, false};
    case RawVideoLayout::nv24: return {kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_444YpCbCr8BiPlanarFullRange, 2, false, false};
    case RawVideoLayout::bgra: return {kCVPixelFormatType_32BGRA, kCVPixelFormatType_32BGRA, 1, false, true};
    case RawVideoLayout::rgba: return {kCVPixelFormatType_32BGRA, kCVPixelFormatType_32BGRA, 1, true, true};
    }
    return {0, 0, 0, false, false};
}

CMVideoCodecType codecType(VideoCodec codec) noexcept
{
    switch (codec) {
    case VideoCodec::h264: return kCMVideoCodecType_H264;
    case VideoCodec::hevc: return kCMVideoCodecType_HEVC;
    case VideoCodec::jpeg: return kCMVideoCodecType_JPEG;
    case VideoCodec::vp9: break;
    }
    return 0;
}

VideoCodec profileCodec(VideoEncodeProfile profile) noexcept
{
    switch (profile) {
    case VideoEncodeProfile::h264Baseline:
    case VideoEncodeProfile::h264ConstrainedBaseline:
    case VideoEncodeProfile::h264Main:
    case VideoEncodeProfile::h264High:
    case VideoEncodeProfile::h264ConstrainedHigh:
    case VideoEncodeProfile::h264High422:
    case VideoEncodeProfile::h264High444Predictive: return VideoCodec::h264;
    case VideoEncodeProfile::hevcMain:
    case VideoEncodeProfile::hevcMain10:
    case VideoEncodeProfile::hevcMainStill: return VideoCodec::hevc;
    case VideoEncodeProfile::jpegBaseline: return VideoCodec::jpeg;
    }
    return VideoCodec::vp9;
}

NSString* colorPrimaries(VideoColorPrimaries value)
{
    switch (value) {
    case VideoColorPrimaries::bt709: return (__bridge NSString*)kCVImageBufferColorPrimaries_ITU_R_709_2;
    case VideoColorPrimaries::bt601Pal: return (__bridge NSString*)kCVImageBufferColorPrimaries_EBU_3213;
    case VideoColorPrimaries::bt601Ntsc: return (__bridge NSString*)kCVImageBufferColorPrimaries_SMPTE_C;
    case VideoColorPrimaries::bt2020: return (__bridge NSString*)kCVImageBufferColorPrimaries_ITU_R_2020;
    case VideoColorPrimaries::p3D65: return (__bridge NSString*)kCVImageBufferColorPrimaries_P3_D65;
    case VideoColorPrimaries::unspecified: break;
    }
    return nil;
}

NSString* transferFunction(VideoTransferFunction value)
{
    switch (value) {
    case VideoTransferFunction::bt709: return (__bridge NSString*)kCVImageBufferTransferFunction_ITU_R_709_2;
    case VideoTransferFunction::srgb: return (__bridge NSString*)kCVImageBufferTransferFunction_sRGB;
    case VideoTransferFunction::pq: return (__bridge NSString*)kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
    case VideoTransferFunction::hlg: return (__bridge NSString*)kCVImageBufferTransferFunction_ITU_R_2100_HLG;
    case VideoTransferFunction::linear: return (__bridge NSString*)kCVImageBufferTransferFunction_Linear;
    case VideoTransferFunction::unspecified: break;
    }
    return nil;
}

NSString* matrixCoefficients(VideoMatrixCoefficients value)
{
    switch (value) {
    case VideoMatrixCoefficients::bt601: return (__bridge NSString*)kCVImageBufferYCbCrMatrix_ITU_R_601_4;
    case VideoMatrixCoefficients::bt709: return (__bridge NSString*)kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    case VideoMatrixCoefficients::bt2020: return (__bridge NSString*)kCVImageBufferYCbCrMatrix_ITU_R_2020;
    case VideoMatrixCoefficients::unspecified: break;
    }
    return nil;
}

// ---- Levels. The tables, the level a stream needs and the rewriting of the
// level in its parameter sets are in video_encoder_levels.hpp.

using levels::h264RequiredLevel;

// The ProfileLevel value of a profile at an explicit level, or nil where
// VideoToolbox names none and the encoder has to choose.
NSString* h264ExplicitLevel(VideoEncodeProfile profile, unsigned level)
{
#define UML_H264_LEVELS(NAME) \
    case 30: return (__bridge NSString*)kVTProfileLevel_H264_##NAME##_3_0; \
    case 31: return (__bridge NSString*)kVTProfileLevel_H264_##NAME##_3_1; \
    case 32: return (__bridge NSString*)kVTProfileLevel_H264_##NAME##_3_2; \
    case 40: return (__bridge NSString*)kVTProfileLevel_H264_##NAME##_4_0; \
    case 41: return (__bridge NSString*)kVTProfileLevel_H264_##NAME##_4_1; \
    case 42: return (__bridge NSString*)kVTProfileLevel_H264_##NAME##_4_2; \
    case 50: return (__bridge NSString*)kVTProfileLevel_H264_##NAME##_5_0; \
    case 51: return (__bridge NSString*)kVTProfileLevel_H264_##NAME##_5_1; \
    case 52: return (__bridge NSString*)kVTProfileLevel_H264_##NAME##_5_2;
    switch (profile) {
    case VideoEncodeProfile::h264Baseline:
        switch (level) {
        case 13: return (__bridge NSString*)kVTProfileLevel_H264_Baseline_1_3;
        UML_H264_LEVELS(Baseline)
        default: return nil;
        }
    case VideoEncodeProfile::h264Main:
        switch (level) {
        UML_H264_LEVELS(Main)
        default: return nil;
        }
    case VideoEncodeProfile::h264High:
        switch (level) {
        UML_H264_LEVELS(High)
        default: return nil;
        }
    default: return nil;
    }
#undef UML_H264_LEVELS
}

NSString* autoLevelValue(VideoEncodeProfile profile)
{
    switch (profile) {
    case VideoEncodeProfile::h264Baseline: return (__bridge NSString*)kVTProfileLevel_H264_Baseline_AutoLevel;
    case VideoEncodeProfile::h264ConstrainedBaseline: return (__bridge NSString*)kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel;
    case VideoEncodeProfile::h264Main: return (__bridge NSString*)kVTProfileLevel_H264_Main_AutoLevel;
    case VideoEncodeProfile::h264High: return (__bridge NSString*)kVTProfileLevel_H264_High_AutoLevel;
    case VideoEncodeProfile::h264ConstrainedHigh: return (__bridge NSString*)kVTProfileLevel_H264_ConstrainedHigh_AutoLevel;
    // VideoToolbox lists these values in the property's own SupportedValueList
    // but declares no constant for them; discovery only keeps a profile whose
    // value the encoder listed and then wrote into a matching stream.
    case VideoEncodeProfile::h264High422: return @"H264_High422_AutoLevel";
    case VideoEncodeProfile::h264High444Predictive: return @"H264_High444Predictive_AutoLevel";
    case VideoEncodeProfile::hevcMain: return (__bridge NSString*)kVTProfileLevel_HEVC_Main_AutoLevel;
    case VideoEncodeProfile::hevcMain10: return (__bridge NSString*)kVTProfileLevel_HEVC_Main10_AutoLevel;
    case VideoEncodeProfile::hevcMainStill: return @"HEVC_MainStill_AutoLevel";
    case VideoEncodeProfile::jpegBaseline: break;
    }
    return nil;
}

// ---- RGB to full-range YCbCr.

struct KrKb {
    float kr, kb;
};

// The luma weights of a matrix. An unstated matrix is what a decoder assumes
// for the picture size: BT.709 from 720 lines up, BT.601 below.
KrKb lumaWeights(VideoMatrixCoefficients matrix, std::uint32_t height) noexcept
{
    switch (matrix) {
    case VideoMatrixCoefficients::bt601: return {0.299f, 0.114f};
    case VideoMatrixCoefficients::bt709: return {0.2126f, 0.0722f};
    case VideoMatrixCoefficients::bt2020: return {0.2627f, 0.0593f};
    case VideoMatrixCoefficients::unspecified: break;
    }
    return height >= 720 ? KrKb {0.2126f, 0.0722f} : KrKb {0.299f, 0.114f};
}

// vImage's conversion from 8-bit RGB to 4:2:0 YCbCr over the full 0..255
// range, for the matrix of the settings.
bool makeFullRangeConversion(const VideoEncodeSettings& settings, vImage_ARGBToYpCbCr& conversion) noexcept
{
    const auto [kr, kb] = lumaWeights(settings.color.matrix, settings.height);
    const float kg = 1.0f - kr - kb;
    const vImage_ARGBToYpCbCrMatrix matrix {kr, kg, kb, -0.5f * kr / (1.0f - kb), -0.5f * kg / (1.0f - kb), 0.5f,
                                            -0.5f * kg / (1.0f - kr), -0.5f * kb / (1.0f - kr)};
    const vImage_YpCbCrPixelRange range {0, 128, 255, 255, 255, 0, 255, 0};
    return vImageConvert_ARGBToYpCbCr_GenerateConversion(&matrix, &range, &conversion, kvImageARGB8888, kvImage420Yp8_CbCr8,
                                                         kvImageNoFlags) == kvImageNoError;
}

// ---- A frame in flight.

struct Pending {
    std::uint64_t timestamp {};
    std::int64_t presentation {};   // kTimescale ticks
    bool forceKey {};
    CFRef<CVPixelBufferRef> pixels;   // kept until the output, so a lost session can be answered by encoding it again
};

// What turns one coded sample into Annex B frames. Immutable per session.
struct AssemblyConfig {
    VideoCodec codec {VideoCodec::h264};
    bool separateParameterSets {};
    bool parameterSetsWithEveryKeyFrame {true};
    unsigned temporalLayers {1};
    // What decides the level the parameter sets state, in place of the one
    // VideoToolbox wrote (levels::restateLevel). A zero floor leaves them as
    // written.
    levels::Policy levelPolicy;
    // The HDR10 static metadata SEI unit, with its start code, that precedes
    // the pictures of every key frame. VideoToolbox keeps the values in the
    // format description it hands the muxer, so the Annex B stream needs them
    // written here. Empty when the stream has none.
    std::vector<std::uint8_t> hdrSei;
};

// What carries from one sample to the next. Touched only by the output
// callback, which VideoToolbox delivers in order.
struct AssemblyState {
    std::vector<std::uint8_t> parameterSets;
    std::int64_t highestPresentation {std::numeric_limits<std::int64_t>::min()};
    bool keyFrameSeen {};
};

struct Shared {
    std::mutex mutex;
    std::condition_variable condition;
    WakeSocket wake {"video encoder readiness"};
    std::deque<EncodedVideoFrame> output;
    std::map<std::uint64_t, Pending> pending;   // by submission index
    // The stream: a reset or a new configuration starts another, and whatever
    // belongs to the old one (frames acquired from it, a drain waiting on it)
    // is dropped. A lost session does not end the stream.
    std::uint64_t generation {1};
    // The session whose callbacks are heard. Every new session, and every
    // detected loss, starts another, so a dying session cannot deliver into
    // the stream that replaced it.
    std::uint64_t epoch {1};
    unsigned acquired {};    // frames handed out and neither submitted nor released
    bool lost {};            // VideoToolbox reported the session gone
    unsigned lossesWithoutOutput {};
    // The live session, so that a drain which outlasts a recovery can flush
    // the session that replaced the one it started on.
    CFRef<VTCompressionSessionRef> session;
#if UML_VIDEO_ENCODER_TESTING
    std::atomic<unsigned> injectedLosses {};
#endif

    // Under the lock. The descriptor is readable exactly while `output` is not
    // empty: it is signalled on the transition and drained with the queue.
    void pushLocked(EncodedVideoFrame frame)
    {
        const bool wasEmpty = output.empty();
        output.push_back(std::move(frame));
        if (wasEmpty) (void)wake.notify();
    }
    void pushErrorLocked(std::uint64_t timestamp)
    {
        EncodedVideoFrame error;
        error.kind = EncodedVideoFrameKind::error;
        error.timestamp = timestamp;
        pushLocked(std::move(error));
    }
};

// The JFIF segment VideoToolbox leaves out of a JPEG image. It states square
// pixels with no density unit, which is what the hardware encodes.
constexpr std::uint8_t kJfifSegment[] = {0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};

class SessionContext {
public:
    SessionContext(std::shared_ptr<Shared> shared, std::uint64_t epoch, AssemblyConfig config)
        : shared_(std::move(shared)), epoch_(epoch), config_(config) {}

    static void callback(void* context, void* frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
    {
        @autoreleasepool {
            static_cast<SessionContext*>(context)->handle(std::uint64_t(reinterpret_cast<std::uintptr_t>(frame)), status, flags, sample);
        }
    }

private:
    struct Assembled {
        bool ok {};
        std::optional<EncodedVideoFrame> headers;
        EncodedVideoFrame frame;
    };

    void handle(std::uint64_t index, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
    {
        std::uint64_t timestamp = 0;
        bool alreadyLost = false;
        {
            std::lock_guard lock(shared_->mutex);
            if (epoch_ != shared_->epoch) return;
            const auto found = shared_->pending.find(index);
            if (found == shared_->pending.end()) return;
            timestamp = found->second.timestamp;
            alreadyLost = shared_->lost;
        }
#if UML_VIDEO_ENCODER_TESTING
        // One injected loss is one session lost, however many outputs the
        // dying session still delivers.
        if (status == noErr && !alreadyLost) {
            unsigned injected = shared_->injectedLosses.load();
            while (injected && !shared_->injectedLosses.compare_exchange_weak(injected, injected - 1)) {}
            if (injected) status = kVTInvalidSessionErr;
        }
#endif
        Assembled assembled;
        const bool dropped = (flags & kVTEncodeInfo_FrameDropped) != 0;
        if (status == noErr && sample && !dropped && !alreadyLost) assembled = assemble(sample, timestamp);

        std::lock_guard lock(shared_->mutex);
        if (epoch_ != shared_->epoch) return;
        const auto found = shared_->pending.find(index);
        if (found == shared_->pending.end()) return;
        // A lost session keeps its frames: the owner encodes them again on a
        // new one. Whatever the dying session still delivers is ignored, so
        // the stream does not continue after a gap.
        if (shared_->lost || (status != noErr && sessionLoss(status))) {
            shared_->lost = true;
            (void)shared_->wake.notify();
            shared_->condition.notify_all();
            return;
        }
        shared_->pending.erase(found);
        if (!assembled.ok) {
            shared_->pushErrorLocked(timestamp);
        } else {
            shared_->lossesWithoutOutput = 0;
            if (assembled.headers) shared_->pushLocked(std::move(*assembled.headers));
            shared_->pushLocked(std::move(assembled.frame));
        }
        shared_->condition.notify_all();
    }

    Assembled assemble(CMSampleBufferRef sample, std::uint64_t timestamp)
    {
        Assembled result;
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        if (!block) return result;
        const std::size_t total = CMBlockBufferGetDataLength(block);
        if (total == 0) return result;
        char* pointer = nullptr;
        std::size_t contiguous = 0;
        std::vector<std::uint8_t> copy;
        const std::uint8_t* data = nullptr;
        if (CMBlockBufferGetDataPointer(block, 0, &contiguous, nullptr, &pointer) == kCMBlockBufferNoErr && contiguous == total) {
            data = reinterpret_cast<const std::uint8_t*>(pointer);
        } else {
            copy.resize(total);
            if (CMBlockBufferCopyDataBytes(block, 0, total, copy.data()) != kCMBlockBufferNoErr) return result;
            data = copy.data();
        }

        bool key = true;
        bool dependedOn = true;
        if (CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
            attachments && CFArrayGetCount(attachments) > 0) {
            const auto attachment = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
            key = CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_NotSync) != kCFBooleanTrue;
            dependedOn = CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_IsDependedOnByOthers) != kCFBooleanFalse;
        }
        const std::int64_t presentation = CMSampleBufferGetPresentationTimeStamp(sample).value;

        EncodedVideoFrame& frame = result.frame;
        frame.timestamp = timestamp;
        frame.kind = EncodedVideoFrameKind::frame;
        if (config_.codec == VideoCodec::jpeg) {
            frame.type = VideoFrameType::key;
            frame.bytes.reserve(total + sizeof(kJfifSegment));
            const bool jfif = total >= 4 && data[2] == 0xff && data[3] == 0xe0;
            if (total < 4 || data[0] != 0xff || data[1] != 0xd8) return result;
            frame.bytes.insert(frame.bytes.end(), data, data + 2);
            if (!jfif) frame.bytes.insert(frame.bytes.end(), std::begin(kJfifSegment), std::end(kJfifSegment));
            frame.bytes.insert(frame.bytes.end(), data + 2, data + total);
            result.ok = true;
            return result;
        }

        // The parameter sets travel in the format description, not the sample.
        CMFormatDescriptionRef description = CMSampleBufferGetFormatDescription(sample);
        if (!description) return result;
        std::vector<std::uint8_t> sets;
        int lengthSize = 0;
        std::size_t count = 0;
        const bool hevc = config_.codec == VideoCodec::hevc;
        const auto setAt = [&](std::size_t at, const std::uint8_t** bytes, std::size_t* size) {
            return hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(description, at, bytes, size, &count, &lengthSize)
                        : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, at, bytes, size, &count, &lengthSize);
        };
        {
            const std::uint8_t* bytes = nullptr;
            std::size_t size = 0;
            if (setAt(0, &bytes, &size) != noErr) return result;
            std::vector<std::vector<std::uint8_t>> parameterSets;
            for (std::size_t at = 0; at < count; ++at) {
                if (setAt(at, &bytes, &size) != noErr) return result;
                parameterSets.emplace_back(bytes, bytes + size);
            }
            // VideoToolbox's level is replaced by the one the settings call for.
            if (config_.levelPolicy.floor) (void)levels::restateLevel(config_.codec, parameterSets, config_.levelPolicy);
            for (const auto& unit : parameterSets) bitstream::appendAnnexB(sets, unit.data(), unit.size());
        }
        if (lengthSize < 1 || lengthSize > 4) return result;

        std::vector<std::uint8_t> units;
        units.reserve(total + total / 8 + 16);
        for (std::size_t at = 0; at < total;) {
            if (total - at < std::size_t(lengthSize)) return result;
            std::size_t length = 0;
            for (int i = 0; i < lengthSize; ++i) length = (length << 8) | data[at + std::size_t(i)];
            at += std::size_t(lengthSize);
            if (length == 0 || length > total - at) return result;
            bitstream::appendAnnexB(units, data + at, length);
            at += length;
        }

        if (key) {
            const bool changed = state_.parameterSets != sets;
            const bool fresh = !state_.keyFrameSeen || changed;
            if (config_.separateParameterSets && fresh) {
                EncodedVideoFrame headers;
                headers.kind = EncodedVideoFrameKind::parameterSets;
                headers.type = VideoFrameType::key;
                headers.timestamp = timestamp;
                headers.bytes = sets;
                result.headers = std::move(headers);
            }
            const bool include = config_.parameterSetsWithEveryKeyFrame || (!config_.separateParameterSets && fresh);
            frame.bytes.reserve(units.size() + sets.size());
            if (include) frame.bytes = sets;
            frame.bytes.insert(frame.bytes.end(), config_.hdrSei.begin(), config_.hdrSei.end());
            frame.bytes.insert(frame.bytes.end(), units.begin(), units.end());
            state_.parameterSets = std::move(sets);
            state_.keyFrameSeen = true;
            state_.highestPresentation = presentation;
            frame.type = VideoFrameType::key;
        } else {
            frame.bytes = std::move(units);
            frame.type = presentation < state_.highestPresentation ? VideoFrameType::bidirectional : VideoFrameType::predicted;
            state_.highestPresentation = std::max(state_.highestPresentation, presentation);
            if (config_.temporalLayers > 1) frame.temporalLayer = dependedOn ? 0 : 1;
        }
        result.ok = true;
        return result;
    }

    std::shared_ptr<Shared> shared_;
    std::uint64_t epoch_;
    AssemblyConfig config_;
    AssemblyState state_;
};

// A pixel buffer from the session's pool, locked for writing. It returns the
// slot it holds when it is dropped without being submitted.
class PoolFrame final : public WritableVideoFrame {
public:
    PoolFrame(std::shared_ptr<Shared> shared, std::uint64_t generation, CFRef<CVPixelBufferRef> pixels, bool swapsRedBlue)
        : shared_(std::move(shared)), generation_(generation), pixels_(std::move(pixels)), swapsRedBlue_(swapsRedBlue)
    {
        CVPixelBufferLockBaseAddress(pixels_.get(), 0);
        locked_ = true;
        const std::size_t count = CVPixelBufferGetPlaneCount(pixels_.get());
        planes_ = count ? std::min<std::size_t>(count, planeData_.size()) : 1;
        for (std::size_t plane = 0; plane < planes_; ++plane) {
            auto& data = planeData_[plane];
            if (count) {
                data.base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels_.get(), plane));
                data.stride = std::uint32_t(CVPixelBufferGetBytesPerRowOfPlane(pixels_.get(), plane));
                data.rows = std::uint32_t(CVPixelBufferGetHeightOfPlane(pixels_.get(), plane));
            } else {
                data.base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixels_.get()));
                data.stride = std::uint32_t(CVPixelBufferGetBytesPerRow(pixels_.get()));
                data.rows = std::uint32_t(CVPixelBufferGetHeight(pixels_.get()));
            }
        }
    }
    ~PoolFrame() override
    {
        unlock();
        if (!submitted_) release();
    }

    [[nodiscard]] std::size_t planeCount() const noexcept override { return planes_; }
    [[nodiscard]] std::span<std::uint8_t> plane(std::size_t index) noexcept override
    {
        if (index >= planes_) return {};
        return {planeData_[index].base, std::size_t(planeData_[index].stride) * planeData_[index].rows};
    }
    [[nodiscard]] std::uint32_t stride(std::size_t index) const noexcept override
    {
        return index < planes_ ? planeData_[index].stride : 0;
    }
    [[nodiscard]] std::uint32_t rows(std::size_t index) const noexcept override
    {
        return index < planes_ ? planeData_[index].rows : 0;
    }

    // Hands the buffer to the encoder. False when a reset happened since the
    // frame was acquired, which makes the frame part of a discarded stream.
    [[nodiscard]] bool take(CFRef<CVPixelBufferRef>& pixels)
    {
        if (swapsRedBlue_) swapRedBlue();
        unlock();
        submitted_ = true;
        {
            std::lock_guard lock(shared_->mutex);
            if (generation_ != shared_->generation) return false;
            if (shared_->acquired) --shared_->acquired;
        }
        pixels = pixels_;
        return true;
    }

private:
    struct PlaneData {
        std::uint8_t* base {};
        std::uint32_t stride {};
        std::uint32_t rows {};
    };
    // R, G, B, A to B, G, R, A, in place. CoreVideo has no 32RGBA pixel buffer,
    // so RGBA input is carried in the BGRA pool. Untiled: the work is bound by
    // memory, and it runs on the caller's thread instead of waking a dispatch
    // pool that the emulator's scheduler does not account for.
    void swapRedBlue()
    {
        const auto& data = planeData_[0];
        vImage_Buffer buffer {data.base, data.rows, CVPixelBufferGetWidth(pixels_.get()), data.stride};
        static constexpr std::uint8_t map[4] = {2, 1, 0, 3};
        (void)vImagePermuteChannels_ARGB8888(&buffer, &buffer, map, kvImageDoNotTile);
    }
    void unlock()
    {
        if (locked_) CVPixelBufferUnlockBaseAddress(pixels_.get(), 0);
        locked_ = false;
    }
    void release()
    {
        std::lock_guard lock(shared_->mutex);
        if (generation_ == shared_->generation && shared_->acquired) --shared_->acquired;
    }

    std::shared_ptr<Shared> shared_;
    std::uint64_t generation_;
    CFRef<CVPixelBufferRef> pixels_;
    std::array<PlaneData, 3> planeData_ {};
    std::size_t planes_ {};
    bool locked_ {};
    bool submitted_ {};
    bool swapsRedBlue_ {};
};

// Everything the session needs that follows from the settings alone.
struct SessionPlan {
    VideoEncodeSettings settings;
    CMVideoCodecType codec {};
    OSType pixelFormat {};
    bool swapsRedBlue {};        // the caller's frame is RGBA in a BGRA buffer
    bool reorders {};            // the encoder may code B-frames, and so reorder its output
    bool convertsRgb {};         // the caller's RGB frames become full-range NV12 before the encoder sees them
    bool lowLatency {};          // the low-latency encoder, selected by what the stream asks for
    bool ownKeyFrames {};        // the key frames are ours: the frame type chooses the QP
    NSString* profileLevel {};
    unsigned maximumInFlight {kMaximumInFlight};
};

class AppleVideoEncoder final : public ProbeEncoder {
public:
    AppleVideoEncoder()
        : drainQueue_(dispatch_queue_create("org.uml.video-encoder.drain", DISPATCH_QUEUE_SERIAL))
    {
    }

    ~AppleVideoEncoder() override
    {
        @autoreleasepool {
            retireGeneration(false);
            teardownSession();
            // A drain in progress holds the session and the shared state it
            // needs; waiting for it keeps its VideoToolbox call from outliving
            // the object that started it.
            dispatch_sync(drainQueue_, ^{});
        }
    }

    [[nodiscard]] VideoEncodeStatus configure(const VideoEncodeSettings& settings) override
    {
        return configureWith(settings, true);
    }

    [[nodiscard]] VideoEncodeStatus configureUnchecked(const VideoEncodeSettings& settings) override
    {
        return configureWith(settings, false);
    }

    [[nodiscard]] std::string encoderIdentifier() const override { return encoderIdentifier_; }
    [[nodiscard]] std::string diagnostic() const override { return diagnostic_; }

    [[nodiscard]] VideoEncodeStatus update(const VideoEncodeUpdate& update) override
    {
        @autoreleasepool {
            if (!configured_) return VideoEncodeStatus::invalid;
            VideoEncodeSettings next = plan_.settings;
            if (update.bitrate) next.bitrate = *update.bitrate;
            if (update.peakBitrate) next.peakBitrate = *update.peakBitrate;
            if (update.quality) next.quality = *update.quality;
            if (update.maximumQp) next.maximumQp = *update.maximumQp;
            if (update.frameRateNumerator) next.frameRateNumerator = *update.frameRateNumerator;
            if (update.frameRateDenominator) next.frameRateDenominator = *update.frameRateDenominator;
            if (update.jpegQuality) next.jpegQuality = *update.jpegQuality;
            if (!validateUpdate(next)) return VideoEncodeStatus::invalid;
            if (session_) {
                const auto status = applyUpdate(next);
                if (status != VideoEncodeStatus::ok) return status;
            }
            plan_.settings = next;
            durationTicks_ = frameDuration(next.frameRateNumerator, next.frameRateDenominator);
            return VideoEncodeStatus::ok;
        }
    }

    [[nodiscard]] std::unique_ptr<WritableVideoFrame> acquireFrame() override
    {
        @autoreleasepool {
            if (!configured_) return nullptr;
            recoverIfLost();
            if (!ensureSession() && !failed_) return nullptr;
            std::uint64_t generation = 0;
            {
                std::lock_guard lock(shared_->mutex);
                if (shared_->acquired + shared_->pending.size() >= plan_.maximumInFlight) return nullptr;
                generation = shared_->generation;
            }
            // A frame that is converted before the encoder sees it is written in RGB.
            CFRef<CVPixelBufferRef> pixels = plan_.convertsRgb
                ? allocatePixelBuffer(stagingPool_.get(), kCVPixelFormatType_32BGRA)
                : allocatePixelBuffer(pool_.get(), plan_.pixelFormat);
            if (!pixels) return nullptr;
            if (!plan_.convertsRgb) attachColor(pixels.get());
            {
                std::lock_guard lock(shared_->mutex);
                if (generation != shared_->generation) return nullptr;
                ++shared_->acquired;
            }
            return std::make_unique<PoolFrame>(shared_, generation, std::move(pixels), plan_.swapsRedBlue);
        }
    }

    void submit(std::unique_ptr<WritableVideoFrame> frame, std::uint64_t timestamp, bool forceKeyFrame) override
    {
        @autoreleasepool {
            auto* held = dynamic_cast<PoolFrame*>(frame.get());
            if (!held) return;
            CFRef<CVPixelBufferRef> pixels;
            if (!held->take(pixels)) return;   // part of a stream that was reset
            frame.reset();
            if (!configured_) return;
            recoverIfLost();
            if (failed_ || !ensureSession()) {
                std::lock_guard lock(shared_->mutex);
                shared_->pushErrorLocked(timestamp);
                return;
            }
            if (rebuild_ && !rebuildSession()) {
                std::lock_guard lock(shared_->mutex);
                shared_->pushErrorLocked(timestamp);
                return;
            }
            if (plan_.convertsRgb) {
                pixels = convertRgb(pixels);
                if (!pixels) {
                    std::lock_guard lock(shared_->mutex);
                    shared_->pushErrorLocked(timestamp);
                    return;
                }
            }
            const std::uint64_t index = nextIndex_++;
            Pending entry;
            entry.timestamp = timestamp;
            entry.presentation = nextPresentation_;
            entry.forceKey = forceKeyFrame;
            entry.pixels = std::move(pixels);
            nextPresentation_ += durationTicks_;
            {
                std::lock_guard lock(shared_->mutex);
                shared_->pending.emplace(index, std::move(entry));
            }
            encodePending(index);
        }
    }

    void drain() override
    {
        @autoreleasepool {
            recoverIfLost();
            std::uint64_t horizon = 0;
            std::uint64_t generation = 0;
            {
                std::lock_guard lock(shared_->mutex);
                horizon = nextIndex_;
                generation = shared_->generation;
                if (shared_->pending.empty() || !session_) {
                    EncodedVideoFrame done;
                    done.kind = EncodedVideoFrameKind::drained;
                    shared_->pushLocked(std::move(done));
                    return;
                }
            }
            auto shared = shared_;
            auto session = session_;
            dispatch_async(drainQueue_, ^{
                @autoreleasepool {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDrainWaitMilliseconds);
                    const auto outstanding = [&] {
                        return !shared->pending.empty() && shared->pending.begin()->first < horizon;
                    };
                    CFRef<VTCompressionSessionRef> current = session;
                    std::unique_lock lock(shared->mutex, std::defer_lock);
                    for (;;) {
                        // Returns once VideoToolbox has delivered what it holds.
                        // The frames are checked against the pending table next,
                        // because a lost session leaves some to the owner, who
                        // encodes them again on a new session, and that session
                        // has to be flushed in its turn.
                        VTCompressionSessionCompleteFrames(current.get(), kCMTimeInvalid);
                        lock.lock();
                        shared->condition.wait_until(lock, deadline, [&] {
                            return shared->generation != generation || !outstanding() || shared->lost;
                        });
                        if (shared->generation != generation) return;   // a reset cancels the drain
                        if (!outstanding() || !shared->lost) break;
                        shared->condition.wait_until(lock, deadline, [&] {
                            return shared->generation != generation || !outstanding() ||
                                   (!shared->lost && shared->session && shared->session.get() != current.get());
                        });
                        if (shared->generation != generation) return;
                        if (!outstanding() || !shared->session || shared->session.get() == current.get()) break;
                        current = shared->session;
                        lock.unlock();
                    }
                    // Frames VideoToolbox never answered are lost, and the
                    // caller learns which.
                    while (outstanding()) {
                        shared->pushErrorLocked(shared->pending.begin()->second.timestamp);
                        shared->pending.erase(shared->pending.begin());
                    }
                    EncodedVideoFrame done;
                    done.kind = EncodedVideoFrameKind::drained;
                    shared->pushLocked(std::move(done));
                }
            });
        }
    }

    void reset() override
    {
        @autoreleasepool {
            retireGeneration(true);
            teardownSession();
            failed_ = false;
            rebuild_ = false;
            nextPresentation_ = 0;
            framesSinceKey_ = 0;
            // The session is rebuilt by the next frame, so a stream that stops
            // for good pays nothing, and a hardware session is not held by an
            // idle guest.
        }
    }

    [[nodiscard]] std::vector<EncodedVideoFrame> takeFrames() override
    {
        @autoreleasepool {
            recoverIfLost();
            std::lock_guard lock(shared_->mutex);
            std::vector<EncodedVideoFrame> frames(std::make_move_iterator(shared_->output.begin()),
                                                  std::make_move_iterator(shared_->output.end()));
            shared_->output.clear();
            (void)shared_->wake.drain();
            return frames;
        }
    }

    [[nodiscard]] int readinessDescriptor() const noexcept override { return shared_->wake.descriptor(); }

#if UML_VIDEO_ENCODER_TESTING
    void loseSessionForTest(unsigned count) override { shared_->injectedLosses.store(count); }
#endif

private:
    // ---- Configuration.

    VideoEncodeStatus configureWith(const VideoEncodeSettings& requested, bool check)
    {
        @autoreleasepool {
            diagnostic_.clear();
            checked_ = check;
            levelFallback_ = false;
            retireGeneration(true);
            teardownSession();
            configured_ = false;
            failed_ = false;
            nextPresentation_ = 0;
            framesSinceKey_ = 0;

            SessionPlan plan;
            const auto planned = makePlan(requested, check, plan);
            if (planned != VideoEncodeStatus::ok) return planned;
            plan_ = std::move(plan);
            durationTicks_ = frameDuration(plan_.settings.frameRateNumerator, plan_.settings.frameRateDenominator);
            const auto created = createSession();
            if (created != VideoEncodeStatus::ok) {
                plan_ = SessionPlan {};
                return created;
            }
            configured_ = true;
            return VideoEncodeStatus::ok;
        }
    }

    VideoEncodeStatus fail(VideoEncodeStatus status, const std::string& why)
    {
        diagnostic_ = why;
        return status;
    }

    static std::int64_t frameDuration(std::uint32_t numerator, std::uint32_t denominator) noexcept
    {
        if (!numerator || !denominator) return kTimescale / 30;
        return std::max<std::int64_t>(1, (std::int64_t(kTimescale) * denominator + numerator / 2) / numerator);
    }

    VideoEncodeStatus makePlan(const VideoEncodeSettings& requested, bool check, SessionPlan& plan)
    {
        VideoEncodeSettings settings = requested;
        const CMVideoCodecType type = codecType(settings.codec);
        if (!type) return fail(VideoEncodeStatus::unsupported, "codec has no encoder");
        if (profileCodec(settings.profile) != settings.codec) return fail(VideoEncodeStatus::invalid, "profile does not belong to the codec");
        if (settings.width == 0 || settings.height == 0) return fail(VideoEncodeStatus::invalid, "empty picture");
        if (settings.frameRateNumerator == 0 || settings.frameRateDenominator == 0)
            return fail(VideoEncodeStatus::invalid, "frame rate is zero");
        const bool jpeg = settings.codec == VideoCodec::jpeg;
        if (!jpeg && ((settings.width | settings.height) & 1)) return fail(VideoEncodeStatus::invalid, "odd picture size");
        if (settings.temporalLayers == 0) settings.temporalLayers = 1;
        if (settings.bFrames && (jpeg || settings.profile == VideoEncodeProfile::h264Baseline ||
                                 settings.profile == VideoEncodeProfile::h264ConstrainedBaseline ||
                                 settings.profile == VideoEncodeProfile::hevcMainStill))
            return fail(VideoEncodeStatus::invalid, "the profile has no B-frames");
        if (!jpeg) {
            if (!settings.bitrate) settings.bitrate = kDefaultBitrate;
            if (settings.rateControl == VideoRateControl::constantQuality && !settings.quality) settings.quality = kDefaultQuality;
            if (settings.rateControl == VideoRateControl::constantQuality && (settings.quality < 1 || settings.quality > 100))
                return fail(VideoEncodeStatus::invalid, "quality outside 1..100");
        } else if (!settings.jpegQuality) {
            settings.jpegQuality = kDefaultJpegQuality;
        }
        if (jpeg && (settings.jpegQuality < 1 || settings.jpegQuality > 100)) return fail(VideoEncodeStatus::invalid, "JPEG quality outside 1..100");
        if (settings.minimumQp && settings.maximumQp && *settings.minimumQp > *settings.maximumQp)
            return fail(VideoEncodeStatus::invalid, "minimum QP above maximum QP");
        if ((settings.minimumQp && *settings.minimumQp > 51) || (settings.maximumQp && *settings.maximumQp > 51))
            return fail(VideoEncodeStatus::invalid, "QP above 51");
        for (const auto qp : settings.frameQp)
            if (qp > 51) return fail(VideoEncodeStatus::invalid, "frame QP above 51");
        if (settings.peakBitrate && settings.peakBitrate < settings.bitrate)
            return fail(VideoEncodeStatus::invalid, "peak bitrate below the target");

        // The low-latency encoder is chosen by what the stream needs and the
        // standard one cannot do: temporal layers and a QP per frame type. A QP
        // per frame type degrades to the one QP the standard encoder holds when
        // there is no low-latency encoder to give each its own or the stream
        // needs B-frames, which that encoder lacks; layers have no such fallback.
        const bool wantsLayers = !jpeg && settings.temporalLayers > 1;
        const bool wantsFrameQp = !jpeg && settings.rateControl == VideoRateControl::constantQp &&
                                  settings.frameQp[0] != settings.frameQp[1];
        if (wantsLayers) {
            if (settings.bFrames) return fail(VideoEncodeStatus::invalid, "temporal layers and B-frames cannot be combined");
            if (settings.rateControl == VideoRateControl::constantBitrate)
                return fail(VideoEncodeStatus::unsupported, "constant bitrate is not available with temporal layers");
        }
        plan.lowLatency = wantsLayers || (wantsFrameQp && !settings.bFrames);
        plan.ownKeyFrames = plan.lowLatency && settings.rateControl == VideoRateControl::constantQp;

        unsigned fixedBFrameRun = 0;
        if (check) {
            const auto found = discovery();
            const CodecDiscovery* codec = found ? found->find(settings.codec) : nullptr;
            if (!codec) return fail(VideoEncodeStatus::unsupported, "no hardware encoder for the codec");
            const auto& capability = codec->capability;
            const auto profile = std::find_if(capability.profiles.begin(), capability.profiles.end(),
                [&](const auto& entry) { return entry.profile == settings.profile; });
            if (profile == capability.profiles.end()) return fail(VideoEncodeStatus::unsupported, "profile was not verified");
            if (std::find(profile->inputs.begin(), profile->inputs.end(), settings.input) == profile->inputs.end())
                return fail(VideoEncodeStatus::unsupported, "input layout was not verified for the profile");
            if (settings.width < capability.minimumWidth || settings.height < capability.minimumHeight ||
                settings.width > capability.maximumWidth || settings.height > capability.maximumHeight)
                return fail(VideoEncodeStatus::invalid, "picture size is outside the verified range");
            if (capability.sizeAlignment && (settings.width % capability.sizeAlignment || settings.height % capability.sizeAlignment))
                return fail(VideoEncodeStatus::invalid, "picture size is not a multiple of the alignment");
            if (!jpeg && std::find(capability.rateControls.begin(), capability.rateControls.end(), settings.rateControl) == capability.rateControls.end())
                return fail(VideoEncodeStatus::unsupported, "rate control mode was not verified");
            if (!jpeg && (settings.bitrate > capability.maximumBitrate || settings.peakBitrate > capability.maximumBitrate))
                return fail(VideoEncodeStatus::invalid, "bitrate is above the verified maximum");
            if (settings.bFrames > capability.maximumBFrames) return fail(VideoEncodeStatus::invalid, "B-frames above the verified maximum");
            fixedBFrameRun = capability.maximumBFrames;
            if (settings.temporalLayers > capability.maximumTemporalLayers) return fail(VideoEncodeStatus::unsupported, "temporal layers above the verified maximum");
            if (plan.lowLatency && std::find(codec->lowLatencyProfiles.begin(), codec->lowLatencyProfiles.end(), settings.profile) == codec->lowLatencyProfiles.end()) {
                if (wantsLayers) return fail(VideoEncodeStatus::unsupported, "low-latency encoder does not write the profile");
                plan.lowLatency = false;
                plan.ownKeyFrames = false;
            }
            if (settings.minimumQp && *settings.minimumQp < capability.minimumQp) return fail(VideoEncodeStatus::invalid, "QP below the verified minimum");
            if (settings.maximumQp && *settings.maximumQp > capability.maximumQp) return fail(VideoEncodeStatus::invalid, "QP above the verified maximum");
            if (settings.hdr && !capability.hdrStaticMetadata) return fail(VideoEncodeStatus::unsupported, "HDR static metadata was not verified");
        }
        if (settings.hdr && settings.profile != VideoEncodeProfile::hevcMain10)
            return fail(VideoEncodeStatus::unsupported, "HDR static metadata needs HEVC Main 10");

        plan.codec = type;
        const LayoutInfo layout = layoutInfo(settings.input);
        plan.pixelFormat = settings.color.fullRange ? layout.fullRange : layout.limitedRange;
        plan.swapsRedBlue = layout.swapsRedBlue;
        if (layout.packedRgb && settings.color.fullRange && !jpeg) {
            plan.convertsRgb = true;
            plan.swapsRedBlue = false;   // the conversion reads either order
            plan.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
        }
        if (!plan.pixelFormat) return fail(VideoEncodeStatus::invalid, "unknown input layout");
        // The hardware encoders code B-frames in one pattern, a run of
        // fixedBFrameRun per reference, and refuse every property that would
        // shorten it (MaxFrameDelayCount, ReferenceBufferCount). A caller who
        // accepts fewer gets none, so the output never runs further out of
        // order than it was told. An unchecked configuration is a probe, which
        // asks for reordering to see what the encoder does with it.
        plan.reorders = settings.bFrames && (!check || settings.bFrames >= fixedBFrameRun);
        plan.maximumInFlight = plan.reorders ? kMaximumInFlightReordered : kMaximumInFlight;
        plan.profileLevel = jpeg ? nil : profileLevel(settings);
        if (!jpeg && !plan.profileLevel) return fail(VideoEncodeStatus::unsupported, "profile has no VideoToolbox value");
        plan.settings = std::move(settings);
        return VideoEncodeStatus::ok;
    }

    // A level VideoToolbox names is requested from it when the stream fits it.
    // Otherwise the encoder chooses the lowest level that holds the stream
    // itself. Either way the stream states at least the level asked for and
    // the one its size and rates need (levels::statedLevelFloor).
    NSString* profileLevel(const VideoEncodeSettings& settings) const
    {
        if (settings.codec == VideoCodec::h264 && settings.level && !levelFallback_) {
            if (settings.level >= h264RequiredLevel(settings.profile, settings))
                if (NSString* explicitLevel = h264ExplicitLevel(settings.profile, settings.level)) return explicitLevel;
        }
        return autoLevelValue(settings.profile);
    }

    // ---- The session.

    VideoEncodeStatus createSession()
    {
        auto status = createSessionOnce();
        // VideoToolbox accepts a named level only for some picture shapes (it
        // refuses Baseline 1.3 for a picture 704 samples high). The stream
        // states the level asked for either way, so the encoder may choose.
        if (status != VideoEncodeStatus::ok && status != VideoEncodeStatus::exhausted && !levelFallback_ && namesLevel()) {
            levelFallback_ = true;
            plan_.profileLevel = profileLevel(plan_.settings);
            status = createSessionOnce();
        }
        return status;
    }

    [[nodiscard]] bool namesLevel() const
    {
        return plan_.profileLevel && ![plan_.profileLevel isEqualToString:autoLevelValue(plan_.settings.profile)];
    }

    VideoEncodeStatus createSessionOnce()
    {
        const auto& settings = plan_.settings;
        NSMutableDictionary* specification = [NSMutableDictionary dictionary];
        specification[(__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] = @YES;
        if (plan_.lowLatency) specification[(__bridge NSString*)kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = @YES;
        NSDictionary* source = @{
            (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(plan_.pixelFormat),
            (__bridge NSString*)kCVPixelBufferWidthKey: @(settings.width),
            (__bridge NSString*)kCVPixelBufferHeightKey: @(settings.height),
            (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };

        AssemblyConfig assembly;
        assembly.codec = settings.codec;
        assembly.separateParameterSets = settings.separateParameterSets;
        assembly.parameterSetsWithEveryKeyFrame = settings.parameterSetsWithEveryKeyFrame;
        assembly.temporalLayers = settings.temporalLayers;
        assembly.levelPolicy = levels::makePolicy(settings, plan_.reorders, !plan_.lowLatency);
        if (settings.hdr) assembly.hdrSei = hdrSeiUnit(*settings.hdr);
        std::uint64_t epoch = 0;
        {
            std::lock_guard lock(shared_->mutex);
            epoch = ++shared_->epoch;
            shared_->lost = false;
        }
        auto context = std::make_unique<SessionContext>(shared_, epoch, assembly);

        VTCompressionSessionRef created = nullptr;
        const OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault, int32_t(settings.width), int32_t(settings.height),
            plan_.codec, (__bridge CFDictionaryRef)specification, (__bridge CFDictionaryRef)source, kCFAllocatorDefault,
            &SessionContext::callback, context.get(), &created);
        if (status != noErr) {
            diagnostic_ = "VTCompressionSessionCreate " + std::to_string(status);
            if (status == kVTCouldNotFindVideoEncoderErr || status == kVTParameterErr) {
                // The promise discovery made did not hold on this host.
                if (checked_) invalidateDiscoveryCache();
                return VideoEncodeStatus::unsupported;
            }
            if (status == kVTInvalidSessionErr || status == kVTAllocationFailedErr || status == kVTVideoEncoderNotAvailableNowErr)
                return VideoEncodeStatus::exhausted;
            return VideoEncodeStatus::failed;
        }
        CFRef<VTCompressionSessionRef> session = CFRef<VTCompressionSessionRef>::adopt(created);
        const auto applied = applyProperties(session.get());
        if (applied != VideoEncodeStatus::ok) {
            VTCompressionSessionInvalidate(session.get());
            return applied;
        }
        if (const OSStatus prepared = VTCompressionSessionPrepareToEncodeFrames(session.get()); prepared != noErr) {
            diagnostic_ = "VTCompressionSessionPrepareToEncodeFrames " + std::to_string(prepared);
            VTCompressionSessionInvalidate(session.get());
            return prepared == kVTInvalidSessionErr || prepared == kVTAllocationFailedErr ? VideoEncodeStatus::exhausted
                                                                                           : VideoEncodeStatus::failed;
        }
        encoderIdentifier_.clear();
        CFTypeRef identifier = nullptr;
        if (VTSessionCopyProperty(session.get(), kVTCompressionPropertyKey_EncoderID, kCFAllocatorDefault, &identifier) == noErr && identifier) {
            if (CFGetTypeID(identifier) == CFStringGetTypeID())
                encoderIdentifier_ = [(__bridge NSString*)identifier UTF8String];
            CFRelease(identifier);
        }
        pool_ = CFRef<CVPixelBufferPoolRef>::retain(VTCompressionSessionGetPixelBufferPool(session.get()));
        if (!pool_) createOwnPool();
        if (plan_.convertsRgb) {
            if (!makeFullRangeConversion(settings, fullRangeConversion_)) {
                diagnostic_ = "vImage cannot make the full-range conversion";
                VTCompressionSessionInvalidate(session.get());
                return VideoEncodeStatus::failed;
            }
            stagingPool_.reset();
            createStagingPool();
        }
        session_ = std::move(session);
        context_ = std::move(context);
        {
            std::lock_guard lock(shared_->mutex);
            shared_->session = session_;
            shared_->condition.notify_all();
        }
        rebuild_ = false;   // a session made now has the current settings
        return VideoEncodeStatus::ok;
    }

    // VideoToolbox offers no pool for a source format it must convert first
    // and could not reconcile with its own; a pool of the same attributes is
    // what the caller needs, and the encoder takes whatever IOSurface it gets.
    CFRef<CVPixelBufferPoolRef> makePool(OSType format) const
    {
        const auto& settings = plan_.settings;
        NSDictionary* attributes = @{
            (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(format),
            (__bridge NSString*)kCVPixelBufferWidthKey: @(settings.width),
            (__bridge NSString*)kCVPixelBufferHeightKey: @(settings.height),
            (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };
        CVPixelBufferPoolRef created = nullptr;
        if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &created) != kCVReturnSuccess)
            return {};
        return CFRef<CVPixelBufferPoolRef>::adopt(created);
    }

    void createOwnPool() { pool_ = makePool(plan_.pixelFormat); }

    // The pool of the frames a caller writes in RGB, which are converted into
    // the session's own.
    void createStagingPool() { stagingPool_ = makePool(kCVPixelFormatType_32BGRA); }

    static OSStatus setProperty(VTCompressionSessionRef session, CFStringRef key, CFTypeRef value)
    {
        return VTSessionSetProperty(session, key, value);
    }

    // Sets one property. A refusal fails the configuration when the property
    // carries something the stream asked for, and is only noted when the
    // property is a refinement the encoder is free to ignore.
    bool apply(VTCompressionSessionRef session, CFStringRef key, id value, bool required)
    {
        const OSStatus status = setProperty(session, key, (__bridge CFTypeRef)value);
        if (status == noErr) return true;
        if (required) diagnostic_ = std::string("VTSessionSetProperty ") + [(__bridge NSString*)key UTF8String] + " " + std::to_string(status);
        return !required;
    }

    VideoEncodeStatus applyProperties(VTCompressionSessionRef session)
    {
        const auto& s = plan_.settings;
        const auto refuse = [&](const char* what) {
            if (diagnostic_.empty()) diagnostic_ = std::string("cannot set ") + what;
            return VideoEncodeStatus::unsupported;
        };
#define UML_KEY(name) kVTCompressionPropertyKey_##name
        if (s.codec == VideoCodec::jpeg) {
            if (!apply(session, UML_KEY(Quality), @(double(s.jpegQuality) / 100.0), true)) return refuse("Quality");
            applyColor(session);
            return VideoEncodeStatus::ok;
        }
        if (!apply(session, UML_KEY(ProfileLevel), plan_.profileLevel, true)) return refuse("ProfileLevel");
        if (!apply(session, UML_KEY(AllowFrameReordering), plan_.reorders ? @YES : @NO, true)) return refuse("AllowFrameReordering");
        const double rate = double(s.frameRateNumerator) / double(s.frameRateDenominator);
        (void)apply(session, UML_KEY(ExpectedFrameRate), @(rate), false);
        const std::int32_t interval = plan_.ownKeyFrames || s.keyFrameInterval == 0
            ? kNoKeyFrameInterval : std::int32_t(std::min<std::uint32_t>(s.keyFrameInterval, kNoKeyFrameInterval));
        if (!apply(session, UML_KEY(MaxKeyFrameInterval), @(interval), true)) return refuse("MaxKeyFrameInterval");
        // Real-time mode is what lets a bitrate change take effect while a
        // stream encodes: the standard encoder otherwise fixes its rate control
        // when the session starts and ignores later changes, and it overshoots
        // its target by half again on busy content. The quality at a given rate
        // is the same either way. Only a guest that asks for low latency also
        // trades quality for speed.
        (void)apply(session, UML_KEY(RealTime), @YES, false);
        if (s.realTime) (void)apply(session, UML_KEY(PrioritizeEncodingSpeedOverQuality), @YES, false);

        switch (s.rateControl) {
        case VideoRateControl::variableBitrate:
            if (!apply(session, UML_KEY(AverageBitRate), @(s.bitrate), true)) return refuse("AverageBitRate");
            if (s.peakBitrate && !apply(session, UML_KEY(DataRateLimits), @[@(s.peakBitrate / 8), @1.0], !plan_.lowLatency))
                return refuse("DataRateLimits");
            if (s.bufferBits && s.bitrate)
                (void)apply(session, UML_KEY(VBVBufferDuration), @(double(s.bufferBits) / double(s.bitrate)), false);
            break;
        case VideoRateControl::constantBitrate:
            if (!apply(session, UML_KEY(ConstantBitRate), @(s.bitrate), true)) return refuse("ConstantBitRate");
            break;
        case VideoRateControl::constantQuality:
            // Quality is the property that holds a quality: the standard
            // encoder takes it in place of a bitrate and spends what the
            // picture needs. ConstantQualityFactor is accepted and has no
            // effect on the hardware encoders.
            if (!apply(session, UML_KEY(Quality), @(double(s.quality) / 100.0), true)) return refuse("Quality");
            break;
        case VideoRateControl::constantQp:
            // The standard encoder holds a QP by bounding it from both sides.
            // The low-latency one takes the QP of each frame as it is encoded.
            if (!plan_.lowLatency) {
                NSNumber* qp = @(s.frameQp[1]);
                if (!apply(session, UML_KEY(MinAllowedFrameQP), qp, true) || !apply(session, UML_KEY(MaxAllowedFrameQP), qp, true))
                    return refuse("constant QP");
            }
            break;
        }
        if (s.rateControl != VideoRateControl::constantQp) {
            if (s.minimumQp && !apply(session, UML_KEY(MinAllowedFrameQP), @(*s.minimumQp), true)) return refuse("MinAllowedFrameQP");
            if (s.maximumQp && !apply(session, UML_KEY(MaxAllowedFrameQP), @(*s.maximumQp), true)) return refuse("MaxAllowedFrameQP");
        }
        if (s.codec == VideoCodec::h264 && s.profile != VideoEncodeProfile::h264Baseline &&
            s.profile != VideoEncodeProfile::h264ConstrainedBaseline)
            (void)apply(session, UML_KEY(H264EntropyMode),
                        (__bridge NSString*)(s.cabac ? kVTH264EntropyMode_CABAC : kVTH264EntropyMode_CAVLC), false);
        if (s.temporalLayers > 1) {
            if (!apply(session, UML_KEY(BaseLayerFrameRateFraction), @0.5, true)) return refuse("BaseLayerFrameRateFraction");
            if (s.baseLayerBitratePercent &&
                !apply(session, UML_KEY(BaseLayerBitRateFraction), @(double(s.baseLayerBitratePercent) / 100.0), true))
                return refuse("BaseLayerBitRateFraction");
        }
        applyColor(session);
        if (s.hdr) {
            const auto mastering = masteringDisplay(*s.hdr);
            const auto light = contentLight(*s.hdr);
            if (!apply(session, UML_KEY(MasteringDisplayColorVolume), [NSData dataWithBytes:mastering.data() length:mastering.size()], true))
                return refuse("MasteringDisplayColorVolume");
            if (!apply(session, UML_KEY(ContentLightLevelInfo), [NSData dataWithBytes:light.data() length:light.size()], true))
                return refuse("ContentLightLevelInfo");
        }
        return VideoEncodeStatus::ok;
    }

    void applyColor(VTCompressionSessionRef session) const
    {
        const auto& color = plan_.settings.color;
        if (NSString* value = colorPrimaries(color.primaries)) (void)setProperty(session, UML_KEY(ColorPrimaries), (__bridge CFTypeRef)value);
        if (NSString* value = transferFunction(color.transfer)) (void)setProperty(session, UML_KEY(TransferFunction), (__bridge CFTypeRef)value);
        if (NSString* value = matrixCoefficients(color.matrix)) (void)setProperty(session, UML_KEY(YCbCrMatrix), (__bridge CFTypeRef)value);
    }

    // The 24-byte mastering display colour volume and 4-byte content light
    // level of the HEVC SEI messages, big-endian, in the order the messages
    // carry them.
    static std::array<std::uint8_t, 24> masteringDisplay(const VideoHdrStaticMetadata& hdr)
    {
        std::array<std::uint8_t, 24> bytes {};
        std::size_t at = 0;
        const auto put16 = [&](std::uint16_t value) { bytes[at++] = std::uint8_t(value >> 8); bytes[at++] = std::uint8_t(value); };
        const auto put32 = [&](std::uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) bytes[at++] = std::uint8_t(value >> shift); };
        for (const auto value : hdr.displayPrimaries) put16(value);
        put16(hdr.whitePointX);
        put16(hdr.whitePointY);
        put32(hdr.maximumLuminance);
        put32(hdr.minimumLuminance);
        return bytes;
    }
    static std::array<std::uint8_t, 4> contentLight(const VideoHdrStaticMetadata& hdr)
    {
        return {std::uint8_t(hdr.maximumContentLightLevel >> 8), std::uint8_t(hdr.maximumContentLightLevel),
                std::uint8_t(hdr.maximumFrameAverageLightLevel >> 8), std::uint8_t(hdr.maximumFrameAverageLightLevel)};
    }
    // A prefix SEI unit carrying the mastering display colour volume (payload
    // type 137) and the content light level (144), as an HEVC encoder that
    // writes HDR10 puts them before the pictures of each IRAP.
    static std::vector<std::uint8_t> hdrSeiUnit(const VideoHdrStaticMetadata& hdr)
    {
        const auto mastering = masteringDisplay(hdr);
        const auto light = contentLight(hdr);
        std::vector<std::uint8_t> payload;
        payload.push_back(137);
        payload.push_back(std::uint8_t(mastering.size()));
        payload.insert(payload.end(), mastering.begin(), mastering.end());
        payload.push_back(144);
        payload.push_back(std::uint8_t(light.size()));
        payload.insert(payload.end(), light.begin(), light.end());
        payload.push_back(0x80);   // rbsp_trailing_bits
        const auto escaped = bitstream::escapeEmulation(payload);
        std::vector<std::uint8_t> unit {0x4e, 0x01};   // prefix SEI, layer 0, temporal id 0
        unit.insert(unit.end(), escaped.begin(), escaped.end());
        std::vector<std::uint8_t> out;
        bitstream::appendAnnexB(out, unit.data(), unit.size());
        return out;
    }

    bool validateUpdate(const VideoEncodeSettings& next) const
    {
        if (!next.frameRateNumerator || !next.frameRateDenominator) return false;
        if (next.codec == VideoCodec::jpeg) return next.jpegQuality >= 1 && next.jpegQuality <= 100;
        if (next.bitrate < 1000) return false;
        if (next.peakBitrate && next.peakBitrate < next.bitrate) return false;
        if (next.rateControl == VideoRateControl::constantQuality && (next.quality < 1 || next.quality > 100)) return false;
        if (next.maximumQp && (*next.maximumQp > 51 || (next.minimumQp && *next.minimumQp > *next.maximumQp))) return false;
        if (!checked_) return true;
        if (const auto found = discovery()) {
            if (const auto* codec = found->find(next.codec)) {
                if (next.bitrate > codec->capability.maximumBitrate || next.peakBitrate > codec->capability.maximumBitrate) return false;
                if (next.maximumQp && *next.maximumQp > codec->capability.maximumQp) return false;
            }
        }
        return true;
    }

    VideoEncodeStatus applyUpdate(const VideoEncodeSettings& next)
    {
        const auto& was = plan_.settings;
        VTCompressionSessionRef session = session_.get();
        const auto refuse = [&](const char* what) {
            diagnostic_ = std::string("cannot change ") + what;
            return VideoEncodeStatus::unsupported;
        };
        if (next.codec == VideoCodec::jpeg) {
            if (next.jpegQuality != was.jpegQuality &&
                !apply(session, UML_KEY(Quality), @(double(next.jpegQuality) / 100.0), true))
                return refuse("Quality");
            return VideoEncodeStatus::ok;
        }
        switch (next.rateControl) {
        case VideoRateControl::variableBitrate:
            if (next.bitrate != was.bitrate && !apply(session, UML_KEY(AverageBitRate), @(next.bitrate), true)) return refuse("AverageBitRate");
            if ((next.peakBitrate != was.peakBitrate || next.bitrate != was.bitrate) && next.peakBitrate &&
                !apply(session, UML_KEY(DataRateLimits), @[@(next.peakBitrate / 8), @1.0], !plan_.lowLatency))
                return refuse("DataRateLimits");
            break;
        case VideoRateControl::constantBitrate:
            if (next.bitrate != was.bitrate && !apply(session, UML_KEY(ConstantBitRate), @(next.bitrate), true)) return refuse("ConstantBitRate");
            break;
        case VideoRateControl::constantQuality:
            // The encoder accepts a new quality under a running session and
            // goes on with the old one.
            if (next.quality != was.quality) rebuild_ = true;
            break;
        case VideoRateControl::constantQp:
            break;   // the QP of a frame is a setting, not something to change under the stream
        }
        // The level is a property of the stream's start (levels::makePolicy):
        // a rate or frame rate that changes under the running session does not
        // restate it. A session made for any other reason takes the level of
        // the settings it is made from.
        // Likewise a QP bound: it only takes effect from a new session.
        if (next.rateControl != VideoRateControl::constantQp && next.maximumQp != was.maximumQp) rebuild_ = true;
        if (next.frameRateNumerator != was.frameRateNumerator || next.frameRateDenominator != was.frameRateDenominator)
            (void)apply(session, UML_KEY(ExpectedFrameRate), @(double(next.frameRateNumerator) / double(next.frameRateDenominator)), false);
        return VideoEncodeStatus::ok;
    }

    // ---- Frames.

    CFRef<CVPixelBufferRef> allocatePixelBuffer(CVPixelBufferPoolRef pool, OSType format)
    {
        CVPixelBufferRef created = nullptr;
        if (pool) {
            NSDictionary* auxiliary = @{(__bridge NSString*)kCVPixelBufferPoolAllocationThresholdKey: @(plan_.maximumInFlight + kPoolSlack)};
            if (CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, pool, (__bridge CFDictionaryRef)auxiliary, &created) == kCVReturnSuccess)
                return CFRef<CVPixelBufferRef>::adopt(created);
            return {};
        }
        // A stream whose session could not be made still takes its frames, so
        // that each one is answered with an error rather than left waiting.
        NSDictionary* attributes = @{(__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        if (CVPixelBufferCreate(kCFAllocatorDefault, plan_.settings.width, plan_.settings.height, format,
                                (__bridge CFDictionaryRef)attributes, &created) != kCVReturnSuccess)
            return {};
        return CFRef<CVPixelBufferRef>::adopt(created);
    }

    // The caller's RGB frame as a full-range NV12 buffer from the session's
    // pool, which is what the encoder is given when the stream asks for full
    // range: VideoToolbox converts RGB to video range only. The conversion
    // reads either byte order, and runs on the submitting thread (untiled, so
    // that it wakes no dispatch pool the emulator's scheduler does not count).
    CFRef<CVPixelBufferRef> convertRgb(const CFRef<CVPixelBufferRef>& rgb)
    {
        CFRef<CVPixelBufferRef> yuv = allocatePixelBuffer(pool_.get(), plan_.pixelFormat);
        if (!yuv) return {};
        CVPixelBufferLockBaseAddress(rgb.get(), kCVPixelBufferLock_ReadOnly);
        CVPixelBufferLockBaseAddress(yuv.get(), 0);
        const vImage_Buffer source {CVPixelBufferGetBaseAddress(rgb.get()), CVPixelBufferGetHeight(rgb.get()),
                                    CVPixelBufferGetWidth(rgb.get()), CVPixelBufferGetBytesPerRow(rgb.get())};
        const vImage_Buffer luma {CVPixelBufferGetBaseAddressOfPlane(yuv.get(), 0), CVPixelBufferGetHeightOfPlane(yuv.get(), 0),
                                  CVPixelBufferGetWidthOfPlane(yuv.get(), 0), CVPixelBufferGetBytesPerRowOfPlane(yuv.get(), 0)};
        const vImage_Buffer chroma {CVPixelBufferGetBaseAddressOfPlane(yuv.get(), 1), CVPixelBufferGetHeightOfPlane(yuv.get(), 1),
                                    CVPixelBufferGetWidthOfPlane(yuv.get(), 1), CVPixelBufferGetBytesPerRowOfPlane(yuv.get(), 1)};
        // Where A, R, G and B sit in a source pixel.
        static constexpr std::uint8_t bgra[4] = {3, 2, 1, 0};
        static constexpr std::uint8_t rgba[4] = {3, 0, 1, 2};
        const auto result = vImageConvert_ARGB8888To420Yp8_CbCr8(&source, &luma, &chroma, &fullRangeConversion_,
            plan_.settings.input == RawVideoLayout::rgba ? rgba : bgra, kvImageDoNotTile);
        CVPixelBufferUnlockBaseAddress(yuv.get(), 0);
        CVPixelBufferUnlockBaseAddress(rgb.get(), kCVPixelBufferLock_ReadOnly);
        if (result != kvImageNoError) return {};
        attachColor(yuv.get());
        return yuv;
    }

    void attachColor(CVPixelBufferRef pixels) const
    {
        const auto& color = plan_.settings.color;
        if (NSString* value = colorPrimaries(color.primaries))
            CVBufferSetAttachment(pixels, kCVImageBufferColorPrimariesKey, (__bridge CFStringRef)value, kCVAttachmentMode_ShouldPropagate);
        if (NSString* value = transferFunction(color.transfer))
            CVBufferSetAttachment(pixels, kCVImageBufferTransferFunctionKey, (__bridge CFStringRef)value, kCVAttachmentMode_ShouldPropagate);
        if (NSString* value = matrixCoefficients(color.matrix))
            CVBufferSetAttachment(pixels, kCVImageBufferYCbCrMatrixKey, (__bridge CFStringRef)value, kCVAttachmentMode_ShouldPropagate);
    }

    // Hands one pending frame to the session. A session-level refusal marks
    // the session lost, which recovery answers; any other refusal is the
    // frame's alone.
    void encodePending(std::uint64_t index, bool forceNext = false)
    {
        CFRef<CVPixelBufferRef> pixels;
        std::int64_t presentation = 0;
        std::uint64_t timestamp = 0;
        bool force = forceNext;
        {
            std::lock_guard lock(shared_->mutex);
            const auto found = shared_->pending.find(index);
            if (found == shared_->pending.end()) return;
            pixels = found->second.pixels;
            presentation = found->second.presentation;
            timestamp = found->second.timestamp;
            force = force || found->second.forceKey;
        }
        const auto& s = plan_.settings;
        bool key = force;
        if (plan_.ownKeyFrames) {
            // The QP of a frame depends on its type, so the encoder is not
            // left to place key frames.
            if (framesSinceKey_ == 0 || (s.keyFrameInterval && framesSinceKey_ >= s.keyFrameInterval)) key = true;
            framesSinceKey_ = key ? 1 : framesSinceKey_ + 1;
        }
        NSMutableDictionary* options = nil;
        if (key) {
            options = [NSMutableDictionary dictionary];
            options[(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame] = @YES;
        }
        if (plan_.ownKeyFrames) {
            if (!options) options = [NSMutableDictionary dictionary];
            options[(__bridge NSString*)kVTEncodeFrameOptionKey_BaseFrameQP] = @(s.frameQp[key ? 0 : 1]);
        }
        const OSStatus status = VTCompressionSessionEncodeFrame(session_.get(), pixels.get(), CMTimeMake(presentation, kTimescale),
            CMTimeMake(durationTicks_, kTimescale), (__bridge CFDictionaryRef)options,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(index)), nullptr);
        if (status == noErr) return;
        std::lock_guard lock(shared_->mutex);
        if (sessionLoss(status)) {
            shared_->lost = true;
            (void)shared_->wake.notify();
            shared_->condition.notify_all();
            return;
        }
        diagnostic_ = "VTCompressionSessionEncodeFrame " + std::to_string(status);
        if (shared_->pending.erase(index)) shared_->pushErrorLocked(timestamp);
        shared_->condition.notify_all();
    }

    // ---- Loss, reset and teardown.

    bool ensureSession()
    {
        if (session_) return true;
        if (failed_ || !configured_) return false;
        if (createSession() == VideoEncodeStatus::ok) return true;
        failStream();
        return false;
    }

    // Every frame still waiting for output is lost, and the caller is told so.
    void failStream()
    {
        failed_ = true;
        std::lock_guard lock(shared_->mutex);
        for (const auto& entry : shared_->pending) shared_->pushErrorLocked(entry.second.timestamp);
        shared_->pending.clear();
        shared_->condition.notify_all();
    }

    // Replaces the session between two frames with one made from the current
    // settings. What the old session still holds is delivered first, so the
    // stream continues from where it was, at the key frame every session starts
    // with.
    bool rebuildSession()
    {
        if (session_) {
            VTCompressionSessionCompleteFrames(session_.get(), kCMTimeInvalid);
            std::unique_lock lock(shared_->mutex);
            shared_->condition.wait_for(lock, std::chrono::milliseconds(kDrainWaitMilliseconds),
                                        [&] { return shared_->pending.empty() || shared_->lost; });
        }
        recoverIfLost();
        if (failed_) return false;
        if (!rebuild_) return true;   // a recovery made the session from the new settings
        {
            // Frames the old session never answered are lost with it.
            std::lock_guard lock(shared_->mutex);
            for (const auto& entry : shared_->pending) shared_->pushErrorLocked(entry.second.timestamp);
            shared_->pending.clear();
            ++shared_->epoch;
        }
        teardownSession();
        framesSinceKey_ = 0;
        if (createSession() != VideoEncodeStatus::ok) {
            failStream();
            return false;
        }
        return true;
    }

    // The session was revoked or failed. Make a new one at the same settings
    // and encode the frames that never got an answer, the first of them as a
    // key frame. A second loss with no output in between is final.
    void recoverIfLost()
    {
        unsigned losses = 0;
        {
            std::lock_guard lock(shared_->mutex);
            if (!shared_->lost) return;
            shared_->lost = false;
            ++shared_->epoch;   // what the dying session still delivers is not heard
            losses = ++shared_->lossesWithoutOutput;
        }
        teardownSession();
        if (failed_ || !configured_ || losses > 1) {
            failStream();
            return;
        }
        // A level the encoder rejected frame by frame is a likely cause of the
        // loss; the second attempt leaves the level to the encoder.
        levelFallback_ = true;
        plan_.profileLevel = profileLevel(plan_.settings);
        framesSinceKey_ = 0;
        if (createSession() != VideoEncodeStatus::ok) {
            failStream();
            return;
        }
        std::vector<std::uint64_t> indexes;
        {
            std::lock_guard lock(shared_->mutex);
            for (const auto& entry : shared_->pending) indexes.push_back(entry.first);
        }
        bool first = true;
        for (const auto index : indexes) {
            encodePending(index, first);
            first = false;
            std::lock_guard lock(shared_->mutex);
            if (shared_->lost) return;   // lost again; the next call decides
        }
    }

    // Starts a new epoch: whatever the old session still delivers is ignored.
    void retireGeneration(bool discardOutput)
    {
        std::lock_guard lock(shared_->mutex);
        ++shared_->generation;
        ++shared_->epoch;
        shared_->pending.clear();
        shared_->acquired = 0;
        shared_->lost = false;
        shared_->lossesWithoutOutput = 0;
        if (discardOutput) {
            shared_->output.clear();
            (void)shared_->wake.drain();
        }
        shared_->condition.notify_all();
    }

    void teardownSession()
    {
        {
            std::lock_guard lock(shared_->mutex);
            shared_->session.reset();
        }
        if (session_) {
            VTCompressionSessionInvalidate(session_.get());
            session_.reset();
        }
        pool_.reset();
        stagingPool_.reset();
        context_.reset();
        encoderIdentifier_.clear();
    }

    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    SessionPlan plan_;
    bool configured_ {};
    bool failed_ {};
    bool checked_ {};
    bool levelFallback_ {};
    bool rebuild_ {};   // a setting the running session ignores changed; the next frame starts a new session
    CFRef<VTCompressionSessionRef> session_;
    CFRef<CVPixelBufferPoolRef> pool_;
    CFRef<CVPixelBufferPoolRef> stagingPool_;   // plan_.convertsRgb: the caller's frames
    vImage_ARGBToYpCbCr fullRangeConversion_ {};
    std::unique_ptr<SessionContext> context_;
    std::uint64_t nextIndex_ {};
    std::int64_t nextPresentation_ {};
    std::int64_t durationTicks_ {kTimescale / 30};
    std::uint64_t framesSinceKey_ {};
    dispatch_queue_t drainQueue_;
    std::string encoderIdentifier_;
    std::string diagnostic_;
};

#undef UML_KEY

} // namespace

std::unique_ptr<ProbeEncoder> makeProbeEncoder()
{
    return std::make_unique<AppleVideoEncoder>();
}

std::string autoLevelName(VideoEncodeProfile profile)
{
    NSString* value = autoLevelValue(profile);
    return value ? std::string([value UTF8String]) : std::string();
}

} // namespace uml::platform::apple_encoder

namespace uml::platform {

std::unique_ptr<VideoEncoder> makeVideoEncoder()
{
    return apple_encoder::makeProbeEncoder();
}

#if UML_VIDEO_ENCODER_TESTING
void testing::loseSession(VideoEncoder& encoder, unsigned count)
{
    // The variant's factory only ever makes the Apple session.
    static_cast<apple_encoder::ProbeEncoder&>(encoder).loseSessionForTest(count);
}
#endif

} // namespace uml::platform
