// SPDX-License-Identifier: GPL-2.0-or-later

#include "context_memory_ledger.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace uml::gpu {
namespace {

constexpr std::size_t maximumPidDigits = 10;

/* Unnamed contexts share the key space of processes: a process key is its
 * pid, and an unnamed context's key is above every pid. */
constexpr std::uint64_t unnamedKey(std::uint32_t contextId) noexcept
{
    return (std::uint64_t {1} << 32) | contextId;
}

} // namespace

std::optional<ContextOwner> parseContextOwner(std::string_view debugName)
{
    constexpr std::string_view commKey = "|comm=";
    constexpr std::string_view pidKey = "|pid=";
    /* The kernel's suffix is the last thing in the name, and the command
     * name in it holds no '|', so the last "|comm=" and the last "|pid="
     * before it are the kernel's, whatever the name before them says. */
    const auto commAt = debugName.rfind(commKey);
    if (commAt == std::string_view::npos)
        return std::nullopt;
    const auto pidAt = debugName.substr(0, commAt).rfind(pidKey);
    if (pidAt == std::string_view::npos)
        return std::nullopt;
    const auto digits = debugName.substr(
        pidAt + pidKey.size(), commAt - pidAt - pidKey.size());
    const auto comm = debugName.substr(commAt + commKey.size());
    if (digits.empty() || digits.size() > maximumPidDigits || comm.empty())
        return std::nullopt;
    std::uint64_t pid = 0;
    for (const char digit : digits) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        pid = pid * 10 + static_cast<std::uint64_t>(digit - '0');
    }
    if (pid == 0 || pid > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return ContextOwner {
        .pid = static_cast<std::uint32_t>(pid),
        .comm = std::string(comm),
    };
}

class ContextMemoryLedger::Change {
public:
    explicit Change(ContextMemoryLedger& ledger)
        : ledger_(ledger)
        , lock_(ledger.mutex_)
        , before_(ledger.heldResourceBytes_ + ledger.venusTotal_)
    {
    }
    Change(const Change&) = delete;
    Change& operator=(const Change&) = delete;
    ~Change()
    {
        const bool changed =
            ledger_.heldResourceBytes_ + ledger_.venusTotal_ != before_;
        lock_.unlock();
        if (changed)
            ledger_.notify();
    }

private:
    ContextMemoryLedger& ledger_;
    std::unique_lock<std::mutex> lock_;
    std::uint64_t before_;
};

void ContextMemoryLedger::setObserver(std::function<void()> observer)
{
    std::lock_guard lock(observerMutex_);
    observed_.store(static_cast<bool>(observer), std::memory_order_relaxed);
    observer_ = std::move(observer);
}

void ContextMemoryLedger::notify() noexcept
{
    if (!observed_.load(std::memory_order_relaxed))
        return;
    std::lock_guard lock(observerMutex_);
    if (observer_)
        observer_();
}

bool ContextMemoryLedger::Resource::holdsBy(
    std::uint32_t contextId) const noexcept
{
    return firstHolder == contextId ||
        std::find(otherHolders.begin(), otherHolders.end(), contextId) !=
            otherHolders.end();
}

void ContextMemoryLedger::Resource::add(std::uint32_t contextId)
{
    if (firstHolder == 0)
        firstHolder = contextId;
    else
        otherHolders.push_back(contextId);
}

bool ContextMemoryLedger::Resource::remove(std::uint32_t contextId) noexcept
{
    if (firstHolder == contextId) {
        if (otherHolders.empty()) {
            firstHolder = 0;
        } else {
            firstHolder = otherHolders.back();
            otherHolders.pop_back();
        }
        return true;
    }
    const auto found =
        std::find(otherHolders.begin(), otherHolders.end(), contextId);
    if (found == otherHolders.end())
        return false;
    *found = otherHolders.back();
    otherHolders.pop_back();
    return true;
}

void ContextMemoryLedger::contextCreated(
    std::uint32_t contextId,
    std::string_view debugName,
    bool venus,
    Clock::time_point now)
{
    if (contextId == 0)
        return;
    Context context;
    context.name = std::string(debugName);
    context.owner = parseContextOwner(debugName);
    context.venus = venus;
    context.lastActive = now;
    Change change(*this);
    destroyLocked(contextId);
    contexts_.emplace(contextId, std::move(context));
}

