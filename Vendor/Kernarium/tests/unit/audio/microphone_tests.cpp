// SPDX-License-Identifier: GPL-2.0-or-later

#include "capture_ring.hpp"
#include "microphone_capture_source.hpp"
#include "microphone_switch.hpp"
#include "period_clock.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "test_clock.hpp"
#include "virtio_sound_device.hpp"
#include "wait_until.hpp"
#include "wire_bytes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

namespace protocol = uml::audio::protocol;

using uml::audio::CaptureRing;
using uml::audio::MicrophoneSwitch;
using uml::audio_testing::TestClock;
using uml::platform::MicrophonePermission;
using uml::testing::bytes;
using uml::testing::load;
using uml::testing::require;
using uml::testing::waitUntil;

using namespace std::chrono_literals;

/* The system's microphone permission, answered by the test. Nothing here can
 * prompt: request() only parks the completion until the test answers. */
class FakeAuthorization final : public uml::platform::MicrophoneAuthorization {
public:
    MicrophonePermission permission() const noexcept override
    {
        ++reads;
        return current;
    }

    void request(std::function<void(MicrophonePermission)> done) override
    {
        ++requests;
        pending = std::move(done);
    }

    /* The user answers the prompt. */
    void answer(MicrophonePermission result)
    {
        current = result;
        auto done = std::exchange(pending, {});
        require(static_cast<bool>(done), "no prompt was waiting");
        done(result);
    }

    MicrophonePermission current {MicrophonePermission::undetermined};
    std::function<void(MicrophonePermission)> pending;
    mutable unsigned reads {};
    unsigned requests {};
};

/* A microphone the test drives: it writes into the ring the way the audio
 * thread would. */
class FakeInput final : public uml::audio::MicrophoneInput {
public:
    bool start(std::uint8_t channelCount, CaptureRing& target) noexcept override
    {
        ++starts;
        if (!startResult)
            return false;
        channels = channelCount;
        ring = &target;
        running = true;
        if (mirror != nullptr)
            *mirror = true;
        return true;
    }

    void stop() noexcept override
    {
        ++stops;
        running = false;
        ring = nullptr;
        if (mirror != nullptr)
            *mirror = false;
    }

    void deliver(std::uint8_t value, std::size_t count)
    {
        require(running && ring != nullptr, "the microphone is not running");
        const std::vector<std::uint8_t> data(count, value);
        require(ring->write(data) == count);
    }

    CaptureRing* ring {};
    std::uint8_t channels {};
    unsigned starts {};
    unsigned stops {};
    bool startResult {true};
    bool running {};
    // Where the test can still read `running` after the source is gone.
    bool* mirror {};
};

void ringKeepsBytesInOrderAcrossTheEnd()
{
    CaptureRing ring(10);
    require(ring.capacity() == 16);
    std::vector<std::uint8_t> out(16);

    std::vector<std::uint8_t> first(12);
    for (std::size_t i = 0; i < first.size(); ++i)
        first[i] = static_cast<std::uint8_t>(i);
    require(ring.write(first) == 12);
    require(ring.read(std::span(out).first(8)) == 8);
    // 4 bytes held; the next write wraps past the end of the storage.
    std::vector<std::uint8_t> second(10);
    for (std::size_t i = 0; i < second.size(); ++i)
        second[i] = static_cast<std::uint8_t>(100 + i);
    require(ring.write(second) == 10);
    require(ring.size() == 14);
    std::vector<std::uint8_t> all(14);
    require(ring.read(all) == 14);
    for (std::size_t i = 0; i < 4; ++i)
        require(all[i] == 8 + i);
    for (std::size_t i = 0; i < 10; ++i)
        require(all[4 + i] == 100 + i);
    require(ring.size() == 0);
    require(ring.read(out) == 0);
}

