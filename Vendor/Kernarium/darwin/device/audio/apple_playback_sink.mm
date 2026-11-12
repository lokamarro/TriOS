// SPDX-License-Identifier: GPL-2.0-or-later

#include "playback_sink.hpp"

#include "audio_session.hpp"
#include "playback_mute_clock.hpp"
#include "playback_output_cycle.hpp"
#include "protocol.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace uml::audio {
namespace {

struct NotificationFlags final {
    explicit NotificationFlags(std::shared_ptr<const Waker> source)
        : wake(std::move(source))
    {
    }

    void signal(std::atomic_bool& flag) noexcept
    {
        flag.store(true, std::memory_order_release);
        wake->notify();
    }

    std::shared_ptr<const Waker> wake;
    std::atomic_bool configurationChanged {};
#if TARGET_OS_IPHONE
    std::atomic_bool interruptionBegan {};
    std::atomic_bool interruptionEnded {};
    // A recommendation since the last look said to resume.
    std::atomic_bool resumeRecommended {};
#endif
};

class CompletionQueue final {
public:
    static constexpr std::size_t capacity = 512;

    CompletionQueue()
    {
        for (std::size_t index = 0; index < cells_.size(); ++index)
            cells_[index].sequence.store(index, std::memory_order_relaxed);
    }

    void push(const PlaybackCompletion& completion) noexcept
    {
        const auto position =
            enqueuePosition_.fetch_add(1, std::memory_order_relaxed);
        auto& cell = cells_[position % capacity];
        while (cell.sequence.load(std::memory_order_acquire) != position) {
        }
        cell.completion = completion;
        cell.sequence.store(position + 1U, std::memory_order_release);
    }

    [[nodiscard]] bool pop(PlaybackCompletion& completion) noexcept
    {
        auto& cell = cells_[dequeuePosition_ % capacity];
        if (cell.sequence.load(std::memory_order_acquire) !=
            dequeuePosition_ + 1U) {
            return false;
        }
        completion = cell.completion;
        cell.sequence.store(
            dequeuePosition_ + capacity, std::memory_order_release);
        ++dequeuePosition_;
        return true;
    }

private:
    struct Cell final {
        std::atomic<std::size_t> sequence {};
        PlaybackCompletion completion {};
    };

    std::array<Cell, capacity> cells_;
    std::atomic<std::size_t> enqueuePosition_ {};
    std::size_t dequeuePosition_ {};
};

struct PlaybackState final {
    struct Slot final {
        AVAudioPCMBuffer* __strong buffer {};
        std::atomic<std::uint64_t> ticket {};
        std::uint64_t requestId {};
    };

    explicit PlaybackState(std::shared_ptr<const Waker> source)
        : wake(std::move(source))
        , slots(std::make_unique<Slot[]>(protocol::maximumPeriods))
    {
    }

    // `latencyBytes` is how long the period still takes to be heard (see
    // PlaybackCompletion): the periods still queued are the guest's to count.
    void finish(
        std::uint64_t requestId,
        PlaybackOutcome outcome,
        std::uint32_t latencyBytes) noexcept
    {
        if (!consumePending())
            return;
        finished[static_cast<std::size_t>(outcome)].fetch_add(
            1, std::memory_order_relaxed);
        completions.push({
            .requestId = requestId,
            .latencyBytes = latencyBytes,
            .outcome = outcome,
        });
        wake->notify();
    }

    void discard() noexcept { (void)consumePending(); }

private:
    [[nodiscard]] bool consumePending() noexcept
    {
        auto previous = pending.load(std::memory_order_acquire);
        while (previous != 0 &&
               !pending.compare_exchange_weak(
                   previous, previous - 1U, std::memory_order_acq_rel)) {
        }
        return previous != 0;
    }

public:
    std::shared_ptr<const Waker> wake;
    std::unique_ptr<Slot[]> slots;
    CompletionQueue completions;
    std::atomic<std::uint32_t> pending {};
    // Periods completed so far, by outcome, and how many of the played ones
    // a mute completed rather than the output: only the pace check reads
    // them.
    std::array<std::atomic<std::uint64_t>, 3> finished {};
    std::atomic<std::uint64_t> finishedByMute {};
    // What a period the output consumed reports, set whenever the output
    // starts (measureOutputLatency).
    std::atomic<std::uint32_t> outputLatencyBytes {};
#if TARGET_OS_IPHONE
    // Bumped when a mute takes over the queued periods: the completion
    // handlers of buffers scheduled before it then stand down.
    std::atomic<std::uint64_t> epoch {};
#endif
    std::size_t slotCount {};
    std::uint32_t periodBytes {};
    std::uint64_t nextTicket {1};
    std::size_t nextSlot {};
};

class ApplePlaybackSink final : public PlaybackSink {
public:
    explicit ApplePlaybackSink(std::shared_ptr<const Waker> waker)
        : wake_(std::move(waker))
        , notifications_(std::make_shared<NotificationFlags>(wake_))
        , state_(std::make_shared<PlaybackState>(wake_))
        , engine_([[AVAudioEngine alloc] init])
        , player_([[AVAudioPlayerNode alloc] init])
    {
        [engine_ attachNode:player_];
        const std::weak_ptr<NotificationFlags> weakNotifications =
            notifications_;
        configurationObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioEngineConfigurationChangeNotification
                        object:engine_
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      if (const auto flags = weakNotifications.lock())
                          flags->signal(flags->configurationChanged);
                    }];
#if TARGET_OS_IPHONE
        routeObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioSessionRouteChangeNotification
                        object:[AVAudioSession sharedInstance]
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      if (const auto flags = weakNotifications.lock())
                          flags->signal(flags->configurationChanged);
                    }];
        /* iOS 27 replaces the interruption notification with two: the session
         * becoming inactive, which also reports the app's own deactivations,
         * and the system's resumption recommendation. An interruption is a
         * deactivation that carries an interruption context. Every
         * recommendation ends the interruption; whether the sink takes the
         * session back at once depends on its advice (see endInterruption). */
        deactivationObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioSessionDidBecomeInactiveNotification
                        object:[AVAudioSession sharedInstance]
                         queue:nil
                    usingBlock:^(NSNotification* notification) {
                      const auto flags = weakNotifications.lock();
                      if (!flags)
                          return;
                      const AVAudioSessionDeactivationContext* context =
                          notification.userInfo[
                              AVAudioSessionDeactivationContextKey];
                      if (context.interruptionContext != nil)
                          flags->signal(flags->interruptionBegan);
                    }];
        resumptionObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioSessionResumptionRecommendationNotification
                        object:[AVAudioSession sharedInstance]
                         queue:nil
                    usingBlock:^(NSNotification* notification) {
                      const auto flags = weakNotifications.lock();
                      if (!flags)
                          return;
                      const AVAudioSessionResumptionContext* context =
                          notification.userInfo[
                              AVAudioSessionResumptionContextKey];
                      if (context.recommendation ==
                          AVAudioSessionResumptionRecommendationShouldResume) {
                          flags->resumeRecommended.store(
                              true, std::memory_order_release);
                      }
                      flags->signal(flags->interruptionEnded);
                    }];
