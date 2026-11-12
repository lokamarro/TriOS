// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_sound_device.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace uml::audio {
namespace {

using uml::virtio::fromBytes;
using uml::virtio::toBytes;

[[nodiscard]] uml::virtio::VhostUserQueueResult complete(
    std::vector<std::uint8_t> response)
{
    return {
        .action = uml::virtio::VhostUserQueueAction::complete,
        .response = std::move(response),
    };
}

[[nodiscard]] uml::virtio::VhostUserQueueResult status(
    protocol::Status value,
    std::size_t responseCapacity)
{
    if (responseCapacity < sizeof(protocol::Header))
        return complete({});
    return complete(toBytes(protocol::Header {
        .code = static_cast<std::uint32_t>(value),
    }));
}

[[nodiscard]] bool validStreamHeader(
    const protocol::PcmHeader& header) noexcept
{
    return header.streamId == protocol::streamId;
}

/* The parameters a stream of `minimumChannels` to `maximumChannels` accepts:
 * 48 kHz signed 16-bit frames, whole periods in a buffer of at most 256. */
[[nodiscard]] bool supportedParameters(
    const protocol::PcmSetParameters& parameters,
    std::uint8_t minimumChannels,
    std::uint8_t maximumChannels) noexcept
{
    return parameters.bufferBytes != 0 && parameters.periodBytes != 0 &&
        parameters.bufferBytes <= protocol::maximumBufferBytes &&
        parameters.bufferBytes % parameters.periodBytes == 0 &&
        parameters.bufferBytes / parameters.periodBytes <=
            protocol::maximumPeriods &&
        parameters.channels >= minimumChannels &&
        parameters.channels <= maximumChannels &&
        parameters.periodBytes %
                (parameters.channels * protocol::bytesPerSample) == 0 &&
        parameters.features == 0 &&
        parameters.format == protocol::PcmFormat::signed16 &&
        parameters.rate == protocol::PcmRate::rate48000 &&
        parameters.padding == 0;
}

/* A request for `count` consecutive entries of `entrySize` bytes each,
 * all of which exist. */
[[nodiscard]] bool validQuery(
    const protocol::QueryInfo& query,
    std::size_t entrySize) noexcept
{
    return query.count != 0 && query.startId < protocol::streamCount &&
        query.count <= protocol::streamCount - query.startId &&
        query.size == entrySize;
}

// The shortest wait the clock is asked for. A guest that chooses periods
// shorter than this is served in batches rather than woken for each one.
constexpr auto minimumWake = std::chrono::microseconds(250);

} // namespace

VirtioSoundDevice::VirtioSoundDevice(
    std::unique_ptr<PlaybackSink> sink,
    std::unique_ptr<CaptureSource> capture,
    std::shared_ptr<PeriodClock> clock)
    : sink_(std::move(sink))
    , capture_(std::move(capture))
    , clock_(std::move(clock))
{
    if (!sink_)
        throw std::invalid_argument("Virtio Sound requires a playback sink");
    if (!capture_)
        throw std::invalid_argument("Virtio Sound requires a capture source");
    if (!clock_)
        throw std::invalid_argument("Virtio Sound requires a period clock");
}

VirtioSoundDevice::~VirtioSoundDevice() = default;

const char* VirtioSoundDevice::name() const noexcept
{
    return "Virtio Sound";
}

std::uint64_t VirtioSoundDevice::features() const noexcept
{
    return 0;
}

std::size_t VirtioSoundDevice::queueCount() const noexcept
{
    return protocol::queueCount;
}

std::vector<std::uint8_t> VirtioSoundDevice::configuration() const
{
    const protocol::Configuration configuration {
        .jacks = 0,
        .streams = protocol::streamCount,
        .channelMaps = protocol::streamCount,
        .controls = 0,
    };
    return toBytes(configuration);
}

void VirtioSoundDevice::writeConfiguration(
    std::uint32_t,
    std::uint32_t,
    std::span<const std::uint8_t> bytes)
{
    if (!bytes.empty())
        throw std::runtime_error("Virtio Sound configuration is read-only");
}