void ContextMemoryLedger::destroyLocked(std::uint32_t contextId) noexcept
{
    const auto found = contexts_.find(contextId);
    if (found == contexts_.end())
        return;
    venusTotal_ -= std::min(venusTotal_, found->second.venusBytes);
    contexts_.erase(found);
    for (auto& entry : resources_) {
        auto& resource = entry.second;
        if (resource.remove(contextId) && !resource.held()) {
            heldResourceBytes_ -=
                std::min(heldResourceBytes_, resource.bytes);
        }
    }
}

void ContextMemoryLedger::contextDestroyed(std::uint32_t contextId) noexcept
{
    Change change(*this);
    destroyLocked(contextId);
}

void ContextMemoryLedger::contextActive(
    std::uint32_t contextId, Clock::time_point now) noexcept
{
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(contextId);
    if (found != contexts_.end())
        found->second.lastActive = std::max(found->second.lastActive, now);
}

void ContextMemoryLedger::rebaseLocked(
    Resource& resource,
    std::uint64_t previousBytes,
    std::uint64_t bytes) noexcept
{
    if (!resource.held())
        return;
    const auto rebase = [&](std::uint32_t contextId) {
        if (const auto context = contexts_.find(contextId);
            context != contexts_.end()) {
            auto& held = context->second.resourceBytes;
            held = held - std::min(held, previousBytes) + bytes;
        }
    };
    rebase(resource.firstHolder);
    for (const auto holder : resource.otherHolders)
        rebase(holder);
    heldResourceBytes_ =
        heldResourceBytes_ - std::min(heldResourceBytes_, previousBytes) +
        bytes;
}

void ContextMemoryLedger::reserveResource(std::uint32_t resourceId)
{
    if (resourceId == 0)
        return;
    std::lock_guard lock(mutex_);
    resources_.try_emplace(resourceId);
}

void ContextMemoryLedger::setResourceBytes(
    std::uint32_t resourceId, std::uint64_t bytes) noexcept
{
    Change change(*this);
    const auto found = resources_.find(resourceId);
    if (found == resources_.end())
        return;
    const auto previousBytes = found->second.bytes;
    found->second.bytes = bytes;
    rebaseLocked(found->second, previousBytes, bytes);
}

void ContextMemoryLedger::blobMade(
    std::uint32_t resourceId,
    std::uint32_t contextId,
    std::uint64_t bytes) noexcept
{
    Change change(*this);
    const auto resource = resources_.find(resourceId);
    const auto context = contexts_.find(contextId);
    if (resource == resources_.end() || context == contexts_.end() ||
        resource->second.held()) {
        return;
    }
    /* A Venus context's blob is memory the renderer reports by context. */
    resource->second.bytes = context->second.venus ? 0 : bytes;
    resource->second.add(contextId);
    heldResourceBytes_ += resource->second.bytes;
    context->second.resourceBytes += resource->second.bytes;
}

bool ContextMemoryLedger::resourceAttached(
    std::uint32_t contextId, std::uint32_t resourceId)
{
    Change change(*this);
    const auto context = contexts_.find(contextId);
    const auto resource = resources_.find(resourceId);
    if (context == contexts_.end() || resource == resources_.end() ||
        resource->second.holdsBy(contextId)) {
        return false;
    }
    resource->second.add(contextId);
    if (resource->second.otherHolders.empty())
        heldResourceBytes_ += resource->second.bytes;
    context->second.resourceBytes += resource->second.bytes;
    return true;
}

void ContextMemoryLedger::resourceDetached(
    std::uint32_t contextId, std::uint32_t resourceId) noexcept
{
    Change change(*this);
    const auto resource = resources_.find(resourceId);
    if (resource == resources_.end() || !resource->second.remove(contextId))
        return;
    if (!resource->second.held()) {
        heldResourceBytes_ -=
            std::min(heldResourceBytes_, resource->second.bytes);
    }
    if (const auto context = contexts_.find(contextId);
        context != contexts_.end()) {
        context->second.resourceBytes -=
            std::min(context->second.resourceBytes, resource->second.bytes);
    }
}

