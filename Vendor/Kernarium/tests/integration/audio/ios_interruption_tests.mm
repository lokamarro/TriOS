// SPDX-License-Identifier: GPL-2.0-or-later
//
// Plays a stream through the Apple playback sink in the iOS Simulator and posts
// the audio-session notifications that iOS posts when another app takes the
// session and gives it back. While the session is interrupted the sink must
// play nothing yet keep the guest's clock running; it must take the session
// back when the system says so, or when no other app plays primary audio even
// though iOS never says so, and never while another app does; and it must
// never wedge. run_ios_interruption.py builds this file with the sink for the
// Simulator and runs it. The notifications are posted rather than produced by
// iOS because the Simulator does not interrupt audio sessions, and the
// session's activation, its hint about other apps' audio and the player's
// scheduling are observed and driven through hooks on AVFAudio's methods.

#include "capture_source.hpp"
#include "ios_session_notifications.hpp"
#include "period_clock.hpp"
#include "playback_sink.hpp"
#include "protocol.hpp"
#include "virtio_sound_device.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace protocol = uml::audio::protocol;

namespace {

using namespace std::chrono_literals;
using uml::audio::PlaybackEnqueueResult;
using uml::audio::PlaybackOutcome;

constexpr std::uint32_t frames = 1024;
constexpr std::uint32_t channels = 2;
constexpr std::uint32_t periodBytes = frames * channels * sizeof(std::int16_t);
// A period lasts about 21 ms, so a stream that plays completes about 47 a
// second. Anything near a tenth of that is playing.
constexpr std::uint64_t playingPerWindow = 10;
constexpr double nominalRate = 48000.0 / frames;
// The sink asks about other apps' audio once a second.
constexpr auto recoveryLimit = 2500ms;

int failures = 0;

void check(bool passed, const char* name, const std::string& detail = {})
{
    failures += passed ? 0 : 1;
    std::printf("%s %s%s%s\n", passed ? "PASS" : "FAIL", name,
        detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
}

// What the tests drive and observe on AVFAudio. The Simulator's session
// answers "no other app is playing" and succeeds every activation, so the
// hooks stand in for the apps and the system that iOS interrupts a device with.
std::atomic<bool> otherAudioPlaying {};
std::atomic<std::uint64_t> activations {};
std::atomic<int> activationsToFail {};
// How long the system takes to give the session back, in milliseconds.
std::atomic<int> activationDelay {};
std::atomic<std::uint64_t> scheduledPeriods {};

void installHooks()
{
    Method hint = class_getInstanceMethod([AVAudioSession class],
        @selector(secondaryAudioShouldBeSilencedHint));
    method_setImplementation(hint, imp_implementationWithBlock(^BOOL(id) {
        return otherAudioPlaying.load();
    }));

    Method activate = class_getInstanceMethod(
        [AVAudioSession class], @selector(setActive:error:));
    using Activate = BOOL (*)(id, SEL, BOOL, NSError* __autoreleasing*);
    static const auto activateOriginal =
        reinterpret_cast<Activate>(method_getImplementation(activate));
    method_setImplementation(activate,
        imp_implementationWithBlock(^BOOL(
            id session, BOOL active, NSError* __autoreleasing* error) {
          if (active) {
              ++activations;
              if (const auto delay = activationDelay.load(); delay > 0)
                  std::this_thread::sleep_for(std::chrono::milliseconds(delay));
              if (activationsToFail.load() > 0) {
                  --activationsToFail;
                  if (error != nullptr) {
                      *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                                   code:560557684
                                               userInfo:nil];
                  }
                  return NO;
              }
          }
          return activateOriginal(
              session, @selector(setActive:error:), active, error);
        }));

    Method schedule = class_getInstanceMethod([AVAudioPlayerNode class],
        @selector(scheduleBuffer:completionCallbackType:completionHandler:));
    using Schedule = void (*)(id, SEL, AVAudioPCMBuffer*,
        AVAudioPlayerNodeCompletionCallbackType, AVAudioNodeCompletionHandler);
    static const auto scheduleOriginal =
        reinterpret_cast<Schedule>(method_getImplementation(schedule));
    method_setImplementation(schedule,
        imp_implementationWithBlock(^(id player, AVAudioPCMBuffer* buffer,
            AVAudioPlayerNodeCompletionCallbackType type,
            AVAudioNodeCompletionHandler handler) {
          ++scheduledPeriods;
          scheduleOriginal(player,
              @selector(scheduleBuffer:completionCallbackType:completionHandler:),
              buffer, type, handler);
        }));
}

// The lines the sink writes to stderr over a span.
class StderrCapture final {
public:
    StderrCapture()
        : file_(std::tmpfile())
    {
        std::fflush(stderr);
        saved_ = dup(STDERR_FILENO);
        dup2(fileno(file_), STDERR_FILENO);
    }

