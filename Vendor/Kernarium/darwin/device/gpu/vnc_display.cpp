// SPDX-License-Identifier: GPL-2.0-or-later

#include "vnc_display.hpp"

#include "display_activity_log.hpp"

#if UML_GPU_VNC_STARTUP_TESTING
#include "vnc_display_testing.hpp"
#endif

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <utility>

extern "C" {
#include <rfb/rfb.h>
}

namespace uml::gpu {

namespace {

namespace vnc_input = uml::input::vnc;

constexpr std::uint64_t clientCountMask =
    std::numeric_limits<std::uint32_t>::max();
constexpr unsigned connectionEpochShift = 32;

/* A scanout only the CPU path can read refreshes on the guest's next flush,
 * which a static screen never sends. */
constexpr auto freshFrameHoldLimit = std::chrono::seconds(2);

/* A client's clientData holds its connection epoch, with the low bit set
 * until its first framebuffer update request has been checked. */
static_assert(sizeof(std::uintptr_t) >= sizeof(std::uint64_t));

[[nodiscard]] void* makeClientData(
    const std::uint32_t epoch,
    const bool firstUpdatePending) noexcept
{
    return reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(epoch) << 1U |
        (firstUpdatePending ? 1U : 0U));
}

[[nodiscard]] constexpr std::uint32_t stateClientCount(
    const std::uint64_t state) noexcept
{
    return static_cast<std::uint32_t>(state & clientCountMask);
}

[[nodiscard]] constexpr std::uint32_t stateConnectionEpoch(
    const std::uint64_t state) noexcept
{
    return static_cast<std::uint32_t>(state >> connectionEpochShift);
}

[[nodiscard]] constexpr std::uint64_t makeClientState(
    const std::uint32_t epoch,
    const std::uint32_t count) noexcept
{
    return static_cast<std::uint64_t>(epoch) << connectionEpochShift | count;
}

class ScreenInitializationGuard final {
public:
    explicit ScreenInitializationGuard(rfbScreenInfoPtr& screen) noexcept
        : screen_(screen)
    {
    }

    ~ScreenInitializationGuard()
    {
        if (!armed_ || screen_ == nullptr)
            return;
        if (initialized_)
            rfbShutdownServer(screen_, TRUE);
        rfbScreenCleanup(screen_);
        screen_ = nullptr;
    }

    void initialized() noexcept { initialized_ = true; }
    void release() noexcept { armed_ = false; }

private:
    rfbScreenInfoPtr& screen_;
    bool initialized_ {};
    bool armed_ {true};
};

} // namespace