void ringDropsWhatDoesNotFitAndTheConsumerDiscardsTheOldest()
{
    CaptureRing ring(16);
    std::vector<std::uint8_t> data(20, 7);
    require(ring.write(data) == 16);
    require(ring.write(data) == 0);
    std::vector<std::uint8_t> head {1, 2, 3, 4};
    ring.discard(4);
    require(ring.write(head) == 4);
    ring.discard(12);
    std::vector<std::uint8_t> out(8);
    require(ring.read(out) == 4);
    require(out[0] == 1 && out[3] == 4);

    require(ring.write(data) == 16);
    ring.clear();
    require(ring.size() == 0);
    ring.discard(100);
    require(ring.size() == 0);
}

/* The two threads never share a byte, and nothing is reordered or invented. */
void ringPassesBytesBetweenThreadsInOrder()
{
    CaptureRing ring(256);
    constexpr std::size_t total = 200'000;
    std::atomic_bool failed {};
    std::thread producer([&] {
        std::size_t sent = 0;
        while (sent < total) {
            std::array<std::uint8_t, 37> chunk;
            const auto count = std::min(chunk.size(), total - sent);
            for (std::size_t i = 0; i < count; ++i)
                chunk[i] = static_cast<std::uint8_t>((sent + i) % 251);
            sent += ring.write(std::span(chunk).first(count));
            // A byte the ring refused is offered again from where it stopped.
            if (sent < total && ring.size() == ring.capacity())
                std::this_thread::yield();
        }
    });
    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    std::array<std::uint8_t, 64> out;
    while (received < total && std::chrono::steady_clock::now() < deadline) {
        const auto count = ring.read(out);
        for (std::size_t i = 0; i < count; ++i) {
            if (out[i] != static_cast<std::uint8_t>((received + i) % 251))
                failed = true;
        }
        received += count;
        if (count == 0)
            std::this_thread::yield();
    }
    producer.join();
    require(received == total && !failed, "the ring lost or reordered bytes");
}

void switchIsOffAndAsksNothingUntilTheUserTurnsItOn()
{
    const auto authorization = std::make_shared<FakeAuthorization>();
    const auto microphone = std::make_shared<MicrophoneSwitch>(authorization);
    require(microphone->state() == MicrophoneSwitch::State::off);
    require(!microphone->active());
    microphone->disable();
    require(authorization->requests == 0);

    // Restoring an earlier choice never asks.
    require(microphone->restore() == MicrophoneSwitch::State::unasked);
    require(!microphone->active());
    require(authorization->requests == 0);
    microphone->disable();

    // The user turns it on: the one call that asks.
    std::optional<MicrophoneSwitch::State> answered;
    require(microphone->enable([&](MicrophoneSwitch::State state) {
                answered = state;
            }) == MicrophoneSwitch::State::asking);
    require(authorization->requests == 1);
    require(!microphone->active());
    // A second tap while the prompt is up does not ask again.
    require(microphone->enable() == MicrophoneSwitch::State::asking);
    require(authorization->requests == 1);

    authorization->answer(MicrophonePermission::granted);
    require(answered && *answered == MicrophoneSwitch::State::on);
    require(microphone->state() == MicrophoneSwitch::State::on);
    require(microphone->active());
    microphone->disable();
    require(!microphone->active());
    require(microphone->state() == MicrophoneSwitch::State::off);
}

void switchReportsADeniedAnswerAndAsksOnlyOnce()
{
    const auto authorization = std::make_shared<FakeAuthorization>();
    const auto microphone = std::make_shared<MicrophoneSwitch>(authorization);
    std::optional<MicrophoneSwitch::State> answered;
    (void)microphone->enable(
        [&](MicrophoneSwitch::State state) { answered = state; });
    authorization->answer(MicrophonePermission::denied);
    require(answered && *answered == MicrophoneSwitch::State::denied);
    require(!microphone->active());
    require(microphone->state() == MicrophoneSwitch::State::denied);
    // The system already answered: turning it on again asks nothing.
    require(microphone->enable() == MicrophoneSwitch::State::denied);
    require(authorization->requests == 1);

    // The user grants access in Settings later.
    authorization->current = MicrophonePermission::granted;
    require(microphone->state() == MicrophoneSwitch::State::on);
    require(microphone->active());
}

