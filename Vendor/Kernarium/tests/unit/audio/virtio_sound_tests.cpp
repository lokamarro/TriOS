// SPDX-License-Identifier: GPL-2.0-or-later

#include "capture_source.hpp"
#include "period_clock.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "test_clock.hpp"
#include "virtio_sound_device.hpp"
#include "wire_bytes.hpp"

#include <poll.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace protocol = uml::audio::protocol;

using uml::testing::bytes;
using uml::testing::load;
using uml::testing::require;

using uml::audio_testing::TestClock;

using namespace std::chrono_literals;

class TestSink final : public uml::audio::PlaybackSink {
public:
    bool configure(const uml::audio::PlaybackParameters& parameters) override
    {
        ++configureCount;
        configured = parameters;
        return configureResult;
    }

    bool prepare() override
    {
        ++prepareCount;
        return prepareResult;
    }

    bool start() override
    {
        ++startCount;
        return startResult;
    }

    void stop() noexcept override { ++stopCount; }
    void release() noexcept override { ++releaseCount; }
    void reset() noexcept override { ++resetCount; }

    uml::audio::PlaybackEnqueueResult enqueue(
        std::uint64_t requestId,
        std::span<const std::uint8_t> pcm) override
    {
        if (enqueueResult != uml::audio::PlaybackEnqueueResult::accepted)
            return enqueueResult;
        enqueuedRequest = requestId;
        enqueued.assign(pcm.begin(), pcm.end());
        return uml::audio::PlaybackEnqueueResult::accepted;
    }

    void handleEvents() override { ++drainCount; }

    bool takeCompletion(uml::audio::PlaybackCompletion& completion) override
    {
        if (completions.empty())
            return false;
        completion = completions.front();
        completions.pop_front();
        return true;
    }

    uml::audio::PlaybackParameters configured {};
    std::vector<std::uint8_t> enqueued;
    std::deque<uml::audio::PlaybackCompletion> completions;
    std::uint64_t enqueuedRequest {};
    unsigned configureCount {};
    unsigned prepareCount {};
    unsigned startCount {};
    unsigned stopCount {};
    unsigned releaseCount {};
    unsigned resetCount {};
    unsigned drainCount {};
    bool configureResult {true};
    bool prepareResult {true};
    bool startResult {true};
    uml::audio::PlaybackEnqueueResult enqueueResult {
        uml::audio::PlaybackEnqueueResult::accepted};
};

/* A capture source that holds whatever the test gives it. */
class TestCapture final : public uml::audio::CaptureSource {
public:
    void configure(const uml::audio::CaptureParameters& chosen) noexcept override
    {
        ++configureCount;
        configured = chosen;
    }
    void prepare() noexcept override { ++prepareCount; }
    void start() noexcept override { ++startCount; }
    void stop() noexcept override { ++stopCount; }
    void release() noexcept override { ++releaseCount; }
    void reset() noexcept override { ++resetCount; }

    std::size_t read(std::span<std::uint8_t> pcm) noexcept override
    {
        ++readCount;
        const auto count = std::min(pcm.size(), supply.size());
        std::memcpy(pcm.data(), supply.data(), count);
        supply.erase(supply.begin(), supply.begin() + static_cast<long>(count));
        return count;
    }

    uml::audio::CaptureParameters configured {};
    std::vector<std::uint8_t> supply;
    unsigned configureCount {};
    unsigned prepareCount {};
    unsigned startCount {};
    unsigned stopCount {};
    unsigned releaseCount {};
    unsigned resetCount {};
    unsigned readCount {};
};

struct Fixture final {
    Fixture()
        : clock(std::make_shared<TestClock>())
    {
        auto owned = std::make_unique<TestSink>();
        sink = owned.get();
        auto source = std::make_unique<TestCapture>();
        capture = source.get();
        device = std::make_unique<uml::audio::VirtioSoundDevice>(
            std::move(owned), std::move(source), clock);
    }

    TestSink* sink {};
    TestCapture* capture {};
    std::shared_ptr<TestClock> clock;
    std::unique_ptr<uml::audio::VirtioSoundDevice> device;
};

uml::virtio::VhostUserQueueResult control(
    uml::audio::VirtioSoundDevice& device,
    std::span<const std::uint8_t> request,
    std::size_t capacity = 256)
{
    const auto result = device.processQueue(
        protocol::controlQueue, 1, request, capacity);
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    return result;
}

protocol::Status responseStatus(
    const uml::virtio::VhostUserQueueResult& result)
{
    return static_cast<protocol::Status>(
        load<protocol::Header>(result.response).code);
}

protocol::PcmSetParameters parameters()
{
    return {
        .header = {
            .header = {
                .code = static_cast<std::uint32_t>(
                    protocol::Request::pcmSetParameters),
            },
            .streamId = protocol::streamId,
        },
        .bufferBytes = 4096,
        .periodBytes = 1024,
        .features = 0,
        .channels = protocol::channelCount,
        .format = protocol::PcmFormat::signed16,
        .rate = protocol::PcmRate::rate48000,
        .padding = 0,
    };
}

protocol::PcmHeader pcmCommand(protocol::Request request)
{
    return {
        .header = {.code = static_cast<std::uint32_t>(request)},
        .streamId = protocol::streamId,
    };
}