uml::virtio::VhostUserQueueResult VirtioSoundDevice::processControl(
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    if (request.size() < sizeof(protocol::Header))
        return status(protocol::Status::badMessage, responseCapacity);
    const auto header = fromBytes<protocol::Header>(request);
    switch (static_cast<protocol::Request>(header.code)) {
    case protocol::Request::pcmInfo: {
        if (request.size() != sizeof(protocol::QueryInfo))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto query = fromBytes<protocol::QueryInfo>(request);
        if (!validQuery(query, sizeof(protocol::PcmInfo)))
            return status(protocol::Status::badMessage, responseCapacity);
        auto response = toBytes(protocol::Header {
            .code = static_cast<std::uint32_t>(protocol::Status::ok),
        });
        for (std::uint32_t id = query.startId;
             id < query.startId + query.count; ++id) {
            const bool input = id == protocol::inputStreamId;
            const auto info = toBytes(protocol::PcmInfo {
                .header = {.functionGroupNodeId = 0},
                .features = 0,
                .formats = 1ULL << static_cast<std::uint8_t>(
                    protocol::PcmFormat::signed16),
                .rates = 1ULL << static_cast<std::uint8_t>(
                    protocol::PcmRate::rate48000),
                .direction = input ? protocol::Direction::input
                                   : protocol::Direction::output,
                .minimumChannels =
                    input ? std::uint8_t {1} : protocol::channelCount,
                .maximumChannels = protocol::channelCount,
                .padding = {},
            });
            response.insert(response.end(), info.begin(), info.end());
        }
        if (responseCapacity < response.size())
            return status(protocol::Status::badMessage, responseCapacity);
        return complete(std::move(response));
    }
    case protocol::Request::channelMapInfo: {
        if (request.size() != sizeof(protocol::QueryInfo))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto query = fromBytes<protocol::QueryInfo>(request);
        if (!validQuery(query, sizeof(protocol::ChannelMapInfo)))
            return status(protocol::Status::badMessage, responseCapacity);
        auto response = toBytes(protocol::Header {
            .code = static_cast<std::uint32_t>(protocol::Status::ok),
        });
        for (std::uint32_t id = query.startId;
             id < query.startId + query.count; ++id) {
            protocol::ChannelMapInfo info {
                .header = {.functionGroupNodeId = 0},
                .direction = id == protocol::inputChannelMapId
                    ? protocol::Direction::input
                    : protocol::Direction::output,
                .channels = protocol::channelCount,
                .positions = {},
            };
            info.positions[0] = static_cast<std::uint8_t>(
                protocol::ChannelPosition::frontLeft);
            info.positions[1] = static_cast<std::uint8_t>(
                protocol::ChannelPosition::frontRight);
            const auto bytes = toBytes(info);
            response.insert(response.end(), bytes.begin(), bytes.end());
        }
        if (responseCapacity < response.size())
            return status(protocol::Status::badMessage, responseCapacity);
        return complete(std::move(response));
    }
    case protocol::Request::pcmSetParameters: {
        if (request.size() != sizeof(protocol::PcmSetParameters))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto parameters = fromBytes<protocol::PcmSetParameters>(request);
        if (parameters.header.streamId == protocol::inputStreamId) {
            return processCaptureControl(
                header.code, request, responseCapacity);
        }
        if (!validStreamHeader(parameters.header))
            return status(protocol::Status::badMessage, responseCapacity);
        if (deferredTransfers_ != 0)
            return status(protocol::Status::ioError, responseCapacity);
        if (state_ == StreamState::running ||
            state_ == StreamState::stopped ||
            !supportedParameters(
                parameters, protocol::channelCount, protocol::channelCount)) {
            return status(protocol::Status::notSupported, responseCapacity);
        }
        PlaybackParameters selected {
            .bufferBytes = parameters.bufferBytes,
            .periodBytes = parameters.periodBytes,
            .sampleRate = protocol::sampleRate,
            .channels = protocol::channelCount,
            .bytesPerSample = protocol::bytesPerSample,
        };
        if (!sink_->configure(selected))
            return status(protocol::Status::ioError, responseCapacity);
        parameters_ = selected;
        state_ = StreamState::configured;
        return status(protocol::Status::ok, responseCapacity);
    }
    case protocol::Request::pcmPrepare:
    case protocol::Request::pcmRelease:
    case protocol::Request::pcmStart:
    case protocol::Request::pcmStop: {
        if (request.size() != sizeof(protocol::PcmHeader))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto pcm = fromBytes<protocol::PcmHeader>(request);
        if (pcm.streamId == protocol::inputStreamId) {
            return processCaptureControl(
                header.code, request, responseCapacity);
        }
        if (!validStreamHeader(pcm))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto command = static_cast<protocol::Request>(header.code);
        if (command == protocol::Request::pcmPrepare) {
            if ((state_ != StreamState::configured &&
                 state_ != StreamState::prepared &&
                 state_ != StreamState::released) ||
                !sink_->prepare()) {
                return status(protocol::Status::ioError, responseCapacity);
            }
            state_ = StreamState::prepared;
        } else if (command == protocol::Request::pcmStart) {
            if ((state_ != StreamState::prepared &&
                 state_ != StreamState::stopped) ||
                !sink_->start()) {
                return status(protocol::Status::ioError, responseCapacity);
            }
            state_ = StreamState::running;
            timePacedTransfers();
        } else if (command == protocol::Request::pcmStop) {
            if (state_ != StreamState::running)
                return status(protocol::Status::ioError, responseCapacity);
            sink_->stop();
            state_ = StreamState::stopped;
            for (auto& transfer : paced_)
                transfer.timed = false;
        } else {
            if (state_ != StreamState::prepared &&
                state_ != StreamState::stopped) {
                return status(protocol::Status::ioError, responseCapacity);
            }
            sink_->release();
            state_ = StreamState::released;
            // A release returns every queued period at once.
            for (const auto& transfer : paced_)
                ready_.push_back(transfer.requestId);
            paced_.clear();
        }
        rearmClock();
        return status(protocol::Status::ok, responseCapacity);
    }
    default:
        return status(protocol::Status::notSupported, responseCapacity);
    }
}

