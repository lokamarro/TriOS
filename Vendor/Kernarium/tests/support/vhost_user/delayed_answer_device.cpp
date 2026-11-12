// SPDX-License-Identifier: GPL-2.0-or-later
#include "delayed_answer_device.hpp"

#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace uml::testing {

using Clock = std::chrono::steady_clock;
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;

DelayedAnswerDevice::DelayedAnswerDevice(
    std::unique_ptr<uml::virtio::VhostUserDevice> device, Policy policy,
    Log log, Answerer answerer)
    : device_(std::move(device))
    , policy_(std::move(policy))
    , log_(std::move(log))
    , answerer_(std::move(answerer))
{
    if (!device_ || !policy_)
        throw std::invalid_argument("a delayed-answer device needs a device and a policy");
    if (::pipe(wake_) != 0)
        throw std::runtime_error("pipe failed: " + std::string(std::strerror(errno)));
    for (const int descriptor : wake_) {
        if (::fcntl(descriptor, F_SETFL, O_NONBLOCK) != 0 ||
            ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0)
            throw std::runtime_error("pipe setup failed: " + std::string(std::strerror(errno)));
    }
    /* One descriptor for the transport to poll: a queue of this pipe and
     * the wrapped device's own event source. */
    events_ = ::kqueue();
    if (events_ < 0)
        throw std::runtime_error("kqueue failed: " + std::string(std::strerror(errno)));
    struct kevent sources[2] {};
    int count = 0;
    EV_SET(&sources[count++], wake_[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (device_->eventDescriptor() >= 0)
        EV_SET(&sources[count++], device_->eventDescriptor(), EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (::kevent(events_, sources, count, nullptr, 0, nullptr) != 0)
        throw std::runtime_error("kevent failed: " + std::string(std::strerror(errno)));
    timer_ = std::thread([this] { runTimer(); });
}

DelayedAnswerDevice::~DelayedAnswerDevice()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    timer_.join();
    ::close(events_);
    ::close(wake_[0]);
    ::close(wake_[1]);
}

const char* DelayedAnswerDevice::name() const noexcept { return device_->name(); }
std::uint64_t DelayedAnswerDevice::features() const noexcept { return device_->features(); }
std::size_t DelayedAnswerDevice::queueCount() const noexcept { return device_->queueCount(); }
std::vector<std::uint8_t> DelayedAnswerDevice::configuration() const
{
    return device_->configuration();
}
void DelayedAnswerDevice::configurationRead(std::uint32_t offset, std::uint32_t length) noexcept
{
    device_->configurationRead(offset, length);
}
void DelayedAnswerDevice::writeConfiguration(std::uint32_t offset, std::uint32_t flags,
                                             std::span<const std::uint8_t> bytes)
{
    device_->writeConfiguration(offset, flags, bytes);
}

VhostUserQueueResult DelayedAnswerDevice::processQueue(std::uint32_t index,
                                                       std::uint64_t requestId,
                                                       std::span<const std::uint8_t> request,
                                                       std::size_t responseCapacity)
{
    if (answerer_) {
        if (auto answer = answerer_(index, request)) {
            note("answered a request in the device's place");
            return {VhostUserQueueAction::complete, std::move(*answer)};
        }
    }
    const auto delay = policy_(index, request);
    auto result = device_->processQueue(index, requestId, request, responseCapacity);
    if (!delay || result.action != VhostUserQueueAction::complete)
        return result;
    Held held;
    if (*delay != never)
        held.due = Clock::now() + *delay;
    held.completion = {requestId, std::move(result.response)};
    {
        std::lock_guard lock(mutex_);
        held_.push_back(std::move(held));
    }
    changed_.notify_all();
    note(*delay == never ? "held an answer until reset"
                         : "held an answer for " + std::to_string(delay->count()) + " ms");
    return {VhostUserQueueAction::defer, {}};
}

bool DelayedAnswerDevice::takeQueueCompletion(VhostUserQueueCompletion& completion)
{
    if (device_->takeQueueCompletion(completion))
        return true;
    {
        std::lock_guard lock(mutex_);
        const auto now = Clock::now();
        auto due = held_.begin();
        while (due != held_.end() && (!due->due || *due->due > now))
            ++due;
        if (due == held_.end())
            return false;
        completion = std::move(due->completion);
        held_.erase(due);
    }
    note("delivered a held answer");
    return true;
}

void DelayedAnswerDevice::reset() noexcept
{
    std::size_t dropped;
    {
        std::lock_guard lock(mutex_);
        dropped = held_.size();
        held_.clear();
    }
    if (dropped)
        note("reset dropped " + std::to_string(dropped) + " held answer(s)");
    device_->reset();
}

void DelayedAnswerDevice::prepareReset() noexcept { device_->prepareReset(); }
void DelayedAnswerDevice::setGuestMapper(GuestMapper mapper)
{
    device_->setGuestMapper(std::move(mapper));
}
void DelayedAnswerDevice::setSharedMemoryRegions(
    std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions)
{
    device_->setSharedMemoryRegions(std::move(regions));
}
std::optional<uml::virtio::VhostUserSharedMemoryRegion>
DelayedAnswerDevice::sharedMemoryRegion() const noexcept
{
    return device_->sharedMemoryRegion();
}
bool DelayedAnswerDevice::takeSharedMemoryAction(uml::virtio::VhostUserSharedMemoryAction& action)
{
    return device_->takeSharedMemoryAction(action);
}
void DelayedAnswerDevice::completeSharedMemoryAction(std::uint64_t actionId,
                                                     bool succeeded) noexcept
{
    device_->completeSharedMemoryAction(actionId, succeeded);
}

int DelayedAnswerDevice::eventDescriptor() const noexcept { return events_; }
void DelayedAnswerDevice::drainEventSource()
{
    char bytes[64];
    while (::read(wake_[0], bytes, sizeof(bytes)) > 0) {
    }
    device_->drainEventSource();
    /* Collect the queue's events once, so that it stays readable only for a
     * source that still is. */
    struct kevent pending[2];
    const timespec immediately {};
    (void)::kevent(events_, nullptr, 0, pending, 2, &immediately);
}

bool DelayedAnswerDevice::configChangePending() const noexcept
{
    return device_->configChangePending();
}
bool DelayedAnswerDevice::claimConfigChange() noexcept { return device_->claimConfigChange(); }
void DelayedAnswerDevice::releaseConfigChange() noexcept { device_->releaseConfigChange(); }
void DelayedAnswerDevice::clearConfigChange() noexcept { device_->clearConfigChange(); }
void DelayedAnswerDevice::setStopRequest(const std::atomic_bool* stop) noexcept
{
    device_->setStopRequest(stop);
}
std::string DelayedAnswerDevice::readinessDescription() const
{
    return device_->readinessDescription();
}

/* Wakes the transport through the event descriptor once for each answer that
 * falls due; the transport takes it on its own thread. */
void DelayedAnswerDevice::runTimer()
{
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        Held* next = nullptr;
        for (auto& held : held_)
            if (held.due && !held.signalled && (!next || *held.due < *next->due))
                next = &held;
        if (!next) {
            changed_.wait(lock);
            continue;
        }
        if (Clock::now() < *next->due) {
            changed_.wait_until(lock, *next->due);
            continue;
        }
        next->signalled = true;
        const char byte = 1;
        (void)!::write(wake_[1], &byte, 1);
    }
}

void DelayedAnswerDevice::note(std::string_view message) const
{
    if (log_)
        log_(message);
}

} // namespace uml::testing