VncDisplay::VncDisplay(
    const std::string& bindAddress,
    std::uint16_t port,
    std::string password,
    std::uint32_t width,
    std::uint32_t height,
    vnc_input::VncInputSink inputSink,
    vnc_input::PointerMode pointerMode)
    : inputSink_(std::move(inputSink))
    , framebuffer_(width, height, FramebufferStorage::retained)
    , geometry_(width, height, false)
    , password_(std::move(password))
    , pointerMode_(pointerMode)
{
    int argumentCount = 0;
    screen_ = rfbGetScreen(
        &argumentCount,
        nullptr,
        static_cast<int>(width),
        static_cast<int>(height),
        8,
        3,
        4);
    if (screen_ == nullptr) {
        throw std::runtime_error("rfbGetScreen failed");
    }
    ScreenInitializationGuard screenGuard{screen_};
    screen_->desktopName = "Darwin UML Virtio-GPU";
    screen_->frameBuffer = reinterpret_cast<char*>(framebuffer_.data());
    screen_->screenData = this;
    /* The GPU transport publishes pixels on a different thread from
     * LibVNCServer's event loop. LibVNCServer deliberately leaves ownership
     * of frameBuffer synchronization to its caller, so bracket every encoder
     * read with the same mutex used by clear(), update(), and the self-test
     * producer. The event loop is single-threaded in this integration. */
    screen_->displayHook = [](rfbClientPtr client) noexcept {
        static_cast<VncDisplay*>(client->screen->screenData)
            ->framebuffer_.lock();
    };
    screen_->displayFinishedHook = [](rfbClientPtr client, int) noexcept {
        static_cast<VncDisplay*>(client->screen->screenData)
            ->framebuffer_.unlock();
    };
    screen_->newClientHook = [](rfbClientPtr client) noexcept {
        auto* display =
            static_cast<VncDisplay*>(client->screen->screenData);
        const auto epoch = display->clientConnected();
        if (!epoch)
            return RFB_CLIENT_REFUSE;
        client->clientData = makeClientData(*epoch, true);
        client->clientGoneHook = [](rfbClientPtr disconnected) noexcept {
            static_cast<VncDisplay*>(disconnected->screen->screenData)
                ->clientDisconnected(disconnected);
        };
        client->clientFramebufferUpdateRequestHook =
            [](rfbClientPtr requesting,
               rfbFramebufferUpdateRequestMsg*) noexcept {
                static_cast<VncDisplay*>(requesting->screen->screenData)
                    ->holdFirstUpdate(requesting);
            };
        return RFB_CLIENT_ACCEPT;
    };
    screen_->kbdAddEvent = [](
                               rfbBool down,
                               rfbKeySym keySymbol,
                               rfbClientPtr client) noexcept {
        static_cast<VncDisplay*>(client->screen->screenData)
            ->handleKey(down != FALSE, keySymbol);
    };
    screen_->ptrAddEvent = [](
                               int buttonMask,
                               int x,
                               int y,
                               rfbClientPtr client) noexcept {
        rfbDefaultPtrAddEvent(buttonMask, x, y, client);
        static_cast<VncDisplay*>(client->screen->screenData)
            ->handlePointer(buttonMask, x, y);
    };
    screen_->alwaysShared = TRUE;
    const bool chooseVncPort = port == 0;
    // LibVNCServer's autoPort mode scans from the current positive port; it
    // does not treat port zero as an OS-assigned ephemeral port.
    screen_->port = chooseVncPort ? 5900 : static_cast<int>(port);
    screen_->autoPort = chooseVncPort ? TRUE : FALSE;
    screen_->ipv6port = -1;
    in_addr interfaceAddress {};
    if (::inet_pton(AF_INET, bindAddress.c_str(), &interfaceAddress) != 1) {
        throw std::invalid_argument(
            "invalid VNC IPv4 bind address: " + bindAddress);
    }
    screen_->listenInterface = interfaceAddress.s_addr;
    screen_->serverFormat.bitsPerPixel = 32;
    screen_->serverFormat.depth = 24;
    screen_->serverFormat.bigEndian = FALSE;
    screen_->serverFormat.trueColour = TRUE;
    screen_->serverFormat.redMax = 255;
    screen_->serverFormat.greenMax = 255;
    screen_->serverFormat.blueMax = 255;
    screen_->serverFormat.redShift = 16;
    screen_->serverFormat.greenShift = 8;
    screen_->serverFormat.blueShift = 0;
    if (!password_.empty()) {
        if (password_.size() > 8) {
            throw std::invalid_argument(
                "VNC password must contain at most 8 bytes");
        }
        passwordList_[0] = password_.data();
        screen_->authPasswdData = passwordList_.data();
        screen_->passwordCheck = rfbCheckPasswordByList;
    }
    rfbInitServer(screen_);
    screenGuard.initialized();
    if (!rfbIsActive(screen_) || screen_->port <= 0 ||
        screen_->listenSock == RFB_INVALID_SOCKET) {
        throw std::runtime_error("VNC server failed to listen");
    }
#if UML_GPU_VNC_STARTUP_TESTING
    testing::eventThreadStartCheckpoint(
        static_cast<std::uint16_t>(screen_->port));
#endif
    metalReadback_ = std::make_unique<MetalReadback>(
        [this](const MetalReadbackFrame& frame) noexcept {
            return publishMetalFrame(frame);
        });
    eventThread_ = std::thread(&VncDisplay::eventLoop, this);
    screenGuard.release();
}

