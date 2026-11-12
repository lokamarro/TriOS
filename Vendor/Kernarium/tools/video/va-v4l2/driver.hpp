// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "encode.hpp"
#include "storage.hpp"
#include "v4l2_decoder.hpp"
#include "vp9.hpp"

namespace uml::va {

struct ConstPlaneView;

// The decoder profiles this driver can serve, and the V4L2 coded format each
// one needs from the Virtio video device.
struct CodecSupport {
    VAProfile profile;
    std::uint32_t v4l2Format;
    unsigned minWidth;
    unsigned minHeight;
    unsigned maxWidth;
    unsigned maxHeight;
};

struct Config {
    VAProfile profile {VAProfileNone};
    VAEntrypoint entrypoint {VAEntrypointVLD};
    unsigned rtFormat {};
    // The one VA_RC_* mode an encode configuration runs in.
    unsigned rateControl {};
};

inline bool isEncodeEntrypoint(VAEntrypoint entrypoint) noexcept
{
    return entrypoint == VAEntrypointEncSlice || entrypoint == VAEntrypointEncSliceLP;
}

struct Buffer {
    VABufferType type {};
    unsigned size {};
    unsigned elements {};
    std::vector<std::uint8_t> owned;
    // An image derived from a surface maps the surface's memory instead of a
    // copy, so the buffer points at it rather than owning it.
    std::uint8_t* external {};
    std::size_t externalSize {};
    // Keeps the surface allocation an image was derived from alive, because a
    // surface that decodes again may be handed a different buffer.
    std::shared_ptr<PixelBuffer> pin;
    // A coded buffer is where an encoder delivers a finished frame. A client
    // maps it to a VACodedBufferSegment that describes the frame, and the
    // frame's bytes are in `owned`.
    struct Coded {
        bool is {};
        VACodedBufferSegment segment {};
        // The input surface of the frame that will fill the buffer, while
        // there is one in flight.
        bool pending {};
        VASurfaceID surface {VA_INVALID_SURFACE};
    } coded;
    std::uint8_t* data() { return external ? external : owned.data(); }
    // How many bytes data() addresses. An application may resize a buffer it
    // was handed, so an image's buffer is checked against the image's size
    // before every copy.
    std::size_t bytes() const noexcept { return external ? externalSize : owned.size(); }
};

struct Surface {
    unsigned width {};
    unsigned height {};
    std::uint32_t fourcc {};
    std::shared_ptr<PixelBuffer> buffer;
    // When the decoder delivers packed RGB, the picture lands here and the
    // NV12 buffer above is filled only if a client really asks for NV12, or
    // holds that buffer already (`exported`).
    std::shared_ptr<PixelBuffer> shadow;
    bool nativeValid {true};
    // Set once a client has been given this surface's own memory - a dma-buf
    // descriptor or a derived image. The memory is then the surface's
    // identity, so a later picture has to be copied into it rather than the
    // surface being pointed at a different buffer.
    bool exported {};
    // Set when the descriptor this surface handed out is its RGB shadow, as
    // packed ARGB, instead of the NV12 buffer. The shadow is then the memory
    // the client looks at: every picture lands in it, and NV12 is made only
    // for a client that asks for it.
    bool exportsArgb {};
    enum class State { idle, pending, ready, failed };
    State state {State::idle};
    std::uint64_t sequence {};
    VAContextID owner {VA_INVALID_ID};
};

struct Context {
    VAConfigID config {VA_INVALID_ID};
    VAProfile profile {VAProfileNone};
    VAEntrypoint entrypoint {VAEntrypointVLD};
    unsigned width {};
    unsigned height {};
    std::unique_ptr<V4l2Decoder> decoder;
    std::unique_ptr<VaEncoder> encoder;
    // An ARGB input converted to NV12 for the encoder.
    std::vector<std::uint8_t> encodeScratch;
    bool reportLayout {};
    Vp9Context vp9;
    std::uint64_t sequence {};
    // The frame being assembled between vaBeginPicture and vaEndPicture.
    VASurfaceID target {VA_INVALID_SURFACE};
    bool picture {};
    std::vector<std::uint8_t> sliceData;
    // Which surface last wrote each VP9 reference slot, so that a
    // show_existing_frame submission can be served without the decoder.
    VASurfaceID slot[8] {};
    std::unordered_map<std::uint64_t, VASurfaceID> awaiting;
    std::string failure;
    // The buffers lent to the decoder's capture queue, one for each slot, and
    // the ones it has handed back. A buffer is free again when nothing but the
    // pool refers to it.
    std::vector<std::shared_ptr<PixelBuffer>> captureSlot;
    std::vector<std::shared_ptr<PixelBuffer>> captureSpare;
    // Video processing only.
    VAProcPipelineParameterBuffer pipeline {};
    bool havePipeline {};
    VARectangle pipelineSource {};
    VARectangle pipelineDestination {};
    bool pipelineSourceValid {};
    bool pipelineDestinationValid {};
};

struct Image {
    VAImage image {};
};

class Driver {
public:
    // `pool` is where the driver gets the V4L2 descriptors its sessions run on;
    // a process has one, and a test brings its own.
    explicit Driver(VADriverContextP context, DescriptorPool& pool);
    ~Driver();