void exposesOneStereoPlaybackStream()
{
    Fixture fixture;
    require(fixture.device->features() == 0);
    require(fixture.device->queueCount() == protocol::queueCount);
    const auto configuration = fixture.device->configuration();
    const auto config = load<protocol::Configuration>(configuration);
    require(config.jacks == 0);
    require(config.streams == 2);
    require(config.channelMaps == 2);
    require(config.controls == 0);

    const protocol::QueryInfo query {
        .header = {
            .code = static_cast<std::uint32_t>(protocol::Request::pcmInfo),
        },
        .startId = 0,
        .count = 1,
        .size = sizeof(protocol::PcmInfo),
    };
    const auto result = control(*fixture.device, bytes(query));
    require(responseStatus(result) == protocol::Status::ok);
    const auto info = load<protocol::PcmInfo>(
        result.response, sizeof(protocol::Header));
    require(info.direction == protocol::Direction::output);
    require(info.minimumChannels == 2 && info.maximumChannels == 2);
    require((info.formats &
             (1ULL << static_cast<unsigned>(protocol::PcmFormat::signed16))) !=
            0);
    require((info.rates &
             (1ULL << static_cast<unsigned>(protocol::PcmRate::rate48000))) !=
            0);
}

void exposesFrontLeftAndRightChannelMap()
{
    Fixture fixture;
    const protocol::QueryInfo query {
        .header = {
            .code = static_cast<std::uint32_t>(
                protocol::Request::channelMapInfo),
        },
        .startId = 0,
        .count = 1,
        .size = sizeof(protocol::ChannelMapInfo),
    };
    const auto result = control(*fixture.device, bytes(query));
    require(responseStatus(result) == protocol::Status::ok);
    const auto info = load<protocol::ChannelMapInfo>(
        result.response, sizeof(protocol::Header));
    require(info.direction == protocol::Direction::output);
    require(info.channels == 2);
    require(info.positions[0] == static_cast<std::uint8_t>(
        protocol::ChannelPosition::frontLeft));
    require(info.positions[1] == static_cast<std::uint8_t>(
        protocol::ChannelPosition::frontRight));
}

void rejectsMalformedAndOutOfOrderControlRequests()
{
    Fixture fixture;
    const std::array<std::uint8_t, 3> shortHeader {};
    require(responseStatus(control(*fixture.device, shortHeader)) ==
            protocol::Status::badMessage);

    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmStart)))) ==
            protocol::Status::ioError);

    const protocol::QueryInfo invalidQuery {
        .header = {
            .code = static_cast<std::uint32_t>(protocol::Request::pcmInfo),
        },
        .startId = 0,
        .count = 3,
        .size = sizeof(protocol::PcmInfo),
    };
    require(responseStatus(control(*fixture.device, bytes(invalidQuery))) ==
            protocol::Status::badMessage);
    auto pastTheEnd = invalidQuery;
    pastTheEnd.startId = 2;
    pastTheEnd.count = 1;
    require(responseStatus(control(*fixture.device, bytes(pastTheEnd))) ==
            protocol::Status::badMessage);
    auto empty = invalidQuery;
    empty.count = 0;
    require(responseStatus(control(*fixture.device, bytes(empty))) ==
            protocol::Status::badMessage);

    const auto event = fixture.device->processQueue(
        protocol::eventQueue, 2, {}, sizeof(protocol::Header));
    require(event.action == uml::virtio::VhostUserQueueAction::retry);
    const auto receive = fixture.device->processQueue(
        protocol::receiveQueue, 3, {}, 0);
    require(receive.action == uml::virtio::VhostUserQueueAction::complete);
    require(receive.response.empty());
    // A receive buffer for a stream that is not capturing is refused.
    const auto strayReceive = fixture.device->processQueue(
        protocol::receiveQueue, 4,
        bytes(protocol::PcmTransfer {.streamId = protocol::streamId}),
        sizeof(protocol::PcmStatus));
    require(strayReceive.action ==
            uml::virtio::VhostUserQueueAction::complete);
    require(responseStatus(strayReceive) == protocol::Status::badMessage);
}

void playbackDefersUntilTheAppleSinkConsumesAPeriod()
{
    Fixture fixture;
    const auto selected = parameters();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(fixture.sink->configured.bufferBytes == selected.bufferBytes);
    require(fixture.sink->configured.sampleRate == protocol::sampleRate);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmStart)))) ==
            protocol::Status::ok);

    const protocol::PcmTransfer transfer {.streamId = protocol::streamId};
    std::vector<std::uint8_t> pcm(selected.periodBytes, 0x5a);
    auto request = bytes(transfer);
    request.insert(request.end(), pcm.begin(), pcm.end());
    const auto queued = fixture.device->processQueue(
        protocol::transmitQueue, 42, request, sizeof(protocol::PcmStatus));
    require(queued.action == uml::virtio::VhostUserQueueAction::defer);
    require(fixture.sink->enqueuedRequest == 42);
    require(fixture.sink->enqueued == pcm);

    fixture.sink->completions.push_back({
        .requestId = 42,
        .latencyBytes = selected.periodBytes,
    });
    uml::virtio::VhostUserQueueCompletion completion;
    require(fixture.device->takeQueueCompletion(completion));
    require(completion.requestId == 42);
    const auto status = load<protocol::PcmStatus>(completion.response);
    require(status.status == protocol::Status::ok);
    require(status.latencyBytes == selected.periodBytes);
    require(!fixture.device->takeQueueCompletion(completion));

    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmStop)))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmRelease)))) ==
            protocol::Status::ok);
    require(fixture.sink->stopCount == 1);
    require(fixture.sink->releaseCount == 1);
}