void switchWithAnEarlierGrantTurnsOnWithoutAnyPrompt()
{
    const auto authorization = std::make_shared<FakeAuthorization>();
    authorization->current = MicrophonePermission::granted;
    const auto microphone = std::make_shared<MicrophoneSwitch>(authorization);
    require(microphone->restore() == MicrophoneSwitch::State::on);
    require(microphone->active());
    require(microphone->enable() == MicrophoneSwitch::State::on);
    require(authorization->requests == 0);
}

class CountingWaker final : public uml::audio::Waker {
public:
    void notify() const noexcept override { ++count; }
    mutable unsigned count {};
};

/* A change to the setting wakes the device's worker, so that a recording
 * guest picks it up at once; a source that is gone no longer hears it. */
void switchWakesItsSourceOnEveryChange()
{
    const auto authorization = std::make_shared<FakeAuthorization>();
    const auto microphone = std::make_shared<MicrophoneSwitch>(authorization);
    const auto waker = std::make_shared<CountingWaker>();
    microphone->setWaker(waker);
    (void)microphone->enable();
    require(waker->count == 1);
    authorization->answer(MicrophonePermission::granted);
    require(waker->count == 2);
    microphone->disable();
    require(waker->count == 3);
    (void)microphone->restore();
    require(waker->count == 4);

    microphone->clearWaker(nullptr);
    microphone->disable();
    require(waker->count == 5);
    microphone->clearWaker(waker.get());
    microphone->disable();
    require(waker->count == 5);
}

struct Source final {
    Source()
        : authorization(std::make_shared<FakeAuthorization>())
        , microphone(std::make_shared<MicrophoneSwitch>(authorization))
        , waker(std::make_shared<CountingWaker>())
    {
        auto owned = std::make_unique<FakeInput>();
        input = owned.get();
        source = uml::audio::makeMicrophoneCaptureSource(
            microphone, std::move(owned), waker);
        source->configure({.periodBytes = 960, .sampleRate = 48000,
                           .channels = 2, .bytesPerSample = 2});
        source->prepare();
    }

    /* The user turns the setting on and allows it. */
    void allow()
    {
        (void)microphone->enable();
        authorization->answer(MicrophonePermission::granted);
        source->handleEvents();
    }

    std::shared_ptr<FakeAuthorization> authorization;
    std::shared_ptr<MicrophoneSwitch> microphone;
    std::shared_ptr<CountingWaker> waker;
    FakeInput* input {};
    std::unique_ptr<uml::audio::CaptureSource> source;
};

/* With the setting off, nothing reaches the microphone however the guest
 * records, and nothing asks for permission. */
void sourceTouchesNothingWhileTheSettingIsOff()
{
    Source fixture;
    fixture.source->start();
    fixture.source->handleEvents();
    std::vector<std::uint8_t> pcm(960, 0x55);
    require(fixture.source->read(pcm) == 0);
    fixture.source->stop();
    fixture.source->release();
    fixture.source->reset();
    require(fixture.input->starts == 0);
    require(fixture.authorization->requests == 0);
}

void sourceRecordsFromTheMicrophoneOnlyWhileTheGuestRecords()
{
    Source fixture;
    fixture.allow();
    // On and permitted, but the guest is not recording: the microphone and
    // its indicator stay off.
    require(fixture.input->starts == 0);

    fixture.source->start();
    require(fixture.input->starts == 1 && fixture.input->running);
    require(fixture.input->channels == 2);
    fixture.input->deliver(0x11, 1920);
    std::vector<std::uint8_t> pcm(960, 0);
    require(fixture.source->read(pcm) == 960);
    require(std::all_of(pcm.begin(), pcm.end(),
                        [](std::uint8_t b) { return b == 0x11; }));
    // Less than a period is handed over as it is; the device pads it.
    fixture.input->deliver(0x22, 100);
    require(fixture.source->read(pcm) == 960);
    require(fixture.source->read(pcm) == 100);
    require(fixture.source->read(pcm) == 0);

    fixture.source->stop();
    require(fixture.input->stops == 1 && !fixture.input->running);
    // What was captured before the stop is not delivered after the next start.
    fixture.source->start();
    require(fixture.input->starts == 2);
    require(fixture.source->read(pcm) == 0);
    fixture.source->release();
    require(fixture.input->stops == 2);
}