#endif
    }

    ~ApplePlaybackSink() override
    {
        @try {
            [[NSNotificationCenter defaultCenter]
                removeObserver:configurationObserver_];
#if TARGET_OS_IPHONE
            [[NSNotificationCenter defaultCenter] removeObserver:routeObserver_];
            [[NSNotificationCenter defaultCenter]
                removeObserver:deactivationObserver_];
            [[NSNotificationCenter defaultCenter]
                removeObserver:resumptionObserver_];
#endif
        } @catch (NSException*) {
        }
        reset();
        @try {
            [engine_ detachNode:player_];
        } @catch (NSException*) {
        }
    }

    [[nodiscard]] bool configure(
        const PlaybackParameters& parameters) override
    {
#if TARGET_OS_IPHONE
        settleResumption();
#endif
        // Queued periods belong to the current state, and their completions
        // arrive only there: replacing it would strand them, so they must
        // come back first, as prepare() also requires.
        if (state_->pending.load(std::memory_order_acquire) != 0)
            return false;
        @try {
            [player_ stop];
            [engine_ stop];
            deactivateSession();
#if TARGET_OS_IPHONE
            endStreamMute();
#endif
            configured_ = false;
            prepared_ = false;
            running_ = false;
            state_ = std::make_shared<PlaybackState>(wake_);
            auto state = state_;
            state->slotCount = parameters.bufferBytes / parameters.periodBytes;
            state->periodBytes = parameters.periodBytes;
            format_ = [[AVAudioFormat alloc]
                initWithCommonFormat:AVAudioPCMFormatInt16
                         sampleRate:parameters.sampleRate
                           channels:parameters.channels
                        interleaved:YES];
            if (format_ == nil || state->slotCount == 0 ||
                state->slotCount > protocol::maximumPeriods)
                return false;
            const auto frameCapacity = static_cast<AVAudioFrameCount>(
                parameters.periodBytes /
                (parameters.channels * parameters.bytesPerSample));
#if TARGET_OS_IPHONE
            muteClock_ = MuteClock(parameters.sampleRate, frameCapacity);
#endif
            periodFrames_ = frameCapacity;
            periodsPerSecond_ = static_cast<double>(parameters.sampleRate) /
                static_cast<double>(frameCapacity);
            outputCycle_ = OutputCycle::seconds(parameters);
            // iOS asks the session, which the next activation applies.
            session::preferIoCycle(session::Client::playback, outputCycle_);
            paceSampled_ = {};
            for (std::size_t index = 0; index < state->slotCount; ++index) {
                state->slots[index].buffer = [[AVAudioPCMBuffer alloc]
                    initWithPCMFormat:format_
                        frameCapacity:frameCapacity];
                if (state->slots[index].buffer == nil)
                    return false;
            }
            [engine_ disconnectNodeOutput:player_];
            NSError* connectError = nil;
            if (![engine_ connect:player_
                               to:engine_.mainMixerNode
                           format:format_
                            error:&connectError]) {
                configured_ = false;
                prepared_ = false;
                running_ = false;
                fault("the engine would not connect");
                return false;
            }
#if !TARGET_OS_IPHONE
            applyOutputCycle();
#endif
            configured_ = true;
            prepared_ = false;
            running_ = false;
            faulted_ = false;
            return true;
        } @catch (NSException*) {
            configured_ = false;
            prepared_ = false;
            running_ = false;
            fault("the sink could not be configured");
            return false;
        }
    }

    [[nodiscard]] bool prepare() override
    {
#if TARGET_OS_IPHONE
        settleResumption();
#endif
        if (!configured_ || state_->pending.load(std::memory_order_acquire) != 0)
            return false;
        @try {
            [engine_ prepare];
            prepared_ = true;
            faulted_ = false;
            return true;
        } @catch (NSException*) {
            fault("the engine would not prepare");
            return false;
        }
    }

    /* An output that will not start does not fail the guest's stream. The
     * stream runs, the sink discards what it is given (enqueue() reports
     * it), the device paces the guest on its own clock, and the next route
     * change or end of an interruption tries the output again. A false
     * return means only that the sink was not prepared. */
    [[nodiscard]] bool start() override
    {
        if (!configured_ || !prepared_)
            return false;
#if TARGET_OS_IPHONE
        settleResumption();
        // Another app holds the session: the stream starts muted and the
        // sink takes the session when that app lets go, not before.
        applyInterruptionEvents();
        running_ = true;
        bool sessionReady = false;
        if (interrupted_) {
            if (session::otherAudioPlaying() || !activateSession()) {
                faulted_ = false;
                enterMute();
                return true;
            }
            leaveMute();
            sessionReady = true;
        }
        if (!sessionReady && !activateSession()) {
#else
        running_ = true;
        if (!activateSession()) {
#endif
            failToStart();
            return true;
        }
        @try {
            NSError* error = nil;
            const bool engineRuns =
                engine_.isRunning || [engine_ startAndReturnError:&error];
            // Before the player plays, so that its first completion has it.
            if (engineRuns)
                measureOutputLatency();
            if (!engineRuns || ![player_ playAndReturnError:&error]) {
                deactivateSession();
                failToStart();
                return true;
            }
            faulted_ = false;
            return true;
        } @catch (NSException*) {
            deactivateSession();
            failToStart();
            return true;
        }
    }

    void stop() noexcept override
    {
#if TARGET_OS_IPHONE
        settleResumption();
#endif
        @try {
            [player_ pause];
        } @catch (NSException*) {
        }
        running_ = false;
#if TARGET_OS_IPHONE
        cancelMuteTimer();
#endif
    }

    void release() noexcept override
    {
#if TARGET_OS_IPHONE
        settleResumption();
#endif
        @try {
            [player_ stop];
            [engine_ pause];
        } @catch (NSException*) {
        }
        abortPending();
        deactivateSession();
#if TARGET_OS_IPHONE
        endStreamMute();
#endif
        prepared_ = false;
        running_ = false;
        faulted_ = false;
    }

    void reset() noexcept override
    {
#if TARGET_OS_IPHONE
        settleResumption();
#endif
        @try {
            [player_ stop];
            [engine_ stop];
        } @catch (NSException*) {
        }
        deactivateSession();
#if TARGET_OS_IPHONE
        endStreamMute();
#endif
        try {
            state_ = std::make_shared<PlaybackState>(wake_);
        } catch (...) {
            // Reset is a transport teardown boundary and cannot report an
            // allocation failure. The stopped state remains safe to replace
            // on the next successful configure request.
        }
        configured_ = false;
        prepared_ = false;
        running_ = false;
        faulted_ = false;
    }

    [[nodiscard]] PlaybackEnqueueResult enqueue(
        std::uint64_t requestId,
        std::span<const std::uint8_t> pcm) override
    {
        auto state = state_;
        if (!prepared_ || pcm.size() != state->periodBytes ||
            state->slotCount == 0)
            return PlaybackEnqueueResult::failed;
        if (faulted_) {
            ++arrived_;
            ++declined_;
            return PlaybackEnqueueResult::discarded;
        }
        PlaybackState::Slot* selected = nullptr;
        std::size_t selectedIndex = 0;
        for (std::size_t offset = 0; offset < state->slotCount; ++offset) {
            const auto index = (state->nextSlot + offset) % state->slotCount;
            if (state->slots[index].ticket.load(std::memory_order_acquire) ==
                0) {
                selected = &state->slots[index];
                selectedIndex = index;
                state->nextSlot = (index + 1U) % state->slotCount;
                break;
            }
        }
        if (selected == nullptr)
            return PlaybackEnqueueResult::retry;
        std::uint64_t ticket {};
        @try {
            selected->buffer.frameLength = selected->buffer.frameCapacity;
            auto* audioBuffer = selected->buffer.mutableAudioBufferList;
            if (audioBuffer == nullptr || audioBuffer->mNumberBuffers != 1 ||
                audioBuffer->mBuffers[0].mData == nullptr ||
                static_cast<std::size_t>(
                    audioBuffer->mBuffers[0].mDataByteSize) < pcm.size())
                return PlaybackEnqueueResult::discarded;
            std::memcpy(audioBuffer->mBuffers[0].mData, pcm.data(), pcm.size());

            ticket = state->nextTicket++;
            selected->requestId = requestId;
            selected->ticket.store(ticket, std::memory_order_release);
            [[maybe_unused]] const auto queued =
                state->pending.fetch_add(1, std::memory_order_release);
            ++arrived_;
#if TARGET_OS_IPHONE
            if (muted_) {
                // The mute clock completes the period; nothing is played.
                if (queued == 0)
                    muteClock_.restart(MuteClock::Clock::now());
                armMute();
                return PlaybackEnqueueResult::accepted;
            }
#endif
            schedule(state, selectedIndex, ticket, requestId);
            return PlaybackEnqueueResult::accepted;
        } @catch (NSException*) {
            if (ticket != 0) {
                auto expected = ticket;
                if (selected->ticket.compare_exchange_strong(
                        expected, 0, std::memory_order_acq_rel))
                    state->discard();
            }
            fault("a period could not be queued");
            return PlaybackEnqueueResult::discarded;
        }
    }

    void handleEvents() override
    {
        @try {
#if TARGET_OS_IPHONE
            applyInterruptionEvents();
#endif
            watchPace();
#if TARGET_OS_IPHONE
            // A resumption in flight owns the engine: a configuration change
            // it provokes is acted on once it has ended.
            const bool configurationChanged = resumption_ == nullptr &&
                notifications_->configurationChanged.exchange(
                    false, std::memory_order_acq_rel);
#else
            const bool configurationChanged =
                notifications_->configurationChanged.exchange(
                    false, std::memory_order_acq_rel);
#endif
            if (configurationChanged && configured_) {
                [player_ stop];
                [engine_ stop];
#if TARGET_OS_IPHONE
                // A muted sink's periods belong to the mute clock.
                if (!muted_)
#endif
                    abortPending();
#if !TARGET_OS_IPHONE
                // The output may be another device now.
                applyOutputCycle();
#endif
                [engine_ prepare];
                faulted_ = false;
            }
#if TARGET_OS_IPHONE
            if (configurationChanged && running_ && !muted_)
#else
            if (configurationChanged && running_)
#endif
            {
                NSError* error = nil;
                const bool engineRuns = activateSession() &&
                    (engine_.isRunning || [engine_ startAndReturnError:&error]);
                if (engineRuns)
                    measureOutputLatency();
                if (engineRuns && [player_ playAndReturnError:&error]) {
                    faulted_ = false;
                } else {
                    abortPending();
                    running_ = false;
                    fault("the output did not restart after a change");
                }
            }
#if TARGET_OS_IPHONE
            serviceMute();
#endif
        } @catch (NSException*) {
            abortPending();
            running_ = false;
            fault("the output raised an exception");
#if TARGET_OS_IPHONE
            endMute();
#endif
        }
    }

    [[nodiscard]] bool takeCompletion(
        PlaybackCompletion& completion) override
    {
        return state_->completions.pop(completion);
    }

private:
    void schedule(
        std::shared_ptr<PlaybackState> state,
        std::size_t index,
        std::uint64_t ticket,
        std::uint64_t requestId)
    {
#if TARGET_OS_IPHONE
        const auto epoch = state->epoch.load(std::memory_order_acquire);
#endif
        [player_
            scheduleBuffer:state->slots[index].buffer
            completionCallbackType:AVAudioPlayerNodeCompletionDataConsumed
            completionHandler:^(AVAudioPlayerNodeCompletionCallbackType) {
#if TARGET_OS_IPHONE
              if (state->epoch.load(std::memory_order_acquire) != epoch)
                  return;
#endif
              auto& slot = state->slots[index];
              auto expected = ticket;
              if (slot.ticket.compare_exchange_strong(
                      expected, 0, std::memory_order_acq_rel)) {
                  state->finish(requestId, PlaybackOutcome::played,
                      state->outputLatencyBytes.load(
                          std::memory_order_relaxed));
              }
            }];
    }

#if TARGET_OS_IPHONE
    using Clock = MuteClock::Clock;

    // How often a muted sink asks whether the app that took the session is
    // still playing.
    static constexpr auto hintPollInterval = std::chrono::seconds(1);
    // A mute that starts within this long of a recovery is not logged.
    static constexpr auto flapInterval = std::chrono::seconds(10);

    /* iOS posts the end of an interruption only when the interrupting app
     * deactivates its session. An app that simply stops, or is killed, posts
     * none, and a sink that waited for it would stay paused for as long as
     * the app is in front, while the guest, which sees no pause, times out
     * every write. So an interrupted sink is muted instead: its queued periods
     * complete on the schedule the audio hardware would have kept, the guest's
     * clock keeps running, and the sink takes the session back as soon as the
     * system advises resuming or no other app is playing primary audio (see
     * endInterruption). */

    // Acts on the notifications that arrived since the last look, so a
    // request that follows them sees the interruption they report.
    void applyInterruptionEvents()
    {
        if (notifications_->interruptionBegan.exchange(
                false, std::memory_order_acq_rel)) {
            interrupted_ = true;
            if (running_ && !muted_)
                enterMute();
        }
        if (notifications_->interruptionEnded.exchange(
                false, std::memory_order_acq_rel)) {
            endInterruption(notifications_->resumeRecommended.exchange(
                false, std::memory_order_acq_rel));
        }
    }

    // The system took the session while the stream runs: mute, or restart a
    // mute the stream was stopped in.
    void enterMute()
    {
        const auto now = Clock::now();
        if (!muted_) {
            muted_ = true;
            muteStarted_ = now;
            // One line per mute, and none for a mute that follows a recovery
            // closely: a session the system keeps taking back would otherwise
            // log every second.
            muteLogged_ = now - lastResumed_ > flapInterval;
            // The handlers of buffers the player still holds stand down:
            // their periods are the clock's from here on.
            state_->epoch.fetch_add(1, std::memory_order_acq_rel);
            @try {
                [player_ stop];
                [engine_ pause];
            } @catch (NSException*) {
            }
            if (muteLogged_) {
                std::fputs(
                    "Audio muted: another app holds the audio session\n",
                    stderr);
            }
        }
        muteClock_.restart(now);
        nextHintPoll_ = now + hintPollInterval;
        armMute();
    }

    // Hands the periods the mute clock still holds back to the player, oldest
    // first, and ends the mute without touching the session.
    void leaveMute()
    {
        if (muted_) {
            const auto state = state_;
            std::array<std::pair<std::uint64_t, std::size_t>,
                protocol::maximumPeriods> queued;
            std::size_t count = 0;
            for (std::size_t index = 0; index < state->slotCount; ++index) {
                const auto ticket = state->slots[index].ticket.load(
                    std::memory_order_acquire);
                if (ticket != 0)
                    queued[count++] = {ticket, index};
            }
            std::sort(queued.begin(), queued.begin() + count);
            for (std::size_t position = 0; position < count; ++position) {
                const auto [ticket, index] = queued[position];
                schedule(state, index, ticket, state->slots[index].requestId);
            }
        }
        endMute();
    }

    /* The stream's mute ends: its periods are no longer the mute clock's.
     * The interruption does not. It is the session's, held by the app that
     * took it, and only the system's word that it is over, the session taken
     * back, or no other app playing ends it; a stream that is torn down and
     * set up again, as when the guest suspends and resumes, finds the session
     * as it was left. */
    void endStreamMute() noexcept
    {
        muted_ = false;
        cancelMuteTimer();
    }

    // Forgets any interruption: the session is ours again, or the sink faulted.
    void endMute() noexcept
    {
        endStreamMute();
        interrupted_ = false;
    }

    /* The system says the interrupter let go. Taking the session back
     * activates it, and a playback session that does not mix with others
     * interrupts an app that plays, such as a call. So the sink takes it back
     * at once only on the system's advice to resume. Without that advice an
     * app that still plays keeps the session: the sink stays muted, its
     * periods keep completing on the mute clock, and the next hint poll takes
     * the session back once that app has stopped. A stream that is not running
     * stays interrupted, and its next start asks the same question. */
    void endInterruption(bool resumeRecommended)
    {
        if (!resumeRecommended && session::otherAudioPlaying())
            return;
        if (!muted_) {
            interrupted_ = false;
            return;
        }
        if (running_)
            beginResume();
        else
            leaveMute();
    }

    /* Taking the session back from an app that has just stopped, starting the
     * engine on the route that follows, and the audio hardware's first cycle
     * each take as long as the system takes: measured on an iPad mini 6, about
     * 70, 75 and 210 ms. The device thread completes the guest's periods, and
     * a guest whose write waits for one longer than its stream's buffer lasts
     * (as little as 100 ms with the usual four periods) gets an I/O error. So
     * the session and the engine are taken over on another thread, which
     * plays one short buffer of silence and waits for the player to consume
     * it, while the mute clock goes on completing periods here. The device
     * thread finishes the resumption when the other thread reports; see
     * completeResume. */
    struct Resumption final {
        enum class Outcome { sessionRefused, engineFailed, ready };

        std::mutex mutex;
        std::condition_variable ended;
        bool finished {};
        Outcome outcome {Outcome::sessionRefused};
    };

    // Starts taking the session back, unless that is already under way.
    void beginResume()
    {
        if (resumption_ != nullptr)
            return;
        const auto resumption = std::make_shared<Resumption>();
        resumption_ = resumption;
        nextHintPoll_ = Clock::now() + hintPollInterval;
        AVAudioEngine* const engine = engine_;
        AVAudioPlayerNode* const player = player_;
        // A few milliseconds of silence, played once the engine runs: see
        // the worker below.
        AVAudioPCMBuffer* const probe = [[AVAudioPCMBuffer alloc]
            initWithPCMFormat:format_
                frameCapacity:256];
        if (probe != nil) {
            probe.frameLength = 256;
            const AudioBufferList* const list = probe.audioBufferList;
            for (UInt32 index = 0; index < list->mNumberBuffers; ++index)
                std::memset(list->mBuffers[index].mData, 0,
                    list->mBuffers[index].mDataByteSize);
        }
        const std::weak_ptr<NotificationFlags> weakNotifications =
            notifications_;
        dispatch_async(
            dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
              auto outcome = Resumption::Outcome::sessionRefused;
              @try {
                  if (activateSession()) {
                      NSError* error = nil;
                      const bool running = engine.isRunning ||
                          [engine startAndReturnError:&error];
                      outcome = running ? Resumption::Outcome::ready
                                        : Resumption::Outcome::engineFailed;
                      // The player starts here, with nothing scheduled, so
                      // the periods handed to it later start at once instead
                      // of after the player's own start-up.
                      if (running) {
                          (void)[player playAndReturnError:&error];
                          // The engine reports that it runs well before the
                          // audio hardware delivers its first cycle: a fifth
                          // of a second has been measured on the iPad mini
                          // 6. The guest is served by the mute clock until
                          // then, so the first of its periods that the
                          // player gets does not wait out that start-up. The
                          // probe is consumed by the first cycle that runs;
                          // an engine that never renders is left to the
                          // guest's own timeout after a second.
                          if (probe != nil) {
                              dispatch_semaphore_t const rendered =
                                  dispatch_semaphore_create(0);
                              [player
                                  scheduleBuffer:probe
                                  completionCallbackType:
                                      AVAudioPlayerNodeCompletionDataConsumed
                                  completionHandler:^(
                                      AVAudioPlayerNodeCompletionCallbackType) {
                                    dispatch_semaphore_signal(rendered);
                                  }];
                              (void)dispatch_semaphore_wait(rendered,
                                  dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
                          }
                      }
                  }
              } @catch (NSException*) {
                  outcome = Resumption::Outcome::engineFailed;
              }
              {
                  const std::lock_guard lock(resumption->mutex);
                  resumption->outcome = outcome;
                  resumption->finished = true;
              }
              resumption->ended.notify_all();
              if (const auto flags = weakNotifications.lock())
                  flags->wake->notify();
            });
    }

    // Waits for a resumption in flight and drops its outcome: the command
    // that waits changes the stream, and the engine is not to be changed
    // under the thread that is starting it.
    void settleResumption() noexcept
    {
        if (resumption_ == nullptr)
            return;
        {
            std::unique_lock lock(resumption_->mutex);
            resumption_->ended.wait(
                lock, [this] { return resumption_->finished; });
        }
        resumption_.reset();
    }

    /* Plays again with the session taken and the engine running: hands the
     * periods the mute clock still holds to the player and ends the mute. A
     * failure of the engine itself ends the mute too, as a fault. */
    void completeResume()
    {
        bool playing = false;
        @try {
            NSError* error = nil;
            if (engine_.isRunning || [engine_ startAndReturnError:&error]) {
                measureOutputLatency();
                leaveMute();
                playing = [player_ playAndReturnError:&error];
            }
        } @catch (NSException*) {
        }
        if (playing) {
            faulted_ = false;
            lastResumed_ = Clock::now();
            if (muteLogged_) {
                const auto milliseconds =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        lastResumed_ - muteStarted_);
                std::fprintf(stderr, "Audio resumed after %lld ms\n",
                    static_cast<long long>(milliseconds.count()));
            }
            return;
        }
        abortPending();
        running_ = false;
        fault("the engine did not start after the session came back");
        endMute();
    }

    // Completes the periods that are due, finishes a resumption that has
    // ended and asks whether to take the session back; runs on the device
    // thread, on every pass and timer tick.
    void serviceMute()
    {
        if (!muted_ || !running_)
            return;
        const auto now = Clock::now();
        const auto state = state_;
        while (state->pending.load(std::memory_order_acquire) != 0 &&
               muteClock_.take(now)) {
            completeOldest(*state);
        }
        if (resumption_ != nullptr) {
            auto outcome = Resumption::Outcome::sessionRefused;
            bool finished = false;
            {
                const std::lock_guard lock(resumption_->mutex);
                finished = resumption_->finished;
                outcome = resumption_->outcome;
            }
            if (finished) {
                resumption_.reset();
                if (outcome == Resumption::Outcome::ready) {
                    completeResume();
                    return;
                }
                if (outcome == Resumption::Outcome::engineFailed) {
                    abortPending();
                    running_ = false;
                    fault("the engine did not start after the session came back");
                    endMute();
                    return;
                }
                // Another app still holds the session: stay muted and ask
                // again after the next poll interval.
                nextHintPoll_ = now + hintPollInterval;
            } else {
                nextHintPoll_ = now + hintPollInterval;
            }
        } else if (now >= nextHintPoll_) {
            nextHintPoll_ = now + hintPollInterval;
            if (!session::otherAudioPlaying())
                beginResume();
        }
        armMute();
    }

    // The period completes as played: the mute clock keeps the rate the
    // output would have, so the device has nothing left to pace.
    void completeOldest(PlaybackState& state) noexcept
    {
        std::size_t oldest = state.slotCount;
        std::uint64_t oldestTicket {};
        for (std::size_t index = 0; index < state.slotCount; ++index) {
            const auto ticket =
                state.slots[index].ticket.load(std::memory_order_acquire);
            if (ticket != 0 && (oldest == state.slotCount ||
                                   ticket < oldestTicket)) {
                oldest = index;
                oldestTicket = ticket;
            }
        }
        if (oldest == state.slotCount)
            return;
        auto expected = oldestTicket;
        if (state.slots[oldest].ticket.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel)) {
            state.finishedByMute.fetch_add(1, std::memory_order_relaxed);
            state.finish(
                state.slots[oldest].requestId, PlaybackOutcome::played, 0);
        }
    }

    // Wakes the device thread when the next period or the next poll is due.
    void armMute()
    {
        if (!muted_ || !running_) {
            cancelMuteTimer();
            return;
        }
        auto next = nextHintPoll_;
        if (state_->pending.load(std::memory_order_acquire) != 0)
            next = std::min(next, muteClock_.deadline());
        if (muteTimer_ == nil) {
            muteTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
            if (muteTimer_ == nil)
                return;
            const std::weak_ptr<NotificationFlags> weakNotifications =
                notifications_;
            dispatch_source_set_event_handler(muteTimer_, ^{
              if (const auto flags = weakNotifications.lock())
                  flags->wake->notify();
            });
            dispatch_resume(muteTimer_);
        }
        const auto delay = std::max(
            Clock::duration::zero(), next - Clock::now());
        dispatch_source_set_timer(muteTimer_,
            dispatch_time(DISPATCH_TIME_NOW,
                std::chrono::duration_cast<std::chrono::nanoseconds>(delay)
                    .count()),
            DISPATCH_TIME_FOREVER, NSEC_PER_MSEC);
    }

    void cancelMuteTimer() noexcept
    {
        if (muteTimer_ != nil) {
            dispatch_source_cancel(muteTimer_);
            muteTimer_ = nil;
        }
    }