VncDisplay::~VncDisplay()
{
    if (metalReadback_)
        metalReadback_->shutdown();
    running_.store(false, std::memory_order_release);
    if (eventThread_.joinable())
        eventThread_.join();
    if (screen_ != nullptr) {
        rfbShutdownServer(screen_, TRUE);
        rfbScreenCleanup(screen_);
    }
}

std::uint32_t VncDisplay::width() const noexcept
{
    return framebuffer_.width();
}

std::uint32_t VncDisplay::height() const noexcept
{
    return framebuffer_.height();
}

std::uint16_t VncDisplay::port() const noexcept
{
    return screen_ == nullptr ? 0 : static_cast<std::uint16_t>(screen_->port);
}

std::optional<std::uint16_t> VncDisplay::vncPort() const noexcept
{
    return port();
}

DisplayRectangle VncDisplay::placement() const noexcept
{
    return aspectFitPixels(
        {.width = framebuffer_.width(), .height = framebuffer_.height()},
        geometry_.activeGuestSize());
}

ReadbackTarget VncDisplay::readbackTarget() const noexcept
{
    return {
        .placement = placement(),
        .epoch = frameEpoch_.load(std::memory_order_relaxed),
    };
}

void VncDisplay::advanceFrameEpochLocked() noexcept
{
    frameEpoch_.fetch_add(1, std::memory_order_relaxed);
}

bool VncDisplay::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    applyGuestMode(width, height);
    return true;
}

void VncDisplay::applyGuestMode(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    /* Only the GPU thread changes the mode, so this check needs no lock. */
    const auto active = geometry_.activeGuestSize();
    if (width == 0 || height == 0 ||
        (active.width == width && active.height == height)) {
        return;
    }
    DisplayRectangle next;
    {
        /* The contact or buttons the guest holds were placed through the old
         * rectangle, so the guest must not see them move with it. */
        std::lock_guard pointerLock(pointerMutex_);
        releasePointerLocked();
        std::lock_guard geometryLock(geometryMutex_);
        const auto previous = placement();
        (void)geometry_.guestScanoutChanged(width, height);
        next = placement();
        /* Neither path has drawn the new mode yet, so a readback of the old
         * one is discarded, the first of the new one reads its whole view,
         * and a CPU fallback reads its whole scanout again. */
        advanceFrameEpochLocked();
        fullFrameEpoch_.store(0, std::memory_order_release);
        if (next != previous) {
            /* The bars around the new rectangle show nothing. */
            std::lock_guard lock(framebuffer_);
            framebuffer_.clearLocked();
            rfbMarkRectAsModified(
                screen_,
                0,
                0,
                static_cast<int>(framebuffer_.width()),
                static_cast<int>(framebuffer_.height()));
        }
    }
    /* A remembered scanout of the old mode does not describe the new one. */
    metalReadback_->forget();
    if (next.width == width && next.height == height) {
        stagingImage_.reset();
        metalReadback_->releaseStaging();
    }
    displayActivityLog().record(
        "vnc mode=%ux%u placement=%ux%u+%u+%u",
        width,
        height,
        next.width,
        next.height,
        next.x,
        next.y);
}

void VncDisplay::resetGuestSession() noexcept
{
    {
        std::lock_guard pointerLock(pointerMutex_);
        releasePointerLocked();
        std::lock_guard geometryLock(geometryMutex_);
        geometry_.resetGuestSession(
            {.width = framebuffer_.width(), .height = framebuffer_.height()});
        advanceFrameEpochLocked();
        fullFrameEpoch_.store(0, std::memory_order_release);
    }
    stagingImage_.reset();
    metalReadback_->releaseStaging();
    metalReadback_->forget();
}

