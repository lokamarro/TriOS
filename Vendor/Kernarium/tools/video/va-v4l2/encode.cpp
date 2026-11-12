// SPDX-License-Identifier: GPL-2.0-or-later
#include "encode.hpp"

#include <linux/videodev2.h>

#include <algorithm>

namespace uml::va {
namespace {

// V4L2 carries a buffer timestamp as a timeval, so a submission key has to be a
// whole microsecond to survive the round trip through both queues.
constexpr std::uint64_t kTokenUnit = 1000;

}

VaEncoder::VaEncoder(std::shared_ptr<const EncodeSupport> support, VAProfile profile, unsigned rateControl,
                     unsigned width, unsigned height, Lease lease)
    : support_(std::move(support)), profile_(profile), rateControl_(rateControl), width_(width), height_(height),
      lease_(std::move(lease))
{
}

VaEncoder::~VaEncoder()
{
    // A context that never encoded a picture has not touched the descriptor.
    encoder_.reset();
    lease_.giveBack(true, false);
}

void VaEncoder::beginPicture()
{
    state_.picture = {};
    state_.slice = {};
}

VAStatus VaEncoder::add(VABufferType type, const void* data, std::size_t size)
{
    const EncodeProfile* profile = support_->profile(profile_);
    const bool hevc = profile && profile->codec == V4L2_PIX_FMT_HEVC;
    switch (type) {
    case VAEncSequenceParameterBufferType:
        return (hevc ? readHevcSequence(data, size, state_.sequence) : readH264Sequence(data, size, state_.sequence))
                   ? VA_STATUS_SUCCESS
                   : VA_STATUS_ERROR_INVALID_PARAMETER;
    case VAEncPictureParameterBufferType:
        return (hevc ? readHevcPicture(data, size, state_.picture) : readH264Picture(data, size, state_.picture))
                   ? VA_STATUS_SUCCESS
                   : VA_STATUS_ERROR_INVALID_PARAMETER;
    case VAEncSliceParameterBufferType:
        // One slice a picture is all that is offered. A client that sends
        // more has them all described by the first.
        if (!state_.slice.valid)
            return (hevc ? readHevcSlice(data, size, state_.slice) : readH264Slice(data, size, state_.slice))
                       ? VA_STATUS_SUCCESS
                       : VA_STATUS_ERROR_INVALID_PARAMETER;
        return VA_STATUS_SUCCESS;
    case VAEncMiscParameterBufferType:
        return readMisc(data, size, state_) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_PARAMETER;
    default:
        return VA_STATUS_SUCCESS;
    }
}

VAStatus VaEncoder::start(const RawFrame& frame, std::string& error)
{
    const SequenceInfo& sequence = state_.sequence;
    if (!sequence.valid) { error = "the first picture came without sequence parameters"; return VA_STATUS_ERROR_INVALID_PARAMETER; }
    if (sequence.ipPeriod > 1) { error = "B-frames are not offered"; return VA_STATUS_ERROR_INVALID_PARAMETER; }
    if ((sequence.width | sequence.height) & 1u) { error = "an odd picture size"; return VA_STATUS_ERROR_INVALID_PARAMETER; }
    // The coded size a client names is rounded up to the encoder's alignment, so
    // a context and its surfaces may be a few rows short of it.
    if (sequence.codedWidth > width_ + 15 || sequence.codedHeight > height_ + 15 || sequence.width > width_ + 15 ||
        sequence.height > height_ + 15) {
        error = "the sequence is larger than the context";
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    // A surface a few rows short of the stream is the picture: HEVC sequence
    // parameters cannot name a crop, so a client that rounds its coded size up
    // (FFmpeg does, to 16) hands in surfaces of the size it actually wants.
    // Anything shorter than that is a client error.
    const unsigned width = std::min(sequence.width, frame.width) & ~1u;
    const unsigned height = std::min(sequence.height, frame.height) & ~1u;
    if (sequence.width - width > 15 || sequence.height - height > 15 || !width || !height) {
        error = "the surface is smaller than the stream";
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (!state_.picture.valid) { error = "the first picture came without picture parameters"; return VA_STATUS_ERROR_INVALID_PARAMETER; }
    const EncodeProfile* profile = support_->profile(profile_);
    if (!profile) { error = "unsupported profile"; return VA_STATUS_ERROR_UNSUPPORTED_PROFILE; }

    V4l2Encoder::Settings settings;
    settings.codec = profile->codec;
    settings.width = width;
    settings.height = height;
    const FrameRateInfo rate = effectiveFrameRate(state_);
    settings.frameRateNumerator = rate.numerator;
    settings.frameRateDenominator = rate.denominator;
    settings.controls = startupControls(*support_, profile_, rateControl_, state_);
    encoder_ = V4l2Encoder::open(std::move(lease_), support_->capabilities, settings, error);
    if (!encoder_) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    sequenceWidth_ = sequence.width;
    sequenceHeight_ = sequence.height;
    streamWidth_ = width;
    streamHeight_ = height;
    applied_ = state_;
    for (auto& note : encoder_->notes()) notes_.push_back(std::move(note));
    encoder_->clearNotes();
    return VA_STATUS_SUCCESS;
}

VAStatus VaEncoder::encode(const RawFrame& frame, VASurfaceID surface, std::uint64_t& token, std::string& error)
{
    if (failed_) { error = "the encoder failed earlier"; return VA_STATUS_ERROR_ENCODING_ERROR; }
    if (!encoder_) {
        const VAStatus status = start(frame, error);
        if (status != VA_STATUS_SUCCESS) { failed_ = true; return status; }
    } else {
        if (state_.sequence.valid &&
            (state_.sequence.width != sequenceWidth_ || state_.sequence.height != sequenceHeight_)) {
            error = "a stream cannot change its size; the client needs a new context";
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        // What changed since the last picture and can change under a running
        // stream is applied before this picture goes in.
        const auto controls = dynamicControls(*support_, profile_, rateControl_, applied_, state_);
        if (!controls.empty()) encoder_->update(controls);
        const FrameRateInfo before = effectiveFrameRate(applied_);
        const FrameRateInfo now = effectiveFrameRate(state_);
        if (before.numerator != now.numerator || before.denominator != now.denominator) {
            std::string ignored;
            encoder_->setFrameRate(now.numerator, now.denominator, ignored);
        }
        applied_ = state_;
    }
    if (frame.width < streamWidth_ || frame.height < streamHeight_) {
        error = "the surface is smaller than the stream";
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    RawFrame visible = frame;
    visible.width = streamWidth_;
    visible.height = streamHeight_;
    token = ++counter_ * kTokenUnit;
    const bool key = state_.picture.idr;
    if (!encoder_->submit(visible, token, key, error)) {
        failed_ = true;
        return VA_STATUS_ERROR_ENCODING_ERROR;
    }
    pending_.push_back({token, surface, state_.picture.codedBuffer, key});
    for (auto& note : encoder_->notes()) notes_.push_back(std::move(note));
    encoder_->clearNotes();
    return VA_STATUS_SUCCESS;
}

bool VaEncoder::poll(Completed& completed, std::chrono::milliseconds wait, std::string& error)
{
    if (!encoder_ || pending_.empty()) return false;
    EncodedFrame frame;
    if (!encoder_->collect(frame, wait, error)) {
        if (!error.empty()) failed_ = true;
        return false;
    }
    // The device returns frames in the order they went in, each with the
    // timestamp of the picture it encodes.
    auto found = std::find_if(pending_.begin(), pending_.end(),
                              [&frame](const Pending& entry) { return entry.token == frame.timestamp; });
    if (found == pending_.end()) {
        error = "the encoder returned a frame nobody submitted";
        failed_ = true;
        return false;
    }
    completed.token = found->token;
    completed.surface = found->surface;
    completed.codedBuffer = found->codedBuffer;
    completed.keyWanted = found->keyWanted;
    completed.frame = std::move(frame);
    pending_.erase(found);
    return true;
}

std::vector<std::string> VaEncoder::takeNotes()
{
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

}
