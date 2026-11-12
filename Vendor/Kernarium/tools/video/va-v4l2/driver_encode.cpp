// SPDX-License-Identifier: GPL-2.0-or-later
// The encode half of the driver: what it advertises from what the encoder node
// publishes, how an encode context is opened, and how a finished frame gets
// into the coded buffer the client mapped.
#include <linux/videodev2.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "convert.hpp"
#include "driver.hpp"

namespace uml::va {
namespace {

// How long a client waits for a frame to come back. The encoder returns each
// frame within milliseconds, so this is the time after which something is
// wrong rather than a latency anyone sees.
constexpr std::chrono::milliseconds kEncodeWait {4000};

const char* codecName(std::uint32_t codec) noexcept
{
    switch (codec) {
    case V4L2_PIX_FMT_H264: return "H264";
    case V4L2_PIX_FMT_HEVC: return "HEVC";
    case V4L2_PIX_FMT_JPEG: return "JPEG";
    default: return "other";
    }
}

}

void Driver::probeEncoding()
{
    auto& pool = pool_;
    // The node is looked for once per process: a driver initialised again
    // inside a sandbox cannot search, and does not need to.
    encoderDevice_ = pool.knownDevice(Role::encoder);
    if (encoderDevice_.empty()) encoderDevice_ = findEncoderDevice(pool.io());
    if (encoderDevice_.empty()) { note("no Virtio video encoder node"); return; }

    std::string error;
    Lease lease = pool.acquire(Role::encoder, encoderDevice_, error);
    if (!lease) { note(error.c_str()); return; }
    auto capabilities = std::make_shared<EncoderCapabilities>();
    capabilities->device = encoderDevice_;
    const bool probed = probeEncoder(pool.io(), lease.fd(), *capabilities, error);
    // Reading what the node offers changes nothing on the descriptor.
    lease.giveBack(probed, false);
    if (!probed) { note(("encoder node: " + error).c_str()); return; }

    auto support = std::make_shared<EncodeSupport>(deriveEncodeSupport(capabilities));
    if (!support->any()) { note("the encoder node offers no profile or rate control this driver can use"); return; }
    // The descriptors the sessions will need are opened now, while a node can
    // still be opened.
    pool.fill(Role::encoder, encoderDevice_);
    std::string summary = "encoder " + encoderDevice_ + ":";
    for (const auto& profile : support->profiles) {
        const EncodeCodec* limits = capabilities->codec(profile.codec);
        char text[96];
        std::snprintf(text, sizeof(text), " %s profile %d up to %ux%u", codecName(profile.codec), int(profile.profile),
                      limits ? limits->maxWidth : 0u, limits ? limits->maxHeight : 0u);
        summary += text;
    }
    char rates[48];
    std::snprintf(rates, sizeof(rates), "; rate control 0x%x", support->rateControl);
    note((summary + rates).c_str());
    encoding_ = std::move(support);
}

void Driver::encodeProfiles(std::vector<VAProfile>& out) const
{
    if (!encoding_) return;
    for (const auto& profile : encoding_->profiles) out.push_back(profile.profile);
}

bool Driver::isEncodeProfile(VAProfile profile) const noexcept
{
    return encoding_ && encoding_->profile(profile);
}

VAStatus Driver::encodeAttributes(VAProfile profile, VAConfigAttrib* attributes, int count) const
{
    const EncodeCodec* limits = encoding_ ? encoding_->limits(profile) : nullptr;
    if (!limits) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    for (int index = 0; index < count; ++index) {
        switch (attributes[index].type) {
        case VAConfigAttribRTFormat:
            attributes[index].value = VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribRateControl:
            attributes[index].value = encoding_->rateControl;
            break;
        case VAConfigAttribEncPackedHeaders:
            // The encoder writes the parameter sets and the headers itself.
            attributes[index].value = VA_ENC_PACKED_HEADER_NONE;
            break;
        case VAConfigAttribEncMaxRefFrames:
            // One reference picture in list 0 and none in list 1: every
            // picture after the first predicts from the one before it.
            attributes[index].value = 1;
            break;
        case VAConfigAttribEncMaxSlices:
            attributes[index].value = 1;
            break;
        case VAConfigAttribEncInterlaced:
            attributes[index].value = VA_ENC_INTERLACED_NONE;
            break;
        case VAConfigAttribEncQualityRange:
            attributes[index].value = 1;
            break;
        case VAConfigAttribMaxPictureWidth:
            attributes[index].value = limits->maxWidth;
            break;
        case VAConfigAttribMaxPictureHeight:
            attributes[index].value = limits->maxHeight;
            break;
        default:
            attributes[index].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus Driver::createEncodeConfig(VAProfile profile, VAEntrypoint entrypoint, const VAConfigAttrib* attributes,
                                    int count, Config& config) const
{
    if (!isEncodeProfile(profile)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (!isEncodeEntrypoint(entrypoint)) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    config = {};
    config.profile = profile;
    config.entrypoint = entrypoint;
    config.rtFormat = VA_RT_FORMAT_YUV420;
    const unsigned offered = encoding_->rateControl;
    unsigned wanted = 0;
    for (int index = 0; index < count; ++index) {
        switch (attributes[index].type) {
        case VAConfigAttribRTFormat:
            if ((attributes[index].value & VA_RT_FORMAT_YUV420) != attributes[index].value)
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            break;
        case VAConfigAttribRateControl:
            wanted = attributes[index].value;
            break;
        default:
            // Packed headers a client offers are ignored, because the encoder
            // writes its own, and the other attributes are hints.
            break;
        }
    }
    if (wanted) {
        // A client names one mode. Several bits mean it accepts any of them, in
        // which case the first this driver has is the one it gets.
        const unsigned usable = wanted & offered;
        if (!usable) return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (const unsigned mode : {VA_RC_CBR, VA_RC_VBR, VA_RC_CQP, VA_RC_ICQ})
            if (usable & mode) { config.rateControl = mode; break; }
    } else {
        for (const unsigned mode : {VA_RC_CBR, VA_RC_VBR, VA_RC_CQP, VA_RC_ICQ})
            if (offered & mode) { config.rateControl = mode; break; }
    }
    if (!config.rateControl) return VA_STATUS_ERROR_INVALID_PARAMETER;
    return VA_STATUS_SUCCESS;
}

VAStatus Driver::openEncoder(Context& context, const Config& config)
{
    const EncodeCodec* limits = encoding_ ? encoding_->limits(config.profile) : nullptr;
    if (!limits) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (context.width < limits->minWidth || context.height < limits->minHeight || context.width > limits->maxWidth ||
        context.height > limits->maxHeight)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    std::string error;
    // A pooled descriptor is the one a sandboxed process can still get.
    Lease lease = pool_.acquire(Role::encoder, encoderDevice_, error);
    if (!lease) {
        note(error.c_str());
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    context.encoder = std::make_unique<VaEncoder>(encoding_, config.profile, config.rateControl, context.width,
                                                  context.height, std::move(lease));
    return VA_STATUS_SUCCESS;
}

VAStatus Driver::addEncodeBuffer(Context& context, const Buffer& buffer)
{
    if (!context.encoder) return VA_STATUS_ERROR_INVALID_CONTEXT;
    const std::uint8_t* data = buffer.external ? buffer.external : buffer.owned.data();
    const VAStatus status = context.encoder->add(buffer.type, data, std::size_t(buffer.size) * buffer.elements);
    if (status != VA_STATUS_SUCCESS) note("an encode buffer is shorter than its type says");
    return status;
}

namespace {

// The part of a surface the encoder reads, as NV12 in guest memory.
VAStatus rawFrameOf(Driver& driver, Context& owner, Surface& target, RawFrame& frame)
{
    if (!target.buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    // A surface a decoder wrote as packed RGB is converted back to the NV12
    // the encoder takes.
    driver.materialise(target);
    // The encoder only reads the surface, so a descriptor a client exported
    // read-only is enough. Each plane is read where its own object maps, which
    // is one object for a surface the driver allocated and one per plane for a
    // planar buffer a graphics stack exported.
    errno = 0;
    const std::uint8_t* luma = target.buffer->planeForRead(0);
    const std::uint8_t* chroma = target.fourcc == VA_FOURCC_NV12 && target.buffer->planes() >= 2
        ? target.buffer->planeForRead(1) : nullptr;
    if (!luma || (target.fourcc == VA_FOURCC_NV12 && !chroma)) {
        const int reason = errno;
        driver.note(("cannot map the memory of an encode input surface: " +
                     std::string(reason ? std::strerror(reason) : "no reason given")).c_str());
        return target.fourcc == VA_FOURCC_NV12 && target.buffer->planes() < 2
            ? VA_STATUS_ERROR_INVALID_SURFACE : VA_STATUS_ERROR_OPERATION_FAILED;
    }
    const unsigned width = std::min(target.width, target.buffer->width());
    const unsigned height = std::min(target.height, target.buffer->height());
    if (target.fourcc == VA_FOURCC_NV12) {
        frame = {luma, target.buffer->pitch(0), chroma, target.buffer->pitch(1), width, height};
        return VA_STATUS_SUCCESS;
    }
    if (target.fourcc == VA_FOURCC_ARGB) {
        // Packed pixels in; the encoder wants NV12, so convert once here.
        const std::size_t pitch = (std::size_t(width) + 1) & ~std::size_t(1);
        const std::size_t rows = (std::size_t(height) + 1) & ~std::size_t(1);
        owner.encodeScratch.assign(pitch * rows + pitch * (rows / 2), 0);
        std::uint8_t* out = owner.encodeScratch.data();
        std::uint8_t* outChroma = out + pitch * rows;
        const Region region {0, 0, width, height};
        const PlaneView planes[2] {{out, pitch}, {outChroma, pitch}};
        convertArgbToNv12(ConstPlaneView {luma, target.buffer->pitch(0)}, region, planes, region, height >= 720);
        frame = {out, pitch, outChroma, pitch, width, height};
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
}

}

VAStatus Driver::submitEncode(Context& owner, VASurfaceID id, Surface& target)
{
    if (!owner.encoder) return VA_STATUS_ERROR_INVALID_CONTEXT;
    VaEncoder& encoder = *owner.encoder;
    if (!encoder.state().picture.valid) {
        note("an encode picture came without picture parameters");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    Buffer* coded = buffer(encoder.state().picture.codedBuffer);
    if (!coded || !coded->coded.is) {
        note("an encode picture named no coded buffer");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!target.buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    // A frame a graphics stack drew reaches the guest's memory only when the
    // kernel is told the CPU is about to read it, and it must stay current
    // until the encoder has copied it.
    const PixelBuffer::CpuAccess access(*target.buffer, false);
    RawFrame frame;
    VAStatus status = rawFrameOf(*this, owner, target, frame);
    if (status != VA_STATUS_SUCCESS) return status;
    std::string error;
    std::uint64_t token = 0;
    if (tracing()) {
        // Whether real pictures arrive: the mean of a sparse sample of each plane.
        unsigned long luma = 0, cb = 0, cr = 0, lumaCount = 0, chromaCount = 0;
        for (unsigned y = 0; y < frame.height; y += 8)
            for (unsigned x = 0; x < frame.width; x += 8, ++lumaCount) luma += frame.luma[y * frame.lumaPitch + x];
        for (unsigned y = 0; y < (frame.height + 1) / 2; y += 4)
            for (unsigned x = 0; x + 1 < frame.width; x += 8, ++chromaCount) {
                cb += frame.chroma[y * frame.chromaPitch + x];
                cr += frame.chroma[y * frame.chromaPitch + x + 1];
            }
        char text[120];
        std::snprintf(text, sizeof text, "encode input %ux%u mean Y %lu U %lu V %lu", frame.width, frame.height,
                      lumaCount ? luma / lumaCount : 0, chromaCount ? cb / chromaCount : 0,
                      chromaCount ? cr / chromaCount : 0);
        note(text);
    }
    status = encoder.encode(frame, id, token, error);
    for (const auto& text : encoder.takeNotes()) note(text.c_str());
    if (status != VA_STATUS_SUCCESS) {
        note(error.c_str());
        target.state = Surface::State::failed;
        return status;
    }
    target.state = Surface::State::pending;
    target.sequence = token;
    coded->coded.pending = true;
    coded->coded.surface = id;
    coded->coded.segment = {};
    coded->coded.segment.buf = coded->owned.data();
    return VA_STATUS_SUCCESS;
}

void Driver::harvestEncode(Context& owner, std::chrono::milliseconds wait)
{
    if (!owner.encoder) return;
    VaEncoder::Completed done;
    std::string error;
    while (owner.encoder->poll(done, wait, error)) {
        wait = std::chrono::milliseconds(0);
        const bool bad = done.frame.error || done.frame.data.empty();
        if (Buffer* coded = buffer(done.codedBuffer); coded && coded->coded.is) {
            auto& segment = coded->coded.segment;
            const std::size_t capacity = coded->owned.size();
            const std::size_t used = bad ? 0 : std::min(done.frame.data.size(), capacity);
            if (used) std::memcpy(coded->owned.data(), done.frame.data.data(), used);
            segment = {};
            segment.size = std::uint32_t(used);
            segment.buf = coded->owned.data();
            if (!bad && done.frame.data.size() > capacity) segment.status |= VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK;
            if (bad) segment.status |= VA_CODED_BUF_STATUS_BAD_BITSTREAM;
            if (coded->coded.surface == done.surface) coded->coded.pending = false;
        }
        if (done.keyWanted && !done.frame.key && !bad)
            note("an IDR picture came back as a delta picture; the encoder did not honour the key frame request");
        if (Surface* target = surface(done.surface); target && target->sequence == done.token)
            target->state = bad ? Surface::State::failed : Surface::State::ready;
    }
    for (const auto& text : owner.encoder->takeNotes()) note(text.c_str());
    if (!error.empty()) {
        note(error.c_str());
        owner.failure = error;
        // Nothing more will come back, so nobody may keep waiting.
        for (auto& entry : surfaces)
            if (entry.second->state == Surface::State::pending && context(entry.second->owner) == &owner)
                entry.second->state = Surface::State::failed;
    }
}

VAStatus Driver::waitForEncode(Surface& target, std::chrono::milliseconds wait)
{
    if (target.state == Surface::State::failed) return VA_STATUS_ERROR_ENCODING_ERROR;
    if (target.state != Surface::State::pending) return VA_STATUS_SUCCESS;
    Context* owner = context(target.owner);
    if (!owner || !owner->encoder) {
        target.state = Surface::State::failed;
        return VA_STATUS_ERROR_ENCODING_ERROR;
    }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (target.state == Surface::State::pending) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        harvestEncode(*owner, std::min(left, std::chrono::milliseconds(100)));
        if (owner->encoder->failed() && target.state == Surface::State::pending) {
            target.state = Surface::State::failed;
            break;
        }
    }
    if (target.state == Surface::State::pending) {
        target.state = Surface::State::failed;
        note("an encoded frame never arrived");
        return VA_STATUS_ERROR_ENCODING_ERROR;
    }
    return target.state == Surface::State::ready ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_ENCODING_ERROR;
}

VAStatus Driver::waitForCoded(Buffer& coded)
{
    if (!coded.coded.is || !coded.coded.pending) return VA_STATUS_SUCCESS;
    Surface* target = surface(coded.coded.surface);
    if (!target || target->state != Surface::State::pending) {
        // Whatever it was waiting for has finished, or its surface is gone.
        coded.coded.pending = false;
        return target && target->state == Surface::State::failed ? VA_STATUS_ERROR_ENCODING_ERROR
                                                                : VA_STATUS_SUCCESS;
    }
    const VAStatus status = waitForEncode(*target, kEncodeWait);
    if (status != VA_STATUS_SUCCESS) coded.coded.pending = false;
    return status;
}

}