void VncDisplay::clear() noexcept
{
    {
        std::lock_guard scanoutLock(scanoutMutex_);
        idleFramesDropped_ = false;
        metalReadback_->forget();
    }
    /* A CPU fallback's staged image is part of the picture being cleared. */
    stagingImage_.reset();
    metalReadback_->releaseStaging();
    /* The framebuffer holds no guest image now, so a readback requested
     * before this one must not repaint it or count as a complete frame, and
     * a CPU fallback reads its whole scanout again. */
    std::lock_guard geometryLock(geometryMutex_);
    advanceFrameEpochLocked();
    fullFrameEpoch_.store(0, std::memory_order_release);
    std::lock_guard lock(framebuffer_);
    framebuffer_.clearLocked();
    // Scaled client framebuffers are derived synchronously by this call, so
    // keep them in the same publication critical section as the source image.
    rfbMarkRectAsModified(
        screen_, 0, 0, static_cast<int>(width()), static_cast<int>(height()));
}

void VncDisplay::update(
    std::span<const std::uint8_t> source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::size_t sourceRowBytes,
    protocol::GpuFormat sourceFormat,
    std::uint32_t sourceX,
    std::uint32_t sourceY,
    std::uint32_t destinationX,
    std::uint32_t destinationY,
    std::uint32_t width,
    std::uint32_t height)
{
    const auto updateEpoch = connectionEpoch();
    const auto mode = geometry_.activeGuestSize();
    const auto target = readbackTarget();
    const auto& placed = target.placement;
    /* The CPU path now owns the image, so a remembered native scanout would
     * only overwrite it with an older one. */
    metalReadback_->forget();
    if (destinationX >= mode.width || destinationY >= mode.height)
        return;
    width = std::min(width, mode.width - destinationX);
    height = std::min(height, mode.height - destinationY);
    if (placed.width != mode.width || placed.height != mode.height) {
        /* Keep the whole mode here and let the GPU scale it, exactly as it
         * scales a native scanout. */
        if (!stagingImage_ || stagingImage_->width() != mode.width ||
            stagingImage_->height() != mode.height) {
            stagingImage_.reset();
            stagingImage_ =
                std::make_unique<ScanoutFramebuffer>(mode.width, mode.height);
        }
        const auto staged = stagingImage_->update(
            source,
            sourceWidth,
            sourceHeight,
            sourceRowBytes,
            sourceFormat,
            sourceX,
            sourceY,
            destinationX,
            destinationY,
            width,
            height);
        if (staged.width == 0 || staged.height == 0)
            return;
        const auto scanout = metalReadback_->stage(
            stagingImage_->pixels(),
            stagingImage_->rowBytes(),
            mode,
            {
                .x = staged.x,
                .y = staged.y,
                .width = staged.width,
                .height = staged.height,
            });
        if (!scanout) {
            displayActivityLog().record(
                "present display=vnc staging=failed mode=%ux%u",
                mode.width,
                mode.height);
            reportStagingFailure();
            return;
        }
        presentStaged(*scanout, target);
        return;
    }
    bool complete = false;
    {
        /* This copy is newer than any readback still in flight, which must
         * not paint over it. */
        std::lock_guard geometryLock(geometryMutex_);
        advanceFrameEpochLocked();
        const auto rectangle = framebuffer_.update(
            source,
            sourceWidth,
            sourceHeight,
            sourceRowBytes,
            sourceFormat,
            sourceX,
            sourceY,
            placed.x + destinationX,
            placed.y + destinationY,
            width,
            height);
        if (rectangle.width == 0 || rectangle.height == 0)
            return;
        complete = rectangle.x == placed.x && rectangle.y == placed.y &&
            rectangle.width == placed.width &&
            rectangle.height == placed.height;
        std::lock_guard lock(framebuffer_);
        rfbMarkRectAsModified(
            screen_,
            static_cast<int>(rectangle.x),
            static_cast<int>(rectangle.y),
            static_cast<int>(rectangle.x + rectangle.width),
            static_cast<int>(rectangle.y + rectangle.height));
        if (complete)
            recordFullFrame(updateEpoch);
    }
    if (complete) {
        std::lock_guard scanoutLock(scanoutMutex_);
        idleFramesDropped_ = false;
    }
}