uml::virtio::VhostUserQueueResult VirtioSoundDevice::processTransmit(
    std::uint64_t requestId,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    if (responseCapacity < sizeof(protocol::PcmStatus))
        return complete({});
    if (request.size() < sizeof(protocol::PcmTransfer))
        return complete(toBytes(protocol::PcmStatus {
            .status = protocol::Status::badMessage,
            .latencyBytes = 0,
        }));
    const auto transfer = fromBytes<protocol::PcmTransfer>(request);
    const auto pcm = request.subspan(sizeof(transfer));
    if (transfer.streamId != protocol::streamId ||
        (state_ != StreamState::prepared &&
         state_ != StreamState::running) ||
        pcm.size() != parameters_.periodBytes) {
        return complete(toBytes(protocol::PcmStatus {
            .status = protocol::Status::badMessage,
            .latencyBytes = 0,
        }));
    }
    const auto enqueueResult = sink_->enqueue(requestId, pcm);
    if (enqueueResult == PlaybackEnqueueResult::retry)
        return {};
    if (enqueueResult == PlaybackEnqueueResult::failed) {
        return complete(toBytes(protocol::PcmStatus {
            .status = protocol::Status::ioError,
            .latencyBytes = 0,
        }));
    }
    ++deferredTransfers_;
    if (enqueueResult == PlaybackEnqueueResult::discarded)
        pace(requestId);
    return {
        .action = uml::virtio::VhostUserQueueAction::defer,
        .response = {},
    };
}

uml::virtio::VhostUserQueueResult VirtioSoundDevice::processQueue(
    std::uint32_t index,
    std::uint64_t requestId,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    switch (index) {
    case protocol::controlQueue:
        return processControl(request, responseCapacity);
    case protocol::eventQueue:
        return {};
    case protocol::transmitQueue:
        return processTransmit(requestId, request, responseCapacity);
    case protocol::receiveQueue:
        return processReceive(request, responseCapacity);
    default:
        throw std::runtime_error("invalid Virtio Sound queue index");
    }
}

void VirtioSoundDevice::reset() noexcept
{
    sink_->reset();
    capture_->reset();
    captureState_ = StreamState::unconfigured;
    captureParameters_ = {};
    captureBufferBytes_ = 0;
    captureCredit_ = 0;
    clock_->cancel();
    armed_.reset();
    state_ = StreamState::unconfigured;
    parameters_ = {};
    deferredTransfers_ = 0;
    paced_.clear();
    ready_.clear();
}

int VirtioSoundDevice::eventDescriptor() const noexcept
{
    return clock_->descriptor();
}

void VirtioSoundDevice::drainEventSource()
{
    clock_->drain();
    const auto now = clock_->now();
    // A deadline that has passed has fired; one still ahead is still armed,
    // and this wake was the sink's.
    if (armed_ && *armed_ <= now)
        armed_.reset();
    sink_->handleEvents();
    capture_->handleEvents();
    releasePacedTransfers(now);
    advanceCapture(now);
    rearmClock();
}

