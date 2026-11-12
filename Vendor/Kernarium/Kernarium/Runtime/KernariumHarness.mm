// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumHarness.hpp"

#include "KernariumPayloadFile.hpp"
#include "console/in_process_service.hpp"
#include "console/protocol.hpp"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace kernarium::harness {

namespace {

std::filesystem::path documentsDirectory()
{
    NSArray<NSString*>* directories = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    NSString* directory = directories.firstObject;
    if (directory == nil)
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD: no Documents directory");
    return std::filesystem::path {directory.fileSystemRepresentation};
}

bool validSystemdTarget(std::string_view unit) noexcept
{
    constexpr std::string_view suffix {".target"};
    if (unit.size() <= suffix.size() || unit.size() > 128U ||
        !unit.ends_with(suffix))
        return false;
    return std::ranges::all_of(unit, [](char character) {
        return (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.' ||
            character == '@' || character == ':';
    });
}

} // namespace

/* Relays the artifact frames a guest campaign writes to its virtio-serial
 * port to standard output, where the host reads the console, and
 * acknowledges each one. */
class ArtifactRelay final {
public:
    explicit ArtifactRelay(std::mutex& stdoutMutex)
        : stdoutMutex_(stdoutMutex)
    {
        int descriptors[2] {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
            throw std::runtime_error(
                "artifact relay socketpair failed: " +
                std::string(std::strerror(errno)));
        }
        try {
            service_ = uml::console::makeInProcessService(
                descriptors[0],
                std::string {
                    uml::console::protocol::hostSerialPortName});
            ::close(descriptors[0]);
            descriptors[0] = -1;
            host_ = descriptors[1];
            descriptors[1] = -1;
            relay_ = std::thread([this] { relay(); });
        } catch (...) {
            if (descriptors[0] >= 0)
                ::close(descriptors[0]);
            if (descriptors[1] >= 0)
                ::close(descriptors[1]);
            if (host_ >= 0) {
                ::close(host_);
                host_ = -1;
            }
            if (service_)
                service_->stop();
            throw;
        }
    }

    ~ArtifactRelay()
    {
        /* Stopping the service joins its thread, which destroys the console
         * device and with it the channel that holds the only other end of the
         * socketpair. That close ends the relay's stream, so the reader needs
         * no wakeup of its own. */
        if (service_)
            service_->stop();
        if (relay_.joinable())
            relay_.join();
        if (host_ >= 0)
            ::close(host_);
    }

    ArtifactRelay(const ArtifactRelay&) = delete;
    ArtifactRelay& operator=(const ArtifactRelay&) = delete;

    [[nodiscard]] std::string takeConnectionName()
    {
        return service_->takeConnectionName();
    }

private:
    /* One frame is one base64 line plus its short header. The GL driver
     * encodes 1 KiB lines and the Vulkan driver 3 KiB ones, so a bound below
     * the larger of the two rejects every Vulkan artifact past the first
     * single-chunk file. */
    static constexpr std::size_t maximumFrameBytes = 4096U;

    [[nodiscard]] static bool decimal(std::string_view text) noexcept
    {
        return !text.empty() && std::ranges::all_of(
            text, [](char character) {
                return character >= '0' && character <= '9';
            });
    }

    [[nodiscard]] static bool lowercaseHex(
        std::string_view text,
        std::size_t expectedSize) noexcept
    {
        return text.size() == expectedSize && std::ranges::all_of(
            text, [](char character) {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            });
    }

    [[nodiscard]] static bool base64(std::string_view text) noexcept
    {
        /* One chunk is one encoded line: the GL driver writes 1 KiB lines and
         * the Vulkan driver 3 KiB ones. */
        return !text.empty() && text.size() <= 3072U &&
            std::ranges::all_of(text, [](char character) {
                return (character >= 'A' && character <= 'Z') ||
                    (character >= 'a' && character <= 'z') ||
                    (character >= '0' && character <= '9') ||
                    character == '+' || character == '/' || character == '=';
            });
    }

    [[nodiscard]] static bool allowedArtifact(
        std::string_view name) noexcept
    {
        for (const std::string_view allowed : {
                 "renderer.txt",
                 "selected.txt",
                 "expected.txt",
                 /* A campaign whose device cannot hold every enumerated case
                  * reports which ones it left out, and under which reason. */
                 "skipped.txt",
                 "results.qpa",
                 "summary.txt"}) {
            if (name == allowed)
                return true;
        }
        /* A campaign that checkpoints its log ships it as fixed-width
         * numbered segments, which the host reassembles in order. */
        constexpr std::string_view segmentPrefix = "results.qpa.";
        constexpr std::size_t segmentDigits = 4U;
        if (name.size() != segmentPrefix.size() + segmentDigits ||
            name.substr(0, segmentPrefix.size()) != segmentPrefix) {
            return false;
        }
        return decimal(name.substr(segmentPrefix.size()));
    }

    [[nodiscard]] static std::vector<std::string_view> split(
        std::string_view frame)
    {
        std::vector<std::string_view> fields;
        while (true) {
            const auto separator = frame.find('|');
            fields.push_back(frame.substr(0, separator));
            if (separator == std::string_view::npos)
                break;
            frame.remove_prefix(separator + 1U);
        }
        return fields;
    }