    DescriptorPool& pool() noexcept { return pool_; }

    std::mutex& lock() noexcept { return lock_; }

    const std::vector<CodecSupport>& codecs() const noexcept { return codecs_; }
    const CodecSupport* codec(VAProfile profile) const noexcept;
    const std::string& device() const noexcept { return device_; }
    // What the encoder node lets this driver advertise; null when there is no
    // encoder.
    const EncodeSupport* encoding() const noexcept { return encoding_.get(); }
    const std::string& encoderDevice() const noexcept { return encoderDevice_; }

    std::map<VAConfigID, Config> configs;
    std::map<VAContextID, std::unique_ptr<Context>> contexts;
    std::map<VASurfaceID, std::unique_ptr<Surface>> surfaces;
    std::map<VABufferID, std::unique_ptr<Buffer>> buffers;
    std::map<VAImageID, std::unique_ptr<Image>> images;
    // Every allocation this driver still owns, by the identity of the
    // descriptor it exports, so that a client handing one back is served the
    // same memory instead of a copy.
    std::unordered_map<std::uint64_t, std::weak_ptr<PixelBuffer>> known;

    VAConfigID nextConfig {0x01000000};
    VAContextID nextContext {0x02000000};
    VASurfaceID nextSurface {0x03000000};
    VABufferID nextBuffer {0x04000000};
    VAImageID nextImage {0x05000000};

    Surface* surface(VASurfaceID id);
    Context* context(VAContextID id);
    Buffer* buffer(VABufferID id);
    // Finds an allocation this driver already owns by the identity of a
    // descriptor a client handed back.
    std::shared_ptr<PixelBuffer> knownBuffer(std::uint64_t identity);

    // Decode plumbing.
    VAStatus submitFrame(Context& context, VASurfaceID id, Surface& target);
    VAStatus waitFor(Surface& surface, std::chrono::milliseconds wait);
    // Collects decoded pictures; the context must have a decoder.
    void harvest(Context& context, std::chrono::milliseconds wait);
    // Lends the decoder a buffer for one capture slot, and keeps a pool of the
    // ones it has given back.
    // `fresh` starts a new queue: the buffers lent to the old one go back to
    // the pool first. Refilling one slot of a live queue never does that.
    bool provideCapture(Context& context, unsigned index, CaptureTarget& target, bool fresh);
    // A buffer for one capture slot; the context must have a decoder.
    std::shared_ptr<PixelBuffer> captureBuffer(Context& context);
    void recycleCapture(Context& context, std::shared_ptr<PixelBuffer> buffer);
    // Copies a lent buffer's picture into a surface that has to keep its own
    // memory, because a client already holds it.
    bool fillFromLent(Surface& target, PixelBuffer& lent, bool packed);
    // Copies packed pixels into a surface's RGB shadow, allocating it first,
    // and leaves the NV12 buffer stale until somebody asks for it - unless a
    // client already holds that buffer, which then gets the picture at once.
    bool storePacked(Surface& target, const ConstPlaneView& source, unsigned width, unsigned height);
    // Brings the RGB shadow of a surface that exports it up to date from the
    // surface's NV12 buffer, after something wrote that buffer.
    void refreshExport(Surface& surface);
    // Whether a surface's exported descriptor is packed ARGB rather than NV12
    // (UML_VA_V4L2_EXPORT=argb).
    bool exportsPackedArgb() const noexcept { return exportArgb_; }
    VAStatus process(Context& context, Surface& target);

