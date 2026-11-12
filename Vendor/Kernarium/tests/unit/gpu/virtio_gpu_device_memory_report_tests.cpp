// SPDX-License-Identifier: GPL-2.0-or-later

/* The device tells a guest reclaimer how near the device memory budget is and
 * who holds it (kernel/common/gpu). It offers the extension only while the
 * renderer has a budget; a query is answered from the host's counters and cut
 * to the room the guest left, whatever it asks; the holders are the named
 * guest processes that fit the filters, largest first; a subscription raises
 * the event exactly once per change, through the same events_read and
 * events_clear handshake as the display event; and what a guest set (the
 * subscription, the reclaimer flag) ends with the connection. The test drives
 * the device with a renderer that has a budget and its own clock. */

#include "context_memory_ledger.hpp"
#include "memory_pressure.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virtio_gpu_device.hpp"
#include "virtio_gpu_device_testing.hpp"
#include "virtio_gpu_uml.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uml::gpu::testing {
namespace {

using ::uml::testing::require;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint64_t mebibyte = 1U << 20U;
constexpr std::uint64_t budget = 600 * mebibyte;

/* A renderer with nothing in it, except a budget and a pool usage to read,
 * and memory its drivers cache, which its reclaim frees out of the usage. */
class BudgetedRenderer final : public VirtioGpuDeviceDependencies {
public:
    std::optional<VirglRenderer::DeviceMemory> memory;
    std::uint64_t refusals {};
    std::atomic<int> wakes {};
    std::uint64_t cachedBytes {};
    int reclaims {};

    [[nodiscard]] std::uint64_t reclaimCachedMemory() noexcept override
    {
        ++reclaims;
        if (cachedBytes == 0 || !memory)
            return 0;
        memory->usageBytes -= std::min(memory->usageBytes, cachedBytes);
        cachedBytes = 0;
        return 1;
    }

    [[nodiscard]] std::optional<VirglRenderer::DeviceMemory> deviceMemory()
        const noexcept override
    {
        return memory;
    }
    [[nodiscard]] std::uint64_t memoryRefusals() const noexcept override
    {
        return refusals;
    }
    /* A display raises the ledger's observer for a refusal. */
    void recordMemoryRefusal() noexcept override
    {
        ++refusals;
        graphicsLedger().notify();
    }