void acceptsTheVirtioPcmLifecycleTransitions()
{
    Fixture fixture;
    auto selected = parameters();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(fixture.sink->configureCount == 2);

    const auto prepare = bytes(pcmCommand(protocol::Request::pcmPrepare));
    require(responseStatus(control(*fixture.device, prepare)) ==
            protocol::Status::ok);
    require(responseStatus(control(*fixture.device, prepare)) ==
            protocol::Status::ok);
    require(fixture.sink->prepareCount == 2);

    selected.periodBytes = 2048;
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(fixture.sink->configured.periodBytes == selected.periodBytes);
    require(responseStatus(control(*fixture.device, prepare)) ==
            protocol::Status::ok);

    const auto release = bytes(pcmCommand(protocol::Request::pcmRelease));
    require(responseStatus(control(*fixture.device, release)) ==
            protocol::Status::ok);
    require(fixture.sink->releaseCount == 1);
    require(responseStatus(control(*fixture.device, prepare)) ==
            protocol::Status::ok);
    require(fixture.sink->prepareCount == 4);
}

void rejectsUnsupportedParametersAndRetriesWhenFull()
{
    Fixture fixture;
    auto selected = parameters();
    selected.rate = static_cast<protocol::PcmRate>(6);
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::notSupported);

    selected = parameters();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::retry;
    const protocol::PcmTransfer transfer {.streamId = protocol::streamId};
    std::vector<std::uint8_t> pcm(selected.periodBytes);
    auto request = bytes(transfer);
    request.insert(request.end(), pcm.begin(), pcm.end());
    const auto result = fixture.device->processQueue(
        protocol::transmitQueue, 99, request, sizeof(protocol::PcmStatus));
    require(result.action == uml::virtio::VhostUserQueueAction::retry);

    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::failed;
    const auto failed = fixture.device->processQueue(
        protocol::transmitQueue, 100, request, sizeof(protocol::PcmStatus));
    require(failed.action == uml::virtio::VhostUserQueueAction::complete);
    require(load<protocol::PcmStatus>(failed.response).status ==
            protocol::Status::ioError);
}

/* A transfer's buffer belongs to the parameters it was queued under, so new
 * parameters wait until every queued transfer has come back. */