    // Encode plumbing. Profiles come from the encoder node, so a profile it
    // did not verify is never listed.
    void encodeProfiles(std::vector<VAProfile>& out) const;
    bool isEncodeProfile(VAProfile profile) const noexcept;
    VAStatus encodeAttributes(VAProfile profile, VAConfigAttrib* attributes, int count) const;
    VAStatus createEncodeConfig(VAProfile profile, VAEntrypoint entrypoint, const VAConfigAttrib* attributes,
                                int count, Config& config) const;
    VAStatus openEncoder(Context& context, const Config& config);
    // One buffer of the picture being assembled.
    VAStatus addEncodeBuffer(Context& context, const Buffer& buffer);
    VAStatus submitEncode(Context& context, VASurfaceID id, Surface& target);
    // Collects finished frames into their coded buffers.
    void harvestEncode(Context& context, std::chrono::milliseconds wait);
    VAStatus waitForEncode(Surface& surface, std::chrono::milliseconds wait);
    // Waits for the frame that will fill a coded buffer.
    VAStatus waitForCoded(Buffer& buffer);

    // Fills a surface's NV12 buffer from its RGB shadow, if it has one and the
    // buffer is not already current. A client that only ever uses the video
    // processing path never pays for this.
    void materialise(Surface& surface);
    // Reports the first decoded picture as the device delivered it.
    void reportPicture(const DecodedPicture& picture, bool packed);
    // The same, for a picture the device wrote straight into a lent buffer.
    void reportLent(PixelBuffer& buffer, bool packed);
    bool wantsReport();
    // The decoded format to ask a new decode session for: packed RGB when the
    // device offers it and the policy wants it, otherwise zero for NV12.
    std::uint32_t preferredCapture();
    // Whether to offer the decoder this driver's own buffers for its capture
    // queue. The queue itself decides whether to take them.
    bool importsCapture();

    void note(const char* message) const;
    // Whether UML_VA_V4L2_TRACE asked for the extra diagnostics.
    bool tracing() const noexcept { return tracing_; }
    void setTracing(bool on) noexcept { tracing_ = on; }

private:
    bool tracing_ {};
    void probeCodecs();
    // Learns once what the encoder node offers.
    void probeEncoding();
    // Learns once what the decoder's capture queue can do, on a descriptor the
    // pool lent for the purpose.
    void probeCapture(int descriptor);

    DescriptorPool& pool_;
    std::mutex lock_;
    std::vector<CodecSupport> codecs_;
    std::string device_;
    std::shared_ptr<const EncodeSupport> encoding_;
    std::string encoderDevice_;
    RgbCapture rgbPolicy_ {RgbCapture::automatic};
    CaptureMemory capturePolicy_ {CaptureMemory::automatic};
    bool exportArgb_ {};
    // Probed once at initialisation, before a sandbox can close the device nodes.
    bool captureProbed_ {};
    bool rgbOffered_ {};
    bool lendCapture_ {};
    int reportedPictures_ {};
    int reportLimit_ {-1};
};

// Fills a libva driver context with this driver's entry points over a new
// Driver, which the context owns from then on. __vaDriverInit_1_22 is this
// function on the process's pool.
VAStatus initializeDriver(VADriverContextP ctx, DescriptorPool& pool);

}