    ~StderrCapture()
    {
        if (file_ != nullptr)
            (void)finish();
    }

    [[nodiscard]] std::string finish()
    {
        std::fflush(stderr);
        dup2(saved_, STDERR_FILENO);
        close(saved_);
        std::string text;
        std::rewind(file_);
        char buffer[512];
        while (const auto count = std::fread(buffer, 1, sizeof buffer, file_))
            text.append(buffer, count);
        std::fclose(file_);
        file_ = nullptr;
        return text;
    }

    [[nodiscard]] static std::size_t count(
        const std::string& text, const std::string& line)
    {
        std::size_t found = 0;
        for (auto at = text.find(line); at != std::string::npos;
             at = text.find(line, at + line.size()))
            ++found;
        return found;
    }

private:
    FILE* file_;
    int saved_ {-1};
};

// What the guest saw, and what reached the player and the session, over a span.
struct Observed final {
    std::uint64_t completed {};
    std::uint64_t scheduled {};
    double seconds {};
    // Periods a second at the rate of real audio.
    double expected {nominalRate};
    // The lowest and highest latency the completions reported, in bytes.
    std::uint32_t latencyLow {UINT32_MAX};
    std::uint32_t latencyHigh {};

    [[nodiscard]] double rate() const { return completed / seconds; }
    // The guest's frame position advances at the rate of real audio. Periods
    // complete one at a time and a window starts and ends between them, so
    // the count may be a period or two off however long the window is: a
    // second holds 46 or 47 of them, a difference of more than two percent.
    [[nodiscard]] bool nominal(double tolerance = 0.02) const
    {
        const double ideal = expected * seconds;
        return std::abs(completed - ideal) <= std::max(tolerance * ideal, 2.0);
    }
    [[nodiscard]] bool playing() const
    {
        return completed >= playingPerWindow && scheduled >= playingPerWindow;
    }
    // The guest's clock runs but nothing reaches the player.
    [[nodiscard]] bool muted(double tolerance = 0.02) const
    {
        return scheduled == 0 && nominal(tolerance);
    }
    // A period the output played is heard after the output's own latency,
    // the same for every period however many are queued; one the mute clock
    // completed is never heard.
    [[nodiscard]] bool outputLatency() const
    {
        return latencyLow == latencyHigh && latencyHigh > 0;
    }
    [[nodiscard]] bool noLatency() const { return latencyHigh == 0; }
    [[nodiscard]] std::string text() const
    {
        // Stereo 16-bit frames at 48 kHz.
        constexpr double bytesPerMillisecond = 48.0 * 4.0;
        char buffer[224];
        std::snprintf(buffer, sizeof buffer,
            "%llu periods completed in %.2f s (%.2f/s, nominal %.2f/s), "
            "%llu scheduled, latency %.1f to %.1f ms",
            static_cast<unsigned long long>(completed), seconds, rate(),
            expected, static_cast<unsigned long long>(scheduled),
            latencyLow == UINT32_MAX ? 0.0 : latencyLow / bytesPerMillisecond,
            latencyHigh / bytesPerMillisecond);
        return buffer;
    }
};

// The device thread of a Virtio sound stream: it keeps the sink's period
// queue full, sleeps on the descriptor of the device's clock, which the sink
// signals, and collects completions.
class Stream final {
public:
    Stream()
        : clock_(uml::audio::makePeriodClock())
        , sinkOwner_(uml::audio::makeApplePlaybackSink(clock_))
        , sink_(*sinkOwner_)
    {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            // Eight cycles per period, so periods join without a click.
            const auto sample = static_cast<std::int16_t>(
                600.0 * std::sin(2.0 * M_PI * 8.0 * frame / frames));
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                std::memcpy(period_.data() +
                        (frame * channels + channel) * sizeof(sample),
                    &sample, sizeof(sample));
            }
        }
    }

    [[nodiscard]] bool open()
    {
        if (!sink_.configure({.bufferBytes = periodBytes * 4,
                .periodBytes = periodBytes, .sampleRate = 48000,
                .channels = channels, .bytesPerSample = 2}) ||
            !sink_.prepare())
            return false;
        fill();
        return sink_.start();
    }

    // What the guest does when it closes the stream.
    void close()
    {
        sink_.stop();
        sink_.release();
        service();
        // A release hands every queued period back unplayed.
        collect(true);
    }

    // What the device does when the guest's virtio device is reset, as when
    // the guest suspends: the transport tears the stream down at once.
    void reset()
    {
        sink_.reset();
        service();
        collect(true);
    }

    // What the guest does when it suspends and resumes the stream.
    void pause() { sink_.stop(); }
    [[nodiscard]] bool resume() { return sink_.start(); }

    void pump(std::chrono::milliseconds span)
    {
        const auto deadline = std::chrono::steady_clock::now() + span;
        do {
            fill();
            pollfd descriptor {clock_->descriptor(), POLLIN, 0};
            (void)poll(&descriptor, 1, 10);
            service();
            collect();
        } while (std::chrono::steady_clock::now() < deadline);
    }

    // What happened over `span`, after `settle` for the state change under
    // test. `eventFirst` has the device thread look at what iOS just posted
    // inside the window, before it writes again, as a real one does.
    [[nodiscard]] Observed window(std::chrono::milliseconds settle,
        std::chrono::milliseconds span, bool eventFirst = false)
    {
        if (settle > 0ms)
            pump(settle);
        const auto begun = std::chrono::steady_clock::now();
        const auto completed = completed_;
        const auto scheduled = scheduledPeriods.load();
        latencyLow_ = UINT32_MAX;
        latencyHigh_ = 0;
        if (eventFirst) {
            service();
            collect();
        }
        pump(span);
        return {.completed = completed_ - completed,
            .scheduled = scheduledPeriods.load() - scheduled,
            .seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - begun).count(),
            .latencyLow = latencyLow_,
            .latencyHigh = latencyHigh_};
    }

    // The guest writes nothing for `span`: the sink only keeps time.
    void idle(std::chrono::milliseconds span)
    {
        const auto deadline = std::chrono::steady_clock::now() + span;
        do {
            pollfd descriptor {clock_->descriptor(), POLLIN, 0};
            (void)poll(&descriptor, 1, 10);
            service();
            collect();
        } while (std::chrono::steady_clock::now() < deadline);
    }

    // How long until a period reaches the player again, or `limit`.
    [[nodiscard]] std::chrono::milliseconds untilPlayed(
        std::chrono::milliseconds limit)
    {
        const auto begun = std::chrono::steady_clock::now();
        const auto scheduled = scheduledPeriods.load();
        while (scheduledPeriods.load() == scheduled &&
            std::chrono::steady_clock::now() - begun < limit)
            pump(20ms);
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begun);
    }

    // Whether the sink wakes the device thread within `span`: a muted sink
    // does, on its timer, until the stream stops.
    [[nodiscard]] bool wakes(std::chrono::milliseconds span)
    {
        service();
        collect();
        std::this_thread::sleep_for(100ms);
        service();
        collect();
        pollfd descriptor {clock_->descriptor(), POLLIN, 0};
        return poll(&descriptor, 1, static_cast<int>(span.count())) > 0;
    }

    [[nodiscard]] bool healthy() const { return !failed_ && rejected_ == 0; }

    // The longest the guest waited for a period to complete since the last
    // call, as the device thread saw it.
    [[nodiscard]] std::chrono::milliseconds longestWait()
    {
        const auto longest = longestWait_;
        longestWait_ = {};
        return std::chrono::duration_cast<std::chrono::milliseconds>(longest);
    }

