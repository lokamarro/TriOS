// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_encoder.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <vector>

namespace uml::testing {

/* A platform video encoder that encodes nothing: it turns each raw frame into
 * a small Annex B (or JFIF) unit whose bytes name the frame, so a test sees
 * from the guest's coded buffer which frame crossed, in what order, and
 * whether it was a key frame. It lets the Virtio video encoder device be
 * tested without VideoToolbox; the platform backend has its own test.
 *
 * Everything a test needs to steer or inspect is a public member. The frame
 * pool is deliberately padded: every plane's stride exceeds its row bytes, so
 * a device that copies with a tight stride corrupts the picture and the test
 * fails on the first row.
 *
 * Output is announced on a pipe, like the real encoder's, and can be held
 * back with `hold` until `release()`, which is how a test reaches the
 * asynchronous paths (coded frames that arrive after the command that
 * queued them returned).
 */
class SyntheticEncoder final : public uml::platform::VideoEncoder {
public:
    using Layout = uml::platform::RawVideoLayout;
    using Codec = uml::platform::VideoCodec;
    using Status = uml::platform::VideoEncodeStatus;
    using Settings = uml::platform::VideoEncodeSettings;
    using Update = uml::platform::VideoEncodeUpdate;
    using Frame = uml::platform::EncodedVideoFrame;
    using Kind = uml::platform::EncodedVideoFrameKind;
    using FrameType = uml::platform::VideoFrameType;

    /* What the device handed to submit(): the frame's planes as the device
     * wrote them (padding included), with the timestamp and the key request. */
    struct Submitted {
        std::uint64_t timestamp {};
        bool forceKey {};
        std::vector<std::vector<std::uint8_t>> planes;
        std::vector<std::uint32_t> strides;
        std::vector<std::uint32_t> rows;
    };

    /* The first byte of a plane's padding, which the device must not touch. */
    static constexpr std::uint8_t paddingByte = 0xee;
    static constexpr std::uint32_t paddingBytes = 24;

    SyntheticEncoder()
    {
        int descriptors[2];
        if (::pipe(descriptors) != 0) throw std::runtime_error("synthetic encoder pipe failed");
        read_ = descriptors[0];
        write_ = descriptors[1];
        for (const int descriptor : descriptors) {
            ::fcntl(descriptor, F_SETFL, ::fcntl(descriptor, F_GETFL) | O_NONBLOCK);
            ::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
        }
    }
    ~SyntheticEncoder() override
    {
        ::close(read_);
        ::close(write_);
    }
    SyntheticEncoder(const SyntheticEncoder&) = delete;
    SyntheticEncoder& operator=(const SyntheticEncoder&) = delete;

    /* Steering. */
    std::size_t pool {4};            // frames in flight before acquireFrame() is null
    bool hold {};                    // keep output back until release()
    Status configureStatus {Status::ok};
    Status updateStatus {Status::ok};
    std::uint32_t gop {};            // key frame every `gop` frames when nonzero
    std::size_t codedSize {};        // pad each coded unit to this size when nonzero
    bool failNext {};                // the next frame comes back as an error entry
    bool separateHeaders {};         // emit parameter sets as their own entry first
    std::uint32_t bFrameEvery {};    // every Nth non-key frame is bidirectional

    /* Inspection. */
    unsigned configures {};
    unsigned resets {};
    unsigned drains {};
    Settings settings;
    std::vector<Update> updates;
    std::vector<Submitted> submitted;
    std::size_t inFlight {};         // submitted and not yet taken

    Status configure(const Settings& requested) override
    {
        ++configures;
        settings = requested;
        if (configureStatus != Status::ok) return configureStatus;
        configured_ = true;
        nextKey_ = true;
        headersSent_ = false;
        return Status::ok;
    }
    Status update(const Update& change) override
    {
        updates.push_back(change);
        return updateStatus;
    }
    std::unique_ptr<uml::platform::WritableVideoFrame> acquireFrame() override
    {
        if (!configured_ || inFlight >= pool) return nullptr;
        return std::make_unique<PoolFrame>(settings.input, settings.width, settings.height);
    }
    void submit(std::unique_ptr<uml::platform::WritableVideoFrame> frame, std::uint64_t timestamp,
                bool forceKeyFrame) override
    {
        auto* pooled = static_cast<PoolFrame*>(frame.get());
        Submitted record;
        record.timestamp = timestamp;
        record.forceKey = forceKeyFrame;
        record.planes = pooled->planes;
        record.strides = pooled->strides;
        record.rows = pooled->rowCounts;
        submitted.push_back(std::move(record));
        ++inFlight;
        const auto number = submitted.size() - 1;
        if (failNext) {
            failNext = false;
            Frame error;
            error.kind = Kind::error;
            error.timestamp = timestamp;
            emit(std::move(error));
            return;
        }
        const bool key = nextKey_ || forceKeyFrame || (gop != 0 && number % gop == 0) || settings.codec == Codec::jpeg;
        nextKey_ = false;
        const auto type = key ? FrameType::key
            : (bFrameEvery != 0 && number % bFrameEvery == 0) ? FrameType::bidirectional : FrameType::predicted;
        if (key && separateHeaders && !headersSent_) {
            Frame headers;
            headers.kind = Kind::parameterSets;
            headers.timestamp = timestamp;
            headers.bytes = parameterSets(settings.codec);
            emit(std::move(headers));
            headersSent_ = true;
        }
        Frame coded;
        coded.kind = Kind::frame;
        coded.type = type;
        coded.timestamp = timestamp;
        coded.bytes = unit(settings.codec, type, number, pooled->planes.empty() || pooled->planes[0].empty()
                                                              ? std::uint8_t(0) : pooled->planes[0][0]);
        if (codedSize > coded.bytes.size()) coded.bytes.resize(codedSize, 0x5a);
        emit(std::move(coded));
    }
    void drain() override
    {
        ++drains;
        Frame marker;
        marker.kind = Kind::drained;
        emit(std::move(marker));
    }
    void reset() override
    {
        ++resets;
        ready_.clear();
        held_.clear();
        inFlight = 0;
        nextKey_ = true;
        headersSent_ = false;
        configured_ = configureStatus == Status::ok && configured_;
        discardPipe();
    }
    std::vector<Frame> takeFrames() override
    {
        discardPipe();
        std::vector<Frame> frames(std::make_move_iterator(ready_.begin()), std::make_move_iterator(ready_.end()));
        ready_.clear();
        for (const auto& frame : frames)
            if ((frame.kind == Kind::frame || frame.kind == Kind::error) && inFlight > 0) --inFlight;
        return frames;
    }
    int readinessDescriptor() const noexcept override { return read_; }