    [[nodiscard]] std::uint32_t width() const noexcept override { return 64; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return 64; }
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t, std::uint32_t) noexcept override
    {
        return true;
    }
    void resetGuestSession() noexcept override {}
    void clear() noexcept override {}
    void update(std::span<const std::uint8_t>, std::uint32_t, std::uint32_t,
        std::size_t, protocol::GpuFormat, std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override
    {
    }
    [[nodiscard]] VirglRenderer::Capset capset(std::uint32_t) const override
    {
        return {};
    }
    [[nodiscard]] int createContext(
        std::uint32_t, std::uint32_t, std::string_view) noexcept override
    {
        return 0;
    }
    void destroyContext(std::uint32_t) noexcept override {}
    [[nodiscard]] int submit(
        std::uint32_t, std::span<const std::uint8_t>) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int createResource3d(
        const VirglRenderer::Resource3d&) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int createResourceBlob(
        const VirglRenderer::ResourceBlob&) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int exportResourceBlob(
        std::uint32_t, std::uint32_t&, int&) noexcept override
    {
        return -1;
    }
    [[nodiscard]] int resourceMapInfo(
        std::uint32_t, std::uint32_t&) noexcept override
    {
        return -1;
    }
    [[nodiscard]] int mapResource(
        std::uint32_t, void*&, std::uint64_t&) noexcept override
    {
        return -1;
    }
    void unmapResource(std::uint32_t) noexcept override {}
    [[nodiscard]] int createBlobScanout(std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
        void*&) noexcept override
    {
        return -1;
    }
    void releaseBlobScanout(void*) noexcept override {}
    [[nodiscard]] int transferResource(bool, std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t,
        const VirglRenderer::Box&, std::uint64_t) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int readResourceRectangle(std::uint32_t,
        std::span<std::uint8_t>, std::uint32_t, std::uint32_t, std::uint32_t,
        std::uint32_t) override
    {
        return -1;
    }
    [[nodiscard]] int resourceMetalTexture(
        std::uint32_t, VirglRenderer::NativeTexture&) noexcept override
    {
        return -1;
    }
    [[nodiscard]] PresentOutcome presentMetal(
        const MetalScanout&) noexcept override
    {
        return PresentOutcome::unsupported;
    }
    [[nodiscard]] bool needsFullCpuFallback() const noexcept override
    {
        return false;
    }
    void recordCpuFallbackFrame() noexcept override {}
    [[nodiscard]] int createFenceAndWait(std::uint32_t, std::uint32_t,
        std::uint64_t, const std::atomic_bool*) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int createContextFence(
        std::uint32_t, std::uint32_t, std::uint64_t) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int pollContextFences(std::uint32_t) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int pollRenderer() noexcept override { return 0; }
    [[nodiscard]] bool hasPendingWork() noexcept override { return false; }
    [[nodiscard]] std::optional<std::chrono::steady_clock::duration>
        nextIdleTrimDelay() const noexcept override
    {
        return std::nullopt;
    }
    void trimIdleContexts() noexcept override {}
    [[nodiscard]] int contextFenceStatus(
        std::uint32_t, std::uint32_t, std::uint64_t) noexcept override
    {
        return 0;
    }
    void unrefResource(std::uint32_t) noexcept override {}
    [[nodiscard]] int attachBacking(
        std::uint32_t, std::span<iovec>) noexcept override
    {
        return 0;
    }
    void detachBacking(std::uint32_t) noexcept override {}
    [[nodiscard]] int attachResource(
        std::uint32_t, std::uint32_t) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int detachResource(
        std::uint32_t, std::uint32_t) noexcept override
    {
        return 0;
    }
    void resetFenceState() noexcept override {}
};

bool noGuestReads(std::uint64_t, std::span<std::uint8_t>)
{
    return false;
}

std::span<std::uint8_t> noGuestMappings(std::uint64_t, std::size_t)
{
    return {};
}

struct Reply {
    protocol::GpuControlHeader header {};
    std::vector<virtio_gpu_uml_memory_holder> holders;
    /* Last: its flexible array member is not stored. */
    virtio_gpu_uml_memory fixed {};
};

[[nodiscard]] std::vector<std::uint8_t> query(
    std::uint32_t flags,
    std::uint32_t maxHolders = VIRTIO_GPU_UML_MAX_HOLDERS,
    std::uint32_t minIdleMs = 0,
    std::uint64_t minBytes = 0)
{
    virtio_gpu_uml_get_memory wire {};
    wire.hdr.type = VIRTIO_GPU_CMD_UML_GET_MEMORY;
    wire.flags = flags;
    wire.max_holders = maxHolders;
    wire.min_idle_ms = minIdleMs;
    wire.min_bytes = minBytes;
    std::vector<std::uint8_t> bytes(sizeof(wire));
    std::memcpy(bytes.data(), &wire, sizeof(wire));
    return bytes;
}

[[nodiscard]] protocol::GpuResponse responseType(
    const std::vector<std::uint8_t>& reply)
{
    require(reply.size() >= sizeof(protocol::GpuControlHeader),
        "a reply was shorter than a header");
    protocol::GpuControlHeader header {};
    std::memcpy(&header, reply.data(), sizeof(header));
    return static_cast<protocol::GpuResponse>(header.type);
}

[[nodiscard]] Reply parse(const std::vector<std::uint8_t>& bytes)
{
    require(responseType(bytes) == protocol::GpuResponse::okUmlMemory,
        "the reply was not a memory reply");
    Reply reply;
    require(bytes.size() >= sizeof(reply.fixed), "a memory reply was short");
    std::memcpy(&reply.fixed, bytes.data(), sizeof(reply.fixed));
    std::memcpy(&reply.header, bytes.data(), sizeof(reply.header));
    require(bytes.size() == sizeof(reply.fixed) +
            reply.fixed.holder_count * sizeof(virtio_gpu_uml_memory_holder),
        "a reply's size did not match its holder count");
    for (std::uint32_t index = 0; index < reply.fixed.holder_count; ++index) {
        virtio_gpu_uml_memory_holder holder {};
        std::memcpy(&holder,
            bytes.data() + sizeof(reply.fixed) + index * sizeof(holder),
            sizeof(holder));
        reply.holders.push_back(holder);
    }
    return reply;
}

/* A query with all the room the guest could give. */
[[nodiscard]] Reply ask(VirtioGpuDevice& device, std::vector<std::uint8_t> request)
{
    return parse(device.process(request, false));
}

[[nodiscard]] std::string name(const virtio_gpu_uml_memory_holder& holder)
{
    return std::string {holder.comm};
}

/* Contexts of three guest processes, an unnamed one and a Venus one, with
 * known bytes, and the clock that makes them idle. */
struct Guest {
    BudgetedRenderer renderer;
    VirtioGpuDevice device {renderer, noGuestReads, noGuestMappings};
    ContextMemoryLedger& ledger {renderer.graphicsLedger()};
    Clock::time_point now {Clock::now()};