private:
    // What the device does when its clock's descriptor is readable.
    void service()
    {
        clock_->drain();
        sink_.handleEvents();
    }

    void fill()
    {
        for (;;) {
            const auto result = sink_.enqueue(nextRequest_, period_);
            // A sink that cannot play reports `discarded`, and the device then
            // paces the guest itself: no scenario here expects that.
            if (result == PlaybackEnqueueResult::failed ||
                result == PlaybackEnqueueResult::discarded)
                failed_ = true;
            if (result != PlaybackEnqueueResult::accepted)
                return;
            ++nextRequest_;
        }
    }

    // `released` is true for the completions a release returns: a period the
    // output dropped is a failure of the sink at any other time.
    void collect(bool released = false)
    {
        uml::audio::PlaybackCompletion completion;
        while (sink_.takeCompletion(completion)) {
            const auto now = std::chrono::steady_clock::now();
            if (lastCompletion_ != std::chrono::steady_clock::time_point {})
                longestWait_ = std::max(longestWait_, now - lastCompletion_);
            lastCompletion_ = now;
            ++completed_;
            const bool dropped = completion.outcome == PlaybackOutcome::failed ||
                (completion.outcome == PlaybackOutcome::discarded && !released);
            rejected_ += dropped ? 1 : 0;
            latencyLow_ = std::min(latencyLow_, completion.latencyBytes);
            latencyHigh_ = std::max(latencyHigh_, completion.latencyBytes);
        }
    }

    std::shared_ptr<uml::audio::PeriodClock> clock_;
    std::unique_ptr<uml::audio::PlaybackSink> sinkOwner_;
    uml::audio::PlaybackSink& sink_;
    std::array<std::uint8_t, periodBytes> period_ {};
    std::uint64_t nextRequest_ {1};
    std::uint64_t completed_ {};
    std::uint64_t rejected_ {};
    bool failed_ {};
    std::chrono::steady_clock::time_point lastCompletion_ {};
    std::chrono::steady_clock::duration longestWait_ {};
    std::uint32_t latencyLow_ {UINT32_MAX};
    std::uint32_t latencyHigh_ {};
};