#endif

    /* A guest that plays has its periods complete at the rate of real audio,
     * whoever completes them: the output, the mute clock, or the device's
     * pacing of the periods the sink discards. Periods arriving much faster
     * for seconds on end mean the guest's clock runs ahead of audio, which
     * nothing else reports, so the sink says so, with who completed them and
     * what the output is doing. A guest that writes a backlog stays under it:
     * a sample covers two seconds. */
    void watchPace() noexcept
    {
        if (!running_ || periodsPerSecond_ <= 0.0) {
            paceSampled_ = {};
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto state = state_;
        const std::array<std::uint64_t, 4> counts {
            arrived_,
            state->finished[static_cast<std::size_t>(PlaybackOutcome::played)]
                    .load(std::memory_order_relaxed) -
                state->finishedByMute.load(std::memory_order_relaxed),
            state->finishedByMute.load(std::memory_order_relaxed),
            state->finished[static_cast<std::size_t>(PlaybackOutcome::discarded)]
                    .load(std::memory_order_relaxed) +
                declined_,
        };
        if (paceSampled_ == std::chrono::steady_clock::time_point {}) {
            paceSampled_ = now;
            paceCounts_ = counts;
            return;
        }
        const auto span = std::chrono::duration<double>(now - paceSampled_);
        if (span < std::chrono::seconds(2))
            return;
        const double rate =
            static_cast<double>(counts[0] - paceCounts_[0]) / span.count();
        if (rate > periodsPerSecond_ * 1.25 &&
            (paceLogged_ == std::chrono::steady_clock::time_point {} ||
                now - paceLogged_ >= std::chrono::seconds(10))) {
            paceLogged_ = now;
            double hardwareRate = 0.0;
            double engineRate = 0.0;
            bool engineRunning = false;
            bool playerPlaying = false;
            @try {
                engineRate =
                    [engine_.outputNode outputFormatForBus:0].sampleRate;
                engineRunning = engine_.isRunning;
                playerPlaying = player_.isPlaying;
#if TARGET_OS_IPHONE
                hardwareRate = [AVAudioSession sharedInstance].sampleRate;
#endif
            } @catch (NSException*) {
            }
            std::fprintf(stderr,
                "Audio pace: %.0f periods/s arrive, %.0f expected (completed "
                "by the output %llu, the mute clock %llu, the device's "
                "pacing %llu in %.1f s; periods of %u frames, engine %s, "
                "player %s, hardware %.0f Hz, engine output %.0f Hz)\n",
                rate, periodsPerSecond_,
                static_cast<unsigned long long>(counts[1] - paceCounts_[1]),
                static_cast<unsigned long long>(counts[2] - paceCounts_[2]),
                static_cast<unsigned long long>(counts[3] - paceCounts_[3]),
                span.count(), periodFrames_,
                engineRunning ? "runs" : "stopped",
                playerPlaying ? "plays" : "stopped", hardwareRate, engineRate);
        }
        paceSampled_ = now;
        paceCounts_ = counts;
    }

    /* A period completes once the player has consumed it, before it is
     * heard: the output reads it a cycle, and on macOS a safety offset,
     * ahead of the hardware, which then takes its own latency. A played
     * completion reports all of it (PlaybackCompletion). Each route has its
     * own, so the device thread measures it whenever the engine starts,
     * before the player gets a period. */
    void measureOutputLatency() noexcept
    {
        double seconds = 0.0;
        @try {
            seconds = outputLatency();
        } @catch (NSException*) {
        }
        // The bound keeps a wrong value from overflowing the status; even a
        // wireless output stays well inside it.
        seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 5.0) : 0.0;
        const auto frames =
            static_cast<std::uint32_t>(std::lround(seconds * format_.sampleRate));
        state_->outputLatencyBytes.store(
            frames * format_.streamDescription->mBytesPerFrame,
            std::memory_order_relaxed);
    }