void VncDisplay::fillSelfTestPattern()
{
    std::lock_guard lock(framebuffer_);
    framebuffer_.fillSelfTestPatternLocked();
    rfbMarkRectAsModified(
        screen_, 0, 0, static_cast<int>(width()), static_cast<int>(height()));
}

PresentOutcome VncDisplay::presentMetal(
    const MetalScanout& scanout) noexcept
{
    /* The scanout rectangle is the guest mode, which the device reports
     * through guestScanoutChanged before it presents a scanout of it. */
    const auto target = readbackTarget();
    std::unique_lock scanoutLock(scanoutMutex_);
    const auto state = clientState_.load(std::memory_order_acquire);
    /* With no remote consumer, accepting and dropping the scanout avoids the
     * Metal-to-CPU readback entirely. The framebuffer falls behind, so keep
     * the scanout for the next client to read back when it connects. */
    if (stateClientCount(state) == 0) {
        idleFramesDropped_ = true;
        const bool remembered = metalReadback_->remember(scanout, target);
        scanoutLock.unlock();
        displayActivityLog().record(
            "present display=vnc drop=no-client remembered=%d",
            remembered ? 1 : 0);
        return PresentOutcome::dropped;
    }
    /* A Venus scanout is a Metal texture with no guest-visible virgl
     * resource, so the CPU framebuffer path cannot reach it. The readback
     * converts it off the guest timeline, scaling it on the GPU when the
     * mode needs it; a scanout it cannot convert returns unsupported, which
     * deliberately selects the ordinary CPU framebuffer path. */
    const auto outcome = metalReadback_->present(
        scanout, target, stateConnectionEpoch(state));
    scanoutLock.unlock();
    if (outcome != PresentOutcome::unsupported)
        return outcome;
    displayActivityLog().record("present display=vnc readback=rejected");
    return PresentOutcome::unsupported;
}

void VncDisplay::presentStaged(
    const MetalScanout& scanout,
    ReadbackTarget target) noexcept
{
    std::unique_lock scanoutLock(scanoutMutex_);
    const auto state = clientState_.load(std::memory_order_acquire);
    /* As for a native scanout, nothing is read back for nobody; a client
     * that connects later reads the staged image then. */
    if (stateClientCount(state) == 0) {
        idleFramesDropped_ = true;
        (void)metalReadback_->remember(
            scanout, target, ReadbackSource::staged);
        return;
    }
    const auto outcome = metalReadback_->present(
        scanout, target, stateConnectionEpoch(state), ReadbackSource::staged);
    scanoutLock.unlock();
    if (outcome == PresentOutcome::unsupported) {
        displayActivityLog().record("present display=vnc staged=rejected");
        reportStagingFailure();
    }
}

bool VncDisplay::needsFullCpuFallback() const noexcept
{
    const auto state = clientState_.load(std::memory_order_acquire);
    return stateClientCount(state) != 0 &&
        fullFrameEpoch_.load(std::memory_order_acquire) !=
            stateConnectionEpoch(state);
}