// A guest's sound driver on the Virtio Sound device with the real sink under
// it, as the Linux driver drives it: it keeps the device's transmit queue
// full while the stream runs, resubmitting a period as each one completes, so
// the rate at which periods complete is the rate at which the guest's audio
// advances. The device thread sleeps on the device's clock like the transport
// worker does.
class DeviceGuest final {
public:
    // `periodFrames` is the period the guest's ALSA picks: 3600 frames for a
    // buffer of 300 ms in four periods.
    explicit DeviceGuest(std::uint32_t periodFrames = 3600)
        : clock_(uml::audio::makePeriodClock())
        , device_(uml::audio::makeApplePlaybackSink(clock_),
              uml::audio::makeSilentCaptureSource(), clock_)
        , periodFrames_(periodFrames)
        , periodBytes_(periodFrames * channels * sizeof(std::int16_t))
        , period_(sizeof(protocol::PcmTransfer) + periodBytes_)
    {
        for (std::uint32_t frame = 0; frame < periodFrames_; ++frame) {
            const auto sample = static_cast<std::int16_t>(
                600.0 * std::sin(2.0 * M_PI * frame / 48.0));
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                std::memcpy(period_.data() + sizeof(protocol::PcmTransfer) +
                        (frame * channels + channel) * sizeof(sample),
                    &sample, sizeof(sample));
            }
        }
        const protocol::PcmTransfer header {.streamId = protocol::streamId};
        std::memcpy(period_.data(), &header, sizeof(header));
    }

    // The driver opens the stream and starts it.
    [[nodiscard]] bool open()
    {
        const protocol::PcmSetParameters parameters {
            .header = {{static_cast<std::uint32_t>(
                            protocol::Request::pcmSetParameters)},
                protocol::streamId},
            .bufferBytes = periodBytes_ * periods,
            .periodBytes = periodBytes_,
            .features = 0,
            .channels = protocol::channelCount,
            .format = protocol::PcmFormat::signed16,
            .rate = protocol::PcmRate::rate48000,
            .padding = 0,
        };
        if (!control(uml::virtio::toBytes(parameters)) ||
            !control(header(protocol::Request::pcmPrepare)) ||
            !control(header(protocol::Request::pcmStart)))
            return false;
        running_ = true;
        submit();
        return true;
    }

    // The driver stops the stream and releases it.
    void close()
    {
        running_ = false;
        (void)control(header(protocol::Request::pcmStop));
        (void)control(header(protocol::Request::pcmRelease));
        // A release hands every queued period back; the driver waits for them.
        pump(50ms);
        outstanding_ = 0;
    }

    // The guest suspends: its device is reset under the stream and what was
    // queued is forgotten.
    void suspend()
    {
        running_ = false;
        device_.reset();
        outstanding_ = 0;
    }

    void pump(std::chrono::milliseconds span)
    {
        const auto deadline = std::chrono::steady_clock::now() + span;
        do {
            submit();
            pollfd descriptor {device_.eventDescriptor(), POLLIN, 0};
            if (poll(&descriptor, 1, 10) > 0)
                device_.drainEventSource();
            collect();
        } while (std::chrono::steady_clock::now() < deadline);
    }

    // The periods the guest saw complete over `span`, after `settle`.
    [[nodiscard]] Observed window(std::chrono::milliseconds settle,
        std::chrono::milliseconds span)
    {
        if (settle > 0ms)
            pump(settle);
        const auto begun = std::chrono::steady_clock::now();
        const auto completed = completed_;
        const auto scheduled = scheduledPeriods.load();
        pump(span);
        return {.completed = completed_ - completed,
            .scheduled = scheduledPeriods.load() - scheduled,
            .seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - begun).count(),
            .expected = 48000.0 / periodFrames_};
    }

    [[nodiscard]] std::uint64_t errors() const { return errors_; }