#if TARGET_OS_IPHONE
    // The session's output latency and IO cycle, which together are the
    // output's latency on iOS.
    [[nodiscard]] double outputLatency() const
    {
        AVAudioSession* const session = [AVAudioSession sharedInstance];
        return session.outputLatency + session.IOBufferDuration;
    }
#else
    // The device's and its stream's latency, which the engine reports, and
    // the cycle and safety offset by which the device reads ahead of them,
    // which it counts in its own frames.
    [[nodiscard]] double outputLatency() const
    {
        const double presentation = engine_.outputNode.presentationLatency;
        AudioUnit unit = engine_.outputNode.audioUnit;
        const double rate = [engine_.outputNode outputFormatForBus:0].sampleRate;
        AudioDeviceID device = kAudioObjectUnknown;
        UInt32 size = sizeof(device);
        if (unit == nullptr || rate <= 0.0 ||
            AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                kAudioUnitScope_Global, 0, &device, &size) != noErr)
            return presentation + outputCycle_;
        UInt32 cycle = 0;
        size = sizeof(cycle);
        if (AudioUnitGetProperty(unit, kAudioDevicePropertyBufferFrameSize,
                kAudioUnitScope_Global, 0, &cycle, &size) != noErr)
            cycle = static_cast<UInt32>(std::lround(outputCycle_ * rate));
        const AudioObjectPropertyAddress address {
            kAudioDevicePropertySafetyOffset,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain,
        };
        UInt32 safety = 0;
        size = sizeof(safety);
        if (AudioObjectGetPropertyData(
                device, &address, 0, nullptr, &size, &safety) != noErr)
            safety = 0;
        return presentation + (cycle + safety) / rate;
    }
