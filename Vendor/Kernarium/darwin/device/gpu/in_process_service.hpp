// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "context_memory_ledger.hpp"
#include "display.hpp"
#include "shared_memory_regions.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uml::gpu {

struct InProcessGpuOptions {
    DisplayMode displayMode {DisplayMode::vnc};
    std::string vncBindAddress {"127.0.0.1"};
    std::uint16_t vncPort {5900};
    std::string vncPassword;
    std::uint32_t width {1280};
    std::uint32_t height {720};
    std::uint32_t dotsPerInch {};
    uml::input::vnc::PointerMode pointerMode {
        uml::input::vnc::PointerMode::mouse};
    void* nativeView {};
    bool autoResizeGuest {true};
    /* What the windowless display (DisplayMode::offscreen) presents into. */
    OffscreenDisplayOptions offscreen {};
    /* Shared with the other backends of the same guest. This service
     * publishes every host-visible blob range it maps there, so a device
     * whose guest names a buffer inside the region can resolve it. */
    std::shared_ptr<uml::virtio::SharedMemoryRegions> sharedMemoryRegions;
    /* The most device memory a Venus guest may allocate, counted against
     * everything the host GPU holds for this process, or zero for no bound. */
    std::uint64_t deviceMemoryLimit {};
};

/* The device memory budget and what the process holds against it. */
struct DeviceMemoryFigures {
    /* The budget, B. */
    std::uint64_t budgetBytes {};
    /* The pool's own count, which the budget refuses by
     * (accountedGpuBytes(), plus the reservations in flight). Memory the
     * host's drivers hold for reuse is in it. */
    std::uint64_t usageBytes {};
};

/* The graphics memory budget and who holds it, at one instant. */
struct GraphicsMemory {
    /* The device memory budget, B; 0 when the process has none. */
    std::uint64_t budgetBytes {};
    /* What the host GPU holds against it, cache the drivers keep for reuse
     * included. */
    std::uint64_t usageBytes {};
    /* What the guest's contexts hold of it (ContextMemoryLedger). */
    std::uint64_t contextBytes {};
    /* Whether a reclaimer in the guest acts on the host's memory reports
     * (Display::guestReclaims): the host's own then stands down. */
    bool guestReclaims {};
    /* The holders that fit the request, largest first. */
    std::vector<ContextMemoryLedger::Holder> holders;
};

/* The GPU of one process's guest, and the display it presents on.
 *
 * The display (the VNC listener and its clients, or the native window or
 * view) and the frame tap belong to the service and last until stop(). The
 * guest's connection does not: it is the Virtio-GPU device, the renderer and
 * every context and resource the guest made, and it lasts as long as that
 * guest. A guest that restarts itself is a new guest on the same machine, so
 * startNextConnection() ends the old connection and opens a fresh one while
 * the display, and what is attached to it, carries on. */
class InProcessGpuService {
public:
    explicit InProcessGpuService(InProcessGpuOptions options);
    ~InProcessGpuService();

    InProcessGpuService(const InProcessGpuService&) = delete;
    InProcessGpuService& operator=(const InProcessGpuService&) = delete;

    /* Transfer each guest endpoint of the current connection exactly once.
     * The service retains only the opposite endpoint used by its worker, so
     * guest closure and numeric descriptor reuse cannot make service teardown
     * close an unrelated file. */
    [[nodiscard]] std::string takeConnectionName();
    [[nodiscard]] std::string takeInputConnectionName();

    /* The guest the connection served has gone. Ends that connection, which
     * resets the device and destroys the renderer with everything the old
     * guest created in it, including its share of the device memory pool, and
     * opens a fresh one for the next guest; take*ConnectionName() then hand
     * out the new endpoints. The display carries on, cleared until the next
     * guest scans out, and so do the clients on it. Display input that
     * arrives while no connection exists is dropped.
     *
     * The request gate stays closed: a hold in place when the guest restarted
     * holds the new device, which opens it with releaseRequests() like the
     * old one. A hold that discarded the old guest's requests because that
     * guest was ending becomes a plain hold again.
     *
     * The old guest's endpoints are the guest's to close: the service shuts
     * its own ends down whatever the guest has done, and an endpoint left open
     * only holds a descriptor until it is closed. Throws, with the display as
     * it was, when the new connection cannot be opened; the service may then
     * have no connection, and a later call tries again. Throws
     * std::logic_error after stop(). */
    void startNextConnection();

