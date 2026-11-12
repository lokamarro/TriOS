// SPDX-License-Identifier: GPL-2.0-or-later
#include "driver.hpp"

#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

#include <va/va_dec_vp9.h>
#include <va/va_drmcommon.h>

#include "convert.hpp"

namespace uml::va {
namespace {

// Chromium picks how it hands a planar buffer to a driver by the vendor string:
// one descriptor per plane (DRM_PRIME_2) for the drivers it lists, and for any
// other a single descriptor with every plane at an offset in it. The planes of
// a buffer allocated here are separate resources of the guest GPU, each with
// its own descriptor (gbm/planar.hpp), so this driver names the class of driver
// that takes the descriptor-per-plane import and follows with its own name.
constexpr const char* kVendor = "Mesa Gallium driver (Kernarium Virtio video V4L2 backend 1.0)";
constexpr std::chrono::milliseconds kDecodeWait {4000};

// The hardware writes limited-range YCbCr; the usual switch between the two
// matrices is the picture height.
bool isBt709(unsigned height) noexcept { return height >= 720; }

// A stateful decoder emits shown frames only, so a VP9 frame with
// show_frame == 0 would leave its surface empty - and a later
// show_existing_frame displays exactly that surface, without ever reaching a
// VA-API driver. Asking the decoder to show the hidden frame straight away,
// through a two-frame superframe, fills the surface without disturbing the
// reference state. UML_VA_V4L2_HIDDEN=skip submits the frame on its own
// instead, which is enough for streams that never show a hidden frame again.
bool showHiddenFrames() noexcept
{
    static const bool show = [] {
        const char* request = std::getenv("UML_VA_V4L2_HIDDEN");
        return !request || std::strcmp(request, "skip") != 0;
    }();
    return show;
}

// A view of a mapped buffer's first plane, writable when the mapping is.
template <class Byte>
using PlaneViewOf = std::conditional_t<std::is_const_v<Byte>, ConstPlaneView, PlaneView>;

template <class Byte>
PlaneViewOf<Byte> planeView(const PixelBuffer& buffer, Byte* base) noexcept
{
    return {base + buffer.offset(0), buffer.pitch(0)};
}

// Views of a mapped NV12 buffer's two planes from the pixel at (x, y). A
// chroma row covers two luma rows and a sample pair two columns, so the
// chroma view starts at the pair holding that pixel.
template <class Byte>
std::array<PlaneViewOf<Byte>, 2> planeViews(const PixelBuffer& buffer, Byte* base, unsigned x = 0,
                                            unsigned y = 0) noexcept
{
    return {{{base + buffer.offset(0) + y * buffer.pitch(0) + x, buffer.pitch(0)},
             {base + buffer.offset(1) + (y / 2) * buffer.pitch(1) + (x & ~1u), buffer.pitch(1)}}};
}

unsigned lowestSlot(unsigned mask) noexcept
{
    for (unsigned slot = 0; slot < 8; ++slot)
        if (mask & (1u << slot)) return slot;
    return 8;
}

// Sizes a buffer whose size the application chose. An impossible size is an
// allocation failure; an exception must not unwind through libva's C
// interface into the calling process.
bool resizeOwned(std::vector<std::uint8_t>& owned, std::uint64_t size) noexcept
{
    try {
        owned.resize(size);
        return true;
    } catch (const std::bad_alloc&) {
    } catch (const std::length_error&) {
    }
    return false;
}

}

Driver::Driver(VADriverContextP context, DescriptorPool& pool) : pool_(pool)
{
    if (const auto* state = static_cast<const drm_state*>(context->drm_state))
        useClientDrmDescriptor(state->fd);
    selectStorage(std::getenv("UML_VA_V4L2_SURFACES"));
    // Which backing store a client's surfaces will come from decides whether
    // an exported handle is a dma-buf at all, so it belongs in the log.
    note((std::string("surfaces ") + storageName(selectedStorage()) + " (" + storageReason() + ")").c_str());
    rgbPolicy_ = rgbCaptureRequest(std::getenv("UML_VA_V4L2_RGB"));
    capturePolicy_ = captureMemoryRequest(std::getenv("UML_VA_V4L2_CAPTURE"));
    if (const char* request = std::getenv("UML_VA_V4L2_EXPORT")) exportArgb_ = std::strcmp(request, "argb") == 0;
    probeCodecs();
    probeEncoding();
}

Driver::~Driver() = default;

void Driver::note(const char* message) const
{
    static const char* path = std::getenv("UML_VA_V4L2_LOG");
    if (!path || !*path) return;
    if (std::FILE* file = std::fopen(path, "ae")) {
        std::fprintf(file, "uml-va %d %s\n", int(getpid()), message);
        std::fclose(file);
    }
}

namespace {

// UML_VA_V4L2_TRACE=1 logs every entry point a client calls, and what it
// answered, to the UML_VA_V4L2_LOG file. The line is written before the call,
// so the last line of a client that died inside the driver names the call.
bool traceRequested() noexcept
{
    const char* request = std::getenv("UML_VA_V4L2_TRACE");
    return request && *request && std::strcmp(request, "0") != 0;
}

void traceLine(const char* format, const char* name, long value)
{
    static const char* path = std::getenv("UML_VA_V4L2_LOG");
    if (!path || !*path) return;
    if (std::FILE* file = std::fopen(path, "ae")) {
        std::fprintf(file, format, int(getpid()), name, value);
        std::fclose(file);
    }
}

template <std::size_t N>
struct EntryName {
    char text[N];
    constexpr EntryName(const char (&source)[N])
    {
        for (std::size_t index = 0; index < N; ++index) text[index] = source[index];
    }
};

template <EntryName label, auto Function>
struct Traced;

template <EntryName label, typename Result, typename... Arguments, Result (*Function)(Arguments...)>
struct Traced<label, Function> {
    static Result call(Arguments... arguments)
    {
        traceLine("uml-va %d call %s %ld\n", label.text, 0);
        const Result result = Function(arguments...);
        traceLine("uml-va %d done %s %ld\n", label.text, long(result));
        return result;
    }
};

}

void Driver::probeCodecs()
{
    // The descriptors a session will need are opened here, at initialisation,
    // because a browser engages its sandbox after initialising VA-API and no
    // device node can be opened after that.
    auto& pool = pool_;
    const PoolSizes sizes = poolSizes(std::getenv("UML_VA_V4L2_POOL"));
    pool.setTarget(Role::decoder, sizes.decoders);
    pool.setTarget(Role::encoder, sizes.encoders);
    device_ = pool.knownDevice(Role::decoder);
    if (device_.empty()) device_ = findDecoderDevice(V4L2_PIX_FMT_VP9);
    if (device_.empty()) { note("no Virtio video decoder node"); return; }
    pool.fill(Role::decoder, device_);
    std::string error;
    Lease lease = pool.acquire(Role::decoder, device_, error);
    if (!lease) { note(error.c_str()); return; }
    // Only the codecs whose VA-API submissions this driver can turn back into
    // the byte stream a stateful decoder needs are advertised. H.264 and HEVC
    // need their parameter sets rebuilt from the picture parameters first.
    CodecSupport vp9 {VAProfileVP9Profile0, V4L2_PIX_FMT_VP9, 16, 16, 4096, 2304};
    queryCodecLimits(lease.fd(), V4L2_PIX_FMT_VP9, vp9.minWidth, vp9.minHeight, vp9.maxWidth, vp9.maxHeight);
    codecs_.push_back(vp9);
    probeCapture(lease.fd());
    // Nothing above changed the descriptor's state, so it goes back as it was.
    lease.giveBack(true, false);
}

void Driver::probeCapture(int descriptor)
{
    if (captureProbed_) return;
    captureProbed_ = true;
    rgbOffered_ = decoderOffersCapture(descriptor, V4L2_PIX_FMT_ABGR32);
    // Everything a decode session needs to know is settled here, at
    // initialisation, so that the first session of a boot - which is the one
    // that plays a startup movie - gets the same answer as every later one.
    // Whether a queue takes one of this driver's buffers is not a question a
    // synthetic probe can answer: the queue negotiates its own geometry, and
    // only the real one knows what it will accept. So the decision is made by
    // lending the real queue a real buffer, and a queue that refuses falls
    // back to the buffers the device allocates, once, with the reason logged.
    lendCapture_ = capturePolicy_ != CaptureMemory::device && selectedStorage() == StorageKind::gbm;
}

bool Driver::importsCapture()
{
    return lendCapture_;
}

std::uint32_t Driver::preferredCapture()
{
    if (rgbPolicy_ == RgbCapture::off) return 0;
    if (!useRgbCapture(rgbPolicy_, rgbOffered_)) return 0;
    // The device writes B, G, R, A in memory, which is what VA-API calls
    // VA_FOURCC_ARGB and what a browser's BGRX pixmap expects.
    return V4L2_PIX_FMT_ABGR32;
}

void Driver::materialise(Surface& target)
{
    if (target.nativeValid || !target.shadow || !target.buffer) return;
    const PixelBuffer::CpuAccess writing(*target.buffer, true);
    const std::uint8_t* from = target.shadow->map();
    std::uint8_t* to = target.buffer->map();
    if (!from || !to) return;
    const Region region {0, 0, target.width, target.height};
    convertArgbToNv12(planeView(*target.shadow, from), region, planeViews(*target.buffer, to).data(), region,
                      isBt709(target.height));
    target.nativeValid = true;
}

const CodecSupport* Driver::codec(VAProfile profile) const noexcept
{
    for (const auto& entry : codecs_)
        if (entry.profile == profile) return &entry;
    return nullptr;
}

Surface* Driver::surface(VASurfaceID id)
{
    const auto found = surfaces.find(id);
    return found == surfaces.end() ? nullptr : found->second.get();
}

Context* Driver::context(VAContextID id)
{
    const auto found = contexts.find(id);
    return found == contexts.end() ? nullptr : found->second.get();
}

Buffer* Driver::buffer(VABufferID id)
{
    const auto found = buffers.find(id);
    return found == buffers.end() ? nullptr : found->second.get();
}

std::shared_ptr<PixelBuffer> Driver::knownBuffer(std::uint64_t identity)
{
    if (!identity) return {};
    const auto found = known.find(identity);
    if (found == known.end()) return {};
    auto buffer = found->second.lock();
    if (!buffer) known.erase(found);
    return buffer;
}

bool Driver::wantsReport()
{
    if (reportLimit_ < 0) {
        const char* wanted = std::getenv("UML_VA_V4L2_REPORT");
        reportLimit_ = wanted && *wanted ? std::atoi(wanted) : 1;
    }
    return reportedPictures_ < reportLimit_;
}

void Driver::reportLent(PixelBuffer& buffer, bool packed)
{
    // A lent buffer is the picture, so it is read where it lies rather than
    // through the capture queue's own mapping, which an imported queue has not
    // got.
    std::uint8_t* data = buffer.map();
    if (!data) return;
    DecodedPicture picture {};
    picture.width = buffer.width();
    picture.height = buffer.height();
    picture.planes = buffer.planes();
    for (unsigned plane = 0; plane < picture.planes && plane < 3; ++plane) {
        picture.plane[plane] = data + buffer.offset(plane);
        picture.pitch[plane] = buffer.pitch(plane);
    }
    reportPicture(picture, packed);
}

void Driver::reportPicture(const DecodedPicture& picture, bool packed)
{
    // The picture exactly as the capture queue delivered it, before this
    // driver has touched a byte of it: the only place where a colour question
    // can be settled between the device and everything above it.
    if (!picture.plane[0] || !picture.width || !picture.height) return;
    if (!wantsReport()) return;
    const int index = reportedPictures_++;
    char message[256] {};
    if (packed) {
        double sum[3] {};
        for (unsigned row = 0; row < picture.height; ++row) {
            const std::uint8_t* line = picture.plane[0] + std::size_t(row) * picture.pitch[0];
            for (unsigned column = 0; column < picture.width; ++column) {
                sum[0] += line[std::size_t(column) * 4 + 0];
                sum[1] += line[std::size_t(column) * 4 + 1];
                sum[2] += line[std::size_t(column) * 4 + 2];
            }
        }
        const double samples = double(picture.width) * double(picture.height);
        const double blue = sum[0] / samples;
        const double green = sum[1] / samples;
        const double red = sum[2] / samples;
        // The studio-swing BT.709 luma the same picture would carry as NV12,
        // so a packed and an NV12 run can be compared frame by frame.
        const double luma = 16.0 + (0.2126 * red + 0.7152 * green + 0.0722 * blue) * 219.0 / 255.0;
        std::snprintf(message, sizeof(message),
                      "captured packed frame %d means B %.3f G %.3f R %.3f luma %.3f", index,
                      blue, green, red, luma);
    } else {
        double luma = 0;
        for (unsigned row = 0; row < picture.height; ++row) {
            const std::uint8_t* line = picture.plane[0] + std::size_t(row) * picture.pitch[0];
            for (unsigned column = 0; column < picture.width; ++column) luma += line[column];
        }
        std::snprintf(message, sizeof(message), "captured NV12 frame %d mean Y %.3f", index,
                      luma / (double(picture.width) * double(picture.height)));
    }
    note(message);
    const char* wantedFrame = std::getenv("UML_VA_V4L2_DUMP_FRAME");
    if (const char* path = std::getenv("UML_VA_V4L2_DUMP");
        path && *path && index == (wantedFrame && *wantedFrame ? std::atoi(wantedFrame) : 0)) {
        if (std::FILE* file = std::fopen(path, "wbe")) {
            for (unsigned row = 0; row < picture.height; ++row)
                std::fwrite(picture.plane[0] + std::size_t(row) * picture.pitch[0], 1,
                            std::size_t(picture.width) * (packed ? 4u : 1u), file);
            // A planar picture is written whole, so a reader can convert it.
            if (!packed && picture.planes > 1 && picture.plane[1])
                for (unsigned row = 0; row < (picture.height + 1) / 2; ++row)
                    std::fwrite(picture.plane[1] + std::size_t(row) * picture.pitch[1], 1,
                                picture.width, file);
            std::fclose(file);
        }
    }
}

std::shared_ptr<PixelBuffer> Driver::captureBuffer(Context& context)
{
    const bool packed = context.decoder->captureFormat() == V4L2_PIX_FMT_ABGR32;
    const std::uint32_t fourcc = packed ? VA_FOURCC_ARGB : VA_FOURCC_NV12;
    const unsigned width = context.decoder->width();
    const unsigned height = context.decoder->height();
    if (auto reused = takeReusable(context.captureSpare, fourcc, width, height)) return reused;
    std::string error;
    auto buffer = PixelBuffer::allocate(fourcc, width, height, error);
    if (!buffer) { if (!error.empty()) note(error.c_str()); return nullptr; }
    // The decoder writes into it directly, so the device's own row length has
    // to be the one this buffer has.
    if (buffer->kind() != StorageKind::gbm || buffer->descriptor() < 0) return nullptr;
    for (unsigned plane = 0; plane < context.decoder->capturePlaneCount(); ++plane)
        if (buffer->pitch(plane) != context.decoder->capturePitch(plane)) return nullptr;
    return buffer;
}

void Driver::recycleCapture(Context& context, std::shared_ptr<PixelBuffer> buffer)
{
    returnToPool(context.captureSpare, std::move(buffer));
}

bool Driver::provideCapture(Context& context, unsigned index, CaptureTarget& target, bool fresh)
{
    if (!context.decoder) return false;
    // A new queue - the first one, or the one a source change builds - starts
    // by taking back every buffer the old queue held. Refilling a single slot
    // of a live queue must leave the others exactly where they are.
    if (fresh) {
        for (auto& slot : context.captureSlot) recycleCapture(context, std::move(slot));
        context.captureSlot.clear();
    }
    auto buffer = captureBuffer(context);
    if (!buffer) return false;
    const unsigned planes = context.decoder->capturePlaneCount();
    if (!planes || planes > 3) return false;
    target.planes = planes;
    std::size_t laid = 0;
    for (unsigned plane = 0; plane < planes; ++plane) {
        target.fd[plane] = buffer->descriptor();
        target.offset[plane] = buffer->offset(plane);
        target.pitch[plane] = buffer->pitch(plane);
        const std::size_t wanted = context.decoder->capturePlaneSize(plane);
        const std::size_t available = buffer->size() > buffer->offset(plane)
            ? buffer->size() - buffer->offset(plane) : 0;
        if (wanted > available) return false;
        // One object carries every plane, and a queue that takes it puts the
        // planes where its own negotiated sizes say. A buffer laid out any
        // other way would have its chroma written over its luma.
        if (buffer->offset(plane) != laid) return false;
        laid += wanted;
        target.length[plane] = wanted ? wanted : available;
    }
    if (context.captureSlot.size() <= index) context.captureSlot.resize(index + 1);
    context.captureSlot[index] = std::move(buffer);
    return true;
}

bool Driver::fillFromLent(Surface& target, PixelBuffer& lent, bool packed)
{
    // A surface whose own memory a client already holds cannot be pointed at
    // the buffer the decoder wrote: the picture has to arrive in the memory
    // the client is looking at.
    const std::uint8_t* from = lent.map();
    if (!from) return false;
    if (packed) return storePacked(target, planeView(lent, from), lent.width(), lent.height());
    if (!target.buffer || lent.planes() < 2) return false;
    const PixelBuffer::CpuAccess writing(*target.buffer, true);
    std::uint8_t* to = target.buffer->map();
    if (!to) return false;
    copyNv12(planeViews(lent, from).data(), planeViews(*target.buffer, to).data(),
             std::min(target.width, lent.width()), std::min(target.height, lent.height()));
    target.nativeValid = true;
    refreshExport(target);
    return true;
}

bool Driver::storePacked(Surface& target, const ConstPlaneView& source, unsigned width, unsigned height)
{
    std::string error;
    if (!target.shadow)
        target.shadow = PixelBuffer::allocate(VA_FOURCC_ARGB, target.width, target.height, error);
    std::uint8_t* to = target.shadow ? target.shadow->map() : nullptr;
    if (!to) {
        if (!error.empty()) note(error.c_str());
        return false;
    }
    const Region region {0, 0, std::min(target.width, width), std::min(target.height, height)};
    {
        // The shadow is what the client samples when the surface exports it.
        const PixelBuffer::CpuAccess writing(*target.shadow, true);
        copyPacked32(source, region, planeView(*target.shadow, to), region);
    }
    target.nativeValid = false;
    // A client that exported the surface or derived an image from it looks at
    // the surface's own memory and never asks this driver for it again, so the
    // picture has to be there now rather than when it is next requested. A
    // surface that exports its shadow already has it there.
    if (target.exported && !target.exportsArgb) materialise(target);
    return true;
}

void Driver::refreshExport(Surface& surface)
{
    if (!surface.exportsArgb || !surface.shadow || !surface.buffer || !surface.nativeValid) return;
    const std::uint8_t* from = surface.buffer->map();
    std::uint8_t* to = surface.shadow->map();
    if (!from || !to) return;
    const Region region {0, 0, surface.width, surface.height};
    const PixelBuffer::CpuAccess writing(*surface.shadow, true);
    convertNv12ToArgb(planeViews(*surface.buffer, from).data(), region, planeView(*surface.shadow, to), region,
                      isBt709(surface.height));
}

void Driver::harvest(Context& context, std::chrono::milliseconds wait)
{
    if (context.reportLayout && !context.decoder->captureLayout().empty()) {
        context.reportLayout = false;
        const bool packed = context.decoder->captureFormat() == V4L2_PIX_FMT_ABGR32;
        note(((packed ? "decoded packed RGB " : "decoded NV12 ") + context.decoder->captureLayout() +
              (context.decoder->importsCapture()
                   ? std::string(" into lent buffers")
                   : " into device buffers (" + (context.decoder->importFailure().empty()
                       ? std::string("not offered") : context.decoder->importFailure()) + ")")).c_str());
    }
    DecodedPicture picture {};
    std::string error;
    while (context.decoder->collect(picture, wait, error)) {
        wait = std::chrono::milliseconds(0);
        const auto found = context.awaiting.find(picture.timestamp);
        if (found != context.awaiting.end()) {
            Surface* target = surface(found->second);
            context.awaiting.erase(found);
            const bool packed = context.decoder->captureFormat() == V4L2_PIX_FMT_ABGR32;
            if (!picture.error) reportPicture(picture, packed);
            if (picture.imported) {
                // The device wrote the picture into a buffer this driver lent
                // it, so the surface only has to take the buffer over. The one
                // it was holding goes back to the pool, and the slot gets
                // another before it is queued again.
                auto lent = picture.index < context.captureSlot.size()
                    ? context.captureSlot[picture.index] : nullptr;
                if (target && lent && !picture.error) {
                    bool ready = true;
                    // The surface can only take the buffer over when the
                    // buffer really holds the surface: a decode session at a
                    // coded size larger than the surface asked for hands back
                    // a picture that has to be copied into it instead.
                    const bool fits = lent->width() >= target->width && lent->height() >= target->height;
                    if (target->exported || !fits) {
                        ready = fillFromLent(*target, *lent, packed);
                    } else if (packed) {
                        recycleCapture(context, std::move(target->shadow));
                        target->shadow = lent;
                        target->nativeValid = false;
                    } else {
                        recycleCapture(context, std::move(target->buffer));
                        target->buffer = lent;
                        target->nativeValid = true;
                    }
                    if (wantsReport()) reportLent(*lent, packed);
                    target->state = ready ? Surface::State::ready : Surface::State::failed;
                } else if (target) {
                    target->state = Surface::State::failed;
                }
            } else if (target && target->buffer && target->fourcc == VA_FOURCC_NV12 && packed &&
                picture.planes >= 1 && !picture.error) {
                // The host already converted, so keep the picture packed.
                const bool stored = storePacked(*target, ConstPlaneView {picture.plane[0], picture.pitch[0]},
                                                picture.width, picture.height);
                target->state = stored ? Surface::State::ready : Surface::State::failed;
            } else if (target && target->buffer && target->fourcc == VA_FOURCC_NV12 && picture.planes >= 2 &&
                !picture.error) {
                const PixelBuffer::CpuAccess writing(*target->buffer, true);
                if (std::uint8_t* destination = target->buffer->map()) {
                    const ConstPlaneView source[2] {
                        {picture.plane[0], picture.pitch[0]},
                        {picture.plane[1], picture.pitch[1]}};
                    copyNv12(source, planeViews(*target->buffer, destination).data(),
                             std::min(target->width, picture.width),
                             std::min(target->height, picture.height));
                    target->nativeValid = true;
                    refreshExport(*target);
                    target->state = Surface::State::ready;
                } else {
                    target->state = Surface::State::failed;
                }
            } else if (target) {
                target->state = Surface::State::failed;
            }
        }
        if (picture.imported) {
            CaptureTarget next {};
            std::string requeue;
            if (!provideCapture(context, picture.index, next, false) ||
                !context.decoder->release(picture.index, next, requeue)) {
                context.failure = requeue.empty() ? "no buffer to lend the decoder" : requeue;
                note(context.failure.c_str());
            }
        } else if (std::string requeue; !context.decoder->release(picture.index, requeue)) {
            context.failure = requeue;
            note(context.failure.c_str());
        }
        picture = {};
    }
    if (!error.empty()) {
        context.failure = error;
        note(error.c_str());
    }
}

VAStatus Driver::waitFor(Surface& target, std::chrono::milliseconds wait)
{
    if (target.state == Surface::State::failed)
        return context(target.owner) && context(target.owner)->encoder ? VA_STATUS_ERROR_ENCODING_ERROR
                                                                        : VA_STATUS_ERROR_DECODING_ERROR;
    if (target.state != Surface::State::pending) return VA_STATUS_SUCCESS;
    Context* owner = context(target.owner);
    if (owner && owner->encoder) return waitForEncode(target, wait);
    if (!owner || !owner->decoder) { target.state = Surface::State::failed; return VA_STATUS_ERROR_DECODING_ERROR; }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (target.state == Surface::State::pending) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        harvest(*owner, std::min(left, std::chrono::milliseconds(100)));
    }
    if (target.state == Surface::State::pending) {
        target.state = Surface::State::failed;
        note("a decoded picture never arrived");
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    return target.state == Surface::State::ready ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_DECODING_ERROR;
}

VAStatus Driver::submitFrame(Context& owner, VASurfaceID id, Surface& target)
{
    if (!owner.decoder) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (owner.sliceData.empty()) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (target.fourcc != VA_FOURCC_NV12) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    const std::span<const std::uint8_t> frame(owner.sliceData);
    const auto header = owner.vp9.parse(frame);
    if (!header.valid) { note("unparsable VP9 frame header"); return VA_STATUS_ERROR_INVALID_PARAMETER; }
    if (header.profile != 0) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

    if (header.showExistingFrame) {
        // A stateful decoder was never asked to decode this frame, so serve it
        // from the surface that last wrote the slot.
        Surface* source = surface(owner.slot[header.frameToShowMapIndex]);
        if (!source || !source->buffer || !target.buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
        const auto status = waitFor(*source, kDecodeWait);
        if (status != VA_STATUS_SUCCESS) return status;
        if (source->shadow && !source->nativeValid && target.shadow) {
            // Both sides hold packed pixels, so serve the repeat frame from
            // the shadow and leave the NV12 buffers alone.
            const std::uint8_t* packed = source->shadow->map();
            if (!packed ||
                !storePacked(target, planeView(*source->shadow, packed), source->width, source->height))
                return VA_STATUS_ERROR_OPERATION_FAILED;
            target.state = Surface::State::ready;
            return VA_STATUS_SUCCESS;
        }
        materialise(*source);
        const PixelBuffer::CpuAccess reading(*source->buffer, false);
        const PixelBuffer::CpuAccess writing(*target.buffer, true);
        const std::uint8_t* from = source->buffer->map();
        std::uint8_t* to = target.buffer->map();
        if (!from || !to) return VA_STATUS_ERROR_OPERATION_FAILED;
        copyNv12(planeViews(*source->buffer, from).data(), planeViews(*target.buffer, to).data(),
                 std::min(source->width, target.width), std::min(source->height, target.height));
        // The NV12 buffer is now the truth for this surface, whatever an
        // earlier packed decode left in its shadow.
        target.nativeValid = true;
        refreshExport(target);
        target.state = Surface::State::ready;
        return VA_STATUS_SUCCESS;
    }

    std::vector<std::uint8_t> packet;
    std::uint8_t tail = 0;
    const unsigned slot = header.showFrame || !showHiddenFrames() ? 8u : lowestSlot(header.refreshFrameFlags);
    if (!header.showFrame && slot < 8) {
        // The decoder emits shown frames only, so ask it to show this hidden
        // frame straight away through a superframe. A later
        // show_existing_frame in the stream displays this surface.
        tail = vp9ShowExistingFrameByte(slot);
        const std::span<const std::uint8_t> parts[2] {frame, std::span<const std::uint8_t>(&tail, 1)};
        packet = vp9BuildSuperframe(std::span<const std::span<const std::uint8_t>>(parts, 2));
    } else {
        packet.assign(frame.begin(), frame.end());
    }
    if (packet.empty()) return VA_STATUS_ERROR_OPERATION_FAILED;

    // V4L2 carries a buffer timestamp as a timeval, so a submission key has to
    // be a whole microsecond to survive the round trip through both queues.
    const auto sequence = ++owner.sequence * 1000ull;
    std::string error;
    if (!owner.decoder->submit(packet, sequence, error)) {
        note(error.c_str());
        owner.failure = error;
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    target.state = Surface::State::pending;
    target.sequence = sequence;
    owner.awaiting[sequence] = id;
    const bool expectPicture = header.showFrame || slot < 8;
    for (unsigned index = 0; index < 8; ++index)
        if (header.refreshFrameFlags & (1u << index)) owner.slot[index] = id;
    // The device decodes synchronously, so the picture is normally already on
    // its way; waiting here keeps the surface valid for every client, including
    // the ones that never call vaSyncSurface.
    if (!expectPicture) {
        // Nothing will come back for this submission. Let the decoder deliver
        // whatever else is ready and leave the surface as it is.
        harvest(owner, std::chrono::milliseconds(0));
        owner.awaiting.erase(sequence);
        target.state = Surface::State::ready;
        return VA_STATUS_SUCCESS;
    }
    return waitFor(target, kDecodeWait);
}

VAStatus Driver::process(Context& owner, Surface& target)
{
    if (!owner.havePipeline) return VA_STATUS_ERROR_INVALID_PARAMETER;
    Surface* source = surface(owner.pipeline.surface);
    if (!source || !source->buffer || !target.buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    const auto status = waitFor(*source, kDecodeWait);
    if (status != VA_STATUS_SUCCESS) return status;
    const PixelBuffer::CpuAccess reading(*source->buffer, false);
    const PixelBuffer::CpuAccess writing(*target.buffer, true);
    const std::uint8_t* from = source->buffer->map();
    std::uint8_t* to = target.buffer->map();
    if (!from || !to) return VA_STATUS_ERROR_OPERATION_FAILED;
    Region sourceRegion {0, 0, source->width, source->height};
    if (owner.pipelineSourceValid) {
        const VARectangle& asked = owner.pipelineSource;
        const auto region = regionWithin(asked.x, asked.y, asked.width, asked.height, source->width, source->height);
        if (!region) return VA_STATUS_ERROR_INVALID_PARAMETER;
        sourceRegion = *region;
    }
    Region targetRegion {0, 0, target.width, target.height};
    if (owner.pipelineDestinationValid) {
        const VARectangle& asked = owner.pipelineDestination;
        const auto region = regionWithin(asked.x, asked.y, asked.width, asked.height, target.width, target.height);
        if (!region) return VA_STATUS_ERROR_INVALID_PARAMETER;
        targetRegion = *region;
    }

    if (source->shadow && !source->nativeValid && target.buffer->planes() == 1 &&
        (target.fourcc == VA_FOURCC_ARGB || target.fourcc == VA_FOURCC_BGRA ||
         target.fourcc == VA_FOURCC_RGBX || target.fourcc == VA_FOURCC_BGRX)) {
        // The device decoded straight into the format this blit was going to
        // produce, so the conversion the caller asked for is a copy.
        const std::uint8_t* packed = source->shadow->map();
        if (!packed) return VA_STATUS_ERROR_OPERATION_FAILED;
        copyPacked32(planeView(*source->shadow, packed), sourceRegion, planeView(*target.buffer, to),
                     targetRegion);
        return VA_STATUS_SUCCESS;
    }
    if (source->shadow && !source->nativeValid) {
        // Any other destination needs the NV12 the decoder did not produce.
        materialise(*source);
        from = source->buffer->map();
        if (!from) return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (source->fourcc == VA_FOURCC_NV12 && target.fourcc == VA_FOURCC_NV12) {
        if (sourceRegion.width != targetRegion.width || sourceRegion.height != targetRegion.height)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        copyNv12(planeViews(*source->buffer, from, sourceRegion.x, sourceRegion.y).data(),
                 planeViews(*target.buffer, to, targetRegion.x, targetRegion.y).data(), targetRegion.width,
                 targetRegion.height);
    } else if (source->fourcc == VA_FOURCC_NV12) {
        convertNv12ToArgb(planeViews(*source->buffer, from).data(), sourceRegion, planeView(*target.buffer, to),
                          targetRegion, isBt709(source->height));
    } else if (source->fourcc == target.fourcc) {
        copyPacked32(planeView(*source->buffer, from), sourceRegion, planeView(*target.buffer, to), targetRegion);
    } else {
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    target.state = Surface::State::ready;
    return VA_STATUS_SUCCESS;
}

namespace {

Driver& driverOf(VADriverContextP ctx) { return *static_cast<Driver*>(ctx->pDriverData); }

std::uint32_t defaultFourcc(unsigned rtFormat) noexcept
{
    switch (rtFormat) {
    case VA_RT_FORMAT_YUV420: return VA_FOURCC_NV12;
    case VA_RT_FORMAT_RGB32: return VA_FOURCC_ARGB;
    default: return 0;
    }
}

bool supportedFourcc(std::uint32_t fourcc) noexcept
{
    switch (fourcc) {
    case VA_FOURCC_NV12:
    case VA_FOURCC_ARGB:
    case VA_FOURCC_BGRA:
    case VA_FOURCC_RGBA:
    case VA_FOURCC_BGRX:
    case VA_FOURCC_RGBX:
        return true;
    default:
        return false;
    }
}

VAStatus terminate(VADriverContextP ctx)
{
    delete static_cast<Driver*>(ctx->pDriverData);
    ctx->pDriverData = nullptr;
    return VA_STATUS_SUCCESS;
}

VAStatus queryConfigProfiles(VADriverContextP ctx, VAProfile* profiles, int* count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    int written = 0;
    for (const auto& codec : driver.codecs()) profiles[written++] = codec.profile;
    std::vector<VAProfile> encoding;
    driver.encodeProfiles(encoding);
    for (const VAProfile profile : encoding) profiles[written++] = profile;
    profiles[written++] = VAProfileNone;
    *count = written;
    return VA_STATUS_SUCCESS;
}

VAStatus queryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint* entrypoints, int* count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    if (profile == VAProfileNone) {
        entrypoints[0] = VAEntrypointVideoProc;
        *count = 1;
        return VA_STATUS_SUCCESS;
    }
    int written = 0;
    if (driver.codec(profile)) entrypoints[written++] = VAEntrypointVLD;
    if (driver.isEncodeProfile(profile)) {
        entrypoints[written++] = VAEntrypointEncSlice;
        entrypoints[written++] = VAEntrypointEncSliceLP;
    }
    if (!written) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    *count = written;
    return VA_STATUS_SUCCESS;
}

VAStatus getConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
                             VAConfigAttrib* attributes, int count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    if (isEncodeEntrypoint(entrypoint) && driver.isEncodeProfile(profile))
        return driver.encodeAttributes(profile, attributes, count);
    const bool proc = profile == VAProfileNone && entrypoint == VAEntrypointVideoProc;
    if (!proc && (!driver.codec(profile) || entrypoint != VAEntrypointVLD))
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (int index = 0; index < count; ++index) {
        switch (attributes[index].type) {
        case VAConfigAttribRTFormat:
            attributes[index].value = proc ? (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32) : VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribMaxPictureWidth:
            attributes[index].value = proc ? 4096u : driver.codec(profile)->maxWidth;
            break;
        case VAConfigAttribMaxPictureHeight:
            attributes[index].value = proc ? 4096u : driver.codec(profile)->maxHeight;
            break;
        case VAConfigAttribDecSliceMode:
            attributes[index].value = VA_DEC_SLICE_MODE_NORMAL;
            break;
        default:
            attributes[index].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus createConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
                      VAConfigAttrib* attributes, int count, VAConfigID* id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    if (isEncodeEntrypoint(entrypoint)) {
        Config encoding;
        const VAStatus status = driver.createEncodeConfig(profile, entrypoint, attributes, count, encoding);
        if (status != VA_STATUS_SUCCESS) return status;
        *id = driver.nextConfig++;
        driver.configs.emplace(*id, encoding);
        return VA_STATUS_SUCCESS;
    }
    const bool proc = profile == VAProfileNone && entrypoint == VAEntrypointVideoProc;
    if (!proc) {
        if (!driver.codec(profile)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        if (entrypoint != VAEntrypointVLD) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    Config config;
    config.profile = profile;
    config.entrypoint = entrypoint;
    config.rtFormat = proc ? (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32) : VA_RT_FORMAT_YUV420;
    for (int index = 0; index < count; ++index) {
        if (attributes[index].type != VAConfigAttribRTFormat) continue;
        const unsigned allowed = proc ? (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32) : VA_RT_FORMAT_YUV420;
        if ((attributes[index].value & allowed) != attributes[index].value)
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        config.rtFormat = attributes[index].value;
    }
    *id = driver.nextConfig++;
    driver.configs.emplace(*id, config);
    return VA_STATUS_SUCCESS;
}

VAStatus destroyConfig(VADriverContextP ctx, VAConfigID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    return driver.configs.erase(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONFIG;
}

VAStatus queryConfigAttributes(VADriverContextP ctx, VAConfigID id, VAProfile* profile, VAEntrypoint* entrypoint,
                               VAConfigAttrib* attributes, int* count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const auto found = driver.configs.find(id);
    if (found == driver.configs.end()) return VA_STATUS_ERROR_INVALID_CONFIG;
    *profile = found->second.profile;
    *entrypoint = found->second.entrypoint;
    attributes[0].type = VAConfigAttribRTFormat;
    attributes[0].value = found->second.rtFormat;
    *count = 1;
    if (isEncodeEntrypoint(found->second.entrypoint)) {
        attributes[1].type = VAConfigAttribRateControl;
        attributes[1].value = found->second.rateControl;
        attributes[2].type = VAConfigAttribEncPackedHeaders;
        attributes[2].value = VA_ENC_PACKED_HEADER_NONE;
        *count = 3;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus querySurfaceAttributes(VADriverContextP ctx, VAConfigID id, VASurfaceAttrib* attributes, unsigned* count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const auto found = driver.configs.find(id);
    if (found == driver.configs.end()) return VA_STATUS_ERROR_INVALID_CONFIG;
    const bool proc = found->second.entrypoint == VAEntrypointVideoProc;
    const bool encode = isEncodeEntrypoint(found->second.entrypoint);
    const CodecSupport* codec = proc || encode ? nullptr : driver.codec(found->second.profile);
    const EncodeCodec* limits = encode && driver.encoding() ? driver.encoding()->limits(found->second.profile) : nullptr;
    if (!proc && !codec && !limits) return VA_STATUS_ERROR_INVALID_CONFIG;
    std::vector<VASurfaceAttrib> list;
    const auto pixelFormat = [&list](std::uint32_t fourcc) {
        VASurfaceAttrib attribute {};
        attribute.type = VASurfaceAttribPixelFormat;
        attribute.flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
        attribute.value.type = VAGenericValueTypeInteger;
        attribute.value.value.i = int(fourcc);
        list.push_back(attribute);
    };
    const auto integer = [&list](VASurfaceAttribType type, int value) {
        VASurfaceAttrib attribute {};
        attribute.type = type;
        attribute.flags = VA_SURFACE_ATTRIB_GETTABLE;
        attribute.value.type = VAGenericValueTypeInteger;
        attribute.value.value.i = value;
        list.push_back(attribute);
    };
    pixelFormat(VA_FOURCC_NV12);
    if (proc) pixelFormat(VA_FOURCC_ARGB);
    integer(VASurfaceAttribMinWidth, proc ? 16 : int(limits ? limits->minWidth : codec->minWidth));
    integer(VASurfaceAttribMinHeight, proc ? 16 : int(limits ? limits->minHeight : codec->minHeight));
    integer(VASurfaceAttribMaxWidth, proc ? 4096 : int(limits ? limits->maxWidth : codec->maxWidth));
    integer(VASurfaceAttribMaxHeight, proc ? 4096 : int(limits ? limits->maxHeight : codec->maxHeight));
    VASurfaceAttrib memory {};
    memory.type = VASurfaceAttribMemoryType;
    memory.flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    memory.value.type = VAGenericValueTypeInteger;
    memory.value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME |
                           VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    list.push_back(memory);
    if (!attributes) { *count = unsigned(list.size()); return VA_STATUS_SUCCESS; }
    if (*count < list.size()) { *count = unsigned(list.size()); return VA_STATUS_ERROR_MAX_NUM_EXCEEDED; }
    std::copy(list.begin(), list.end(), attributes);
    *count = unsigned(list.size());
    return VA_STATUS_SUCCESS;
}

VAStatus createSurfaces2(VADriverContextP ctx, unsigned format, unsigned width, unsigned height,
                         VASurfaceID* ids, unsigned count, VASurfaceAttrib* attributes, unsigned attributeCount)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    std::uint32_t fourcc = defaultFourcc(format);
    unsigned memoryType = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    void* descriptor = nullptr;
    for (unsigned index = 0; index < attributeCount; ++index) {
        switch (attributes[index].type) {
        case VASurfaceAttribPixelFormat:
            fourcc = std::uint32_t(attributes[index].value.value.i);
            break;
        case VASurfaceAttribMemoryType:
            memoryType = unsigned(attributes[index].value.value.i);
            break;
        case VASurfaceAttribExternalBufferDescriptor:
            descriptor = attributes[index].value.value.p;
            break;
        default:
            break;
        }
    }
    if (!fourcc || !supportedFourcc(fourcc)) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (descriptor && count != 1) return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (traceRequested()) {
        char text[160];
        std::snprintf(text, sizeof(text), "createSurfaces2 %ux%u x%u format=0x%x fourcc=0x%08x memory=0x%x descriptor=%d",
                      width, height, count, format, fourcc, memoryType, descriptor ? 1 : 0);
        driver.note(text);
    }
    for (unsigned index = 0; index < count; ++index) {
        auto surface = std::make_unique<Surface>();
        surface->width = width;
        surface->height = height;
        surface->fourcc = fourcc;
        if (descriptor) {
            int fds[4] {-1, -1, -1, -1};
            std::size_t sizes[4] {};
            unsigned objectCount = 1;
            unsigned planeObject[4] {};
            unsigned planes = 0;
            std::uint32_t pitch[4] {};
            std::uint32_t offset[4] {};
            std::uint64_t modifier = 0;
            if (memoryType == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
                const auto* prime = static_cast<const VADRMPRIMESurfaceDescriptor*>(descriptor);
                constexpr std::uint32_t kObjects = sizeof(prime->objects) / sizeof(prime->objects[0]);
                constexpr std::uint32_t kLayers = sizeof(prime->layers) / sizeof(prime->layers[0]);
                if (!prime->num_objects || !prime->num_layers || prime->num_objects > kObjects ||
                    prime->num_layers > kLayers || prime->num_objects > 4)
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                fourcc = prime->fourcc;
                objectCount = prime->num_objects;
                modifier = prime->objects[0].drm_format_modifier;
                for (unsigned object = 0; object < objectCount; ++object) {
                    fds[object] = prime->objects[object].fd;
                    sizes[object] = prime->objects[object].size;
                }
                // A planar image may live in one object, every plane at its own
                // offset, or in an object per plane, as a graphics stack that
                // allocates a resource per plane exports it.
                for (unsigned layer = 0; layer < prime->num_layers && planes < 4; ++layer)
                    for (unsigned plane = 0; plane < prime->layers[layer].num_planes && planes < 4; ++plane) {
                        const unsigned object = prime->layers[layer].object_index[plane];
                        if (object >= objectCount) return VA_STATUS_ERROR_INVALID_PARAMETER;
                        planeObject[planes] = object;
                        pitch[planes] = prime->layers[layer].pitch[plane];
                        offset[planes] = prime->layers[layer].offset[plane];
                        ++planes;
                    }
            } else if (memoryType == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) {
                const auto* external = static_cast<const VASurfaceAttribExternalBuffers*>(descriptor);
                if (!external->num_buffers || !external->num_planes) return VA_STATUS_ERROR_INVALID_PARAMETER;
                fourcc = external->pixel_format;
                fds[0] = int(external->buffers[0]);
                sizes[0] = external->data_size;
                planes = std::min<unsigned>(external->num_planes, 4);
                for (unsigned plane = 0; plane < planes; ++plane) {
                    pitch[plane] = external->pitches[plane];
                    offset[plane] = external->offsets[plane];
                }
            } else {
                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
            }
            // The descriptor names the format, which has to be one this
            // driver can read and write as well.
            if (!supportedFourcc(fourcc)) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            surface->fourcc = fourcc;
            // The same memory imported as another image gets its own checked
            // layout rather than the earlier import's.
            auto shared = driver.knownBuffer(descriptorIdentity(fds[0]));
            bool sameObjects = shared && shared->objects() == objectCount;
            for (unsigned object = 0; sameObjects && object < objectCount; ++object)
                sameObjects = shared->objectIdentity(object) == descriptorIdentity(fds[object]);
            if (shared && sameObjects && shared->holds(fourcc, width, height)) {
                surface->buffer = std::move(shared);
            } else {
                PixelBuffer::Object objects[4] {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};
                bool duplicated = true;
                for (unsigned object = 0; object < objectCount; ++object) {
                    objects[object] = {fds[object] >= 0 ? fcntl(fds[object], F_DUPFD_CLOEXEC, 0) : -1,
                                       sizes[object]};
                    duplicated = duplicated && objects[object].fd >= 0;
                }
                std::string error;
                auto adopted = duplicated ? PixelBuffer::adoptObjects(objects, objectCount, fourcc, width, height,
                                                                      planes, pitch, offset, planeObject, modifier,
                                                                      error)
                                          : nullptr;
                if (!adopted) {
                    for (unsigned object = 0; object < objectCount; ++object)
                        if (objects[object].fd >= 0) ::close(objects[object].fd);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                surface->buffer = std::shared_ptr<PixelBuffer>(adopted.release());
                driver.known.emplace(surface->buffer->identity(), surface->buffer);
            }
        } else {
            std::string error;
            auto allocated = PixelBuffer::allocate(fourcc, width, height, error);
            if (!allocated) { driver.note(error.c_str()); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
            surface->buffer = std::shared_ptr<PixelBuffer>(allocated.release());
            driver.known.emplace(surface->buffer->identity(), surface->buffer);
        }
        ids[index] = driver.nextSurface++;
        driver.surfaces.emplace(ids[index], std::move(surface));
    }
    return VA_STATUS_SUCCESS;
}

VAStatus createSurfaces(VADriverContextP ctx, int width, int height, int format, int count, VASurfaceID* ids)
{
    return createSurfaces2(ctx, unsigned(format), unsigned(width), unsigned(height), ids, unsigned(count),
                           nullptr, 0);
}

VAStatus destroySurfaces(VADriverContextP ctx, VASurfaceID* ids, int count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    for (int index = 0; index < count; ++index) {
        Surface* surface = driver.surface(ids[index]);
        if (!surface) continue;
        if (Context* owner = driver.context(surface->owner))
            std::erase_if(owner->awaiting, [&ids, index](const auto& entry) { return entry.second == ids[index]; });
        driver.surfaces.erase(ids[index]);
    }
    return VA_STATUS_SUCCESS;
}

VAStatus createContext(VADriverContextP ctx, VAConfigID configId, int width, int height, int flag,
                       VASurfaceID* targets, int targetCount, VAContextID* id)
{
    (void)flag;
    (void)targets;
    (void)targetCount;
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const auto found = driver.configs.find(configId);
    if (found == driver.configs.end()) return VA_STATUS_ERROR_INVALID_CONFIG;
    auto context = std::make_unique<Context>();
    context->config = configId;
    context->profile = found->second.profile;
    context->entrypoint = found->second.entrypoint;
    context->width = unsigned(width);
    context->height = unsigned(height);
    for (auto& slot : context->slot) slot = VA_INVALID_SURFACE;
    if (context->entrypoint == VAEntrypointVLD) {
        const CodecSupport* codec = driver.codec(context->profile);
        if (!codec) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        if (!width || !height) return VA_STATUS_ERROR_INVALID_PARAMETER;
        std::string error;
        // A pooled descriptor is the one a sandboxed process can still get.
        Lease lease = driver.pool().acquire(Role::decoder, driver.device(), error);
        if (!lease) { driver.note(error.c_str()); return VA_STATUS_ERROR_MAX_NUM_EXCEEDED; }
        context->decoder = V4l2Decoder::open(std::move(lease), codec->v4l2Format, unsigned(width),
                                             unsigned(height), driver.preferredCapture(), error);
        if (!context->decoder) { driver.note(error.c_str()); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
        // Lend the decoder this driver's own buffers, so that the device
        // writes each picture where the client will read it. Only where a
        // buffer of this kind was shown to be importable: a queue that refuses
        // one falls back to buffers the device allocates, and asking it every
        // session only fills the log.
        if (driver.importsCapture()) {
            Context* raw = context.get();
            context->decoder->setCaptureSource(
                [&driver, raw](unsigned index, bool fresh, CaptureTarget& target) {
                    return driver.provideCapture(*raw, index, target, fresh);
                });
        }
        // The decoded format is only settled when the decoder has parsed the
        // stream, so it is reported with the layout at the first picture.
        context->reportLayout = true;
    } else if (isEncodeEntrypoint(context->entrypoint)) {
        if (!width || !height) return VA_STATUS_ERROR_INVALID_PARAMETER;
        const VAStatus status = driver.openEncoder(*context, found->second);
        if (status != VA_STATUS_SUCCESS) return status;
    }
    *id = driver.nextContext++;
    driver.contexts.emplace(*id, std::move(context));
    return VA_STATUS_SUCCESS;
}

VAStatus destroyContext(VADriverContextP ctx, VAContextID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Context* context = driver.context(id);
    if (!context) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (context->decoder) {
        std::string error;
        context->decoder->drain(error);
        driver.harvest(*context, std::chrono::milliseconds(200));
    }
    for (auto& entry : driver.surfaces)
        if (entry.second->owner == id) {
            entry.second->owner = VA_INVALID_ID;
            if (entry.second->state == Surface::State::pending) entry.second->state = Surface::State::failed;
        }
    driver.contexts.erase(id);
    return VA_STATUS_SUCCESS;
}

VAStatus createBuffer(VADriverContextP ctx, VAContextID, VABufferType type, unsigned size, unsigned elements,
                      void* data, VABufferID* id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    if (!size || !elements) return VA_STATUS_ERROR_INVALID_PARAMETER;
    auto buffer = std::make_unique<Buffer>();
    buffer->type = type;
    buffer->size = size;
    buffer->elements = elements;
    if (!resizeOwned(buffer->owned, std::uint64_t(size) * elements)) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (data) std::memcpy(buffer->owned.data(), data, buffer->owned.size());
    if (type == VAEncCodedBufferType) {
        buffer->coded.is = true;
        buffer->coded.segment.buf = buffer->owned.data();
    }
    *id = driver.nextBuffer++;
    driver.buffers.emplace(*id, std::move(buffer));
    return VA_STATUS_SUCCESS;
}

VAStatus bufferSetNumElements(VADriverContextP ctx, VABufferID id, unsigned elements)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Buffer* buffer = driver.buffer(id);
    if (!buffer || buffer->external) return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!resizeOwned(buffer->owned, std::uint64_t(buffer->size) * elements)) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    buffer->elements = elements;
    return VA_STATUS_SUCCESS;
}

VAStatus mapBuffer(VADriverContextP ctx, VABufferID id, void** address)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Buffer* buffer = driver.buffer(id);
    if (!buffer) return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->coded.is) {
        // Mapping a coded buffer waits for the frame that fills it, so a
        // client that never calls vaSyncSurface still gets the whole frame.
        const VAStatus status = driver.waitForCoded(*buffer);
        if (status != VA_STATUS_SUCCESS) return status;
        *address = &buffer->coded.segment;
        return VA_STATUS_SUCCESS;
    }
    std::uint8_t* data = buffer->data();
    if (!data) return VA_STATUS_ERROR_OPERATION_FAILED;
    *address = data;
    return VA_STATUS_SUCCESS;
}

VAStatus mapBuffer2(VADriverContextP ctx, VABufferID id, void** address, uint32_t)
{
    return mapBuffer(ctx, id, address);
}

VAStatus unmapBuffer(VADriverContextP ctx, VABufferID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    return driver.buffer(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus destroyBuffer(VADriverContextP ctx, VABufferID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    return driver.buffers.erase(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus bufferInfo(VADriverContextP ctx, VABufferID id, VABufferType* type, unsigned* size, unsigned* elements)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Buffer* buffer = driver.buffer(id);
    if (!buffer) return VA_STATUS_ERROR_INVALID_BUFFER;
    *type = buffer->type;
    *size = buffer->size;
    *elements = buffer->elements;
    return VA_STATUS_SUCCESS;
}

VAStatus beginPicture(VADriverContextP ctx, VAContextID id, VASurfaceID target)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Context* context = driver.context(id);
    if (!context) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!driver.surface(target)) return VA_STATUS_ERROR_INVALID_SURFACE;
    context->target = target;
    context->picture = true;
    context->sliceData.clear();
    if (context->encoder) context->encoder->beginPicture();
    context->havePipeline = false;
    context->pipelineSourceValid = false;
    context->pipelineDestinationValid = false;
    return VA_STATUS_SUCCESS;
}

VAStatus renderPicture(VADriverContextP ctx, VAContextID id, VABufferID* ids, int count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Context* context = driver.context(id);
    if (!context || !context->picture) return VA_STATUS_ERROR_INVALID_CONTEXT;
    for (int index = 0; index < count; ++index) {
        Buffer* buffer = driver.buffer(ids[index]);
        if (!buffer) return VA_STATUS_ERROR_INVALID_BUFFER;
        if (context->encoder) {
            const VAStatus status = driver.addEncodeBuffer(*context, *buffer);
            if (status != VA_STATUS_SUCCESS) return status;
            continue;
        }
        switch (buffer->type) {
        case VASliceDataBufferType: {
            const std::uint8_t* data = buffer->data();
            const std::size_t size = std::size_t(buffer->size) * buffer->elements;
            context->sliceData.insert(context->sliceData.end(), data, data + size);
            break;
        }
        case VAProcPipelineParameterBufferType: {
            if (std::size_t(buffer->size) * buffer->elements < sizeof(VAProcPipelineParameterBuffer))
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            std::memcpy(&context->pipeline, buffer->data(), sizeof(VAProcPipelineParameterBuffer));
            context->havePipeline = true;
            if (context->pipeline.surface_region) {
                context->pipelineSource = *context->pipeline.surface_region;
                context->pipelineSourceValid = true;
            }
            if (context->pipeline.output_region) {
                context->pipelineDestination = *context->pipeline.output_region;
                context->pipelineDestinationValid = true;
            }
            break;
        }
        case VAPictureParameterBufferType:
        case VASliceParameterBufferType:
        case VAIQMatrixBufferType:
        case VAProbabilityBufferType:
            // The stateful decoder reads these from the bitstream itself.
            break;
        default:
            break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus endPicture(VADriverContextP ctx, VAContextID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Context* context = driver.context(id);
    if (!context || !context->picture) return VA_STATUS_ERROR_INVALID_CONTEXT;
    context->picture = false;
    Surface* target = driver.surface(context->target);
    if (!target) return VA_STATUS_ERROR_INVALID_SURFACE;
    target->owner = id;
    const auto status = context->entrypoint == VAEntrypointVideoProc
        ? driver.process(*context, *target)
        : context->encoder ? driver.submitEncode(*context, context->target, *target)
                           : driver.submitFrame(*context, context->target, *target);
    context->sliceData.clear();
    return status;
}

VAStatus syncSurface(VADriverContextP ctx, VASurfaceID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface) return VA_STATUS_ERROR_INVALID_SURFACE;
    return driver.waitFor(*surface, kDecodeWait);
}

VAStatus syncSurface2(VADriverContextP ctx, VASurfaceID id, uint64_t timeout)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface) return VA_STATUS_ERROR_INVALID_SURFACE;
    const auto wait = timeout == VA_TIMEOUT_INFINITE
        ? kDecodeWait
        : std::chrono::milliseconds(timeout / 1000000ull);
    const auto status = driver.waitFor(*surface, wait);
    if (status != VA_STATUS_SUCCESS && surface->state == Surface::State::pending)
        return VA_STATUS_ERROR_TIMEDOUT;
    return status;
}

VAStatus syncBuffer(VADriverContextP ctx, VABufferID id, uint64_t)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    return driver.buffer(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus querySurfaceStatus(VADriverContextP ctx, VASurfaceID id, VASurfaceStatus* status)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface) return VA_STATUS_ERROR_INVALID_SURFACE;
    switch (surface->state) {
    case Surface::State::pending: *status = VASurfaceRendering; break;
    case Surface::State::failed: *status = VASurfaceReady; break;
    default: *status = VASurfaceReady; break;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus querySurfaceError(VADriverContextP, VASurfaceID, VAStatus, void** info)
{
    if (info) *info = nullptr;
    return VA_STATUS_SUCCESS;
}

const VAImageFormat kImageFormats[] = {
    {VA_FOURCC_NV12, VA_LSB_FIRST, 12, 0, 0, 0, 0, 0, {}},
    {VA_FOURCC_I420, VA_LSB_FIRST, 12, 0, 0, 0, 0, 0, {}},
    {VA_FOURCC_ARGB, VA_LSB_FIRST, 32, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000, {}},
};

VAStatus queryImageFormats(VADriverContextP, VAImageFormat* formats, int* count)
{
    const int total = int(sizeof(kImageFormats) / sizeof(kImageFormats[0]));
    for (int index = 0; index < total; ++index) formats[index] = kImageFormats[index];
    *count = total;
    return VA_STATUS_SUCCESS;
}

// Lays out a driver-allocated image. VAImage keeps every size and offset in
// 32 bits, so a layout that does not fit is refused instead of wrapping.
VAStatus imageLayout(std::uint32_t fourcc, unsigned width, unsigned height, VAImage& image) noexcept
{
    const std::uint64_t even = (std::uint64_t(width) + 1) & ~std::uint64_t(1);
    const std::uint64_t rows = (std::uint64_t(height) + 1) & ~std::uint64_t(1);
    std::uint64_t pitches[3] {};
    std::uint64_t offsets[3] {};
    std::uint64_t size = 0;
    unsigned planes = 0;
    switch (fourcc) {
    case VA_FOURCC_NV12:
        planes = 2;
        pitches[0] = even;
        pitches[1] = even;
        offsets[1] = even * rows;
        size = offsets[1] + even * (rows / 2);
        break;
    case VA_FOURCC_I420:
        planes = 3;
        pitches[0] = even;
        pitches[1] = even / 2;
        offsets[1] = even * rows;
        pitches[2] = even / 2;
        offsets[2] = offsets[1] + (even / 2) * (rows / 2);
        size = offsets[2] + (even / 2) * (rows / 2);
        break;
    case VA_FOURCC_ARGB:
        planes = 1;
        pitches[0] = std::uint64_t(width) * 4;
        size = pitches[0] * height;
        break;
    default:
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (size > std::numeric_limits<std::uint32_t>::max()) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    image.num_planes = planes;
    for (unsigned plane = 0; plane < planes; ++plane) {
        image.pitches[plane] = std::uint32_t(pitches[plane]);
        image.offsets[plane] = std::uint32_t(offsets[plane]);
    }
    image.data_size = std::uint32_t(size);
    return VA_STATUS_SUCCESS;
}

VAStatus createImage(VADriverContextP ctx, VAImageFormat* format, int width, int height, VAImage* out)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    // VAImage records the extent in 16 bits.
    constexpr int kLargest = std::numeric_limits<unsigned short>::max();
    if (!format || width <= 0 || height <= 0 || width > kLargest || height > kLargest)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    VAImage image {};
    image.format = *format;
    image.width = static_cast<unsigned short>(width);
    image.height = static_cast<unsigned short>(height);
    if (const auto status = imageLayout(format->fourcc, unsigned(width), unsigned(height), image);
        status != VA_STATUS_SUCCESS)
        return status;
    auto buffer = std::make_unique<Buffer>();
    buffer->type = VAImageBufferType;
    buffer->size = image.data_size;
    buffer->elements = 1;
    if (!resizeOwned(buffer->owned, image.data_size)) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    const auto bufferId = driver.nextBuffer++;
    driver.buffers.emplace(bufferId, std::move(buffer));
    image.buf = bufferId;
    image.image_id = driver.nextImage++;
    auto record = std::make_unique<Image>();
    record->image = image;
    driver.images.emplace(image.image_id, std::move(record));
    *out = image;
    return VA_STATUS_SUCCESS;
}

VAStatus deriveImage(VADriverContextP ctx, VASurfaceID id, VAImage* out)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface || !surface->buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    const auto status = driver.waitFor(*surface, kDecodeWait);
    if (status != VA_STATUS_SUCCESS) return status;
    driver.materialise(*surface);
    std::uint8_t* data = surface->buffer->map();
    if (!data) return VA_STATUS_ERROR_OPERATION_FAILED;
    VAImage image {};
    for (const auto& format : kImageFormats)
        if (format.fourcc == surface->fourcc) image.format = format;
    if (!image.format.fourcc) return VA_STATUS_ERROR_OPERATION_FAILED;
    image.width = static_cast<unsigned short>(surface->width);
    image.height = static_cast<unsigned short>(surface->height);
    image.num_planes = surface->buffer->planes();
    for (unsigned plane = 0; plane < image.num_planes; ++plane) {
        image.pitches[plane] = unsigned(surface->buffer->pitch(plane));
        image.offsets[plane] = unsigned(surface->buffer->offset(plane));
    }
    image.data_size = unsigned(surface->buffer->size());
    auto buffer = std::make_unique<Buffer>();
    buffer->type = VAImageBufferType;
    buffer->size = image.data_size;
    buffer->elements = 1;
    buffer->external = data;
    buffer->externalSize = surface->buffer->size();
    buffer->pin = surface->buffer;
    surface->exported = true;
    const auto bufferId = driver.nextBuffer++;
    driver.buffers.emplace(bufferId, std::move(buffer));
    image.buf = bufferId;
    image.image_id = driver.nextImage++;
    auto record = std::make_unique<Image>();
    record->image = image;
    driver.images.emplace(image.image_id, std::move(record));
    *out = image;
    return VA_STATUS_SUCCESS;
}

VAStatus destroyImage(VADriverContextP ctx, VAImageID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const auto found = driver.images.find(id);
    if (found == driver.images.end()) return VA_STATUS_ERROR_INVALID_IMAGE;
    driver.buffers.erase(found->second->image.buf);
    driver.images.erase(found);
    return VA_STATUS_SUCCESS;
}

// Copies a surface region into an image buffer, converting between the few
// layouts clients ask for.
VAStatus readSurface(Driver& driver, Surface& surface, const Region& region, Image& target)
{
    driver.materialise(surface);
    std::uint8_t* destination = nullptr;
    if (Buffer* buffer = driver.buffer(target.image.buf); buffer && buffer->bytes() >= target.image.data_size)
        destination = buffer->data();
    if (!surface.buffer) return VA_STATUS_ERROR_OPERATION_FAILED;
    const PixelBuffer::CpuAccess reading(*surface.buffer, false);
    const std::uint8_t* source = surface.buffer->map();
    if (!destination || !source) return VA_STATUS_ERROR_OPERATION_FAILED;
    const auto width = std::min<unsigned>(region.width, target.image.width);
    const auto height = std::min<unsigned>(region.height, target.image.height);
    if (surface.fourcc == VA_FOURCC_NV12 && target.image.format.fourcc == VA_FOURCC_NV12) {
        const PlaneView out[2] {{destination + target.image.offsets[0], target.image.pitches[0]},
                                {destination + target.image.offsets[1], target.image.pitches[1]}};
        copyNv12(planeViews(*surface.buffer, source, region.x, region.y).data(), out, width, height);
        return VA_STATUS_SUCCESS;
    }
    if (surface.fourcc == VA_FOURCC_NV12 && target.image.format.fourcc == VA_FOURCC_I420) {
        const auto [luma, chroma] = planeViews(*surface.buffer, source, region.x, region.y);
        for (unsigned row = 0; row < height; ++row)
            std::memcpy(destination + target.image.offsets[0] + std::size_t(row) * target.image.pitches[0],
                        luma.data + std::size_t(row) * luma.pitch, width);
        for (unsigned row = 0; row < (height + 1) / 2; ++row) {
            std::uint8_t* u = destination + target.image.offsets[1] + std::size_t(row) * target.image.pitches[1];
            std::uint8_t* v = destination + target.image.offsets[2] + std::size_t(row) * target.image.pitches[2];
            const std::uint8_t* pair = chroma.data + std::size_t(row) * chroma.pitch;
            for (unsigned column = 0; column < (width + 1) / 2; ++column) {
                u[column] = pair[2 * column];
                v[column] = pair[2 * column + 1];
            }
        }
        return VA_STATUS_SUCCESS;
    }
    if (surface.fourcc == VA_FOURCC_NV12 && target.image.format.fourcc == VA_FOURCC_ARGB) {
        const PlaneView out {destination + target.image.offsets[0], target.image.pitches[0]};
        convertNv12ToArgb(planeViews(*surface.buffer, source).data(), region, out, Region {0, 0, width, height},
                          isBt709(surface.height));
        return VA_STATUS_SUCCESS;
    }
    if (surface.fourcc == target.image.format.fourcc && surface.buffer->planes() == 1) {
        const PlaneView out {destination + target.image.offsets[0], target.image.pitches[0]};
        copyPacked32(planeView(*surface.buffer, source), region, out, Region {0, 0, width, height});
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus getImage(VADriverContextP ctx, VASurfaceID id, int x, int y, unsigned width, unsigned height,
                  VAImageID imageId)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    const auto found = driver.images.find(imageId);
    if (!surface || found == driver.images.end()) return VA_STATUS_ERROR_INVALID_IMAGE;
    const auto region = regionWithin(x, y, width, height, surface->width, surface->height);
    if (!region) return VA_STATUS_ERROR_INVALID_PARAMETER;
    const auto status = driver.waitFor(*surface, kDecodeWait);
    if (status != VA_STATUS_SUCCESS) return status;
    return readSurface(driver, *surface, *region, *found->second);
}

VAStatus putImage(VADriverContextP ctx, VASurfaceID id, VAImageID imageId, int sourceX, int sourceY,
                  unsigned sourceWidth, unsigned sourceHeight, int destinationX, int destinationY,
                  unsigned destinationWidth, unsigned destinationHeight)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    const auto found = driver.images.find(imageId);
    if (!surface || found == driver.images.end() || !surface->buffer) return VA_STATUS_ERROR_INVALID_IMAGE;
    const VAImage& image = found->second->image;
    const auto from = regionWithin(sourceX, sourceY, sourceWidth, sourceHeight, image.width, image.height);
    const auto to = regionWithin(destinationX, destinationY, destinationWidth, destinationHeight, surface->width,
                                 surface->height);
    if (!from || !to) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (sourceWidth != destinationWidth || sourceHeight != destinationHeight) return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (image.format.fourcc != surface->fourcc) return VA_STATUS_ERROR_UNIMPLEMENTED;
    Buffer* buffer = driver.buffer(image.buf);
    const PixelBuffer::CpuAccess writing(*surface->buffer, true);
    std::uint8_t* destination = surface->buffer->map();
    if (!buffer || !destination || buffer->bytes() < image.data_size) return VA_STATUS_ERROR_OPERATION_FAILED;
    const std::uint8_t* source = buffer->data();
    // Whatever the decoder left in a shadow is now stale. A shadow a client
    // holds is the surface's memory and stays; it is refreshed below.
    if (!surface->exportsArgb) surface->shadow.reset();
    surface->nativeValid = true;
    if (surface->fourcc == VA_FOURCC_NV12) {
        const ConstPlaneView in[2] {
            {source + image.offsets[0] + std::size_t(from->y) * image.pitches[0] + from->x, image.pitches[0]},
            {source + image.offsets[1] + std::size_t(from->y / 2) * image.pitches[1] + (from->x & ~1u),
             image.pitches[1]}};
        copyNv12(in, planeViews(*surface->buffer, destination, to->x, to->y).data(), to->width, to->height);
    } else {
        const ConstPlaneView in {source + image.offsets[0], image.pitches[0]};
        copyPacked32(in, *from, planeView(*surface->buffer, destination), *to);
    }
    driver.refreshExport(*surface);
    surface->state = Surface::State::ready;
    return VA_STATUS_SUCCESS;
}

// A surface's descriptor as packed ARGB: its RGB shadow, one object, one plane.
// The picture is made current for the client now and after every later decode.
VAStatus exportShadow(Driver& driver, Surface& surface, void* out)
{
    if (!surface.shadow) {
        std::string error;
        surface.shadow = PixelBuffer::allocate(VA_FOURCC_ARGB, surface.width, surface.height, error);
        if (!surface.shadow) {
            if (!error.empty()) driver.note(error.c_str());
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }
    surface.exportsArgb = true;
    if (surface.state == Surface::State::ready) driver.refreshExport(surface);
    const int fd = surface.shadow->exportDescriptor();
    if (fd < 0) return VA_STATUS_ERROR_OPERATION_FAILED;
    surface.exported = true;
    auto* descriptor = static_cast<VADRMPRIMESurfaceDescriptor*>(out);
    std::memset(descriptor, 0, sizeof(*descriptor));
    descriptor->fourcc = VA_FOURCC_ARGB;
    descriptor->width = surface.width;
    descriptor->height = surface.height;
    descriptor->num_objects = 1;
    descriptor->objects[0].fd = fd;
    descriptor->objects[0].size = unsigned(surface.shadow->size());
    descriptor->objects[0].drm_format_modifier = surface.shadow->modifier();
    descriptor->num_layers = 1;
    descriptor->layers[0].drm_format = DRM_FORMAT_ARGB8888;
    descriptor->layers[0].num_planes = 1;
    descriptor->layers[0].object_index[0] = 0;
    descriptor->layers[0].offset[0] = unsigned(surface.shadow->offset(0));
    descriptor->layers[0].pitch[0] = unsigned(surface.shadow->pitch(0));
    return VA_STATUS_SUCCESS;
}

VAStatus exportSurfaceHandle(VADriverContextP ctx, VASurfaceID id, uint32_t memoryType, uint32_t flags,
                             void* out)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface || !surface->buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    if (memoryType != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    if (driver.exportsPackedArgb() && surface->fourcc == VA_FOURCC_NV12) return exportShadow(driver, *surface, out);
    // A descriptor names the surface's own format, so it has to hold the
    // surface's own pixels even when the decoder delivered packed RGB.
    driver.materialise(*surface);
    const int fd = surface->buffer->exportDescriptor();
    if (fd < 0) return VA_STATUS_ERROR_OPERATION_FAILED;
    surface->exported = true;
    auto* descriptor = static_cast<VADRMPRIMESurfaceDescriptor*>(out);
    std::memset(descriptor, 0, sizeof(*descriptor));
    descriptor->fourcc = surface->fourcc;
    descriptor->width = surface->width;
    descriptor->height = surface->height;
    descriptor->num_objects = 1;
    descriptor->objects[0].fd = fd;
    descriptor->objects[0].size = unsigned(surface->buffer->size());
    descriptor->objects[0].drm_format_modifier = surface->buffer->modifier();
    const unsigned planes = surface->buffer->planes();
    if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
        descriptor->num_layers = planes;
        for (unsigned plane = 0; plane < planes; ++plane) {
            descriptor->layers[plane].drm_format = surface->fourcc == VA_FOURCC_NV12
                ? (plane == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88)
                : DRM_FORMAT_ARGB8888;
            descriptor->layers[plane].num_planes = 1;
            descriptor->layers[plane].object_index[0] = 0;
            descriptor->layers[plane].offset[0] = unsigned(surface->buffer->offset(plane));
            descriptor->layers[plane].pitch[0] = unsigned(surface->buffer->pitch(plane));
        }
    } else {
        descriptor->num_layers = 1;
        descriptor->layers[0].drm_format = surface->fourcc == VA_FOURCC_NV12 ? DRM_FORMAT_NV12 : DRM_FORMAT_ARGB8888;
        descriptor->layers[0].num_planes = planes;
        for (unsigned plane = 0; plane < planes; ++plane) {
            descriptor->layers[0].object_index[plane] = 0;
            descriptor->layers[0].offset[plane] = unsigned(surface->buffer->offset(plane));
            descriptor->layers[0].pitch[plane] = unsigned(surface->buffer->pitch(plane));
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus lockSurface(VADriverContextP ctx, VASurfaceID id, unsigned* fourcc, unsigned* lumaStride,
                     unsigned* chromaUStride, unsigned* chromaVStride, unsigned* lumaOffset,
                     unsigned* chromaUOffset, unsigned* chromaVOffset, unsigned* name, void** address)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface || !surface->buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    const auto status = driver.waitFor(*surface, kDecodeWait);
    if (status != VA_STATUS_SUCCESS) return status;
    if (fourcc) *fourcc = surface->fourcc;
    if (lumaStride) *lumaStride = unsigned(surface->buffer->pitch(0));
    if (lumaOffset) *lumaOffset = unsigned(surface->buffer->offset(0));
    const unsigned chromaPlane = surface->buffer->planes() > 1 ? 1 : 0;
    if (chromaUStride) *chromaUStride = unsigned(surface->buffer->pitch(chromaPlane));
    if (chromaVStride) *chromaVStride = unsigned(surface->buffer->pitch(chromaPlane));
    if (chromaUOffset) *chromaUOffset = unsigned(surface->buffer->offset(chromaPlane));
    if (chromaVOffset) *chromaVOffset = unsigned(surface->buffer->offset(chromaPlane));
    if (name) *name = 0;
    if (address) *address = surface->buffer->map();
    return VA_STATUS_SUCCESS;
}

VAStatus unlockSurface(VADriverContextP, VASurfaceID) { return VA_STATUS_SUCCESS; }

// libva refuses a driver with a hole in its table, so the parts of VA-API this
// backend does not serve answer honestly instead of being absent.
VAStatus setImagePalette(VADriverContextP, VAImageID, unsigned char*) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus createSubpicture(VADriverContextP, VAImageID, VASubpictureID*) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus destroySubpicture(VADriverContextP, VASubpictureID) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setSubpictureImage(VADriverContextP, VASubpictureID, VAImageID) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setSubpictureChromakey(VADriverContextP, VASubpictureID, unsigned, unsigned, unsigned)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
VAStatus setSubpictureGlobalAlpha(VADriverContextP, VASubpictureID, float) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus associateSubpicture(VADriverContextP, VASubpictureID, VASurfaceID*, int, short, short, unsigned short,
                             unsigned short, short, short, unsigned short, unsigned short, unsigned)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
VAStatus deassociateSubpicture(VADriverContextP, VASubpictureID, VASurfaceID*, int)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus unimplemented() { return VA_STATUS_ERROR_UNIMPLEMENTED; }

VAStatus putSurface(VADriverContextP, VASurfaceID, void*, short, short, unsigned short, unsigned short,
                    short, short, unsigned short, unsigned short, VARectangle*, unsigned, unsigned)
{
    return unimplemented();
}

VAStatus querySubpictureFormats(VADriverContextP, VAImageFormat*, unsigned*, unsigned* count)
{
    *count = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus queryDisplayAttributes(VADriverContextP, VADisplayAttribute*, int* count)
{
    *count = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus getDisplayAttributes(VADriverContextP, VADisplayAttribute*, int) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setDisplayAttributes(VADriverContextP, VADisplayAttribute*, int) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

VAStatus queryVideoProcFilters(VADriverContextP, VAContextID, VAProcFilterType*, unsigned* count)
{
    *count = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus queryVideoProcFilterCaps(VADriverContextP, VAContextID, VAProcFilterType, void*, unsigned* count)
{
    *count = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
}

VAStatus queryVideoProcPipelineCaps(VADriverContextP, VAContextID, VABufferID*, unsigned count,
                                    VAProcPipelineCaps* caps)
{
    if (count) return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
    if (!caps) return VA_STATUS_ERROR_INVALID_PARAMETER;
    std::memset(caps, 0, sizeof(*caps));
    caps->pipeline_flags = 0;
    caps->filter_flags = 0;
    caps->num_forward_references = 0;
    caps->num_backward_references = 0;
    caps->rotation_flags = 1u << VA_ROTATION_NONE;
    caps->min_input_width = 16;
    caps->min_input_height = 16;
    caps->max_input_width = 4096;
    caps->max_input_height = 4096;
    caps->min_output_width = 16;
    caps->min_output_height = 16;
    caps->max_output_width = 4096;
    caps->max_output_height = 4096;
    return VA_STATUS_SUCCESS;
}

}

VAStatus initializeDriver(VADriverContextP ctx, DescriptorPool& pool)
{
    if (!ctx) return VA_STATUS_ERROR_INVALID_CONTEXT;
    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = 16;
    ctx->max_entrypoints = 4;
    ctx->max_attributes = 16;
    ctx->max_image_formats = 4;
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 1;
    ctx->str_vendor = kVendor;

    auto* driver = new (std::nothrow) Driver(ctx, pool);
    if (!driver) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (driver->codecs().empty() && !driver->encoding()) {
        driver->note("no usable decoder or encoder; refusing to initialise");
        delete driver;
        return VA_STATUS_ERROR_HW_BUSY;
    }
    ctx->pDriverData = driver;

    auto* table = ctx->vtable;
    const bool trace = traceRequested();
    driver->setTracing(trace);
    // The tracing form wraps the same function; a name appears once.
#define UML_BIND(field, function) table->field = trace ? &Traced<#field, function>::call : function
    UML_BIND(vaTerminate, terminate);
    UML_BIND(vaQueryConfigProfiles, queryConfigProfiles);
    UML_BIND(vaQueryConfigEntrypoints, queryConfigEntrypoints);
    UML_BIND(vaGetConfigAttributes, getConfigAttributes);
    UML_BIND(vaCreateConfig, createConfig);
    UML_BIND(vaDestroyConfig, destroyConfig);
    UML_BIND(vaQueryConfigAttributes, queryConfigAttributes);
    UML_BIND(vaCreateSurfaces, createSurfaces);
    UML_BIND(vaCreateSurfaces2, createSurfaces2);
    UML_BIND(vaDestroySurfaces, destroySurfaces);
    UML_BIND(vaQuerySurfaceAttributes, querySurfaceAttributes);
    UML_BIND(vaCreateContext, createContext);
    UML_BIND(vaDestroyContext, destroyContext);
    UML_BIND(vaCreateBuffer, createBuffer);
    UML_BIND(vaBufferSetNumElements, bufferSetNumElements);
    UML_BIND(vaMapBuffer, mapBuffer);
    UML_BIND(vaMapBuffer2, mapBuffer2);
    UML_BIND(vaUnmapBuffer, unmapBuffer);
    UML_BIND(vaDestroyBuffer, destroyBuffer);
    UML_BIND(vaBufferInfo, bufferInfo);
    UML_BIND(vaBeginPicture, beginPicture);
    UML_BIND(vaRenderPicture, renderPicture);
    UML_BIND(vaEndPicture, endPicture);
    UML_BIND(vaSyncSurface, syncSurface);
    UML_BIND(vaSyncSurface2, syncSurface2);
    UML_BIND(vaSyncBuffer, syncBuffer);
    UML_BIND(vaQuerySurfaceStatus, querySurfaceStatus);
    UML_BIND(vaQuerySurfaceError, querySurfaceError);
    UML_BIND(vaPutSurface, putSurface);
    UML_BIND(vaQueryImageFormats, queryImageFormats);
    UML_BIND(vaCreateImage, createImage);
    UML_BIND(vaDeriveImage, deriveImage);
    UML_BIND(vaDestroyImage, destroyImage);
    UML_BIND(vaGetImage, getImage);
    UML_BIND(vaPutImage, putImage);
    UML_BIND(vaQuerySubpictureFormats, querySubpictureFormats);
    UML_BIND(vaQueryDisplayAttributes, queryDisplayAttributes);
    UML_BIND(vaGetDisplayAttributes, getDisplayAttributes);
    UML_BIND(vaSetDisplayAttributes, setDisplayAttributes);
    UML_BIND(vaSetImagePalette, setImagePalette);
    UML_BIND(vaCreateSubpicture, createSubpicture);
    UML_BIND(vaDestroySubpicture, destroySubpicture);
    UML_BIND(vaSetSubpictureImage, setSubpictureImage);
    UML_BIND(vaSetSubpictureChromakey, setSubpictureChromakey);
    UML_BIND(vaSetSubpictureGlobalAlpha, setSubpictureGlobalAlpha);
    UML_BIND(vaAssociateSubpicture, associateSubpicture);
    UML_BIND(vaDeassociateSubpicture, deassociateSubpicture);
    UML_BIND(vaLockSurface, lockSurface);
    UML_BIND(vaUnlockSurface, unlockSurface);
    UML_BIND(vaExportSurfaceHandle, exportSurfaceHandle);

    if (auto* vpp = ctx->vtable_vpp) {
        vpp->version = VA_DRIVER_VTABLE_VPP_VERSION;
        vpp->vaQueryVideoProcFilters = queryVideoProcFilters;
        vpp->vaQueryVideoProcFilterCaps = queryVideoProcFilterCaps;
        vpp->vaQueryVideoProcPipelineCaps = queryVideoProcPipelineCaps;
    }
#undef UML_BIND
    return VA_STATUS_SUCCESS;
}

}