#endif

#if !TARGET_OS_IPHONE
    /* Asks the output device for the stream's IO cycle (OutputCycle), which
     * the device counts in its own frames and only within the range it
     * supports. Core Audio keeps the cycle for this process alone. A device
     * that refuses keeps its own cycle, which only lengthens the latency. */
    void applyOutputCycle() noexcept
    {
        @try {
            AudioUnit unit = engine_.outputNode.audioUnit;
            const double rate = [engine_.outputNode outputFormatForBus:0].sampleRate;
            if (unit == nullptr || outputCycle_ <= 0.0 || rate <= 0.0)
                return;
            AudioDeviceID device = kAudioObjectUnknown;
            UInt32 size = sizeof(device);
            if (AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                    kAudioUnitScope_Global, 0, &device, &size) != noErr)
                return;
            const AudioObjectPropertyAddress address {
                kAudioDevicePropertyBufferFrameSizeRange,
                kAudioObjectPropertyScopeOutput,
                kAudioObjectPropertyElementMain,
            };
            AudioValueRange range {};
            size = sizeof(range);
            if (AudioObjectGetPropertyData(
                    device, &address, 0, nullptr, &size, &range) != noErr ||
                range.mMaximum < range.mMinimum || range.mMinimum < 1.0)
                return;
            const auto frames = static_cast<UInt32>(std::clamp(
                std::lround(outputCycle_ * rate),
                std::lround(range.mMinimum), std::lround(range.mMaximum)));
            (void)AudioUnitSetProperty(unit, kAudioDevicePropertyBufferFrameSize,
                kAudioUnitScope_Global, 0, &frames, sizeof(frames));
        } @catch (NSException*) {
        }
    }