    Guest()
    {
        renderer.memory = VirglRenderer::DeviceMemory {
            .budgetBytes = budget, .usageBytes = 150 * mebibyte};
    }

    void context(std::uint32_t id, std::string_view debugName, std::uint64_t bytes,
        Clock::duration idle)
    {
        ledger.contextCreated(id, debugName, true, now - idle);
        ledger.venusMemory(id, static_cast<std::int64_t>(bytes));
    }
};

void theExtensionIsOfferedWithABudget()
{
    BudgetedRenderer renderer;
    VirtioGpuDevice device {renderer, noGuestReads, noGuestMappings};
    require(!device.offersMemoryReport(), "a renderer without a budget offered the report");
    require(responseType(device.process(query(0), false)) ==
        protocol::GpuResponse::errorUnspecified,
        "a query without the extension was answered");
    require(!renderer.guestReclaims(), "a refused query set the reclaimer flag");
    require(!device.memoryDeadline().has_value(), "an unoffered report named a deadline");

    renderer.memory = VirglRenderer::DeviceMemory {
        .budgetBytes = budget, .usageBytes = 10 * mebibyte};
    require(device.offersMemoryReport(), "a renderer with a budget did not offer the report");
    const auto reply = ask(device, query(0));
    require(reply.fixed.budget_bytes == budget && reply.fixed.used_bytes == 10 * mebibyte,
        "the reply did not carry the budget and the pool's usage");
    require(reply.fixed.holder_count == 0 && reply.fixed.state == 0 &&
            reply.fixed.context_bytes == 0 && reply.fixed.refusals == 0,
        "an empty guest's reply was not empty");
}

void aReplyNamesTheHoldersLargestFirst()
{
    Guest guest;
    guest.context(1, "t|pid=100|comm=small", 40 * mebibyte, 20s);
    guest.context(2, "t|pid=200|comm=big", 90 * mebibyte, 30s);
    guest.context(3, "t|pid=300|comm=mid", 60 * mebibyte, 15s);
    guest.context(4, "t|pid=300|comm=mid", 5 * mebibyte, 12s);
    guest.context(5, "an unnamed context", 500 * mebibyte, 60s);
    guest.context(6, "t|pid=400|comm=busy", 80 * mebibyte, 1s);
    guest.renderer.refusals = 3;

    const auto reply = ask(guest.device, query(0));
    require(reply.header.flags == 0 && reply.header.contextId == 0,
        "the reply did not echo the request's header");
    require(reply.fixed.context_bytes == 775 * mebibyte,
        "the context bytes were not the ledger's total");
    require(reply.fixed.refusals == 3, "the reply did not carry the refusals");
    require(reply.fixed.holder_count == 4, "an unnamed holder was listed");
    require(name(reply.holders[0]) == "big" && reply.holders[0].pid == 200 &&
            reply.holders[0].bytes == 90 * mebibyte && reply.holders[0].contexts == 1,
        "the largest holder was not first");
    require(name(reply.holders[1]) == "busy" && name(reply.holders[2]) == "mid" &&
            name(reply.holders[3]) == "small",
        "the holders were not largest first");
    require(reply.holders[2].pid == 300 && reply.holders[2].contexts == 2 &&
            reply.holders[2].bytes == 65 * mebibyte,
        "a process's contexts were not added up");
    require(reply.holders[2].idle_ms >= 12000 && reply.holders[2].idle_ms < 13000,
        "a process was not as idle as its most recently active context");
    require(reply.holders[1].idle_ms >= 1000 && reply.holders[1].idle_ms < 2000,
        "a busy process was not as idle as its context");

    /* The filters keep the small and the busy out. */
    const auto filtered = ask(guest.device, query(0, 16, 10'000, 50 * mebibyte));
    require(filtered.fixed.holder_count == 2 && name(filtered.holders[0]) == "big" &&
            name(filtered.holders[1]) == "mid",
        "the size and idle filters did not apply");
    const auto limited = ask(guest.device, query(0, 1));
    require(limited.fixed.holder_count == 1 && name(limited.holders[0]) == "big",
        "max_holders did not apply");
    require(ask(guest.device, query(0, 0)).fixed.holder_count == 0,
        "max_holders 0 listed holders");
    require(ask(guest.device, query(0, 1000)).fixed.holder_count == 4,
        "a large max_holders was not capped to what exists");
}

void aCommandNameIsOneSafeLine()
{
    Guest guest;
    guest.context(1, "t|pid=100|comm=x\ny z\x7f", 40 * mebibyte, 20s);
    guest.context(2, "t|pid=200|comm=0123456789abcdefXYZ", 30 * mebibyte, 20s);
    const auto reply = ask(guest.device, query(0));
    require(reply.fixed.holder_count == 2, "a holder was lost");
    require(name(reply.holders[0]) == "x?y z?",
        "a control character stayed in a command name");
    require(name(reply.holders[1]) == "0123456789abcde" &&
            reply.holders[1].comm[VIRTIO_GPU_UML_COMM_LEN - 1] == '\0',
        "a long command name was not cut and terminated");
}

void aReplyIsCutToTheRoomTheGuestGave()
{
    Guest guest;
    for (std::uint32_t pid = 1; pid <= 6; ++pid)
        guest.context(pid, "t|pid=" + std::to_string(pid) + "|comm=p", pid * 10 * mebibyte, 20s);
    const auto fixed = sizeof(virtio_gpu_uml_memory);
    const auto holder = sizeof(virtio_gpu_uml_memory_holder);
    const auto request = query(VIRTIO_GPU_UML_MEMORY_GUEST_RECLAIMS);
    const auto run = [&](std::size_t capacity) {
        return guest.device.processWithDeferredFence(request, false, 0, capacity).response;
    };

    /* No room for the reply header: refused before it runs. */
    require(responseType(run(fixed - 1)) == protocol::GpuResponse::errorInvalidParameter,
        "a reply that did not fit was not refused");
    require(!guest.renderer.guestReclaims(), "a refused query changed the reclaimer flag");
    /* Room for the header and a part of a holder: no holder. */
    require(parse(run(fixed)).fixed.holder_count == 0, "a holder was sent without room");
    require(parse(run(fixed + holder - 1)).fixed.holder_count == 0,
        "a partial holder was sent");
    const auto two = parse(run(fixed + 2 * holder));
    require(two.fixed.holder_count == 2 && two.holders[0].pid == 6 && two.holders[1].pid == 5,
        "the reply was not cut to the room, largest first");
    require(run(fixed + 2 * holder).size() == fixed + 2 * holder,
        "the reply was larger than the room");
    require(parse(run(100000)).fixed.holder_count == 6, "a roomy reply lost a holder");
    require(parse(run(100000)).holders.size() == 6, "a roomy reply lost a holder");
    require(guest.renderer.guestReclaims(), "a query that ran did not set the reclaimer flag");

    /* A reply never lists more than the wire allows. */
    for (std::uint32_t pid = 7; pid <= 30; ++pid)
        guest.context(pid, "t|pid=" + std::to_string(pid) + "|comm=p", pid * mebibyte, 20s);
    require(parse(run(100000)).fixed.holder_count == VIRTIO_GPU_UML_MAX_HOLDERS,
        "a reply listed more holders than the wire allows");
}

void aBadQueryIsRefusedAndChangesNothing()
{
    Guest guest;
    guest.context(1, "t|pid=100|comm=p", 40 * mebibyte, 20s);
    auto request = query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE | VIRTIO_GPU_UML_MEMORY_GUEST_RECLAIMS);
    request.resize(request.size() - 1);
    require(responseType(guest.device.process(request, false)) ==
        protocol::GpuResponse::errorInvalidParameter, "a short query was answered");
    require(responseType(guest.device.process(query(1U << 2U), false)) ==
        protocol::GpuResponse::errorInvalidParameter, "a query with an unknown flag was answered");
    auto padded = query(0);
    padded[offsetof(virtio_gpu_uml_get_memory, padding)] = 1;
    require(responseType(guest.device.process(padded, false)) ==
        protocol::GpuResponse::errorInvalidParameter, "a query with padding was answered");
    require(!guest.renderer.guestReclaims() && !guest.device.memoryDeadline().has_value() &&
            !guest.device.evaluateMemory(guest.now + 1h),
        "a refused query subscribed the guest");
}

/* One change of the pool's pressure, the way the transport drives it. */
void setHeld(Guest& guest, std::uint64_t bytes)
{
    /* A context's memory goes to `bytes` in total. */
    const auto total = guest.ledger.totalBytes();
    guest.ledger.venusMemory(
        1, static_cast<std::int64_t>(bytes) - static_cast<std::int64_t>(total));
    guest.renderer.memory->usageBytes = bytes + 20 * mebibyte;
}

void aSubscriptionRaisesTheEventOncePerChange()
{
    Guest guest;
    guest.context(1, "t|pid=100|comm=p", 100 * mebibyte, 20s);
    guest.renderer.memory->usageBytes = 120 * mebibyte;
    const auto subscribe = query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE);
    require(ask(guest.device, subscribe).fixed.state == 0, "a calm guest was told of pressure");
    require(guest.device.config().eventsRead == 0, "the event was set without a change");
    require(!guest.device.evaluateMemory(guest.now), "an evaluation without a change raised the event");

    setHeld(guest, 500 * mebibyte);
    require(!guest.device.evaluateMemory(guest.now), "the event came before the sustain time");
    require(guest.device.memoryDeadline() == guest.now + 1s,
        "the deadline was not the end of the sustain time");
    require(guest.device.evaluateMemory(guest.now + 1s), "no event after the sustain time");
    require((guest.device.config().eventsRead & protocol::gpuEventMemory) != 0 &&
            guest.device.memoryEventPending(),
        "the event bit was not set");
    require(!guest.device.evaluateMemory(guest.now + 2s), "a second event came for one pressure");

    /* The guest reads events_read and clears the bit it read. */
    guest.device.recordConfigRead(0, 4);
    guest.device.clearEvents(protocol::gpuEventMemory);
    require(!guest.device.memoryEventPending() && guest.device.config().eventsRead == 0,
        "events_clear did not clear the bit");
    const auto told = ask(guest.device, subscribe);
    require((told.fixed.state & VIRTIO_GPU_UML_STATE_PRESSURE) != 0 &&
            told.fixed.generation == 1,
        "the reply did not carry the pressure and the event count");

    /* Calm ends the pressure after five calm seconds, with an event. */
    setHeld(guest, 100 * mebibyte);
    require(!guest.device.evaluateMemory(guest.now + 10s), "calm raised the event at once");
    require(guest.device.memoryDeadline() == guest.now + 15s, "the deadline was not the end of the calm");
    require(guest.device.evaluateMemory(guest.now + 15s), "no event after the calm time");
    require(guest.device.memoryEventPending(), "the calm event bit was not set");

    /* An event raised between the guest's read and its clear survives it. */
    guest.device.recordConfigRead(0, 4);
    setHeld(guest, 500 * mebibyte);
    require(!guest.device.evaluateMemory(guest.now + 20s), "early event");
    require(guest.device.evaluateMemory(guest.now + 21s), "no event for the second pressure");
    guest.device.clearEvents(protocol::gpuEventMemory);
    require(guest.device.memoryEventPending(),
        "a clear acknowledged an event the guest had not read");
    guest.device.recordConfigRead(0, 4);
    guest.device.clearEvents(protocol::gpuEventMemory);
    require(!guest.device.memoryEventPending(), "the second clear did not clear the bit");

    /* The display event is a bit of its own. */
    guest.device.notifyDisplayChanged();
    require(guest.device.config().eventsRead == protocol::gpuEventDisplay,
        "a display event set the memory bit");
    guest.device.recordConfigRead(0, 4);
    guest.device.clearEvents(protocol::gpuEventMemory);
    require(guest.device.displayEventPending(), "clearing the memory event cleared the display event");
}

/* The pool's usage is within the margin of the budget, but the host's drivers
 * cache enough of it that the reclaim brings it back under: no pressure. */
void memoryTheHostFreesItselfIsNoPressure()
{
    Guest guest;
    guest.context(1, "t|pid=100|comm=p", 100 * mebibyte, 20s);
    (void)ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    guest.renderer.memory->usageBytes = budget - 20 * mebibyte;
    guest.renderer.cachedBytes = 150 * mebibyte;

    require(!guest.device.evaluateMemory(guest.now), "reaching the margin raised the event");
    require(guest.renderer.reclaims == 1, "the host's reclaim was not run");
    require(guest.renderer.memory->usageBytes == budget - 170 * mebibyte,
        "the reclaim did not free the cached memory");
    require(!guest.device.evaluateMemory(guest.now + 300ms), "freed memory raised the event");
    require(!guest.device.memoryDeadline().has_value(), "freed memory kept a deadline");
    require(!guest.device.evaluateMemory(guest.now + 10s), "freed memory raised the event later");
    const auto reply = ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    require(reply.fixed.state == 0 && reply.fixed.refusals == 0 &&
            !guest.device.memoryEventPending(),
        "memory the host freed itself was reported as pressure");
    require(guest.renderer.reclaims == 1, "the host reclaimed again with nothing cached");
}

/* The same usage with too little cached stays within the margin once the
 * reclaim has run: the pressure is declared with no allocation refused. */
void memoryTheHostCannotFreeIsPressureBeforeAnyRefusal()
{
    Guest guest;
    guest.context(1, "t|pid=100|comm=p", 100 * mebibyte, 20s);
    (void)ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    guest.renderer.memory->usageBytes = budget - 20 * mebibyte;
    guest.renderer.cachedBytes = 30 * mebibyte;

    require(!guest.device.evaluateMemory(guest.now), "reaching the margin raised the event");
    require(guest.renderer.reclaims == 1, "the host's reclaim was not run");
    require(guest.device.memoryDeadline() == guest.now + 1s,
        "the deadline was not the end of the sustain time");
    /* A reply before the sustain time reports nothing yet. */
    require(ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE)).fixed.state == 0,
        "a reply declared pressure before the sustain time");
    require(guest.device.evaluateMemory(guest.now + 1s), "no event for memory the host could not free");
    require(guest.device.memoryEventPending(), "the event bit was not set");
    const auto reply = ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    require((reply.fixed.state & VIRTIO_GPU_UML_STATE_PRESSURE) != 0 &&
            (reply.fixed.state & VIRTIO_GPU_UML_STATE_REFUSED) == 0 && reply.fixed.refusals == 0,
        "the reply did not carry the pressure with no refusal");
    require(guest.renderer.reclaims == 1, "the host reclaimed more than once for one stretch");
}