private:
    [[nodiscard]] static std::vector<std::uint8_t> header(
        protocol::Request request)
    {
        return uml::virtio::toBytes(protocol::PcmHeader {
            .header = {static_cast<std::uint32_t>(request)},
            .streamId = protocol::streamId});
    }

    [[nodiscard]] bool control(std::vector<std::uint8_t> request)
    {
        const auto result = device_.processQueue(
            protocol::controlQueue, ++requestId_, request, 64);
        if (result.action != uml::virtio::VhostUserQueueAction::complete ||
            result.response.size() < sizeof(protocol::Header))
            return false;
        return uml::virtio::fromBytes<protocol::Header>(result.response).code ==
            static_cast<std::uint32_t>(protocol::Status::ok);
    }

    void submit()
    {
        while (running_ && outstanding_ < periods) {
            const auto result = device_.processQueue(protocol::transmitQueue,
                requestId_ + 1, period_, sizeof(protocol::PcmStatus));
            if (result.action == uml::virtio::VhostUserQueueAction::retry)
                return;
            ++requestId_;
            if (result.action == uml::virtio::VhostUserQueueAction::complete) {
                ++errors_;
                return;
            }
            ++outstanding_;
        }
    }

    void collect()
    {
        uml::virtio::VhostUserQueueCompletion completion;
        while (device_.takeQueueCompletion(completion)) {
            if (outstanding_ != 0)
                --outstanding_;
            ++completed_;
            if (completion.response.size() < sizeof(protocol::PcmStatus) ||
                uml::virtio::fromBytes<protocol::PcmStatus>(completion.response)
                        .status != protocol::Status::ok)
                ++errors_;
        }
    }

    static constexpr std::uint32_t periods = 4;

    std::shared_ptr<uml::audio::PeriodClock> clock_;
    uml::audio::VirtioSoundDevice device_;
    std::uint32_t periodFrames_;
    std::uint32_t periodBytes_;
    std::vector<std::uint8_t> period_;
    std::uint64_t requestId_ {};
    std::uint32_t outstanding_ {};
    std::uint64_t completed_ {};
    std::uint64_t errors_ {};
    bool running_ {};
};

void interruptionMutesAndResumes()
{
    Stream stream;
    check(stream.open(), "the stream opens");
    auto seen = stream.window(500ms, 1500ms);
    check(seen.playing(), "the stream plays", seen.text());
    check(seen.outputLatency(),
        "every played period reports the output's latency, and only that",
        seen.text());

    postDeactivation(AVAudioSessionDeactivationSourceApp, false);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing(), "the app's own deactivation leaves playback running",
        seen.text());
    postDeactivation(AVAudioSessionDeactivationSourceSystem, false);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing(),
        "a system deactivation that interrupts nothing leaves playback running",
        seen.text());

    // The interrupter plays on, so the sink must not take the session back.
    otherAudioPlaying = true;
    auto mark = activations.load();
    postInterruptionBegan();
    seen = stream.window(400ms, 4000ms);
    check(seen.scheduled == 0, "an interruption stops playback",
        seen.text());
    check(seen.nominal(),
        "the guest's frame position keeps its rate across the mute",
        seen.text());
    check(seen.noLatency(), "a period the mute clock completes reports no latency",
        seen.text());
    check(activations.load() == mark,
        "a muted sink leaves the session to the app that plays", seen.text());
    check(stream.healthy(), "a muted sink refuses nothing");

    // The end advises resuming; the hint has not caught up with it yet.
    postInterruptionEnded(true);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing(), "an end that advises resuming resumes playback",
        seen.text());
    check(activations.load() == mark + 1,
        "the sink takes the session back once", seen.text());
    check(stream.healthy(), "the resume failed no period");

    postInterruptionBegan();
    seen = stream.window(400ms, 1000ms);
    check(seen.scheduled == 0, "a second interruption stops playback",
        seen.text());

    // The end does not advise resuming and the interrupter still plays.
    // Activating a playback session interrupts such an app, so the sink
    // stays muted, keeps the guest's clock, and takes no session.
    mark = activations.load();
    postInterruptionEnded(false);
    seen = stream.window(400ms, 4000ms);
    check(seen.muted(),
        "an end that does not advise resuming leaves the sink muted while "
        "another app plays",
        seen.text());
    check(activations.load() == mark,
        "and the sink takes no session from the app that plays", seen.text());
    check(stream.healthy(), "the muted sink refuses nothing");
    // The app stops: the hint poll takes the session back.
    otherAudioPlaying = false;
    const auto polled = stream.untilPlayed(recoveryLimit);
    check(polled < recoveryLimit && activations.load() == mark + 1,
        "the sink resumes by one activation once that app has stopped",
        std::to_string(polled.count()) + " ms");
    seen = stream.window(300ms, 1500ms);
    check(seen.playing() && stream.healthy(),
        "and plays on without failing a period", seen.text());

    // With no other app playing, an end that does not advise resuming
    // resumes at once, well before the hint poll could.
    postInterruptionBegan();
    postInterruptionEnded(false);
    const auto prompt = stream.untilPlayed(600ms);
    check(prompt < 600ms,
        "an end that does not advise resuming resumes at once when no other "
        "app plays",
        std::to_string(prompt.count()) + " ms");
    seen = stream.window(300ms, 1500ms);
    check(seen.playing(), "the stream plays after it", seen.text());

    mark = activations.load();
    postInterruptionEnded(true);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing() && activations.load() == mark,
        "an end without an interruption leaves playback running",
        seen.text());

    postInterruptionBegan();
    postInterruptionEnded(true);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing(),
        "an interruption that ends before the sink looks leaves playback "
        "running",
        seen.text());
    check(stream.healthy(), "the sink never failed a period");
    stream.close();
}