    [[nodiscard]] static std::string acknowledgement(
        std::string_view frame)
    {
        const auto fields = split(frame);
        if (fields.size() == 5U &&
            fields[0] == "UML_CTS_ARTIFACT_BEGIN" &&
            allowedArtifact(fields[1]) &&
            fields[2] == "gzip+base64" && decimal(fields[3]) &&
            lowercaseHex(fields[4], 64U)) {
            return "UML_CTS_ARTIFACT_ACK|" + std::string {fields[1]} +
                "|begin\n";
        }
        if (fields.size() == 4U &&
            fields[0] == "UML_CTS_ARTIFACT_CHUNK" &&
            allowedArtifact(fields[1]) && decimal(fields[2]) &&
            base64(fields[3])) {
            return "UML_CTS_ARTIFACT_ACK|" + std::string {fields[1]} +
                "|" + std::string {fields[2]} + "\n";
        }
        if (fields.size() == 3U &&
            fields[0] == "UML_CTS_ARTIFACT_END" &&
            allowedArtifact(fields[1]) && decimal(fields[2])) {
            return "UML_CTS_ARTIFACT_ACK|" + std::string {fields[1]} +
                "|end\n";
        }
        return {};
    }

    [[nodiscard]] static bool writeAll(
        int descriptor,
        std::string_view bytes) noexcept
    {
        while (!bytes.empty()) {
            const ssize_t written = ::write(
                descriptor, bytes.data(), bytes.size());
            if (written > 0) {
                bytes.remove_prefix(static_cast<std::size_t>(written));
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    }

    void report(std::string_view record) noexcept
    {
        std::lock_guard outputLock(stdoutMutex_);
        (void)writeAll(STDOUT_FILENO, record);
    }

    void relay() noexcept
    {
        (void)::pthread_setname_np("uml-cts-artifacts");
        std::string pending;
        std::size_t frames = 0;
        std::size_t acknowledgements = 0;
        bool valid = true;
        char buffer[4096];
        for (;;) {
            /* Never sleep in recv(): a local stream socket can lose the wakeup
             * of a blocked reader (docs/architecture.md, "Local stream
             * sockets"). Read what is there, and poll when nothing is. */
            const ssize_t received =
                ::recv(host_, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (received > 0) {
                pending.append(buffer, static_cast<std::size_t>(received));
                while (true) {
                    const auto newline = pending.find('\n');
                    if (newline == std::string::npos)
                        break;
                    std::string frame = pending.substr(0, newline);
                    pending.erase(0, newline + 1U);
                    if (!frame.empty() && frame.back() == '\r')
                        frame.pop_back();
                    const std::string ack = acknowledgement(frame);
                    if (frame.size() > maximumFrameBytes || ack.empty()) {
                        valid = false;
                        break;
                    }
                    report(frame + "\n");
                    ++frames;
                    if (!writeAll(host_, ack)) {
                        valid = false;
                        break;
                    }
                    ++acknowledgements;
                }
                if (!valid || pending.size() > maximumFrameBytes)
                    break;
                continue;
            }
            if (received < 0 && errno == EINTR)
                continue;
            if (received < 0 && errno == EAGAIN) {
                pollfd waiting {host_, POLLIN, 0};
                if (::poll(&waiting, 1, -1) < 0 && errno != EINTR) {
                    valid = false;
                    break;
                }
                continue;
            }
            if (received < 0 && errno != ECONNRESET)
                valid = false;
            break;
        }
        if (!pending.empty())
            valid = false;
        if (!valid)
            report("UML_VK_CTS_ERROR: invalid artifact relay frame\n");
        report(
            "UML_VK_CTS_RELAY|frames=" + std::to_string(frames) +
            "|acks=" + std::to_string(acknowledgements) +
            "|valid=" + std::to_string(valid ? 1U : 0U) + "\n");
    }

    std::mutex& stdoutMutex_;
    int host_ {-1};
    std::unique_ptr<uml::gpu::InProcessVhostUserService> service_;
    std::thread relay_;
};

AutomationBoot::AutomationBoot() = default;

AutomationBoot::~AutomationBoot() = default;

std::unique_ptr<AutomationBoot> AutomationBoot::fromEnvironment(
    std::mutex& consoleStdoutMutex)
{
    const char* payload = std::getenv("KERNARIUM_BOOT_PAYLOAD");
    const char* unit = std::getenv("KERNARIUM_SYSTEMD_UNIT");
    const bool hasPayload = payload != nullptr && payload[0] != '\0';
    const bool hasUnit = unit != nullptr && unit[0] != '\0';
    if (!hasPayload && !hasUnit)
        return nullptr;
    std::unique_ptr<AutomationBoot> boot {new AutomationBoot};
    if (hasUnit) {
        if (!validSystemdTarget(unit))
            throw std::invalid_argument(
                "KERNARIUM_SYSTEMD_UNIT must name a systemd target");
        boot->systemdUnit_ = unit;
    }
    if (hasPayload) {
        const bool remove =
            removalRequested(std::getenv("KERNARIUM_PAYLOAD_REMOVE"));
        std::error_code removal;
        boot->payload_ =
            takePayload(documentsDirectory(), payload, remove, removal);
        if (removal)
            std::fprintf(
                stderr,
                "Kernarium: cannot remove the boot payload Documents/%s: %s\n",
                payload, removal.message().c_str());
        else if (remove)
            std::fprintf(
                stderr, "Kernarium: removed the boot payload Documents/%s\n",
                payload);
        boot->stdoutMutex_ = &consoleStdoutMutex;
        boot->relay_ = std::make_unique<ArtifactRelay>(consoleStdoutMutex);
    }
    return boot;
}

std::string AutomationBoot::relayDeviceArgument()
{
    if (stdoutMutex_ == nullptr)
        return {};
    if (relayTaken_) {
        // Ends the old relay, which joins its reader, before the next starts.
        relay_.reset();
        relay_ = std::make_unique<ArtifactRelay>(*stdoutMutex_);
    }
    relayTaken_ = true;
    return uml::gpu::virtioDeviceArgument(
        relay_->takeConnectionName(), uml::console::protocol::deviceId);
}

} // namespace kernarium::harness