/* A pool that keeps a reserve refuses at its ceiling, below the budget: the
 * pressure comes the margin ahead of the ceiling, and the reply still carries
 * the budget. */
void thePressureIsTheMarginAheadOfTheCeiling()
{
    constexpr std::uint64_t ceiling = budget - 16 * mebibyte;
    constexpr std::uint64_t usage = ceiling - uml::gpu::MemoryPressure::margin;
    Guest guest;
    guest.context(1, "t|pid=100|comm=p", 100 * mebibyte, 20s);
    (void)ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    guest.renderer.memory = VirglRenderer::DeviceMemory {
        .budgetBytes = budget, .usageBytes = usage, .ceilingBytes = budget};
    require(!guest.device.evaluateMemory(guest.now) &&
            !guest.device.memoryDeadline().has_value(),
        "a usage short of the margin of the budget was not calm");

    guest.renderer.memory->ceilingBytes = ceiling;
    require(!guest.device.evaluateMemory(guest.now), "reaching the margin raised the event");
    require(guest.renderer.reclaims == 1, "the host's reclaim was not run");
    require(guest.device.evaluateMemory(guest.now + 1s),
        "the margin ahead of the pool's ceiling was no pressure");
    const auto reply = ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    require((reply.fixed.state & VIRTIO_GPU_UML_STATE_PRESSURE) != 0 &&
            reply.fixed.budget_bytes == budget && reply.fixed.used_bytes == usage,
        "the reply did not carry the pressure, the budget and the usage");
}

