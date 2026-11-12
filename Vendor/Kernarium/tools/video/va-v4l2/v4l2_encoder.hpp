// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "v4l2_io.hpp"
#include "v4l2_pool.hpp"

namespace uml::va {

// A control the encoder node publishes. The host publishes exactly what an
// encode proved on its hardware, so a control that is absent is unsupported.
struct ControlInfo {
    std::uint32_t id {};
    std::uint32_t type {};
    std::int64_t minimum {};
    std::int64_t maximum {};
    std::int64_t step {1};
    std::int64_t defaultValue {};
    std::uint32_t flags {};
    // Bit n is set when menu item n can be selected. Only menu types have one.
    std::uint64_t menuMask {};
};

// A coded format the encoder produces, and the frame sizes it accepts for it.
struct EncodeCodec {
    std::uint32_t format {};
    unsigned minWidth {16};
    unsigned minHeight {16};
    unsigned maxWidth {4096};
    unsigned maxHeight {4096};
    unsigned stepWidth {2};
    unsigned stepHeight {2};
};

// What a descriptor on an encoder node offers.
struct EncoderCapabilities {
    std::string device;
    // The OUTPUT queue's raw formats, in the order the device lists them.
    std::vector<std::uint32_t> rawFormats;
    std::vector<EncodeCodec> codecs;
    std::map<std::uint32_t, ControlInfo> controls;

    const EncodeCodec* codec(std::uint32_t format) const noexcept;
    const ControlInfo* control(std::uint32_t id) const noexcept;
    bool offers(std::uint32_t id) const noexcept { return control(id) != nullptr; }
    // Whether a menu control offers item `item`.
    bool offersItem(std::uint32_t id, unsigned item) const noexcept;
    bool offersRaw(std::uint32_t format) const noexcept;
};

// Reads the formats, frame sizes and controls an encoder node publishes. It
// changes nothing on the device. Returns false, with `error` set, for a node
// that is not a stateful encoder: one with no compressed CAPTURE format.
bool probeEncoder(V4l2Io& io, int descriptor, EncoderCapabilities& capabilities, std::string& error);

// The encoder node: UML_VA_V4L2_ENCODER_DEVICE, or the first node under /dev
// that probes as a stateful encoder. Only meant for an unsandboxed process.
std::string findEncoderDevice(V4l2Io& io);

struct ControlValue {
    std::uint32_t id {};
    std::int64_t value {};
};

// An NV12 picture in guest memory.
struct RawFrame {
    const std::uint8_t* luma {};
    std::size_t lumaPitch {};
    const std::uint8_t* chroma {};
    std::size_t chromaPitch {};
    unsigned width {};
    unsigned height {};
};

struct EncodedFrame {
    // The timestamp of the picture the data encodes.
    std::uint64_t timestamp {};
    std::vector<std::uint8_t> data;
    bool key {};
    // The device could not encode the picture.
    bool error {};
};

// One stateful V4L2 encode session: a descriptor on the encoder node, an
// OUTPUT queue of raw frames and a CAPTURE queue of coded buffers. All calls
// are serialised by the caller.
//
// Frames are submitted and collected independently, so a client may keep
// several in flight. The device returns them in submission order, each with the
// timestamp of the picture it encodes. Whenever a submission has to wait for a
// raw buffer, the coded buffers that have arrived are collected meanwhile, so
// that a client which submits many frames before collecting any cannot stall
// the device.
class V4l2Encoder {
public:
    struct Settings {
        std::uint32_t codec {};
        unsigned width {};
        unsigned height {};
        unsigned frameRateNumerator {30};
        unsigned frameRateDenominator {1};
        // Applied in order before the stream starts. A control the device does
        // not publish, or a value it does not offer, is skipped and noted.
        std::vector<ControlValue> controls;
        unsigned rawBuffers {4};
        unsigned codedBuffers {4};
    };

    // `lease` is the descriptor the session runs on. It goes back to the pool
    // when the session ends.
    static std::unique_ptr<V4l2Encoder> open(Lease lease, std::shared_ptr<const EncoderCapabilities> capabilities,
                                             const Settings& settings, std::string& error);
    // Stops and releases both queues and gives the descriptor back: to the
    // pool as it is when every step worked, and closed when one did not.
    ~V4l2Encoder();

    V4l2Encoder(const V4l2Encoder&) = delete;
    V4l2Encoder& operator=(const V4l2Encoder&) = delete;

    // Hands the encoder one picture. `forceKey` makes it a key frame.
    bool submit(const RawFrame& frame, std::uint64_t timestamp, bool forceKey, std::string& error);

    // Changes controls while the stream runs. A control the device holds fixed
    // while streaming, or does not publish, is skipped and noted, and the
    // others still apply. Returns the number applied.
    unsigned update(const std::vector<ControlValue>& controls);
    bool setFrameRate(unsigned numerator, unsigned denominator, std::string& error);

    // Collects one coded frame if one is ready, waiting up to `wait` for it.
    // False with an empty `error` means none arrived in time.
    bool collect(EncodedFrame& frame, std::chrono::milliseconds wait, std::string& error);

    // Pictures submitted and not yet collected.
    unsigned inFlight() const noexcept { return unsigned(submitted_ - collected_); }

    // How long a submission waits for a raw buffer before giving up.
    void setBound(std::chrono::milliseconds bound) noexcept { bound_ = bound; }

    std::uint32_t rawFormat() const noexcept { return rawFormat_; }
    unsigned rawPlanes() const noexcept { return rawPlanes_; }
    std::size_t codedBufferSize() const noexcept { return codedSize_; }
    // What the session skipped or could not apply, for the driver's log.
    const std::vector<std::string>& notes() const noexcept { return notes_; }
    void clearNotes() noexcept { notes_.clear(); }

private:
    struct Plane {
        std::uint8_t* data {};
        std::size_t length {};
    };
    struct Buffer {
        Plane planes[3];
        unsigned count {};
        bool queued {};
    };

    V4l2Encoder() = default;
    bool negotiate(const Settings& settings, std::string& error);
    bool allocate(unsigned type, unsigned count, std::vector<Buffer>& buffers, std::string& error);
    void unmap(std::vector<Buffer>& buffers);
    void resetControls();
    bool applyControl(const ControlValue& control, bool streaming);
    bool setControl(std::uint32_t id, std::int64_t value, int& error);
    bool reclaimRaw(std::string& error);
    bool pump(std::string& error);
    bool queueCoded(unsigned index, std::string& error);
    void copyFrame(const RawFrame& frame, Buffer& target) const;

    Lease lease_;
    V4l2Io* io_ {};
    int fd_ {-1};
    std::shared_ptr<const EncoderCapabilities> capabilities_;
    std::chrono::milliseconds bound_ {5000};
    std::uint32_t rawFormat_ {};
    unsigned rawPlanes_ {};
    unsigned width_ {};
    unsigned height_ {};
    // The device's own layout of a raw frame: bytes per row and bytes per
    // plane. A frame in one plane carries the chroma straight after the luma.
    std::size_t pitch_[3] {};
    std::size_t planeSize_[3] {};
    std::size_t codedSize_ {};
    std::vector<Buffer> raw_;
    std::vector<Buffer> coded_;
    bool rawStreaming_ {};
    bool codedStreaming_ {};
    std::deque<EncodedFrame> ready_;
    std::uint64_t submitted_ {};
    std::uint64_t collected_ {};
    std::vector<std::string> notes_;
};

}