// iOS posts no end when the interrupter stops or is killed, and the sink has
// to notice by itself.
void beganWithNoEnd()
{
    Stream stream;
    otherAudioPlaying = false;
    check(stream.open(), "the stream opens for the interruption without an end");
    auto seen = stream.window(500ms, 1000ms);
    check(seen.playing(), "the stream plays before the interrupter", seen.text());

    otherAudioPlaying = true;
    const auto mark = activations.load();
    postInterruptionBegan();
    // The periods the player held when it was cut off complete on the clock
    // too, not all at once: 60 ms hold two or three periods, and a burst of
    // the four the player held would make six or seven.
    seen = stream.window(0ms, 60ms, true);
    check(seen.completed <= nominalRate * seen.seconds + 2 &&
            seen.scheduled == 0,
        "the periods the player held complete on the mute clock, not at once",
        seen.text());
    seen = stream.window(0ms, 4000ms);
    check(seen.muted(),
        "an interruption that has no end mutes playback at the guest's rate",
        seen.text());
    check(activations.load() == mark,
        "the sink does not take the session back while the interrupter plays",
        seen.text());

    // The interrupter stops without a word.
    otherAudioPlaying = false;
    const auto taken = stream.untilPlayed(recoveryLimit);
    check(taken < recoveryLimit && activations.load() == mark + 1,
        "playback returns once no other app plays, by one activation",
        std::to_string(taken.count()) + " ms");
    seen = stream.window(300ms, 1500ms);
    check(seen.playing(),
        "the stream plays on after it takes the session back", seen.text());
    check(stream.healthy(), "the mute and the recovery failed no period");

    // The session is still held elsewhere when the hint clears: every
    // attempt that fails leaves the sink muted and tries again.
    otherAudioPlaying = true;
    postInterruptionBegan();
    seen = stream.window(400ms, 1000ms);
    check(seen.muted(), "a second interruption without an end mutes again",
        seen.text());
    activationsToFail = 2;
    const auto before = activations.load();
    otherAudioPlaying = false;
    const auto retaken = stream.untilPlayed(5000ms);
    check(retaken < 5000ms && activations.load() - before == 3,
        "a failed activation leaves the sink muted and tried again",
        std::to_string(retaken.count()) + " ms, " +
            std::to_string(activations.load() - before) + " attempts");
    seen = stream.window(300ms, 1000ms);
    check(seen.playing() && stream.healthy(),
        "the stream plays after the retries", seen.text());
    stream.close();
}

// Taking the session from an app that has stopped can take the system a
// while, and the engine more on the route that follows. The guest's periods
// must go on completing at the rate of real audio meanwhile: a guest whose
// write waits longer than its stream's buffer lasts (100 ms with the usual
// four periods) gets an I/O error, and the stream restarts with a gap.
void slowActivationKeepsTheClock()
{
    Stream stream;
    otherAudioPlaying = false;
    check(stream.open(), "the stream opens for the slow activation");
    auto seen = stream.window(500ms, 1000ms);
    check(seen.playing(), "the stream plays before the slow activation",
        seen.text());

    otherAudioPlaying = true;
    postInterruptionBegan();
    seen = stream.window(400ms, 1000ms);
    check(seen.muted(), "an interruption mutes before the slow activation",
        seen.text());

    // The interrupter stops, and the system then takes half a second to give
    // the session back.
    constexpr int slowMilliseconds = 500;
    activationDelay = slowMilliseconds;
    const auto mark = activations.load();
    (void)stream.longestWait();
    otherAudioPlaying = false;
    const auto taken = stream.untilPlayed(recoveryLimit + 1500ms);
    check(taken < recoveryLimit + 1500ms && activations.load() == mark + 1,
        "playback returns after a slow activation, by one activation",
        std::to_string(taken.count()) + " ms");
    seen = stream.window(300ms, 1500ms);
    check(seen.playing() && stream.healthy(),
        "the stream plays on after the slow activation", seen.text());
    // From the poll that found nobody playing, through the activation and
    // the hand-over, the guest never waited anywhere near the activation.
    const auto longest = stream.longestWait();
    check(longest < 150ms,
        "the periods kept completing through the slow activation",
        "longest wait " + std::to_string(longest.count()) + " ms, activation " +
            std::to_string(slowMilliseconds) + " ms");
    activationDelay = 0;
    stream.close();
}

// A muted stream whose guest wrote nothing for a while owes nothing: the
// periods it writes next complete at the rate of real audio, not in a burst
// that makes up the time that passed.
void idleMuteBuildsNoCredit()
{
    Stream stream;
    otherAudioPlaying = false;
    check(stream.open(), "the stream opens for the idle mute");
    (void)stream.window(300ms, 300ms);
    otherAudioPlaying = true;
    postInterruptionBegan();
    auto seen = stream.window(400ms, 500ms);
    check(seen.scheduled == 0, "an interruption stops the stream that goes idle",
        seen.text());

    stream.idle(2000ms);
    seen = stream.window(0ms, 300ms);
    // About 14 periods fit in 300 ms, and the queue refills as they complete;
    // making up two seconds would complete more than a hundred.
    check(seen.scheduled == 0 &&
            seen.completed <= nominalRate * seen.seconds + 4,
        "periods written after an idle mute complete at the rate of real audio",
        seen.text());
    seen = stream.window(0ms, 2000ms);
    check(seen.muted(), "and the mute keeps the guest's rate", seen.text());
    check(stream.healthy(), "the idle mute failed no period");
    stream.close();
}