void aRefusalRaisesTheEventAndTheNextReplyReportsIt()
{
    Guest guest;
    guest.context(1, "t|pid=100|comm=p", 100 * mebibyte, 20s);
    (void)ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    require(!guest.device.evaluateMemory(guest.now), "an evaluation without a refusal raised the event");
    guest.renderer.recordMemoryRefusal();
    require(guest.device.evaluateMemory(guest.now), "a refusal raised no event");
    guest.renderer.recordMemoryRefusal();
    require(!guest.device.evaluateMemory(guest.now + 100ms), "a refusal within a second raised an event");
    require(guest.device.memoryDeadline() == guest.now + 1s, "the held-back refusal named no deadline");
    const auto reply = ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    require((reply.fixed.state & VIRTIO_GPU_UML_STATE_REFUSED) != 0 && reply.fixed.refusals == 2,
        "the reply did not report the refusals");
    require(!guest.device.evaluateMemory(guest.now + 1s), "a refusal a reply reported raised an event");
    require((ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE)).fixed.state &
            VIRTIO_GPU_UML_STATE_REFUSED) == 0,
        "a refusal was reported twice");
}

void aRefusalWakesTheTransportAtOnce()
{
    Guest guest;
    guest.context(1, "t|pid=100|comm=p", 100 * mebibyte, 20s);
    std::vector<VirtioGpuDevice::MemoryWake> wakes;
    guest.device.setMemoryWake(
        [&](VirtioGpuDevice::MemoryWake wake) { wakes.push_back(wake); });

    /* A refusal made before the subscription is not news to it. */
    guest.renderer.recordMemoryRefusal();
    (void)ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    guest.ledger.venusMemory(1, mebibyte);
    guest.renderer.recordMemoryRefusal();
    guest.ledger.venusMemory(1, mebibyte);
    guest.renderer.recordMemoryRefusal();
    using enum VirtioGpuDevice::MemoryWake;
    require(wakes == std::vector {coalesced, immediate, coalesced, immediate},
        "a change was not coalesced or a refusal not woken at once");

    /* A wake set again while subscribed knows the refusals so far. */
    guest.device.setMemoryWake(
        [&](VirtioGpuDevice::MemoryWake wake) { wakes.push_back(wake); });
    wakes.clear();
    guest.ledger.venusMemory(1, mebibyte);
    require(wakes == std::vector {coalesced},
        "a new wake took an old refusal for a new one");
}