bool VirtioSoundDevice::takeQueueCompletion(
    uml::virtio::VhostUserQueueCompletion& completion)
{
    releasePacedTransfers(clock_->now());
    for (;;) {
        std::uint32_t latencyBytes {};
        auto outcome = PlaybackOutcome::played;
        std::uint64_t requestId {};
        if (!ready_.empty()) {
            requestId = ready_.front();
            ready_.pop_front();
        } else {
            PlaybackCompletion playback;
            if (!sink_->takeCompletion(playback)) {
                rearmClock();
                return false;
            }
            if (playback.outcome == PlaybackOutcome::discarded &&
                state_ == StreamState::running) {
                // The output dropped the period; the guest still waits for
                // a period of real time to pass.
                pace(playback.requestId);
                continue;
            }
            requestId = playback.requestId;
            latencyBytes = playback.latencyBytes;
            outcome = playback.outcome;
        }
        if (deferredTransfers_ != 0)
            --deferredTransfers_;
        completion = {
            .requestId = requestId,
            .response = toBytes(protocol::PcmStatus {
                .status = outcome == PlaybackOutcome::failed
                    ? protocol::Status::ioError
                    : protocol::Status::ok,
                .latencyBytes = latencyBytes,
            }),
        };
        return true;
    }
}

std::chrono::nanoseconds VirtioSoundDevice::playbackPeriod() const noexcept
{
    const std::int64_t frames =
        parameters_.periodBytes / protocol::frameBytes;
    return std::chrono::nanoseconds(
        frames * 1'000'000'000LL / protocol::sampleRate);
}

void VirtioSoundDevice::pace(std::uint64_t requestId)
{
    PacedTransfer transfer {.requestId = requestId};
    if (state_ == StreamState::running) {
        playHead_ = std::max(playHead_, clock_->now()) + playbackPeriod();
        transfer.due = playHead_;
        transfer.timed = true;
    }
    paced_.push_back(transfer);
    rearmClock();
}

/* A stream that starts plays what it has queued from now on, one period
 * after another. */
void VirtioSoundDevice::timePacedTransfers()
{
    playHead_ = clock_->now();
    for (auto& transfer : paced_) {
        playHead_ += playbackPeriod();
        transfer.due = playHead_;
        transfer.timed = true;
    }
}

void VirtioSoundDevice::releasePacedTransfers(ClockTime now)
{
    while (!paced_.empty() && paced_.front().timed &&
           paced_.front().due <= now) {
        ready_.push_back(paced_.front().requestId);
        paced_.pop_front();
    }
}

std::chrono::nanoseconds VirtioSoundDevice::capturePeriod() const noexcept
{
    const std::int64_t frames = captureParameters_.periodBytes /
        (captureParameters_.channels * captureParameters_.bytesPerSample);
    return std::chrono::nanoseconds(
        frames * 1'000'000'000LL / captureParameters_.sampleRate);
}

/* Credits the periods of real time that have passed since the last tick. */
void VirtioSoundDevice::advanceCapture(ClockTime now)
{
    if (captureState_ != StreamState::running || now < captureNextTick_)
        return;
    const auto period = capturePeriod();
    const auto elapsed = (now - captureNextTick_) / period + 1;
    captureNextTick_ += elapsed * period;
    const auto credit = static_cast<std::uint64_t>(captureCredit_) +
        static_cast<std::uint64_t>(elapsed) * captureParameters_.periodBytes;
    captureCredit_ = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(credit, captureBufferBytes_));
}

void VirtioSoundDevice::rearmClock()
{
    std::optional<ClockTime> next;
    if (!paced_.empty() && paced_.front().timed)
        next = paced_.front().due;
    if (captureState_ == StreamState::running &&
        (!next || captureNextTick_ < *next)) {
        next = captureNextTick_;
    }
    if (next == armed_)
        return;
    armed_ = next;
    if (next)
        clock_->wakeAt(std::max(*next, clock_->now() + minimumWake));
    else
        clock_->cancel();
}

uml::virtio::VhostUserQueueResult VirtioSoundDevice::processCaptureControl(
    std::uint32_t code,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    switch (static_cast<protocol::Request>(code)) {
    case protocol::Request::pcmSetParameters: {
        const auto parameters = fromBytes<protocol::PcmSetParameters>(request);
        if (captureState_ == StreamState::running ||
            captureState_ == StreamState::stopped ||
            !supportedParameters(parameters, 1, protocol::channelCount)) {
            return status(protocol::Status::notSupported, responseCapacity);
        }
        captureParameters_ = {
            .periodBytes = parameters.periodBytes,
            .sampleRate = protocol::sampleRate,
            .channels = parameters.channels,
            .bytesPerSample = protocol::bytesPerSample,
        };
        captureBufferBytes_ = parameters.bufferBytes;
        capture_->configure(captureParameters_);
        captureState_ = StreamState::configured;
        return status(protocol::Status::ok, responseCapacity);
    }
    case protocol::Request::pcmPrepare:
        if (captureState_ != StreamState::configured &&
            captureState_ != StreamState::prepared &&
            captureState_ != StreamState::released) {
            return status(protocol::Status::ioError, responseCapacity);
        }
        capture_->prepare();
        captureCredit_ = 0;
        captureState_ = StreamState::prepared;
        return status(protocol::Status::ok, responseCapacity);
    case protocol::Request::pcmStart:
        if (captureState_ != StreamState::prepared &&
            captureState_ != StreamState::stopped) {
            return status(protocol::Status::ioError, responseCapacity);
        }
        capture_->start();
        captureState_ = StreamState::running;
        captureCredit_ = 0;
        captureNextTick_ = clock_->now() + capturePeriod();
        break;
    case protocol::Request::pcmStop:
        if (captureState_ != StreamState::running)
            return status(protocol::Status::ioError, responseCapacity);
        capture_->stop();
        captureState_ = StreamState::stopped;
        captureCredit_ = 0;
        // The buffers still queued come back empty on the next pass.
        clock_->notify();
        break;
    case protocol::Request::pcmRelease:
        if (captureState_ != StreamState::prepared &&
            captureState_ != StreamState::stopped) {
            return status(protocol::Status::ioError, responseCapacity);
        }
        capture_->release();
        captureState_ = StreamState::released;
        captureCredit_ = 0;
        clock_->notify();
        break;
    default:
        return status(protocol::Status::notSupported, responseCapacity);
    }
    rearmClock();
    return status(protocol::Status::ok, responseCapacity);
}

/* A receive buffer is the data the guest asks for followed by a status. */
uml::virtio::VhostUserQueueResult VirtioSoundDevice::processReceive(
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    if (responseCapacity < sizeof(protocol::PcmStatus))
        return complete({});
    const auto reply = [&](protocol::Status value,
                           std::vector<std::uint8_t> pcm = {}) {
        const auto tail = toBytes(protocol::PcmStatus {
            .status = value,
            .latencyBytes = 0,
        });
        pcm.insert(pcm.end(), tail.begin(), tail.end());
        return complete(std::move(pcm));
    };
    if (request.size() < sizeof(protocol::PcmTransfer))
        return reply(protocol::Status::badMessage);
    const auto transfer = fromBytes<protocol::PcmTransfer>(request);
    if (transfer.streamId != protocol::inputStreamId)
        return reply(protocol::Status::badMessage);
    switch (captureState_) {
    case StreamState::prepared:
        // The driver queues its buffers before it starts the stream.
        return {};
    case StreamState::running:
        break;
    case StreamState::stopped:
    case StreamState::released:
        // A stream that is not recording returns its buffers without data.
        return reply(protocol::Status::ok);
    default:
        return reply(protocol::Status::badMessage);
    }
    const std::size_t pcmBytes = responseCapacity - sizeof(protocol::PcmStatus);
    if (pcmBytes != captureParameters_.periodBytes)
        return reply(protocol::Status::badMessage);
    advanceCapture(clock_->now());
    if (captureCredit_ < pcmBytes) {
        // The next period of real time has not passed yet; the clock wakes
        // the transport when it has.
        rearmClock();
        return {};
    }
    captureCredit_ -= captureParameters_.periodBytes;
    std::vector<std::uint8_t> pcm(pcmBytes);
    const auto filled = std::min(
        capture_->read(pcm), pcm.size());
    // What the source did not supply is silence.
    std::fill(pcm.begin() + static_cast<std::ptrdiff_t>(filled), pcm.end(), 0);
    return reply(protocol::Status::ok, std::move(pcm));
}

} // namespace uml::audio