void refusesParametersWhileTransfersAreQueued()
{
    Fixture fixture;
    const auto selected = parameters();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
    const protocol::PcmTransfer transfer {.streamId = protocol::streamId};
    std::vector<std::uint8_t> pcm(selected.periodBytes);
    auto request = bytes(transfer);
    request.insert(request.end(), pcm.begin(), pcm.end());
    require(fixture.device->processQueue(
                protocol::transmitQueue, 7, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);

    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ioError);
    require(fixture.sink->configureCount == 1);

    fixture.sink->completions.push_back({
        .requestId = 7,
        .latencyBytes = selected.periodBytes,
    });
    uml::virtio::VhostUserQueueCompletion completion;
    require(fixture.device->takeQueueCompletion(completion) &&
            completion.requestId == 7);
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(fixture.sink->configureCount == 2);

    /* A device reset drops whatever the sink still held. */
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
    require(fixture.device->processQueue(
                protocol::transmitQueue, 8, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    fixture.device->reset();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
}


protocol::PcmSetParameters pacedParameters()
{
    auto selected = parameters();
    selected.bufferBytes = 3840;
    selected.periodBytes = 960;  // 5 ms of 48 kHz stereo S16.
    return selected;
}

std::vector<std::uint8_t> transferOf(std::uint32_t periodBytes)
{
    const protocol::PcmTransfer transfer {.streamId = protocol::streamId};
    std::vector<std::uint8_t> pcm(periodBytes, 0x5a);
    auto request = bytes(transfer);
    request.insert(request.end(), pcm.begin(), pcm.end());
    return request;
}

void prepareStream(Fixture& fixture, const protocol::PcmSetParameters& selected)
{
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
}

void command(Fixture& fixture, protocol::Request request)
{
    require(responseStatus(control(
                *fixture.device, bytes(pcmCommand(request)))) ==
            protocol::Status::ok);
}

/* The device runs when the transport finds the descriptor readable: it
 * drains the event source, then takes every completion. */
std::vector<std::pair<std::uint64_t, protocol::PcmStatus>> serve(
    Fixture& fixture)
{
    fixture.clock->fire();
    fixture.device->drainEventSource();
    std::vector<std::pair<std::uint64_t, protocol::PcmStatus>> completed;
    uml::virtio::VhostUserQueueCompletion completion;
    while (fixture.device->takeQueueCompletion(completion)) {
        completed.emplace_back(
            completion.requestId,
            load<protocol::PcmStatus>(completion.response));
    }
    return completed;
}

/* An output that cannot play must not make the guest's stream run fast:
 * each discarded period completes one period of real time after the one
 * before it, whatever the sink does. */
void pacesPlaybackWhileTheSinkCannotPlay()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::discarded;
    const auto request = transferOf(selected.periodBytes);
    const auto start = fixture.clock->time;

    // Periods queued before the stream starts wait for it.
    for (const std::uint64_t id : {1, 2}) {
        require(fixture.device->processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    require(!fixture.clock->armed);
    require(serve(fixture).empty());
    require(fixture.sink->enqueuedRequest == 0);

    command(fixture, protocol::Request::pcmStart);
    require(fixture.clock->armed && *fixture.clock->armed == start + 5ms);

    fixture.clock->time = start + 4900us;
    require(serve(fixture).empty());
    fixture.clock->time = start + 5ms;
    auto completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 1);
    require(completed[0].second.status == protocol::Status::ok);
    // Nothing of a paced period is heard after it completes, and the guest
    // counts the periods it still has queued itself: no latency.
    require(completed[0].second.latencyBytes == 0);
    require(*fixture.clock->armed == start + 10ms);

    // The guest refills as its periods complete; the clock keeps its pace.
    require(fixture.device->processQueue(
                protocol::transmitQueue, 3, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    fixture.clock->time = start + 10ms;
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 2);
    require(completed[0].second.latencyBytes == 0);
    fixture.clock->time = start + 15ms;
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 3);
    require(completed[0].second.latencyBytes == 0);
    require(!fixture.clock->armed);

    // After an underrun the next period starts from now, not from the past.
    fixture.clock->time = start + 100ms;
    require(fixture.device->processQueue(
                protocol::transmitQueue, 4, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    require(*fixture.clock->armed == start + 105ms);
    fixture.clock->time = start + 105ms;
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 4);
    require(fixture.sink->enqueuedRequest == 0);
}

/* A period that falls behind by more than one tick completes in order, one
 * per period, however late the device runs. */
void pacedPlaybackCatchesUpOnePeriodAtATime()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::discarded;
    command(fixture, protocol::Request::pcmStart);
    const auto request = transferOf(selected.periodBytes);
    const auto start = fixture.clock->time;
    for (const std::uint64_t id : {1, 2, 3, 4}) {
        require(fixture.device->processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    fixture.clock->time = start + 12ms;
    auto completed = serve(fixture);
    require(completed.size() == 2 && completed[0].first == 1 &&
            completed[1].first == 2);
    require(*fixture.clock->armed == start + 15ms);
    fixture.clock->time = start + 500ms;
    completed = serve(fixture);
    require(completed.size() == 2 && completed[0].first == 3 &&
            completed[1].first == 4);
    require(!fixture.clock->armed);
}

/* A sink that fails on a period it already accepted, a route change for
 * one, hands it back unplayed; the device paces it like any other. */
void pacesPeriodsTheSinkHandsBackUnplayed()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    command(fixture, protocol::Request::pcmStart);
    const auto start = fixture.clock->time;
    const auto request = transferOf(selected.periodBytes);
    for (const std::uint64_t id : {1, 2}) {
        require(fixture.device->processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    require(!fixture.clock->armed);

    fixture.clock->time = start + 1ms;
    for (const std::uint64_t id : {1, 2}) {
        fixture.sink->completions.push_back({
            .requestId = id,
            .latencyBytes = 0,
            .outcome = uml::audio::PlaybackOutcome::discarded,
        });
    }
    require(serve(fixture).empty());
    require(*fixture.clock->armed == start + 6ms);
    fixture.clock->time = start + 6ms;
    auto completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 1 &&
            completed[0].second.status == protocol::Status::ok);
    fixture.clock->time = start + 11ms;
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 2);

    // A period the sink played keeps the sink's own latency.
    require(fixture.device->processQueue(
                protocol::transmitQueue, 5, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    fixture.sink->completions.push_back({
        .requestId = 5,
        .latencyBytes = 1920,
    });
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 5 &&
            completed[0].second.latencyBytes == 1920);

    // A period the sink could not handle is an error, and is not delayed.
    require(fixture.device->processQueue(
                protocol::transmitQueue, 6, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    fixture.sink->completions.push_back({
        .requestId = 6,
        .outcome = uml::audio::PlaybackOutcome::failed,
    });
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 6 &&
            completed[0].second.status == protocol::Status::ioError);
}

/* Stopping holds the queued periods and starting plays them from then on; a
 * release hands every one back at once, as the specification requires. */
void stoppedAndReleasedStreamsReturnPacedPeriods()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::discarded;
    command(fixture, protocol::Request::pcmStart);
    const auto request = transferOf(selected.periodBytes);
    for (const std::uint64_t id : {1, 2, 3}) {
        require(fixture.device->processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    fixture.clock->time += 5ms;
    require(serve(fixture).size() == 1);

    command(fixture, protocol::Request::pcmStop);
    require(!fixture.clock->armed);
    fixture.clock->time += 1s;
    require(serve(fixture).empty());

    command(fixture, protocol::Request::pcmStart);
    require(*fixture.clock->armed == fixture.clock->time + 5ms);
    fixture.clock->time += 5ms;
    auto completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 2);

    command(fixture, protocol::Request::pcmStop);
    command(fixture, protocol::Request::pcmRelease);
    require(!fixture.clock->armed);
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 3 &&
            completed[0].second.status == protocol::Status::ok);

    // Nothing is outstanding, so the stream can be configured again.
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
}

/* A device reset drops the schedule with everything else. */
void resetClearsThePacedSchedule()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::discarded;
    command(fixture, protocol::Request::pcmStart);
    require(fixture.device->processQueue(
                protocol::transmitQueue, 1, transferOf(selected.periodBytes),
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    require(fixture.clock->armed.has_value());
    fixture.device->reset();
    require(!fixture.clock->armed);
    fixture.clock->time += 1s;
    require(serve(fixture).empty());
}

/* The null output discards every period, and the device paces it. */
void nullOutputPlaysNothingInRealTime()
{
    auto clock = std::make_shared<TestClock>();
    uml::audio::VirtioSoundDevice device(
        uml::audio::makeNullPlaybackSink(),
        uml::audio::makeSilentCaptureSource(), clock);
    const auto selected = pacedParameters();
    require(responseStatus(control(device, bytes(selected))) ==
            protocol::Status::ok);
    for (const auto request : {protocol::Request::pcmPrepare,
                               protocol::Request::pcmStart}) {
        require(responseStatus(control(
                    device, bytes(pcmCommand(request)))) ==
                protocol::Status::ok);
    }
    const auto start = clock->time;
    const auto request = transferOf(selected.periodBytes);
    for (const std::uint64_t id : {1, 2}) {
        require(device.processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    uml::virtio::VhostUserQueueCompletion completion;
    device.drainEventSource();
    require(!device.takeQueueCompletion(completion));
    clock->time = start + 5ms;
    device.drainEventSource();
    require(device.takeQueueCompletion(completion) &&
            completion.requestId == 1);
    require(load<protocol::PcmStatus>(completion.response).status ==
            protocol::Status::ok);
    require(!device.takeQueueCompletion(completion));
    clock->time = start + 10ms;
    device.drainEventSource();
    require(device.takeQueueCompletion(completion) &&
            completion.requestId == 2);
}


protocol::PcmSetParameters captureParameters(
    std::uint8_t channels = protocol::channelCount)
{
    auto selected = pacedParameters();
    selected.header.streamId = protocol::inputStreamId;
    selected.channels = channels;
    // 5 ms at either channel count.
    selected.periodBytes = 480U * channels;
    selected.bufferBytes = selected.periodBytes * 4;
    return selected;
}

protocol::PcmHeader captureCommand(protocol::Request request)
{
    return {
        .header = {.code = static_cast<std::uint32_t>(request)},
        .streamId = protocol::inputStreamId,
    };
}

protocol::Status captureControl(Fixture& fixture, protocol::Request request)
{
    return responseStatus(control(
        *fixture.device, bytes(captureCommand(request))));
}

void prepareCapture(Fixture& fixture, const protocol::PcmSetParameters& chosen)
{
    require(responseStatus(control(*fixture.device, bytes(chosen))) ==
            protocol::Status::ok);
    require(captureControl(fixture, protocol::Request::pcmPrepare) ==
            protocol::Status::ok);
}

/* One receive buffer: the period of data and the status behind it. */
uml::virtio::VhostUserQueueResult receive(
    Fixture& fixture,
    std::uint64_t id,
    std::uint32_t periodBytes)
{
    return fixture.device->processQueue(
        protocol::receiveQueue, id,
        bytes(protocol::PcmTransfer {.streamId = protocol::inputStreamId}),
        periodBytes + sizeof(protocol::PcmStatus));
}

bool isRetry(const uml::virtio::VhostUserQueueResult& result)
{
    return result.action == uml::virtio::VhostUserQueueAction::retry;
}

/* The transport finds the descriptor readable, drains the device, and runs
 * every queue again. */
void wake(Fixture& fixture)
{
    fixture.clock->fire();
    fixture.device->drainEventSource();
}

/* Returns the data of a completed receive buffer, checking its status. */
std::vector<std::uint8_t> filled(
    const uml::virtio::VhostUserQueueResult& result,
    std::uint32_t periodBytes)
{
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    require(result.response.size() == periodBytes + sizeof(protocol::PcmStatus));
    const auto status = load<protocol::PcmStatus>(result.response, periodBytes);
    require(status.status == protocol::Status::ok && status.latencyBytes == 0);
    return {result.response.begin(), result.response.begin() + periodBytes};
}

void capturePcmInfoNamesAnInputStream()
{
    Fixture fixture;
    protocol::QueryInfo query {
        .header = {
            .code = static_cast<std::uint32_t>(protocol::Request::pcmInfo),
        },
        .startId = 0,
        .count = 2,
        .size = sizeof(protocol::PcmInfo),
    };
    auto result = control(*fixture.device, bytes(query));
    require(responseStatus(result) == protocol::Status::ok);
    require(result.response.size() ==
            sizeof(protocol::Header) + 2 * sizeof(protocol::PcmInfo));
    const auto output = load<protocol::PcmInfo>(
        result.response, sizeof(protocol::Header));
    const auto input = load<protocol::PcmInfo>(
        result.response, sizeof(protocol::Header) + sizeof(protocol::PcmInfo));
    require(output.direction == protocol::Direction::output);
    require(input.direction == protocol::Direction::input);
    // One function group, so the guest pairs them as PCM device 0.
    require(output.header.functionGroupNodeId == 0 &&
            input.header.functionGroupNodeId == 0);
    require(input.minimumChannels == 1 && input.maximumChannels == 2);
    require(input.formats == output.formats && input.rates == output.rates);

    query.startId = 1;
    query.count = 1;
    result = control(*fixture.device, bytes(query));
    require(result.response.size() ==
            sizeof(protocol::Header) + sizeof(protocol::PcmInfo));
    require(load<protocol::PcmInfo>(
                result.response, sizeof(protocol::Header)).direction ==
            protocol::Direction::input);

    const protocol::QueryInfo maps {
        .header = {
            .code = static_cast<std::uint32_t>(
                protocol::Request::channelMapInfo),
        },
        .startId = 0,
        .count = 2,
        .size = sizeof(protocol::ChannelMapInfo),
    };
    result = control(*fixture.device, bytes(maps));
    require(responseStatus(result) == protocol::Status::ok);
    require(result.response.size() ==
            sizeof(protocol::Header) + 2 * sizeof(protocol::ChannelMapInfo));
    require(load<protocol::ChannelMapInfo>(
                result.response, sizeof(protocol::Header)).direction ==
            protocol::Direction::output);
    const auto inputMap = load<protocol::ChannelMapInfo>(
        result.response,
        sizeof(protocol::Header) + sizeof(protocol::ChannelMapInfo));
    require(inputMap.direction == protocol::Direction::input);
    require(inputMap.channels == 2 &&
            inputMap.header.functionGroupNodeId == 0);

    // A response buffer too small for both entries is refused.
    require(responseStatus(fixture.device->processQueue(
                protocol::controlQueue, 1, bytes(maps),
                sizeof(protocol::Header) + sizeof(protocol::ChannelMapInfo))) ==
            protocol::Status::badMessage);
}

void captureNegotiatesItsParametersApartFromPlayback()
{
    Fixture fixture;
    auto chosen = captureParameters();
    require(responseStatus(control(*fixture.device, bytes(chosen))) ==
            protocol::Status::ok);
    require(fixture.capture->configureCount == 1);
    require(fixture.capture->configured.periodBytes == chosen.periodBytes);
    require(fixture.capture->configured.channels == 2);
    require(fixture.capture->configured.sampleRate == protocol::sampleRate);
    require(fixture.capture->configured.bytesPerSample == 2);
    // Playback is not involved.
    require(fixture.sink->configureCount == 0);

    chosen = captureParameters(1);
    require(responseStatus(control(*fixture.device, bytes(chosen))) ==
            protocol::Status::ok);
    require(fixture.capture->configured.channels == 1);
    require(fixture.capture->configured.periodBytes == 480);

    // Mono playback is not offered, mono capture is; neither takes other
    // rates, formats, channel counts or a ragged period.
    auto playback = parameters();
    playback.channels = 1;
    require(responseStatus(control(*fixture.device, bytes(playback))) ==
            protocol::Status::notSupported);
    for (const auto& change : std::vector<
             std::pair<const char*, void (*)(protocol::PcmSetParameters&)>> {
             {"three channels",
              [](protocol::PcmSetParameters& p) { p.channels = 3; }},
             {"zero channels",
              [](protocol::PcmSetParameters& p) { p.channels = 0; }},
             {"rate",
              [](protocol::PcmSetParameters& p) {
                  p.rate = static_cast<protocol::PcmRate>(6);
              }},
             {"format",
              [](protocol::PcmSetParameters& p) {
                  p.format = static_cast<protocol::PcmFormat>(4);
              }},
             {"ragged period",
              [](protocol::PcmSetParameters& p) { p.periodBytes -= 2; }},
             {"feature flags",
              [](protocol::PcmSetParameters& p) { p.features = 1; }},
             {"too many periods",
              [](protocol::PcmSetParameters& p) {
                  p.bufferBytes = p.periodBytes * 300;
              }},
         }) {
        auto bad = captureParameters();
        change.second(bad);
        require(responseStatus(control(*fixture.device, bytes(bad))) ==
                    protocol::Status::notSupported,
                change.first);
    }
    require(fixture.capture->configureCount == 2);
}

/* The capture stream delivers a period for each period of real time, from
 * the moment it starts, whatever the guest or the source does. */
void captureDeliversPeriodsInRealTime()
{
    Fixture fixture;
    const auto chosen = captureParameters();
    prepareCapture(fixture, chosen);
    const auto period = chosen.periodBytes;

    // The driver queues buffers before it starts the stream.
    require(isRetry(receive(fixture, 1, period)));
    require(!fixture.clock->armed);
    require(captureControl(fixture, protocol::Request::pcmStart) ==
            protocol::Status::ok);
    const auto start = fixture.clock->time;
    require(fixture.capture->startCount == 1);
    require(fixture.clock->armed && *fixture.clock->armed == start + 5ms);

    fixture.clock->time = start + 4ms;
    wake(fixture);
    require(isRetry(receive(fixture, 1, period)));
    fixture.clock->time = start + 5ms;
    wake(fixture);
    const auto first = receive(fixture, 1, period);
    const auto data = filled(first, period);
    require(std::all_of(data.begin(), data.end(),
                        [](std::uint8_t byte) { return byte == 0; }));
    require(isRetry(receive(fixture, 2, period)));
    require(*fixture.clock->armed == start + 10ms);

    // A guest that keeps up receives one period per five milliseconds: two
    // seconds of real time are 400 periods, not one more or less.
    unsigned delivered = 1;
    for (int tick = 2; tick <= 400; ++tick) {
        fixture.clock->time = start + tick * 5ms;
        wake(fixture);
        require(*fixture.clock->armed == start + (tick + 1) * 5ms);
        const auto result = receive(fixture, 100 + tick, period);
        require(!isRetry(result));
        (void)filled(result, period);
        ++delivered;
        require(isRetry(receive(fixture, 7, period)));
    }
    require(delivered == 400);
    require(fixture.capture->readCount == 400);
}

/* A guest that stalls loses the oldest audio, not the clock: the credit is
 * the stream's buffer, and the next period still falls on the old grid. */
void captureOverrunDropsTheOldestAudio()
{
    Fixture fixture;
    const auto chosen = captureParameters();
    prepareCapture(fixture, chosen);
    const auto period = chosen.periodBytes;
    require(captureControl(fixture, protocol::Request::pcmStart) ==
            protocol::Status::ok);
    const auto start = fixture.clock->time;

    fixture.clock->time = start + 10s + 1ms;
    wake(fixture);
    for (std::uint64_t id = 1; id <= 4; ++id)
        (void)filled(receive(fixture, id, period), period);
    require(isRetry(receive(fixture, 5, period)));
    // 10.001 s is 2000 periods and a fifth; the next tick is the 2001st.
    require(*fixture.clock->armed == start + 10s + 5ms);
}

/* What the source supplies leads the period and the rest is zero. */
void captureSourceDataIsPaddedWithSilence()
{
    Fixture fixture;
    const auto chosen = captureParameters();
    prepareCapture(fixture, chosen);
    const auto period = chosen.periodBytes;
    require(captureControl(fixture, protocol::Request::pcmStart) ==
            protocol::Status::ok);
    fixture.capture->supply.assign(period + 100, 0xab);

    fixture.clock->time += 5ms;
    wake(fixture);
    auto data = filled(receive(fixture, 1, period), period);
    require(std::all_of(data.begin(), data.end(),
                        [](std::uint8_t byte) { return byte == 0xab; }));
    fixture.clock->time += 5ms;
    wake(fixture);
    data = filled(receive(fixture, 2, period), period);
    require(std::all_of(data.begin(), data.begin() + 100,
                        [](std::uint8_t byte) { return byte == 0xab; }));
    require(std::all_of(data.begin() + 100, data.end(),
                        [](std::uint8_t byte) { return byte == 0; }));
    fixture.clock->time += 5ms;
    wake(fixture);
    data = filled(receive(fixture, 3, period), period);
    require(std::all_of(data.begin(), data.end(),
                        [](std::uint8_t byte) { return byte == 0; }));
}

/* A stopped or released stream returns the buffers the driver still holds,
 * empty, and wakes the transport to collect them. */
void captureStopAndReleaseReturnTheQueuedBuffers()
{
    Fixture fixture;
    const auto chosen = captureParameters();
    prepareCapture(fixture, chosen);
    const auto period = chosen.periodBytes;
    require(captureControl(fixture, protocol::Request::pcmStart) ==
            protocol::Status::ok);
    require(isRetry(receive(fixture, 1, period)));

    const auto before = fixture.clock->notifyCount;
    require(captureControl(fixture, protocol::Request::pcmStop) ==
            protocol::Status::ok);
    require(fixture.clock->notifyCount == before + 1);
    require(fixture.capture->stopCount == 1);
    require(!fixture.clock->armed);
    auto returned = receive(fixture, 1, period);
    require(returned.action == uml::virtio::VhostUserQueueAction::complete);
    require(returned.response.size() == sizeof(protocol::PcmStatus));
    require(responseStatus(returned) == protocol::Status::ok);
    fixture.clock->time += 1s;
    wake(fixture);

    // Starting again begins a fresh stream; the stalled second is not
    // credited.
    require(captureControl(fixture, protocol::Request::pcmStart) ==
            protocol::Status::ok);
    require(isRetry(receive(fixture, 2, period)));
    fixture.clock->time += 5ms;
    wake(fixture);
    (void)filled(receive(fixture, 2, period), period);
    require(isRetry(receive(fixture, 3, period)));

    require(captureControl(fixture, protocol::Request::pcmStop) ==
            protocol::Status::ok);
    require(captureControl(fixture, protocol::Request::pcmRelease) ==
            protocol::Status::ok);
    require(fixture.capture->releaseCount == 1);
    require(fixture.clock->notifyCount == before + 3);
    returned = receive(fixture, 3, period);
    require(returned.response.size() == sizeof(protocol::PcmStatus));
    require(responseStatus(returned) == protocol::Status::ok);

    // Released, the stream can be configured and prepared once more.
    prepareCapture(fixture, captureParameters(1));
    require(fixture.capture->prepareCount == 2);
    require(isRetry(receive(fixture, 4, 480)));
}

void captureLifecycleFollowsThePlaybackRules()
{
    Fixture fixture;
    const auto chosen = captureParameters();
    for (const auto request : {protocol::Request::pcmPrepare,
                               protocol::Request::pcmStart,
                               protocol::Request::pcmStop,
                               protocol::Request::pcmRelease}) {
        require(captureControl(fixture, request) == protocol::Status::ioError);
    }
    // A buffer for a stream nobody configured is refused.
    require(responseStatus(receive(fixture, 1, chosen.periodBytes)) ==
            protocol::Status::badMessage);

    prepareCapture(fixture, chosen);
    require(captureControl(fixture, protocol::Request::pcmPrepare) ==
            protocol::Status::ok);
    require(captureControl(fixture, protocol::Request::pcmStop) ==
            protocol::Status::ioError);
    require(captureControl(fixture, protocol::Request::pcmStart) ==
            protocol::Status::ok);
    // A running stream takes no new parameters and starts only once.
    require(responseStatus(control(*fixture.device, bytes(chosen))) ==
            protocol::Status::notSupported);
    require(captureControl(fixture, protocol::Request::pcmStart) ==
            protocol::Status::ioError);
    require(captureControl(fixture, protocol::Request::pcmRelease) ==
            protocol::Status::ioError);
    // A buffer of the wrong size is refused rather than half filled.
    fixture.clock->time += 5ms;
    wake(fixture);
    require(responseStatus(receive(fixture, 2, chosen.periodBytes / 2)) ==
            protocol::Status::badMessage);
    (void)filled(receive(fixture, 3, chosen.periodBytes), chosen.periodBytes);

    // Playback is untouched by all of it.
    require(fixture.sink->prepareCount == 0 && fixture.sink->startCount == 0);
}

/* The two streams share the clock: it wakes for whichever is due first. */
void captureAndPlaybackShareTheClock()
{
    Fixture fixture;
    const auto playback = pacedParameters();
    prepareStream(fixture, playback);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::discarded;
    command(fixture, protocol::Request::pcmStart);
    const auto start = fixture.clock->time;
    require(fixture.device->processQueue(
                protocol::transmitQueue, 1, transferOf(playback.periodBytes),
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    require(*fixture.clock->armed == start + 5ms);

    // Capture starts a millisecond later, so its first period falls at
    // start + 6 ms, after the playback period that is already queued.
    fixture.clock->time = start + 1ms;
    const auto chosen = captureParameters();
    prepareCapture(fixture, chosen);
    require(captureControl(fixture, protocol::Request::pcmStart) ==
            protocol::Status::ok);
    require(*fixture.clock->armed == start + 5ms);

    fixture.clock->time = start + 5ms;
    const auto completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 1);
    require(*fixture.clock->armed == start + 6ms);

    fixture.clock->time = start + 6ms;
    wake(fixture);
    (void)filled(receive(fixture, 9, chosen.periodBytes), chosen.periodBytes);
    require(*fixture.clock->armed == start + 11ms);

    // With capture stopped the clock has nothing left to do.
    require(captureControl(fixture, protocol::Request::pcmStop) ==
            protocol::Status::ok);
    require(!fixture.clock->armed);
}

void deviceResetStopsCapture()
{
    Fixture fixture;
    const auto chosen = captureParameters();
    prepareCapture(fixture, chosen);
    require(captureControl(fixture, protocol::Request::pcmStart) ==
            protocol::Status::ok);
    require(fixture.clock->armed.has_value());
    fixture.device->reset();
    require(fixture.capture->resetCount == 1);
    require(!fixture.clock->armed);
    fixture.clock->time += 1s;
    wake(fixture);
    require(responseStatus(receive(fixture, 1, chosen.periodBytes)) ==
            protocol::Status::badMessage);
    // The guest can negotiate again from the start.
    prepareCapture(fixture, chosen);
}

/* The silent source: the default of the product. */
void silentSourceReadsNothing()
{
    const auto source = uml::audio::makeSilentCaptureSource();
    source->configure({.periodBytes = 960, .sampleRate = 48000,
                       .channels = 2, .bytesPerSample = 2});
    source->prepare();
    source->start();
    std::array<std::uint8_t, 960> pcm {};
    pcm.fill(0x55);
    require(source->read(pcm) == 0);
    source->stop();
    source->release();
    source->reset();
}

bool readable(int descriptor, std::chrono::milliseconds timeout)
{
    pollfd entry {descriptor, POLLIN, 0};
    return ::poll(&entry, 1, static_cast<int>(timeout.count())) == 1;
}

/* The production clock: a descriptor that reads ready when notified or when
 * the deadline passes, and never before. */
void productionClockWakesOnNotifyAndOnDeadline()
{
    const auto clock = uml::audio::makePeriodClock();
    const int descriptor = clock->descriptor();
    require(descriptor >= 0);
    require(!readable(descriptor, 0ms));

    clock->notify();
    require(readable(descriptor, 1s));
    clock->drain();
    require(!readable(descriptor, 0ms));

    const auto deadline = clock->now() + 30ms;
    clock->wakeAt(deadline);
    require(!readable(descriptor, 0ms));
    require(readable(descriptor, 5s), "the deadline never woke the clock");
    require(clock->now() >= deadline - 1ms);
    clock->drain();
    require(!readable(descriptor, 0ms));

    // A deadline in the past wakes at once; a cancelled one never does.
    clock->wakeAt(clock->now() - 1s);
    require(readable(descriptor, 5s));
    clock->drain();
    clock->wakeAt(clock->now() + 20ms);
    clock->cancel();
    require(!readable(descriptor, 200ms));

    // A later deadline replaces an earlier one.
    clock->wakeAt(clock->now() + 20ms);
    clock->wakeAt(clock->now() + 400ms);
    require(!readable(descriptor, 150ms));
    require(readable(descriptor, 5s));
    clock->drain();
}

} // namespace

int main()
{
    exposesOneStereoPlaybackStream();
    exposesFrontLeftAndRightChannelMap();
    rejectsMalformedAndOutOfOrderControlRequests();
    playbackDefersUntilTheAppleSinkConsumesAPeriod();
    acceptsTheVirtioPcmLifecycleTransitions();
    rejectsUnsupportedParametersAndRetriesWhenFull();
    refusesParametersWhileTransfersAreQueued();
    pacesPlaybackWhileTheSinkCannotPlay();
    pacedPlaybackCatchesUpOnePeriodAtATime();
    pacesPeriodsTheSinkHandsBackUnplayed();
    stoppedAndReleasedStreamsReturnPacedPeriods();
    resetClearsThePacedSchedule();
    nullOutputPlaysNothingInRealTime();
    capturePcmInfoNamesAnInputStream();
    captureNegotiatesItsParametersApartFromPlayback();
    captureDeliversPeriodsInRealTime();
    captureOverrunDropsTheOldestAudio();
    captureSourceDataIsPaddedWithSilence();
    captureStopAndReleaseReturnTheQueuedBuffers();
    captureLifecycleFollowsThePlaybackRules();
    captureAndPlaybackShareTheClock();
    deviceResetStopsCapture();
    silentSourceReadsNothing();
    productionClockWakesOnNotifyAndOnDeadline();
    return EXIT_SUCCESS;
}