void theGuestsSettingsEndWithTheConnection()
{
    Guest guest;
    guest.context(1, "t|pid=100|comm=p", 100 * mebibyte, 20s);
    int wakes = 0;
    guest.device.setMemoryWake([&](VirtioGpuDevice::MemoryWake) { ++wakes; });

    /* Nothing is woken until a guest subscribes. */
    guest.ledger.venusMemory(1, mebibyte);
    guest.renderer.recordMemoryRefusal();
    require(wakes == 0, "the ledger woke the transport for an unsubscribed guest");

    (void)ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE | VIRTIO_GPU_UML_MEMORY_GUEST_RECLAIMS));
    require(guest.renderer.guestReclaims(), "the reclaimer flag was not set");
    guest.ledger.venusMemory(1, mebibyte);
    require(wakes == 1, "a change of the ledger did not wake the transport");
    guest.ledger.venusMemory(1, 0);
    require(wakes == 1, "a change of nothing woke the transport");
    guest.renderer.recordMemoryRefusal();
    require(wakes == 2, "a refusal did not wake the transport");
    (void)ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE | VIRTIO_GPU_UML_MEMORY_GUEST_RECLAIMS));
    guest.ledger.venusMemory(1, -static_cast<std::int64_t>(mebibyte));
    require(wakes == 3, "asking again lost the subscription");

    /* A query without the flag ends the reclaimer's claim and not the
     * subscription; one without the subscription ends that. */
    (void)ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE));
    require(!guest.renderer.guestReclaims(), "the reclaimer flag outlived a query without it");
    guest.ledger.venusMemory(1, mebibyte);
    require(wakes == 4, "a query without the flag ended the subscription");
    (void)ask(guest.device, query(0));
    guest.ledger.venusMemory(1, mebibyte);
    require(wakes == 4, "the ledger woke the transport after the subscription ended");

    /* A reset guest has neither. */
    (void)ask(guest.device, query(VIRTIO_GPU_UML_MEMORY_SUBSCRIBE | VIRTIO_GPU_UML_MEMORY_GUEST_RECLAIMS));
    guest.renderer.recordMemoryRefusal();
    require(wakes == 5, "the resubscription did not wake the transport");
    guest.device.reset();
    require(!guest.renderer.guestReclaims(), "the reclaimer flag outlived the reset");
    require(!guest.device.memoryDeadline().has_value() && !guest.device.memoryEventPending(),
        "the reset left a subscription's state");
    guest.renderer.recordMemoryRefusal();
    require(wakes == 5, "the ledger woke the transport after the reset");
    require(guest.device.config().eventsRead == 0, "the reset left the event bit set");
}

} // namespace
} // namespace uml::gpu::testing

int main()
{
    using namespace uml::gpu::testing;

    theExtensionIsOfferedWithABudget();
    aReplyNamesTheHoldersLargestFirst();
    aCommandNameIsOneSafeLine();
    aReplyIsCutToTheRoomTheGuestGave();
    aBadQueryIsRefusedAndChangesNothing();
    aSubscriptionRaisesTheEventOncePerChange();
    memoryTheHostFreesItselfIsNoPressure();
    memoryTheHostCannotFreeIsPressureBeforeAnyRefusal();
    thePressureIsTheMarginAheadOfTheCeiling();
    aRefusalRaisesTheEventAndTheNextReplyReportsIt();
    aRefusalWakesTheTransportAtOnce();
    theGuestsSettingsEndWithTheConnection();
    std::puts("Virtio-GPU memory report tests passed");
    return 0;
}