std::optional<std::uint32_t> VncDisplay::clientConnected() noexcept
{
    std::lock_guard scanoutLock(scanoutMutex_);
    auto state = clientState_.load(std::memory_order_relaxed);
    while (true) {
        const auto count = stateClientCount(state);
        const auto epoch = stateConnectionEpoch(state);
        if (count == std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        const auto desired = makeClientState(
            count == 0 ? epoch + 1U : epoch,
            count + 1U);
        if (!clientState_.compare_exchange_weak(
                state,
                desired,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            continue;
        }
        const auto connected = stateConnectionEpoch(desired);
        /* Frames dropped while nobody watched never reached the
         * framebuffer. The first client of this epoch waits for a complete
         * frame newer than its connection. A remembered native scanout
         * provides it at once, and so does a remembered staged image of a
         * CPU-path scanout shown scaled; a CPU-path scanout shown pixel for
         * pixel provides it on the guest's next flush. */
        if (count == 0 && std::exchange(idleFramesDropped_, false)) {
            staleEpoch_ = connected;
            const bool refreshing = metalReadback_->refresh(connected);
            displayActivityLog().record(
                "vnc connect epoch=%u stale=1 refresh=%s",
                connected,
                refreshing ? "metal" : "next-flush");
        }
        return connected;
    }
}

void VncDisplay::holdFirstUpdate(rfbClientPtr client) noexcept
{
    const auto data = reinterpret_cast<std::uintptr_t>(client->clientData);
    if ((data & 1U) == 0)
        return;
    const auto epoch = static_cast<std::uint32_t>(data >> 1U);
    client->clientData = makeClientData(epoch, false);
    if (epoch != staleEpoch_ ||
        fullFrameEpoch_.load(std::memory_order_acquire) >= epoch) {
        return;
    }
    /* A held client's messages and updates wait in LibVNCServer; this
     * request is answered once it is released. */
    client->onHold = TRUE;
    try {
        heldClients_.push_back(HeldClient {
            .client = client,
            .epoch = epoch,
            .deadline =
                std::chrono::steady_clock::now() + freshFrameHoldLimit,
        });
    } catch (...) {
        rfbStartOnHoldClient(client);
    }
}

void VncDisplay::releaseHeldClients() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    const auto fresh = fullFrameEpoch_.load(std::memory_order_acquire);
    std::erase_if(heldClients_, [&](const HeldClient& held) {
        const bool current = fresh >= held.epoch;
        if (!current && now < held.deadline)
            return false;
        if (!current) {
            displayActivityLog().record(
                "vnc hold expired epoch=%u", held.epoch);
        }
        rfbStartOnHoldClient(held.client);
        return true;
    });
}

void VncDisplay::clientDisconnected(rfbClientPtr client) noexcept
{
    std::erase_if(heldClients_, [client](const HeldClient& held) {
        return held.client == client;
    });
    auto state = clientState_.load(std::memory_order_relaxed);
    while (stateClientCount(state) != 0) {
        const auto desired = makeClientState(
            stateConnectionEpoch(state), stateClientCount(state) - 1U);
        if (clientState_.compare_exchange_weak(
                state,
                desired,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
    }
}

std::uint32_t VncDisplay::connectionEpoch() const noexcept
{
    return stateConnectionEpoch(
        clientState_.load(std::memory_order_acquire));
}

void VncDisplay::recordFullFrame(const std::uint32_t epoch) noexcept
{
    auto completed = fullFrameEpoch_.load(std::memory_order_relaxed);
    while (completed < epoch &&
           !fullFrameEpoch_.compare_exchange_weak(
               completed,
               epoch,
               std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

DisplayStatistics VncDisplay::statistics() const noexcept
{
    return metalReadback_ ? metalReadback_->statistics()
                          : DisplayStatistics {};
}

void VncDisplay::recordCpuFallbackFrame() noexcept
{
    if (metalReadback_)
        metalReadback_->recordCpuFallbackFrame();
}

bool VncDisplay::publishMetalFrame(
    const MetalReadbackFrame& frame) noexcept
{
    try {
        std::lock_guard geometryLock(geometryMutex_);
        const auto epoch = frameEpoch_.load(std::memory_order_relaxed);
        if (frame.target.epoch != epoch) {
            displayActivityLog().record(
                "vnc discard frame epoch=%llu current=%llu "
                "placement=%ux%u+%u+%u",
                static_cast<unsigned long long>(frame.target.epoch),
                static_cast<unsigned long long>(epoch),
                frame.target.placement.width,
                frame.target.placement.height,
                frame.target.placement.x,
                frame.target.placement.y);
            return false;
        }
        const auto rectangle = framebuffer_.update(
            frame.pixels,
            frame.width,
            frame.height,
            frame.rowBytes,
            frame.format,
            0,
            0,
            frame.destinationX,
            frame.destinationY,
            frame.width,
            frame.height);
        if (rectangle.width == 0 || rectangle.height == 0)
            return false;
        {
            std::lock_guard lock(framebuffer_);
            rfbMarkRectAsModified(
                screen_,
                static_cast<int>(rectangle.x),
                static_cast<int>(rectangle.y),
                static_cast<int>(rectangle.x + rectangle.width),
                static_cast<int>(rectangle.y + rectangle.height));
        }
        if (frame.complete)
            recordFullFrame(frame.generation);
        return true;
    } catch (...) {
        return false;
    }
}

#if UML_GPU_VNC_STARTUP_TESTING
bool VncDisplay::readbackIdleForTesting() const noexcept
{
    return metalReadback_->idleForTesting();
}

std::uint64_t VncDisplay::eventLoopPassesForTesting() const noexcept
{
    return eventLoopPasses_.load(std::memory_order_acquire);
}

std::array<std::uint8_t, 4> VncDisplay::pixelForTesting(
    std::uint32_t x, std::uint32_t y) noexcept
{
    if (x >= width() || y >= height())
        return {};
    std::lock_guard lock(framebuffer_);
    const auto pixels = framebuffer_.pixels();
    const auto offset =
        (static_cast<std::size_t>(y) * width() + x) *
        protocol::gpuBytesPerPixel;
    return {
        pixels[offset],
        pixels[offset + 1U],
        pixels[offset + 2U],
        pixels[offset + 3U],
    };
}
#endif

void VncDisplay::handleKey(bool down, std::uint32_t keySymbol) noexcept
{
    if (!inputSink_)
        return;
    const auto code = vnc_input::keyCode(keySymbol);
    if (!code)
        return;
    const std::array events {
        vnc_input::VncInputEvent {
            .type = vnc_input::eventKey,
            .code = *code,
            .value = down ? 1 : 0,
        },
        vnc_input::VncInputEvent {
            .type = vnc_input::eventSync,
            .code = vnc_input::syncReport,
            .value = 0,
        },
    };
    if (!vnc_input::tryDeliver(inputSink_, events))
        reportInputFailure();
}

void VncDisplay::handlePointer(int buttonMask, int x, int y) noexcept
{
    if (!inputSink_)
        return;
    /* A mode change releases the pointer and moves the placement under this
     * lock, so an event mapped under it uses the rectangle the release left:
     * one read before it could deliver a press in what is now a bar. */
    std::lock_guard lock(pointerMutex_);
    const auto target = placement();
    const AspectFitRectangle content {
        .x = static_cast<double>(target.x),
        .y = static_cast<double>(target.y),
        .width = static_cast<double>(target.width),
        .height = static_cast<double>(target.height),
    };
    /* A VNC position names a framebuffer pixel. Its centre is where the
     * presenter sampled the guest, so it maps onto the guest pixel shown
     * there. */
    const DisplayPoint location {.x = x + 0.5, .y = y + 0.5};
    const auto coordinate = [](double normalized) {
        return vnc_input::normalizedAbsoluteCoordinate(normalized, 1.0);
    };

    std::array<vnc_input::VncInputEvent, 10> events {};
    std::size_t count = 0;
    const auto append = [&](std::uint16_t type,
                            std::uint16_t code,
                            std::int32_t value) {
        events[count++] = {.type = type, .code = code, .value = value};
    };
    const auto deliver = [&] {
        append(vnc_input::eventSync, vnc_input::syncReport, 0);
        if (!vnc_input::tryDeliver(inputSink_, std::span(events).first(count)))
            reportInputFailure();
        count = 0;
    };

    if (pointerMode_ == vnc_input::PointerMode::touchscreen) {
        for (const auto& sample :
             pointer_.touch(content, location, (buttonMask & 1) != 0)) {
            append(
                vnc_input::eventAbsolute,
                vnc_input::absoluteX,
                coordinate(sample.point.x));
            append(
                vnc_input::eventAbsolute,
                vnc_input::absoluteY,
                coordinate(sample.point.y));
            append(
                vnc_input::eventKey,
                vnc_input::buttonTouch,
                sample.buttons != 0 ? 1 : 0);
            deliver();
        }
        return;
    }
    for (const auto& sample : pointer_.mouse(
             content, location, static_cast<std::uint32_t>(buttonMask))) {
        append(
            vnc_input::eventAbsolute,
            vnc_input::absoluteX,
            coordinate(sample.point.x));
        append(
            vnc_input::eventAbsolute,
            vnc_input::absoluteY,
            coordinate(sample.point.y));
        append(
            vnc_input::eventKey,
            vnc_input::buttonLeft,
            static_cast<std::int32_t>(sample.buttons & 1U));
        append(
            vnc_input::eventKey,
            vnc_input::buttonMiddle,
            static_cast<std::int32_t>((sample.buttons >> 1U) & 1U));
        append(
            vnc_input::eventKey,
            vnc_input::buttonRight,
            static_cast<std::int32_t>((sample.buttons >> 2U) & 1U));
        if ((buttonMask & 8) != 0)
            append(vnc_input::eventRelative, vnc_input::relativeWheel, 1);
        if ((buttonMask & 16) != 0)
            append(vnc_input::eventRelative, vnc_input::relativeWheel, -1);
        if ((buttonMask & 32) != 0)
            append(
                vnc_input::eventRelative,
                vnc_input::relativeHorizontalWheel,
                -1);
        if ((buttonMask & 64) != 0)
            append(
                vnc_input::eventRelative,
                vnc_input::relativeHorizontalWheel,
                1);
        deliver();
    }
}

void VncDisplay::releasePointerLocked() noexcept
{
    if (!pointer_.release(true) || !inputSink_)
        return;
    const auto release = [&](auto... codes) {
        const std::array events {
            vnc_input::VncInputEvent {
                .type = vnc_input::eventKey,
                .code = codes,
                .value = 0,
            }...,
            vnc_input::VncInputEvent {
                .type = vnc_input::eventSync,
                .code = vnc_input::syncReport,
                .value = 0,
            },
        };
        if (!vnc_input::tryDeliver(inputSink_, events))
            reportInputFailure();
    };
    if (pointerMode_ == vnc_input::PointerMode::touchscreen)
        release(vnc_input::buttonTouch);
    else
        release(
            vnc_input::buttonLeft,
            vnc_input::buttonMiddle,
            vnc_input::buttonRight);
}

void VncDisplay::reportInputFailure() noexcept
{
    if (!inputFailureReported_.exchange(true, std::memory_order_relaxed)) {
        std::fputs(
            "Unable to deliver VNC input to the emulator\n", stderr);
    }
}

void VncDisplay::reportStagingFailure() noexcept
{
    if (!stagingFailureReported_.exchange(true, std::memory_order_relaxed)) {
        std::fputs(
            "VNC display: cannot scale the CPU framebuffer on the GPU\n",
            stderr);
    }
}

void VncDisplay::eventLoop()
{
    (void)::pthread_setname_np("uml-vnc");
    while (running_.load(std::memory_order_acquire) &&
           rfbIsActive(screen_)) {
        rfbProcessEvents(screen_, heldClients_.empty() ? 10'000 : 1'000);
        if (!heldClients_.empty()) {
            releaseHeldClients();
            /* LibVNCServer still polls a held client's socket without
             * reading it, so pace the loop while any client waits. */
            if (!heldClients_.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#if UML_GPU_VNC_STARTUP_TESTING
        eventLoopPasses_.fetch_add(1, std::memory_order_release);
#endif
    }
}

} // namespace uml::gpu