void sourceFollowsTheSettingUnderARecordingGuest()
{
    Source fixture;
    fixture.source->start();
    require(fixture.input->starts == 0);

    // Turned on mid-recording.
    fixture.allow();
    require(fixture.input->starts == 1 && fixture.input->running);
    // Turned off mid-recording: the microphone stops at once and the guest
    // goes on recording silence.
    fixture.microphone->disable();
    fixture.source->handleEvents();
    require(fixture.input->stops == 1 && !fixture.input->running);
    std::vector<std::uint8_t> pcm(960, 0x55);
    require(fixture.source->read(pcm) == 0);
    // And back on.
    (void)fixture.microphone->restore();
    fixture.source->handleEvents();
    require(fixture.input->starts == 2);

    // The system takes the permission away.
    fixture.authorization->current = MicrophonePermission::denied;
    fixture.source->handleEvents();
    require(fixture.input->stops == 2 && !fixture.input->running);
    fixture.source->reset();
    require(fixture.input->stops == 2);
}

void sourceTriesAgainAfterAStartThatFailed()
{
    Source fixture;
    fixture.allow();
    fixture.input->startResult = false;
    fixture.source->start();
    require(fixture.input->starts == 1 && !fixture.input->running);
    std::vector<std::uint8_t> pcm(960, 0x55);
    require(fixture.source->read(pcm) == 0);

    // The app returns to the foreground and re-applies the setting.
    fixture.input->startResult = true;
    (void)fixture.microphone->restore();
    fixture.source->handleEvents();
    require(fixture.input->starts == 2 && fixture.input->running);
    fixture.input->deliver(0x33, 960);
    require(fixture.source->read(pcm) == 960 && pcm[0] == 0x33);
}

/* A microphone that runs ahead of the guest adds no latency: the oldest
 * audio beyond a quarter of a second is dropped. */
void sourceDropsTheOldestAudioItCannotCatchUpWith()
{
    Source fixture;
    fixture.allow();
    fixture.source->start();
    for (std::uint8_t period = 1; period <= 60; ++period)
        fixture.input->deliver(period, 960);
    std::vector<std::uint8_t> pcm(960);
    // A quarter of a second of stereo, 50 periods, is kept: the newest.
    for (std::uint8_t period = 11; period <= 60; ++period)
        require(fixture.source->read(pcm) == 960 && pcm[0] == period);
    require(fixture.source->read(pcm) == 0);
}

void sourceStopsTheMicrophoneWhenTheDeviceGoesAway()
{
    const auto authorization = std::make_shared<FakeAuthorization>();
    authorization->current = MicrophonePermission::granted;
    const auto microphone = std::make_shared<MicrophoneSwitch>(authorization);
    const auto waker = std::make_shared<CountingWaker>();
    bool running = false;
    auto owned = std::make_unique<FakeInput>();
    owned->mirror = &running;
    auto* input = owned.get();
    {
        const auto source = uml::audio::makeMicrophoneCaptureSource(
            microphone, std::move(owned), waker);
        (void)microphone->restore();
        const auto before = waker->count;
        require(before == 1, "the source did not register for wake-ups");
        source->configure({.periodBytes = 960, .sampleRate = 48000,
                           .channels = 1, .bytesPerSample = 2});
        source->start();
        require(running && input->channels == 1);
    }
    require(!running);
    const auto after = waker->count;
    microphone->disable();
    require(waker->count == after, "a destroyed source was still woken");
}

protocol::PcmSetParameters captureParameters()
{
    return {
        .header = {
            .header = {
                .code = static_cast<std::uint32_t>(
                    protocol::Request::pcmSetParameters),
            },
            .streamId = protocol::inputStreamId,
        },
        .bufferBytes = 3840,
        .periodBytes = 960,
        .features = 0,
        .channels = protocol::channelCount,
        .format = protocol::PcmFormat::signed16,
        .rate = protocol::PcmRate::rate48000,
        .padding = 0,
    };
}