#endif

    void failToStart() noexcept
    {
        // Periods queued before the start will never play.
        abortPending();
        fault("the output would not start");
    }

    /* The output cannot play: the sink discards what it is given and the
     * device paces the guest on its own clock, so the guest hears nothing and
     * notices nothing. One line says so, and a sink that faults again and
     * again says it once in ten seconds. */
    void fault(const char* reason) noexcept
    {
        faulted_ = true;
        const auto now = std::chrono::steady_clock::now();
        if (faultLogged_ != std::chrono::steady_clock::time_point {} &&
            now - faultLogged_ < std::chrono::seconds(10))
            return;
        faultLogged_ = now;
        std::fprintf(stderr,
            "Audio output unavailable (%s): periods are discarded and the "
            "device paces the guest\n", reason);
    }

    /* Hands back every queued period unplayed. The device completes them
     * on its period clock rather than at once. */
    void abortPending() noexcept
    {
        auto state = state_;
        for (std::size_t index = 0; index < state->slotCount; ++index) {
            const auto ticket =
                state->slots[index].ticket.exchange(0, std::memory_order_acq_rel);
            if (ticket != 0)
                state->finish(state->slots[index].requestId,
                              PlaybackOutcome::discarded, 0);
        }
    }

    /* The audio session is shared with the microphone, which asks for a
     * different category while the guest records (see audio_session.hpp). It
     * is static so that the thread taking the session back needs no sink. */
    [[nodiscard]] static bool activateSession() noexcept
    {
        return session::acquire(session::Client::playback);
    }

    void deactivateSession() noexcept
    {
        session::release(session::Client::playback);
    }

    std::shared_ptr<const Waker> wake_;
    std::shared_ptr<NotificationFlags> notifications_;
    std::shared_ptr<PlaybackState> state_;
    AVAudioEngine* __strong engine_;
    AVAudioPlayerNode* __strong player_;
    AVAudioFormat* __strong format_;
    id __strong configurationObserver_;