    /* Ends the connection and the display. */
    void stop() noexcept;

    /* Holds back the guest's GPU requests so none reaches the renderer, and
     * with it the host GPU, until releaseRequests(). A request that arrives
     * meanwhile waits in its virtqueue, through any suspend of the guest that
     * disables and re-enables the ring, and runs in order after the release.
     * Returns whether no request was still running after `budget`; the hold
     * is in place either way. Work the renderer already has, fences included,
     * carries on. The application holds before it enters the background, where
     * the host GPU is revoked within seconds.
     *
     * The hold promises that requests wait, which a guest that is ending
     * cannot do: its shutdown drains the rings. When the guest reports its end
     * (device/guest_end/guest_end.hpp) behind a hold, the service completes
     * its waiting and arriving requests as success, with their fences, and
     * runs none, until startNextConnection(). */
    [[nodiscard]] bool holdRequests(
        std::chrono::milliseconds budget) noexcept;
    void releaseRequests() noexcept;
    [[nodiscard]] bool requestsHeld() const noexcept;

    /* Whether the guest has its primary scanout on: an Android guest whose
     * screen is off has none (Display::guestScanoutActive). Any thread; it
     * does not depend on the request gate, so it answers while requests are
     * held, but only a guest whose requests ran has set or cleared it. Nothing
     * before the display exists, which the first connection makes. */
    [[nodiscard]] std::optional<bool> guestScanoutActive() const noexcept;

    /* The display's frame counters (Display::statistics): the frames the guest
     * offered and the frames that reached the screen, since the display was
     * made, which a guest that restarts does not reset. The display counts
     * them as it presents, whether or not anyone reads them, so asking costs
     * the call alone. Any thread, whether or not requests are held. Nothing
     * once stop() has let the display go. */
    [[nodiscard]] std::optional<DisplayStatistics> displayStatistics()
        const noexcept;

    /* How many times the device answered the guest that the host GPU had no
     * room for an allocation, since the display was made (a guest that
     * restarts does not reset it). Only an answer the guest received counts:
     * a refusal the host reclaimed and retried past does not. Any thread,
     * whether or not requests are held. Nothing once stop() has let the
     * display go. */
    [[nodiscard]] std::optional<std::uint64_t> memoryRefusals() const noexcept;

    /* The device memory budget and what the process holds against it, while a
     * renderer with a budget runs; nothing without one. It reads the pool's
     * counters, about 0.1 ms, and waits for no request: any thread, whether or
     * not requests are held. */
    [[nodiscard]] std::optional<DeviceMemoryFigures> deviceMemory()
        const noexcept;

    /* The device memory budget, what the process holds against it and what
     * the guest's contexts hold of that, with the `maximumHolders` largest
     * holders (a guest process, or a context the host cannot tell the process
     * of) that hold at least `minimumBytes` and have been quiet for at least
     * `minimumIdle`. It reads counters and sums nothing, about 0.1 ms, and
     * waits for no request: any thread, whether or not requests are held.
     * Nothing once stop() has let the display go. The budget and the usage
     * are zero while no renderer has one. */
    [[nodiscard]] std::optional<GraphicsMemory> graphicsMemory(
        std::size_t maximumHolders,
        std::chrono::milliseconds minimumIdle,
        std::uint64_t minimumBytes) const;

    /* The port a VNC display listens on, or nothing for any other display. */
    [[nodiscard]] std::optional<std::uint16_t> vncPort() const noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::gpu