// Neither a stream the guest restarts nor a route change may take the session
// from the app that holds it.
void noActivationWhileInterrupted()
{
    Stream stream;
    otherAudioPlaying = false;
    check(stream.open(), "the stream opens for the interrupted restarts");
    (void)stream.window(300ms, 300ms);
    otherAudioPlaying = true;
    postInterruptionBegan();
    (void)stream.window(400ms, 500ms);
    const auto mark = activations.load();

    // The guest suspends and resumes the stream, as when the app returns to
    // the foreground.
    stream.pause();
    check(stream.resume(), "the guest resumes the stream while interrupted");
    auto seen = stream.window(400ms, 3000ms);
    check(seen.muted() && activations.load() == mark,
        "a resumed stream joins the mute instead of taking the session",
        seen.text());

    // The audio route changes.
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionRouteChangeNotification
                      object:[AVAudioSession sharedInstance]];
    seen = stream.window(400ms, 3000ms);
    check(seen.muted() && activations.load() == mark,
        "a route change does not take the session from the app that plays",
        seen.text());
    check(stream.healthy(), "the muted restarts failed no period");

    // The guest suspends the stream, and no timer outlives it.
    stream.pause();
    check(!stream.wakes(2500ms), "a stopped stream leaves no timer running");

    // Resumed when nobody plays: the stale interruption gives way at once.
    otherAudioPlaying = false;
    check(stream.resume(), "the guest resumes the stream once nobody plays");
    check(activations.load() == mark + 1,
        "a resumed stream takes the session at once when no other app plays");
    seen = stream.window(100ms, 1500ms);
    check(seen.playing(), "and plays", seen.text());
    stream.close();
}

// The interruption arrives while the stream is stopped, and the stream opens
// into it.
void interruptionBeforeStart()
{
    Stream stream;
    postInterruptionBegan();
    postInterruptionEnded(true);
    check(stream.open(), "a stream opens after an interruption");
    auto seen = stream.window(500ms, 1500ms);
    check(seen.playing(), "a stream that opens after an interruption plays",
        seen.text());
    stream.close();

    otherAudioPlaying = true;
    postInterruptionBegan();
    const auto mark = activations.load();
    check(stream.open(), "a stream opens while another app plays");
    seen = stream.window(500ms, 3000ms);
    check(seen.muted() && activations.load() == mark,
        "a stream that opens during an interruption is muted", seen.text());
    otherAudioPlaying = false;
    const auto taken = stream.untilPlayed(recoveryLimit);
    check(taken < recoveryLimit, "the muted stream plays once nobody plays",
        std::to_string(taken.count()) + " ms");
    stream.close();
}

// What the guest does when the system suspends and resumes it while another
// app plays: it stops the stream, releases it, sets the parameters again,
// prepares it and starts it. Closing the stream tears it down; the session
// belongs to the app that took it, so the new stream joins the mute, at the
// rate of real audio, until that app lets go, and then plays at the same
// rate. Only an interruption that has ended, or no other app playing, makes
// the sink take the session.
void restartDuringInterruption()
{
    Stream stream;
    otherAudioPlaying = false;
    check(stream.open(), "the stream opens again");
    auto seen = stream.window(300ms, 1500ms);
    check(seen.playing() && seen.nominal(), "the stream plays at the rate of audio",
        seen.text());

    otherAudioPlaying = true;
    postInterruptionBegan();
    seen = stream.window(400ms, 1000ms);
    check(seen.muted(), "an interruption mutes the new stream", seen.text());

    const auto mark = activations.load();
    for (int round = 0; round < 2; ++round) {
        // The guest closes the stream, or its device is reset.
        if (round == 0)
            stream.close();
        else
            stream.reset();
        check(stream.open(), "the guest reopens the stream while interrupted");
        seen = stream.window(500ms, 1500ms);
        check(seen.muted() && activations.load() == mark,
            "a stream the guest reopens while another app plays joins the mute",
            seen.text() + ", " +
                std::to_string(activations.load() - mark) + " activation(s)");
        check(stream.healthy(), "the reopened stream failed no period");
    }

    // The app lets go without a word: the reopened stream plays at once, and
    // at the rate of audio, not faster.
    otherAudioPlaying = false;
    const auto taken = stream.untilPlayed(recoveryLimit);
    check(taken < recoveryLimit, "the reopened stream plays once nobody plays",
        std::to_string(taken.count()) + " ms");
    seen = stream.window(500ms, 1500ms);
    check(seen.playing() && seen.nominal(),
        "and plays at the rate of audio", seen.text());

    // Closed while the interruption stands, then reopened once it has ended.
    otherAudioPlaying = true;
    postInterruptionBegan();
    seen = stream.window(400ms, 1000ms);
    check(seen.muted(), "a second interruption mutes the stream", seen.text());
    stream.close();
    postInterruptionEnded(true);
    check(stream.open(), "the guest reopens the stream after the interruption");
    otherAudioPlaying = false;
    seen = stream.window(500ms, 1500ms);
    check(seen.playing() && seen.nominal(),
        "a stream reopened after the interruption plays at the rate of audio",
        seen.text());
    check(stream.healthy(), "the restarts failed no period");
    stream.close();
}