    /* Moves up to `count` held entries to the output and announces them. */
    void release(std::size_t count = ~std::size_t(0))
    {
        bool any = false;
        while (count-- > 0 && !held_.empty()) {
            ready_.push_back(std::move(held_.front()));
            held_.pop_front();
            any = true;
        }
        if (any) announce();
    }
    [[nodiscard]] std::size_t heldCount() const noexcept { return held_.size(); }
    [[nodiscard]] std::size_t readyCount() const noexcept { return ready_.size(); }

    /* A coded unit carries, after its start code and NAL header, the index
     * of the frame (four bytes, little endian) and the first sample of its
     * first plane as the device copied it. */
    static constexpr std::size_t payloadBytes = 5;
    static std::vector<std::uint8_t> parameterSets(Codec codec)
    {
        if (codec == Codec::hevc)
            return {0, 0, 0, 1, 0x40, 0x01, 0xaa, 0, 0, 0, 1, 0x42, 0x01, 0xbb, 0, 0, 0, 1, 0x44, 0x01, 0xcc};
        return {0, 0, 0, 1, 0x67, 0x42, 0xaa, 0, 0, 0, 1, 0x68, 0xce, 0xbb};
    }
    /* The coded unit of frame `number`, which a test compares byte for byte
     * with what reached the guest. */
    static std::vector<std::uint8_t> unit(Codec codec, FrameType type, std::size_t number, std::uint8_t sample)
    {
        std::vector<std::uint8_t> bytes;
        const auto payload = [&bytes, number, sample] {
            for (int shift = 0; shift < 32; shift += 8) bytes.push_back(std::uint8_t(number >> shift));
            bytes.push_back(sample);
        };
        if (codec == Codec::jpeg) {
            bytes = {0xff, 0xd8};
            payload();
            bytes.push_back(0xff);
            bytes.push_back(0xd9);
            return bytes;
        }
        if (type == FrameType::key) bytes = parameterSets(codec);
        const std::uint8_t first = codec == Codec::hevc ? (type == FrameType::key ? 0x26 : 0x02)
                                                        : (type == FrameType::key ? 0x65 : 0x41);
        for (const std::uint8_t byte : {0, 0, 0, 1}) bytes.push_back(byte);
        bytes.push_back(first);
        if (codec == Codec::hevc) bytes.push_back(0x01);
        payload();
        return bytes;
    }

private:
    class PoolFrame final : public uml::platform::WritableVideoFrame {
    public:
        PoolFrame(Layout layout, std::uint32_t width, std::uint32_t height)
        {
            const auto half = [](std::uint32_t value) { return (value + 1) / 2; };
            const auto add = [this](std::uint32_t rowBytes, std::uint32_t count) {
                const auto stride = rowBytes + paddingBytes;
                strides.push_back(stride);
                rowCounts.push_back(count);
                planes.emplace_back(std::size_t(stride) * count, paddingByte);
            };
            switch (layout) {
            case Layout::nv12: add(width, height); add(2 * half(width), half(height)); break;
            case Layout::i420: add(width, height); add(half(width), half(height)); add(half(width), half(height)); break;
            case Layout::p010: add(2 * width, height); add(4 * half(width), half(height)); break;
            case Layout::nv16: add(width, height); add(2 * half(width), height); break;
            case Layout::nv24: add(width, height); add(2 * width, height); break;
            case Layout::bgra: case Layout::rgba: add(4 * width, height); break;
            }
        }
        std::size_t planeCount() const noexcept override { return planes.size(); }
        std::span<std::uint8_t> plane(std::size_t index) noexcept override { return planes[index]; }
        std::uint32_t stride(std::size_t index) const noexcept override { return strides[index]; }
        std::uint32_t rows(std::size_t index) const noexcept override { return rowCounts[index]; }

        std::vector<std::vector<std::uint8_t>> planes;
        std::vector<std::uint32_t> strides;
        std::vector<std::uint32_t> rowCounts;
    };

    void emit(Frame frame)
    {
        if (hold) {
            held_.push_back(std::move(frame));
            return;
        }
        ready_.push_back(std::move(frame));
        announce();
    }
    void announce() noexcept
    {
        const std::uint8_t byte = 1;
        (void)::write(write_, &byte, 1);
    }
    void discardPipe() noexcept
    {
        std::array<std::uint8_t, 64> scratch;
        while (::read(read_, scratch.data(), scratch.size()) > 0) {}
    }

    int read_ {-1};
    int write_ {-1};
    bool configured_ {};
    bool nextKey_ {true};
    bool headersSent_ {};
    std::deque<Frame> ready_;
    std::deque<Frame> held_;
};

} // namespace uml::testing
