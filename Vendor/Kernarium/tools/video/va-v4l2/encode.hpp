// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <va/va.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "encode_map.hpp"
#include "v4l2_encoder.hpp"

namespace uml::va {

// One VA-API encode context: the buffers of each picture a client submits,
// turned into a V4L2 encode session and the frames it is fed.
//
// The V4L2 session starts with the first picture, because the controls that
// configure a stream - profile, level, rate control mode, key frame interval -
// cannot change once it runs, and VA-API only names them in the first
// picture's buffers. What a client changes afterwards, the bit rate above all,
// is applied between pictures through the controls the device lets change
// while streaming.
class VaEncoder {
public:
    // `lease` is the node descriptor the context runs on. `width` and `height`
    // are the picture size the context was created for.
    VaEncoder(std::shared_ptr<const EncodeSupport> support, VAProfile profile, unsigned rateControl, unsigned width,
              unsigned height, Lease lease);
    ~VaEncoder();

    VaEncoder(const VaEncoder&) = delete;
    VaEncoder& operator=(const VaEncoder&) = delete;

    // Forgets the buffers of the previous picture.
    void beginPicture();
    // One buffer submitted with vaRenderPicture. Buffers the driver does not
    // act on - packed headers, hints - are accepted and ignored.
    VAStatus add(VABufferType type, const void* data, std::size_t size);

    // What the picture's buffers have named so far.
    const EncodeState& state() const noexcept { return state_; }

    // Encodes the picture whose buffers have been added. `frame` is the whole
    // surface; the stream's size, which the first picture's sequence parameters
    // name, decides how much of it is the picture. `surface` comes back with
    // the finished frame, and `token` names the submission. On failure `error`
    // says why.
    VAStatus encode(const RawFrame& frame, VASurfaceID surface, std::uint64_t& token, std::string& error);

    struct Completed {
        std::uint64_t token {};
        VASurfaceID surface {VA_INVALID_SURFACE};
        VABufferID codedBuffer {VA_INVALID_ID};
        EncodedFrame frame;
        // The picture's buffers asked for an IDR picture.
        bool keyWanted {};
    };
    // The oldest finished frame, waiting up to `wait` for it. False with an
    // empty `error` means none arrived in time.
    bool poll(Completed& completed, std::chrono::milliseconds wait, std::string& error);

    unsigned inFlight() const noexcept { return unsigned(pending_.size()); }
    bool failed() const noexcept { return failed_; }
    // What the session skipped or could not apply, taken for the driver's log.
    std::vector<std::string> takeNotes();
    unsigned width() const noexcept { return width_; }
    unsigned height() const noexcept { return height_; }
    // The size of the stream once it has started, and 0 before that.
    unsigned streamWidth() const noexcept { return streamWidth_; }
    unsigned streamHeight() const noexcept { return streamHeight_; }

private:
    struct Pending {
        std::uint64_t token {};
        VASurfaceID surface {VA_INVALID_SURFACE};
        VABufferID codedBuffer {VA_INVALID_ID};
        bool keyWanted {};
    };

    VAStatus start(const RawFrame& frame, std::string& error);

    std::shared_ptr<const EncodeSupport> support_;
    VAProfile profile_;
    unsigned rateControl_;
    unsigned width_;
    unsigned height_;
    Lease lease_;
    std::unique_ptr<V4l2Encoder> encoder_;
    EncodeState state_;
    // The state the running stream was last configured from.
    EncodeState applied_;
    std::deque<Pending> pending_;
    std::uint64_t counter_ {};
    // The size the first picture's sequence parameters named, which a later
    // picture must not change, and the size of the stream that came of it.
    unsigned sequenceWidth_ {};
    unsigned sequenceHeight_ {};
    unsigned streamWidth_ {};
    unsigned streamHeight_ {};
    bool failed_ {};
    std::vector<std::string> notes_;
};

}