#if TARGET_OS_IPHONE
    id __strong routeObserver_;
    id __strong deactivationObserver_;
    id __strong resumptionObserver_;
    // The system has the session, and no end has come yet.
    bool interrupted_ {};
    // The stream runs but plays nothing: the mute clock completes its periods.
    bool muted_ {};
    MuteClock muteClock_;
    // The session and engine being taken back on another thread, if any.
    std::shared_ptr<Resumption> resumption_;
    Clock::time_point muteStarted_ {};
    Clock::time_point nextHintPoll_ {};
    Clock::time_point lastResumed_ {};
    bool muteLogged_ {};
    dispatch_source_t __strong muteTimer_ {};
#endif
    bool configured_ {};
    bool prepared_ {};
    bool running_ {};
    bool faulted_ {};
    std::chrono::steady_clock::time_point faultLogged_ {};
    // The completion counts the pace check last looked at, and when.
    std::chrono::steady_clock::time_point paceSampled_ {};
    std::chrono::steady_clock::time_point paceLogged_ {};
    std::array<std::uint64_t, 4> paceCounts_ {};
    double periodsPerSecond_ {};
    // The IO cycle the stream asks of the output, in seconds.
    double outputCycle_ {};
    std::uint32_t periodFrames_ {};
    // Periods the guest has handed in, and how many of them the sink refused
    // to play because the output is unavailable: device thread only.
    std::uint64_t arrived_ {};
    std::uint64_t declined_ {};
};

} // namespace

std::unique_ptr<PlaybackSink> makeApplePlaybackSink(
    std::shared_ptr<const Waker> waker)
{
    @try {
        return std::make_unique<ApplePlaybackSink>(std::move(waker));
    } @catch (NSException* exception) {
        const char* reason = exception.reason.UTF8String;
        throw std::runtime_error(
            "initializing Apple audio failed: " +
            std::string(reason == nullptr ? "unknown AVFAudio exception" : reason));
    }
}

} // namespace uml::audio