void ContextMemoryLedger::resourceReleased(std::uint32_t resourceId) noexcept
{
    Change change(*this);
    const auto found = resources_.find(resourceId);
    if (found == resources_.end())
        return;
    const auto& resource = found->second;
    const auto credit = [&](std::uint32_t contextId) {
        if (const auto context = contexts_.find(contextId);
            context != contexts_.end()) {
            context->second.resourceBytes -=
                std::min(context->second.resourceBytes, resource.bytes);
        }
    };
    if (resource.held()) {
        credit(resource.firstHolder);
        for (const auto holder : resource.otherHolders)
            credit(holder);
        heldResourceBytes_ -= std::min(heldResourceBytes_, resource.bytes);
    }
    resources_.erase(found);
}

void ContextMemoryLedger::venusMemory(
    std::uint32_t contextId, std::int64_t delta) noexcept
{
    Change change(*this);
    const auto found = contexts_.find(contextId);
    if (found == contexts_.end())
        return;
    auto& held = found->second.venusBytes;
    if (delta >= 0) {
        const auto grown = static_cast<std::uint64_t>(delta);
        held += grown;
        venusTotal_ += grown;
    } else {
        /* Unsigned negation, which is exact for every negative delta. */
        const auto shrunk = std::min(
            held, std::uint64_t {0} - static_cast<std::uint64_t>(delta));
        held -= shrunk;
        venusTotal_ -= std::min(venusTotal_, shrunk);
    }
}

void ContextMemoryLedger::clear() noexcept
{
    Change change(*this);
    contexts_.clear();
    resources_.clear();
    heldResourceBytes_ = 0;
    venusTotal_ = 0;
}

std::uint64_t ContextMemoryLedger::totalBytes() const noexcept
{
    std::lock_guard lock(mutex_);
    return heldResourceBytes_ + venusTotal_;
}

ContextMemoryLedger::Counts ContextMemoryLedger::counts() const noexcept
{
    std::lock_guard lock(mutex_);
    return {.contexts = contexts_.size(), .resources = resources_.size()};
}

ContextMemoryLedger::Snapshot ContextMemoryLedger::snapshot(
    Clock::time_point now,
    std::size_t maximumHolders,
    Clock::duration minimumIdle,
    std::uint64_t minimumBytes) const
{
    struct Group {
        Holder holder;
        std::uint64_t largest {};
        Clock::time_point lastActive {};
    };
    std::map<std::uint64_t, Group> groups;
    Snapshot result;
    {
        std::lock_guard lock(mutex_);
        result.totalBytes = heldResourceBytes_ + venusTotal_;
        for (const auto& [contextId, context] : contexts_) {
            const std::uint64_t bytes =
                context.resourceBytes + context.venusBytes;
            const std::uint64_t key = context.owner
                ? context.owner->pid
                : unnamedKey(contextId);
            auto& group = groups[key];
            if (group.holder.contexts == 0) {
                group.holder.pid = context.owner ? context.owner->pid : 0;
                group.holder.name =
                    context.owner ? context.owner->comm : context.name;
                group.lastActive = context.lastActive;
            } else {
                group.lastActive =
                    std::max(group.lastActive, context.lastActive);
            }
            ++group.holder.contexts;
            group.holder.bytes += bytes;
            if (bytes >= group.largest) {
                group.largest = bytes;
                group.holder.contextId = contextId;
            }
        }
    }
    for (auto& [key, group] : groups) {
        group.holder.idle = std::max(
            now - group.lastActive, Clock::duration::zero());
        if (group.holder.bytes < minimumBytes || group.holder.bytes == 0 ||
            group.holder.idle < minimumIdle) {
            continue;
        }
        result.holders.push_back(std::move(group.holder));
    }
    std::stable_sort(
        result.holders.begin(),
        result.holders.end(),
        [](const Holder& left, const Holder& right) {
            return left.bytes > right.bytes;
        });
    if (result.holders.size() > maximumHolders)
        result.holders.resize(maximumHolders);
    return result;
}

} // namespace uml::gpu