uml::virtio::VhostUserQueueResult command(
    uml::audio::VirtioSoundDevice& device,
    protocol::Request request)
{
    return device.processQueue(
        protocol::controlQueue, 1,
        bytes(protocol::PcmHeader {
            .header = {.code = static_cast<std::uint32_t>(request)},
            .streamId = protocol::inputStreamId,
        }),
        256);
}

/* The whole path: a guest records, the device's clock paces the receive
 * queue, and what the microphone captured arrives in the buffers while the
 * setting is on; silence otherwise. */
void deviceDeliversMicrophoneAudioOnItsOwnClock()
{
    const auto authorization = std::make_shared<FakeAuthorization>();
    authorization->current = MicrophonePermission::granted;
    const auto microphone = std::make_shared<MicrophoneSwitch>(authorization);
    const auto clock = std::make_shared<TestClock>();
    auto owned = std::make_unique<FakeInput>();
    auto* input = owned.get();
    uml::audio::VirtioSoundDevice device(
        uml::audio::makeNullPlaybackSink(),
        uml::audio::makeMicrophoneCaptureSource(
            microphone, std::move(owned), clock),
        clock);
    const auto parameters = captureParameters();
    require(load<protocol::Header>(device.processQueue(
                protocol::controlQueue, 1, bytes(parameters), 256)
                                       .response).code ==
            static_cast<std::uint32_t>(protocol::Status::ok));
    (void)command(device, protocol::Request::pcmPrepare);
    (void)microphone->restore();
    (void)command(device, protocol::Request::pcmStart);
    require(input->running);
    const auto start = clock->time;

    const auto receive = [&](std::uint64_t id) {
        return device.processQueue(
            protocol::receiveQueue, id,
            bytes(protocol::PcmTransfer {.streamId = protocol::inputStreamId}),
            960 + sizeof(protocol::PcmStatus));
    };
    const auto tick = [&](int count) {
        clock->time = start + count * 5ms;
        clock->fire();
        device.drainEventSource();
    };

    // The microphone delivers 700 bytes in the first period: the buffer
    // carries them, then zeros.
    input->deliver(0x44, 700);
    tick(1);
    auto result = receive(1);
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    require(result.response[0] == 0x44 && result.response[699] == 0x44);
    require(result.response[700] == 0 && result.response[959] == 0);

    // The setting goes off: the microphone stops and the next period is
    // silence, still on time.
    microphone->disable();
    tick(2);
    require(!input->running);
    result = receive(2);
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    require(std::all_of(result.response.begin(), result.response.begin() + 960,
                        [](std::uint8_t b) { return b == 0; }));
    tick(3);
    require(receive(3).action == uml::virtio::VhostUserQueueAction::complete);
}

} // namespace

int main()
{
    ringKeepsBytesInOrderAcrossTheEnd();
    ringDropsWhatDoesNotFitAndTheConsumerDiscardsTheOldest();
    ringPassesBytesBetweenThreadsInOrder();
    switchIsOffAndAsksNothingUntilTheUserTurnsItOn();
    switchReportsADeniedAnswerAndAsksOnlyOnce();
    switchWithAnEarlierGrantTurnsOnWithoutAnyPrompt();
    switchWakesItsSourceOnEveryChange();
    sourceTouchesNothingWhileTheSettingIsOff();
    sourceRecordsFromTheMicrophoneOnlyWhileTheGuestRecords();
    sourceFollowsTheSettingUnderARecordingGuest();
    sourceTriesAgainAfterAStartThatFailed();
    sourceDropsTheOldestAudioItCannotCatchUpWith();
    sourceStopsTheMicrophoneWhenTheDeviceGoesAway();
    deviceDeliversMicrophoneAudioOnItsOwnClock();
    return EXIT_SUCCESS;
}
