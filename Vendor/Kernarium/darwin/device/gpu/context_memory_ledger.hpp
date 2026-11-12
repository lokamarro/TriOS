// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uml::gpu {

/* The guest process that created a context, as its guest kernel names it in
 * the context's debug name. */
struct ContextOwner {
    std::uint32_t pid {};
    std::string comm;
};

/* The debug name a guest kernel that names the creating process gives its
 * contexts ends in "|pid=<tgid>|comm=<comm>": the process's thread group ID in
 * decimal and its command name, which holds no '|'. Anything before the suffix
 * is the name the context had before, cut to fit. Any other name, which is
 * every name a kernel without that topic gives (the creating thread's name),
 * belongs to no process the host can tell. */
[[nodiscard]] std::optional<ContextOwner> parseContextOwner(
    std::string_view debugName);

/* What the guest's contexts hold of the host GPU's memory, by context and by
 * process.
 *
 * The host GPU gives the guest a fixed budget, and the pool that enforces it
 * counts bytes, not owners. The ledger keeps the owners: the storage of each
 * Virgl 3D resource and blob, credited to every context the resource is
 * attached to (the guest kernel attaches a resource to the context of each
 * process that holds a handle to it, so a gralloc buffer is credited to the
 * allocator that made it, to the compositor that shows it and to the
 * application that draws into it), and the device memory of a Venus context,
 * which the renderer reports by context. A resource attached to two contexts
 * is credited to both, and counted once in the total.
 *
 * The ledger is told by the device on the GPU transport thread and by the
 * renderer's threads, and read from any thread. Every call is O(1) in the
 * resource count except a context's destruction, which scans the resources
 * once. Memory is about 100 bytes a resource.
 *
 * The device answers a command that runs out of memory without having
 * changed anything, so the calls that allocate (a context, a reserved
 * resource, a second holder) throw std::bad_alloc and change nothing when
 * they do, and the device makes them before it commits; every other call
 * allocates nothing and cannot fail. */
class ContextMemoryLedger {
public:
    using Clock = std::chrono::steady_clock;

    /* One process's contexts, or one context the host cannot name. */
    struct Holder {
        /* The guest process; 0 for a context whose name carries none. */
        std::uint32_t pid {};
        /* For an unnamed context, its ID; for a process, its largest context. */
        std::uint32_t contextId {};
        std::uint32_t contexts {};
        std::uint64_t bytes {};
        /* How long the holder's most recently active context has been quiet. */
        Clock::duration idle {};
        /* The process's command name, or an unnamed context's debug name. */
        std::string name;
    };

    struct Snapshot {
        /* Every resource some context holds, once, and the Venus device
         * memory of every context. */
        std::uint64_t totalBytes {};
        /* Largest first. */
        std::vector<Holder> holders;
    };

    ContextMemoryLedger() = default;
    ContextMemoryLedger(const ContextMemoryLedger&) = delete;
    ContextMemoryLedger& operator=(const ContextMemoryLedger&) = delete;

    /* A guest context exists. `venus` contexts hold their device memory
     * through venusMemory(), so a blob they create is not counted again. */
    void contextCreated(
        std::uint32_t contextId,
        std::string_view debugName,
        bool venus,
        Clock::time_point now);
    /* The context and everything it held are gone. */
    void contextDestroyed(std::uint32_t contextId) noexcept;
    /* The context ran a command or a fence retired for it. */
    void contextActive(std::uint32_t contextId, Clock::time_point now) noexcept;

    /* A resource will exist: makes the room it takes, with no bytes yet and
     * no holder. */
    void reserveResource(std::uint32_t resourceId);
    /* The host storage of a reserved resource, once the renderer has made
     * it. A resource no context holds yet is not in the total. */
    void setResourceBytes(
        std::uint32_t resourceId, std::uint64_t bytes) noexcept;
    /* A reserved resource is a blob its creating context holds, with the
     * `bytes` the guest asked for: none for a Venus context's, which the
     * renderer counts as device memory. */
    void blobMade(
        std::uint32_t resourceId,
        std::uint32_t contextId,
        std::uint64_t bytes) noexcept;
    /* The context holds the resource: the guest kernel attaches it when the
     * process of the context gets a handle. Returns whether the call made
     * the context a holder, which a caller that fails afterwards takes back
     * with resourceDetached(). */
    [[nodiscard]] bool resourceAttached(
        std::uint32_t contextId, std::uint32_t resourceId);
    void resourceDetached(
        std::uint32_t contextId, std::uint32_t resourceId) noexcept;
    /* The resource is gone from every context. */
    void resourceReleased(std::uint32_t resourceId) noexcept;

    /* The device memory a Venus context's guest holds changed by `delta`
     * bytes. A context the ledger does not know, such as one being
     * destroyed, is ignored. Any thread. */
    void venusMemory(std::uint32_t contextId, std::int64_t delta) noexcept;

    /* Forgets everything: the guest is gone. */
    void clear() noexcept;

    /* While set, `observer` is called after every change of the total bytes,
     * on the thread that made it with no lock of the ledger held, and by
     * notify(). It is told nothing and reads what it wants: it must be quick,
     * must not throw, and must not call setObserver. An empty function clears
     * it, and the call returns once no call of the previous one is running.
     * Without an observer a change costs one relaxed load. */
    void setObserver(std::function<void()> observer);
    /* Calls the observer for a change of the memory the ledger does not keep,
     * such as a refused allocation. */
    void notify() noexcept;

    /* The holders that have been quiet for at least `minimumIdle` and hold at
     * least `minimumBytes`, `maximumHolders` of them at most, largest first,
     * and the total. */
    [[nodiscard]] Snapshot snapshot(
        Clock::time_point now,
        std::size_t maximumHolders,
        Clock::duration minimumIdle,
        std::uint64_t minimumBytes) const;

    [[nodiscard]] std::uint64_t totalBytes() const noexcept;

    /* How many contexts and resources the ledger keeps. */
    struct Counts {
        std::size_t contexts {};
        std::size_t resources {};
        [[nodiscard]] bool operator==(const Counts&) const noexcept = default;
    };
    [[nodiscard]] Counts counts() const noexcept;

private:
    struct Context {
        std::string name;
        std::optional<ContextOwner> owner;
        bool venus {};
        std::uint64_t resourceBytes {};
        std::uint64_t venusBytes {};
        Clock::time_point lastActive {};
    };
    struct Resource {
        std::uint64_t bytes {};
        /* Almost every resource has one or two holders: the first lives here. */
        std::uint32_t firstHolder {};
        std::vector<std::uint32_t> otherHolders;

        [[nodiscard]] bool held() const noexcept { return firstHolder != 0; }
        [[nodiscard]] bool holdsBy(std::uint32_t contextId) const noexcept;
        /* Throws std::bad_alloc for a second holder, and changes nothing. */
        void add(std::uint32_t contextId);
        /* Returns whether the context held it. */
        bool remove(std::uint32_t contextId) noexcept;
    };

    /* Takes the mutex for a change that may alter the total bytes, and calls
     * the observer once it has let go if it did. */
    class Change;

    /* The caller holds the mutex. */
    void destroyLocked(std::uint32_t contextId) noexcept;
    void rebaseLocked(
        Resource& resource,
        std::uint64_t previousBytes,
        std::uint64_t bytes) noexcept;

    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Context> contexts_;
    std::unordered_map<std::uint32_t, Resource> resources_;
    std::uint64_t heldResourceBytes_ {};
    std::uint64_t venusTotal_ {};
    std::atomic_bool observed_ {};
    std::mutex observerMutex_;
    std::function<void()> observer_;
};

} // namespace uml::gpu