void destroyWhileInterrupted()
{
    otherAudioPlaying = true;
    {
        Stream stream;
        check(stream.open(), "a stream opens before the sink is destroyed");
        (void)stream.window(300ms, 300ms);
        postInterruptionBegan();
        (void)stream.window(300ms, 300ms);
    }
    // The observers and the timer went with the sink.
    postInterruptionEnded(true);
    postInterruptionBegan();
    postInterruptionEnded(false);
    otherAudioPlaying = false;
    std::this_thread::sleep_for(1500ms);

    Stream stream;
    check(stream.open(), "a new sink opens after a destroyed one");
    const auto seen = stream.window(500ms, 1500ms);
    check(seen.playing(),
        "a new sink plays after notifications for a destroyed one",
        seen.text());
    stream.close();
}

// The guest's driver on the real device, across a suspend: the rate at which
// its periods complete, in each state the sink can be in.
void devicePaceAcrossSuspend()
{
    StderrCapture lines;
    DeviceGuest guest;
    otherAudioPlaying = false;
    check(guest.open(), "the device stream opens");
    auto seen = guest.window(300ms, 1500ms);
    check(seen.nominal(), "the device stream plays at the rate of audio", seen.text());

    for (const bool interrupted : {false, true}) {
        otherAudioPlaying = interrupted;
        if (interrupted)
            postInterruptionBegan();
        seen = guest.window(300ms, 1500ms);
        guest.suspend();
        check(guest.open(), "the device stream opens after a suspend");
        seen = guest.window(300ms, 2500ms);
        check(seen.nominal(), "the device stream keeps the rate of audio after a suspend",
            seen.text());
    }
    otherAudioPlaying = false;
    seen = guest.window(2500ms, 2000ms);
    check(seen.nominal(), "and after the other app lets go", seen.text());
    check(guest.errors() == 0, "the guest saw no error", std::to_string(guest.errors()));
    guest.close();

    // The session will not activate: the sink discards, the device paces.
    activationsToFail = 1000000;
    check(guest.open(), "the device stream opens with no session");
    seen = guest.window(300ms, 2500ms);
    check(seen.nominal(), "a stream with no session keeps the rate of audio", seen.text());
    guest.suspend();
    check(guest.open(), "the device stream reopens with no session");
    seen = guest.window(300ms, 2500ms);
    check(seen.nominal(), "and after a suspend", seen.text());
    activationsToFail = 0;
    guest.close();
    const auto text = lines.finish();
    check(StderrCapture::count(text, "Audio pace:") == 0,
        "a guest that keeps the rate of audio draws no pace report");
    check(StderrCapture::count(text, "Audio output unavailable (") == 1,
        "an output that will not start is reported once");
}

// A guest whose periods arrive far faster than audio is reported, once, with
// the sink saying who completed them: here nothing plays and the guest does
// not wait, as a device that fails to pace would let it.
void reportsAGuestThatRunsAhead()
{
    StderrCapture lines;
    activationsToFail = 1000000;
    {
        Stream stream;
        check(stream.open(), "a stream opens with no session");
        // The guest writes every few milliseconds and the sink discards.
        for (int round = 0; round < 300; ++round)
            stream.pump(10ms);
    }
    activationsToFail = 0;
    const auto text = lines.finish();
    check(StderrCapture::count(text, "Audio pace:") == 1,
        "a guest that runs ahead of audio is reported once");
    check(StderrCapture::count(text, "Audio output unavailable (") == 1,
        "and so is the output that will not start");
}

} // namespace

int main()
{
    @autoreleasepool {
        installHooks();
        interruptionMutesAndResumes();
        beganWithNoEnd();
        slowActivationKeepsTheClock();
        idleMuteBuildsNoCredit();
        noActivationWhileInterrupted();
        interruptionBeforeStart();
        restartDuringInterruption();
        destroyWhileInterrupted();
        devicePaceAcrossSuspend();
        reportsAGuestThatRunsAhead();
    }
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
        failures);
    return failures == 0 ? 0 : 1;
}
