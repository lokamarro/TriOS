// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumRuntime.h"
#include "KernariumAdbChannel.hpp"
#include "KernariumConsoleInput.hpp"
#include "KernariumConsoleOutput.hpp"
#include "KernariumGuestTimeZone.hpp"
#include "KernariumKeyWake.hpp"
#include "KernariumRestartLoop.hpp"
#include "KernariumStatistics.hpp"
#if KERNARIUM_AUTOMATION_HARNESS
#include "KernariumHarness.hpp"
#endif
#if KERNARIUM_AUTOMATION_JOB_AT_START
#include "KernariumAutomationJob.hpp"
#endif
#if KERNARIUM_AUTOMATION_ADB_AUTH
#include "KernariumAdbAuthorization.hpp"
#endif

#include "audio/in_process_service.hpp"
#include "audio/microphone_switch.hpp"
#include "platform/microphone_authorization.hpp"
#include "video/in_process_service.hpp"
#include "video/virtio_video_device.hpp"
#include "video/virtio_video_encoder_device.hpp"
#include "vsock/in_process_service.hpp"
#include "vsock/protocol.hpp"
#include "audio/protocol.hpp"
#include "battery/in_process_service.hpp"
#include "battery/protocol.hpp"
#include "console/in_process_service.hpp"
#include "console/protocol.hpp"
#ifndef UML_APP_STORE_RUNTIME
#include "gpu/frame_tap.hpp"
#endif
#include "gpu/in_process_service.hpp"
#include "gpu/protocol.hpp"
#include "input/virtio/in_process_service.hpp"
#include "input/virtio/touchscreen.hpp"
#include "block/android_boot.hpp"
#include "block/boot_initramfs.hpp"
#include "block/ubd_path.hpp"
#include "gpu_budget_plan.hpp"
#include "options.hpp"
#include "uml/emulator.h"
#include "uml_slirp_net.h"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <malloc/malloc.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
extern const unsigned char uml_elf_start[]
    __asm("section$start$__TEXT$__uml_elf");
extern const unsigned char uml_elf_end[]
    __asm("section$end$__TEXT$__uml_elf");
#if !KERNARIUM_RUNTIME_ANDROID_GUEST
/* The accepted aarch64 boot base archive and the generic per-run /init
 * (tools/rootfs/boot-ramdisk), linked in beside the kernel. */
extern const unsigned char uml_boot_base_start[]
    __asm("section$start$__TEXT$__uml_boot_base");
extern const unsigned char uml_boot_base_end[]
    __asm("section$end$__TEXT$__uml_boot_base");
extern const unsigned char uml_boot_init_start[]
    __asm("section$start$__TEXT$__uml_boot_init");
extern const unsigned char uml_boot_init_end[]
    __asm("section$end$__TEXT$__uml_boot_init");
#endif
}

namespace {

[[noreturn]] void guestTimerMaskFailure() noexcept
{
    static constexpr char message[] =
        "Kernarium: unable to isolate the UML timer signal\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    _exit(127);
}

void blockGuestTimer(sigset_t* previous) noexcept
{
    sigset_t blocked;
    if (sigemptyset(&blocked) != 0 ||
        sigaddset(&blocked, SIGALRM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &blocked, previous) != 0)
        guestTimerMaskFailure();
}

class ScopedGuestTimerBlock final {
public:
    ScopedGuestTimerBlock() noexcept
    {
        blockGuestTimer(&previous_);
    }

    ~ScopedGuestTimerBlock()
    {
        if (::pthread_sigmask(SIG_SETMASK, &previous_, nullptr) != 0)
            guestTimerMaskFailure();
    }

    ScopedGuestTimerBlock(const ScopedGuestTimerBlock&) = delete;
    ScopedGuestTimerBlock& operator=(const ScopedGuestTimerBlock&) = delete;

private:
    sigset_t previous_ {};
};

std::atomic_bool runtimeActive;
/*
 * Each registration has its own mutex, and a call through a registered pointer
 * holds the mutex that registration's destructor takes, so the pointer stays
 * valid for that call. Sync, quiesce and resume can take seconds, so they
 * serialize on a lifecycle mutex of their own rather than stall a stop request
 * or main-thread touch and key input; the emulator registration takes both.
 * Console input can wait on a guest that stops reading, so it goes through a
 * registry of its own that no caller holds while it writes; see
 * kernarium::console::InputRegistry.
 */
std::mutex emulatorLifecycleMutex;
/* A held key repeats, so an ordinary key sends the guest one wake edge a second
 * at most. */
kernarium::input::KeyWakePolicy keyWakePolicy {std::chrono::seconds(1)};
std::mutex activeEmulatorMutex;
uml_emulator* activeEmulator;
std::mutex activeInputMutex;
uml::input::virtio::InProcessInputService* activeTouchscreen;
uml::input::virtio::Touchscreen touchscreenContacts;
uml::input::virtio::InProcessInputService* activeKeyboard;
/* The way into the running guest's vsock device. Callers copy the pointer and
 * open their stream without the mutex, which an open can hold for seconds. */
std::mutex activeVsockMutex;
std::shared_ptr<uml::vsock::Endpoint> activeVsock;
/* The GPU hold the application last asked for, and the GPU service it applies
 * to while a guest runs. A hold asked for before the guest's GPU exists takes
 * effect when it does. */
std::mutex activeGpuMutex;
uml::gpu::InProcessGpuService* activeGpu;
bool gpuHoldRequested;
/* Longest a hold waits for a GPU request already running. The application
 * holds inside didEnterBackground, which iOS gives about five seconds. */
constexpr std::chrono::milliseconds gpuHoldBudget {1000};
#ifndef UML_APP_STORE_RUNTIME
/* Process lifetime, like the input callers it serves: the application's
 * terminal can still write while the process exits. */
kernarium::console::InputRegistry& consoleInputs =
    *new kernarium::console::InputRegistry;
#endif
/* What the application has told the runtime about the guest's life, which the
 * run loop reads between boots: a stop, a quiesce, the GPU hold. Process
 * lifetime, like the callers that reach it. */
kernarium::restart::Lifecycle& lifecycle = *new kernarium::restart::Lifecycle;
/* The one receiver of restart events, and the page-cache trimming the
 * application last asked for. The restarted guest's console is asked for the
 * latter to see whether it answers, so the question can never undo what the
 * application wants. */
std::mutex restartCallbackMutex;
kernarium_runtime_restart_callback restartCallback;
void* restartContext;
std::atomic_bool pageCacheTrimmingRequested;
/* How many times this launch's guest has restarted, for the statistics
 * readings: stored when a boot starts, with no guest running. */
std::atomic<std::uint32_t> bootIndex;

/* Runs with the receiver's mutex held, which is what makes clearing the
 * receiver wait for a call in progress. */
void publishRestart(std::uint32_t event, std::uint32_t restartCount)
{
    std::lock_guard lock(restartCallbackMutex);
    if (restartCallback != nullptr)
        restartCallback(restartContext, event, restartCount);
}

/* A host socket written after its peer reset it would end the process with
 * SIGPIPE, and the library puts back the disposition it found after every run:
 * ignored here, before the first run, it is the one every later run returns
 * to. The user network marks its own sockets, and the console bridge its
 * socket, with SO_NOSIGPIPE; this covers the rest between two boots. */
void ignoreBrokenPipes() noexcept
{
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, nullptr) < 0)
        std::abort();
}

/* The Microphone setting lives as long as the process and a guest that starts
 * later follows it. It starts off, and creating it touches nothing: no
 * microphone is opened and no permission is asked until the application turns
 * it on and the system has granted access. */
const std::shared_ptr<uml::audio::MicrophoneSwitch>& microphoneSwitch()
{
    static const auto& instance = *new std::shared_ptr<uml::audio::MicrophoneSwitch>(
        std::make_shared<uml::audio::MicrophoneSwitch>(
            uml::platform::makeAppleMicrophoneAuthorization()));
    return instance;
}

void publishConsoleOutput(const void* buffer, std::size_t length)
{
    kernarium::console::output().publish(buffer, length);
}

class AppNetwork final {
public:
    AppNetwork()
        : context_(uml_slirp_net_create())
    {
        if (context_ == nullptr)
            throw std::bad_alloc {};
#ifndef UML_APP_STORE_RUNTIME
        const char* forwarding = std::getenv("KERNARIUM_HOST_FORWARD");
#else
        /* An App Store runtime reads no environment knob. */
        const char* forwarding = nullptr;
#endif
        if (forwarding != nullptr && forwarding[0] != '\0') {
            const int error = uml_slirp_net_add_forward(context_, forwarding);
            if (error != 0) {
                uml_slirp_net_destroy(context_);
                context_ = nullptr;
                throw std::runtime_error(
                    "invalid Kernarium host-forward specification: " +
                    std::to_string(error));
            }
        }
    }

    ~AppNetwork()
    {
        if (context_ != nullptr) {
            uml_slirp_net_stop(context_);
            uml_slirp_net_destroy(context_);
        }
    }

    AppNetwork(const AppNetwork&) = delete;
    AppNetwork& operator=(const AppNetwork&) = delete;

    [[nodiscard]] uml_emulator_network_callbacks callbacks() noexcept
    {
        return {
            .struct_size = sizeof(uml_emulator_network_callbacks),
            .context = context_,
            .open = uml_slirp_net_open,
            .irq_fd = uml_slirp_net_irq_fd,
            .read = uml_slirp_net_read,
            .write = uml_slirp_net_write,
        };
    }

    /* The link goes down with the guest that used it: its connections end, as
     * they would if the cable were pulled, and a connection the guest never
     * closed does not keep the host side open for the next guest. The new link
     * is up at once, so the host forwards listen again while the next guest
     * boots; the guest's own open() then finds it open. The provider replays
     * the recorded forwards onto the new instance and keeps the descriptors
     * the guest's IRQ registration borrows. */
    void resetLink() noexcept
    {
        uml_slirp_net_stop(context_);
        unsigned char mac[6] {};
        unsigned int mtu {};
        const int opened = uml_slirp_net_open(context_, mac, &mtu);
        if (opened < 0) {
            std::fprintf(
                stderr,
                "Kernarium: unable to bring the user network up again: %d\n",
                opened);
        }
    }

private:
    void* context_ {};
};


class ActiveRuntime final {
public:
    ActiveRuntime()
    {
        if (runtimeActive.exchange(true, std::memory_order_acq_rel))
            throw std::runtime_error("a Kernarium guest is already active");
    }

    ~ActiveRuntime()
    {
        runtimeActive.store(false, std::memory_order_release);
    }

    ActiveRuntime(const ActiveRuntime&) = delete;
    ActiveRuntime& operator=(const ActiveRuntime&) = delete;
};

#ifndef UML_APP_STORE_RUNTIME
/* The figures the memory of a restarting guest is checked by, which a guest
 * that gives its RAM back must return to the first boot's level. */
struct MemoryFigures final {
    /* What iOS holds the application to, its lifetime peak (the figure
     * Jetsam acts on), and the share of them that guest RAM wired. */
    std::uint64_t footprint {};
    std::uint64_t peak {};
    std::uint64_t wired {};
    /* The address space the process holds, in which a window reserved again
     * for every boot would show. */
    std::uint64_t virtualSize {};
    /* The default malloc zone: bytes handed out and still in use, and bytes
     * the zone holds, which empty chunks keep until the system takes them. */
    std::uint64_t inUse {};
    std::uint64_t allocated {};
};

/* The resident pages of every mapping that holds a wire. */
std::uint64_t wiredBytes() noexcept
{
    std::uint64_t wired = 0;
    vm_address_t address = 0;
    natural_t depth = 0;
    for (;;) {
        vm_size_t size = 0;
        vm_region_submap_info_data_64_t information {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(
                mach_task_self(),
                &address,
                &size,
                &depth,
                reinterpret_cast<vm_region_recurse_info_t>(&information),
                &count) != KERN_SUCCESS)
            break;
        if (information.is_submap) {
            ++depth;
            continue;
        }
        if (information.user_wired_count != 0)
            wired += static_cast<std::uint64_t>(information.pages_resident) *
                vm_page_size;
        address += size;
    }
    return wired;
}

MemoryFigures measureMemory() noexcept
{
    MemoryFigures figures;
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        figures.footprint = info.phys_footprint;
        figures.peak = info.ledger_phys_footprint_peak;
        figures.virtualSize = info.virtual_size;
    }
    figures.wired = wiredBytes();
    malloc_statistics_t zone {};
    malloc_zone_statistics(malloc_default_zone(), &zone);
    figures.inUse = zone.size_in_use;
    figures.allocated = zone.size_allocated;
    return figures;
}

/* One line of the memory figures for the place in the guest's life `label`
 * names. Only a launch that asked for the footprint log writes them. */
void reportMemory(const char* label)
{
    const MemoryFigures figures = measureMemory();
    constexpr double mebibyte = 1048576.0;
    std::printf(
        "Kernarium: memory %s footprint %.1f MiB peak %.1f MiB wired %.1f MiB "
        "virtual %.1f MiB in-use %.1f MiB allocated %.1f MiB\n",
        label,
        figures.footprint / mebibyte,
        figures.peak / mebibyte,
        figures.wired / mebibyte,
        figures.virtualSize / mebibyte,
        figures.inUse / mebibyte,
        figures.allocated / mebibyte);
}

/* KERNARIUM_FOOTPRINT_LOG_MS reports the process footprint, the figure iOS
 * holds the application to, while the guest runs: one line per interval and
 * the peak when the guest stops. Each line also gives the headroom the kernel
 * leaves below the application's memory limit, so the footprint plus the
 * headroom is the limit and the least headroom is the closest approach, and
 * the process's CPU time, estimated energy, and performance-core time so far:
 * the difference between two lines over their interval is the average core use
 * and power, and how much of it ran on performance cores. Each interval also
 * writes a `Kernarium: memory tick` line (see reportMemory), and a guest that
 * restarts adds `ended` when its run returns and its services have stopped,
 * and `booted` when the restarted guest's console answers. */
class FootprintReporter final {
public:
    static long intervalMilliseconds() noexcept
    {
        const char* value = std::getenv("KERNARIUM_FOOTPRINT_LOG_MS");
        return value == nullptr ? 0 : std::strtol(value, nullptr, 10);
    }

    static bool enabled() noexcept { return intervalMilliseconds() > 0; }

    FootprintReporter()
    {
        const long interval = intervalMilliseconds();
        if (interval <= 0)
            return;
        interval_ = std::chrono::milliseconds {interval};
        /* The guest's timer signal must never land on this thread. */
        const ScopedGuestTimerBlock guestTimerBlock;
        thread_ = std::thread([this] { run(); });
    }

    ~FootprintReporter()
    {
        if (!thread_.joinable())
            return;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        stopped_.notify_one();
        thread_.join();
        std::printf("Kernarium: footprint peak %.1f MiB least remaining %.1f MiB\n",
            peak_ / 1048576.0, leastRemaining_ / 1048576.0);
    }

    FootprintReporter(const FootprintReporter&) = delete;
    FootprintReporter& operator=(const FootprintReporter&) = delete;

private:
    struct Sample {
        std::uint64_t footprint;
        std::uint64_t remaining;
        /* The share of the footprint charged as graphics memory: Metal
         * resources and the driver's command storage alike. */
        std::uint64_t graphics;
        /* The process's user and system CPU time so far, every thread that
         * ever ran included: the difference between two lines over their
         * interval is the process's average core use. */
        double cpuSeconds;
        /* What the system estimates the process has consumed, in joules, and
         * how many of its CPU seconds ran on performance cores. */
        double energyJoules;
        double performanceCoreSeconds;
    };

    struct PowerSample {
        double energyJoules;
        double performanceCoreSeconds;
    };

    static PowerSample processPower() noexcept
    {
        task_power_info_v2_data_t info {};
        mach_msg_type_number_t count = TASK_POWER_INFO_V2_COUNT;
        if (task_info(mach_task_self(), TASK_POWER_INFO_V2,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
            return {0.0, 0.0};
        mach_timebase_info_data_t timebase {};
        (void)mach_timebase_info(&timebase);
        const double ticksToSeconds = timebase.denom == 0 ? 0.0 :
            static_cast<double>(timebase.numer) /
                static_cast<double>(timebase.denom) / 1e9;
        return {
            static_cast<double>(info.task_energy) / 1e9,
            static_cast<double>(info.task_ptime) * ticksToSeconds,
        };
    }

    static double processCpuSeconds() noexcept
    {
        rusage usage {};
        if (getrusage(RUSAGE_SELF, &usage) != 0)
            return 0.0;
        const auto seconds = [](const timeval& time) {
            return static_cast<double>(time.tv_sec) +
                static_cast<double>(time.tv_usec) / 1e6;
        };
        return seconds(usage.ru_utime) + seconds(usage.ru_stime);
    }

    static Sample sample() noexcept
    {
        task_vm_info_data_t info {};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
            return {0, UINT64_MAX, 0, processCpuSeconds(), 0.0, 0.0};
        const std::int64_t graphics = info.ledger_tag_graphics_footprint +
            info.ledger_tag_graphics_footprint_compressed;
        const PowerSample power = processPower();
        return {info.phys_footprint, info.limit_bytes_remaining,
            static_cast<std::uint64_t>(std::max<std::int64_t>(graphics, 0)),
            processCpuSeconds(), power.energyJoules,
            power.performanceCoreSeconds};
    }

    void run()
    {
        (void)pthread_setname_np("kernarium-footprint");
        const auto start = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex_);
        while (!stopped_.wait_for(lock, interval_, [this] { return stopping_; })) {
            const Sample current = sample();
            peak_ = std::max(peak_, current.footprint);
            leastRemaining_ = std::min(leastRemaining_, current.remaining);
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            std::printf("Kernarium: footprint %.1f s %.1f MiB graphics %.1f MiB "
                "remaining %.1f MiB cpu %.2f s energy %.2f J pcore %.2f s\n",
                elapsed, current.footprint / 1048576.0,
                current.graphics / 1048576.0, current.remaining / 1048576.0,
                current.cpuSeconds, current.energyJoules,
                current.performanceCoreSeconds);
            reportMemory("tick");
        }
    }

    std::chrono::milliseconds interval_ {};
    std::mutex mutex_;
    std::condition_variable stopped_;
    bool stopping_ {};
    std::uint64_t peak_ {};
    std::uint64_t leastRemaining_ {UINT64_MAX};
    std::thread thread_;
};
#endif

class ActiveEmulatorRegistration final {
public:
    explicit ActiveEmulatorRegistration(uml_emulator* emulator)
    {
        std::scoped_lock lock(emulatorLifecycleMutex, activeEmulatorMutex);
        activeEmulator = emulator;
    }

    ~ActiveEmulatorRegistration()
    {
        std::scoped_lock lock(emulatorLifecycleMutex, activeEmulatorMutex);
        activeEmulator = nullptr;
    }

    ActiveEmulatorRegistration(const ActiveEmulatorRegistration&) = delete;
    ActiveEmulatorRegistration& operator=(
        const ActiveEmulatorRegistration&) = delete;
};

class ActiveInputRegistration final {
public:
    ActiveInputRegistration(
        uml::input::virtio::InProcessInputService& touchscreen,
        uml::input::virtio::InProcessInputService& keyboard)
    {
        std::lock_guard lock(activeInputMutex);
        touchscreenContacts = {};
        activeTouchscreen = &touchscreen;
        activeKeyboard = &keyboard;
    }

    ~ActiveInputRegistration()
    {
        std::lock_guard lock(activeInputMutex);
        activeTouchscreen = nullptr;
        activeKeyboard = nullptr;
    }

    ActiveInputRegistration(const ActiveInputRegistration&) = delete;
    ActiveInputRegistration& operator=(const ActiveInputRegistration&) = delete;
};

class ActiveVsockRegistration final {
public:
    explicit ActiveVsockRegistration(std::shared_ptr<uml::vsock::Endpoint> endpoint)
    {
        std::lock_guard lock(activeVsockMutex);
        activeVsock = std::move(endpoint);
    }

    ~ActiveVsockRegistration()
    {
        std::lock_guard lock(activeVsockMutex);
        activeVsock.reset();
    }

    ActiveVsockRegistration(const ActiveVsockRegistration&) = delete;
    ActiveVsockRegistration& operator=(const ActiveVsockRegistration&) = delete;
};

class ActiveGpuRegistration final {
public:
    explicit ActiveGpuRegistration(uml::gpu::InProcessGpuService& gpu)
    {
        std::lock_guard lock(activeGpuMutex);
        activeGpu = &gpu;
        /* The request gate belongs to the service, so a hold outlives the
         * guest it was placed for and still holds the next one: the run loop
         * reads it to keep a restarted guest from booting behind it. */
        lifecycle.setGpuHeld([&gpu] { return gpu.requestsHeld(); });
        if (gpuHoldRequested)
            (void)gpu.holdRequests(gpuHoldBudget);
    }

    ~ActiveGpuRegistration()
    {
        std::lock_guard lock(activeGpuMutex);
        activeGpu = nullptr;
        lifecycle.setGpuHeld({});
    }

    ActiveGpuRegistration(const ActiveGpuRegistration&) = delete;
    ActiveGpuRegistration& operator=(const ActiveGpuRegistration&) = delete;
};

class ConsoleBridge final {
public:
    ConsoleBridge()
    {
        int descriptors[2] {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
            throw std::runtime_error(
                "console socketpair failed: " +
                std::string(std::strerror(errno)));
        }
        guest_ = descriptors[0];
        const int host = descriptors[1];
        const int enabled = 1;
        if (setsockopt(
                host,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &enabled,
                sizeof(enabled)) != 0) {
            const int error = errno;
            close(guest_);
            close(host);
            guest_ = -1;
            throw std::runtime_error(
                "console socket setup failed: " +
                std::string(std::strerror(error)));
        }
        try {
            host_.emplace(host);
        } catch (...) {
            close(guest_);
            close(host);
            guest_ = -1;
            throw;
        }
        try {
            relay_ = std::thread([this] {
                char buffer[4096];
                for (;;) {
                    const ssize_t received = host_->receive(
                        buffer, sizeof(buffer));
                    if (received > 0) {
                        publishConsoleOutput(
                            buffer, static_cast<std::size_t>(received));
                        std::lock_guard outputLock(
                            kernarium::console::output().stdoutMutex());
                        std::string_view output(
                            buffer, static_cast<std::size_t>(received));
                        while (!output.empty()) {
                            const ssize_t written = write(
                                STDOUT_FILENO,
                                output.data(),
                                output.size());
                            if (written > 0) {
                                output.remove_prefix(
                                    static_cast<std::size_t>(written));
                            } else if (written < 0 && errno == EINTR) {
                                continue;
                            } else {
                                break;
                            }
                        }
                        continue;
                    }
                    if (received < 0 && errno == EINTR)
                        continue;
                    if (received < 0 && errno != ECONNRESET) {
                        std::fprintf(
                            stderr,
                            "Kernarium: console relay failed: %s\n",
                            std::strerror(errno));
                    }
                    break;
                }
                host_->close();
            });
        } catch (...) {
            close(guest_);
            guest_ = -1;
            throw;
        }
    }

    ~ConsoleBridge()
    {
        /* A console write still in progress ends with the input, and the
         * relay's receive ends when the guest end closes. */
        host_->stop();
        if (guest_ >= 0) {
            (void)shutdown(guest_, SHUT_RDWR);
            close(guest_);
        }
        if (relay_.joinable())
            relay_.join();
    }

    ConsoleBridge(const ConsoleBridge&) = delete;
    ConsoleBridge& operator=(const ConsoleBridge&) = delete;

#ifndef UML_APP_STORE_RUNTIME
    [[nodiscard]] kernarium::console::HostEnd& host() noexcept
    {
        return *host_;
    }
#endif

    [[nodiscard]] std::string kernelArgument() const
    {
        return "ssl0=fd:" + std::to_string(guest_) + ",fd:" +
            std::to_string(guest_);
    }

private:
    int guest_ {-1};
    std::optional<kernarium::console::HostEnd> host_;
    std::thread relay_;
};

#ifndef UML_APP_STORE_RUNTIME
class ActiveConsoleRegistration final {
public:
    explicit ActiveConsoleRegistration(ConsoleBridge& console)
    {
        consoleInputs.publish(console.host());
    }

    /* The guest has stopped reading for good, so a write that still waits
     * for it ends here rather than holding the run's teardown. */
    ~ActiveConsoleRegistration()
    {
        consoleInputs.retire();
    }

    ActiveConsoleRegistration(const ActiveConsoleRegistration&) = delete;
    ActiveConsoleRegistration& operator=(
        const ActiveConsoleRegistration&) = delete;
};
#endif

#if !KERNARIUM_RUNTIME_ANDROID_GUEST
bool usableFile(const char* path)
{
    if (path == nullptr || path[0] == '\0')
        return false;
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool ext4Rootfs(const char* path)
{
    std::string extension = std::filesystem::path {path}.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension == ".ext4";
}

bool immutableExt4Rootfs(const char* path)
{
    if (!ext4Rootfs(path))
        return true;
    std::error_code error;
    const auto permissions = std::filesystem::status(path, error).permissions();
    if (error)
        return false;
    constexpr auto writable =
        std::filesystem::perms::owner_write |
        std::filesystem::perms::group_write |
        std::filesystem::perms::others_write;
    return (permissions & writable) == std::filesystem::perms::none;
}

#endif

/* The translating engine to run guest userspace under.
 *
 * "auto" defers to the build's platform default and its availability check
 * rather than deciding here, which keeps executable-memory policy with its
 * single owner in the CPU layer. Both Apple products resolve that to the
 * data-only gadget engine: a device cannot publish into a MAP_JIT mapping at
 * all, and while the Simulator can, measurement rejects the JIT for the
 * browser workloads that matter (G157). The override exists so that choice
 * stays re-testable without a rebuild.
 */
const char* selectedExecutionEngine()
{
#ifdef UML_APP_STORE_RUNTIME
    return "auto";
#else
    const char* requested = std::getenv("KERNARIUM_EXECUTION_ENGINE");
    if (requested == nullptr)
        return "auto";
    for (const char* allowed : {"auto", "jit", "gadgets"}) {
        if (std::strcmp(requested, allowed) == 0)
            return allowed;
    }
    std::fputs(
        "Kernarium: ignoring unknown KERNARIUM_EXECUTION_ENGINE\n", stderr);
    return "auto";
#endif
}

/* Whether quiescing also hibernates the guest's RAM. "auto" is what the
 * emulator decides, which is a physical device, the one host that wires guest
 * RAM; "off" keeps it wired and resident while paused, and "on" forces it on
 * the Simulator, for diagnosis. */
const char* selectedGuestHibernation()
{
#ifdef UML_APP_STORE_RUNTIME
    return "auto";
#else
    const char* requested = std::getenv("KERNARIUM_GUEST_HIBERNATION");
    if (requested == nullptr)
        return "auto";
    for (const char* allowed : {"auto", "off", "on"}) {
        if (std::strcmp(requested, allowed) == 0)
            return allowed;
    }
    std::fputs(
        "Kernarium: ignoring unknown KERNARIUM_GUEST_HIBERNATION\n", stderr);
    return "auto";
#endif
}

#if KERNARIUM_RUNTIME_ANDROID_GUEST
void appendKernelCommandLine(
    std::vector<std::string>& arguments,
    const uml::android::Storage& storage,
    uint32_t dotsPerInch,
    uint32_t guestProcessorCount,
    std::string_view adbPublicKey,
    std::string_view timeZone)
{
    const auto path = storage.release() / uml::android::releaseCommandLine;
    constexpr std::string_view adbAuthorizationPrefix {
        "androidboot.kernarium.adb_auth="};
    constexpr std::array launcherPrefixes {
        uml::android::lcdDensityPrefix,
        uml::android::hwTimeoutMultiplierPrefix,
        adbAuthorizationPrefix,
        kernarium::adb::keyPrefix,
        kernarium::adb::tcpPrefix,
        kernarium::timezone::prefix};
    for (auto& argument :
         uml::android::readKernelCommandLine(path, launcherPrefixes))
        arguments.push_back(std::move(argument));

    /* Android init imports this as ro.boot.lcd_density. The product's vendor
     * init then publishes the same value as ro.sf.lcd_density, keeping the
     * framework's logical density aligned with the per-launch EDID. Swift has
     * already scaled dotsPerInch by nativeScale when Retina mode is active. */
    arguments.emplace_back(
        std::string {uml::android::lcdDensityPrefix} +
        std::to_string(dotsPerInch));
    /* Android init imports this as ro.boot.hw_timeout_multiplier, which the
     * vendor init publishes as ro.hw_timeout_multiplier: the framework scales
     * its ANR, watchdog and input timeouts by it. It follows the guest's vCPU
     * count. */
    arguments.emplace_back(
        uml::android::hwTimeoutMultiplierArgument(guestProcessorCount));
    /* androidboot properties become immutable ro.boot properties. iOS always
     * keeps the stock authorization prompt for a previously unseen ADB host.
     * Only the automation runtime lets a launch turn it off, as the macOS
     * launcher does, so a test driver can reach a fresh guest without a tap;
     * Kernarium/README.md owns that contract. */
#if KERNARIUM_AUTOMATION_ADB_AUTH
    const std::string_view adbAuthorization =
        kernarium::adb::authorizationValue(
            std::getenv(kernarium::adb::requestVariable.data()),
            std::getenv("KERNARIUM_HOST_FORWARD"));
    if (adbAuthorization == "0")
        std::fputs(
            "Kernarium: ADB authorization is off for this launch\n", stderr);
    arguments.emplace_back(
        std::string {adbAuthorizationPrefix} + std::string {adbAuthorization});
#else
    arguments.emplace_back(std::string {adbAuthorizationPrefix} + "1");
#endif
    /* How the app reaches adbd: the key it asks the guest to authorize this
     * boot, and, for a launch with no host forward, which has no use for a TCP
     * listener, the request that adbd not listen on TCP. adbd reads both as
     * ro.boot properties itself. */
    if (!adbPublicKey.empty()) {
        arguments.emplace_back(
            std::string {kernarium::adb::keyPrefix} + std::string {adbPublicKey});
    }
#ifdef UML_APP_STORE_RUNTIME
    /* An App Store runtime reads no environment knob, so it has no forward. */
    const char* hostForward = nullptr;
#else
    const char* hostForward = std::getenv("KERNARIUM_HOST_FORWARD");
#endif
    if (kernarium::adb::tcpDisabled(hostForward)) {
        arguments.emplace_back(std::string {kernarium::adb::tcpPrefix} + "0");
    }
    /* The host's time zone for this boot, which the guest's time zone script
     * applies while the person lets the guest follow the host. */
    if (!timeZone.empty()) {
        arguments.emplace_back(
            std::string {kernarium::timezone::prefix} + std::string {timeZone});
    }
}

#endif

#if !KERNARIUM_RUNTIME_ANDROID_GUEST
std::string fbconFontArgument(uint32_t dotsPerInch)
{
    static constexpr uint32_t largeConsoleFontDpi = 240U;
    return dotsPerInch >= largeConsoleFontDpi
        ? "fbcon=font:TER16x32"
        : "fbcon=font:VGA8x16";
}

/* A Linux guest boots the embedded base archive and a segment holding the
 * generic per-run /init and these parameters: the image read-only beneath an
 * overlay whose writable layer is a fresh ext4 filesystem on the overlay
 * disk, ubd1, and then the image's own init. Kernarium/README.md and
 * tools/rootfs/boot-ramdisk/README.md own the contract. */
std::string bootParameters(bool ext4Image)
{
    std::string parameters {"UML_BOOT_ROOT=/dev/ubda\nUML_BOOT_ROOT_TYPE="};
    parameters.append(ext4Image ? "ext4" : "erofs");
    parameters.append(
        "\nUML_BOOT_UPPER=ext4\n"
        "UML_BOOT_UPPER_DEVICE=/dev/ubdb\n"
        "UML_BOOT_ENTRY=/sbin/init\n");
    return parameters;
}

/* The run's initrd, a file in the application's temporary directory that the
 * kernel reads while it boots. The process runs one guest, so an initrd an
 * earlier, killed process left behind is removed first; this one is removed
 * when the run returns. */
class RunInitrd final {
public:
    /* `payload` is a padded newc segment appended after the runtime's own,
     * whose files replace the runtime's. */
    RunInitrd(bool ext4Image, std::span<const std::byte> payload)
    {
        const std::filesystem::path directory {
            NSTemporaryDirectory().fileSystemRepresentation};
        std::error_code error;
        for (const auto& entry :
             std::filesystem::directory_iterator(directory, error)) {
            if (entry.path().filename().string().starts_with(prefix)) {
                std::error_code ignored;
                std::filesystem::remove(entry.path(), ignored);
            }
        }
        path_ = directory /
            (std::string {prefix} + std::to_string(getpid()) + ".cpio");
        /* A kernel argument ends at whitespace and treats a double quote as
         * quoting. */
        for (const unsigned char character : path_.string()) {
            if (character <= ' ' || character == '"' || character >= 0x7f) {
                throw std::runtime_error(
                    "the temporary directory cannot be named on the kernel "
                    "command line: " + path_.string());
            }
        }
        uml::boot::InitramfsArchive archive;
        archive.addFile(
            "/init",
            std::as_bytes(std::span {
                uml_boot_init_start,
                static_cast<std::size_t>(
                    uml_boot_init_end - uml_boot_init_start)}),
            0755,
            0);
        const std::string parameters = bootParameters(ext4Image);
        archive.addFile(
            "/boot.env",
            std::as_bytes(std::span {parameters.data(), parameters.size()}),
            0644,
            0);
        auto initrd = uml::boot::composeInitrd(
            std::as_bytes(std::span {
                uml_boot_base_start,
                static_cast<std::size_t>(
                    uml_boot_base_end - uml_boot_base_start)}),
            archive.serialize());
        initrd.insert(initrd.end(), payload.begin(), payload.end());
        uml::boot::writeNewFile(path_, initrd);
    }

    ~RunInitrd()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    RunInitrd(const RunInitrd&) = delete;
    RunInitrd& operator=(const RunInitrd&) = delete;

    [[nodiscard]] std::string kernelArgument() const
    {
        return "initrd=" + path_.string();
    }

private:
    static constexpr std::string_view prefix {"kernarium-initrd-"};
    std::filesystem::path path_;
};
#endif

} // namespace

namespace {

/* What a launch asked for, as the guest needs it at every boot. */
struct GuestRequest final {
    std::uint32_t dotsPerInch {};
    std::uint32_t guestMemoryMiB {};
    std::uint32_t guestProcessorCount {};
    /* The adb host key an Android guest authorizes for the launch, empty for
     * none; a Linux guest ignores it. */
    std::string adbPublicKey;
    /* The host's time zone an Android guest is given for the launch, empty for
     * none; a Linux guest ignores it. */
    std::string timeZone;
    /* Where the run records how the session ended, empty for nowhere. */
    std::string sessionRecordPath;
#if KERNARIUM_RUNTIME_ANDROID_GUEST
    /* The guest's two disks: the release's system disk and the instance's
     * data disk. */
    std::optional<uml::android::Storage> androidStorage;
    uml::android::BootMode androidBootMode {};
#else
    std::string rootfsPath;
    std::string overlayPath;
    bool derivativeRootfs {};
    /* The run's initrd and the systemd target, both fixed for the launch. */
    std::string initrdArgument;
    std::string systemdUnit;
#if KERNARIUM_AUTOMATION_HARNESS
    kernarium::harness::AutomationBoot* automation {};
#endif
#endif
};

/* There is no bootloader to take the request `reboot bootloader` leaves in the
 * misc partition, and left in place it stops Android's init from writing the
 * next `reboot recovery`'s. */
#if KERNARIUM_RUNTIME_ANDROID_GUEST
void sayBootloaderConsumed()
{
    std::fputs(
        "Kernarium: the guest asked for the bootloader; there is none, so the "
        "request is cleared and the guest boots normally\n",
        stdout);
}
#endif

/*
 * A launch's guest: what kernarium::restart::run drives.
 *
 * The machine is what a guest that restarts itself keeps: the emulator handle,
 * the user network with its forwards, the GPU service with the view it draws
 * on, the microphone switch and the footprint log. runtimeRun makes it once
 * and this class borrows it. A boot is what the restarted guest does not keep:
 * the Virtio services, the guest's connection to the GPU, the registrations
 * that route the application's touch, key and console input to the guest, the
 * console bridge and the kernel command line that names them. Every boot makes
 * its own, in the order the guest's Virtio devices are numbered by, and
 * stopBoot() ends them all.
 */
class RestartableGuest final : public kernarium::restart::Machine {
public:
    RestartableGuest(
        GuestRequest request,
        uml::gpu::InProcessGpuService& gpu,
        std::shared_ptr<uml::virtio::SharedMemoryRegions> sharedMemoryRegions,
        AppNetwork& network)
        : request_(std::move(request))
        , gpu_(gpu)
        , sharedMemoryRegions_(std::move(sharedMemoryRegions))
        , network_(network)
    {
    }

    /* The handle every boot runs on; it outlives the guest's last boot. */
    void attach(uml_emulator* emulator) noexcept { emulator_ = emulator; }

    bool startBoot(unsigned restarts) override
    {
        boot_.reset();
        bootIndex.store(restarts, std::memory_order_release);
        try {
            ignoreBrokenPipes();
#if KERNARIUM_RUNTIME_ANDROID_GUEST
            chooseAndroidBoot(restarts);
#endif
            /* Ends the old guest's connection, which resets its device and
             * its renderer, and opens the next one. The hold on the request
             * gate carries over, which is why the loop waited for it. */
            if (restarts != 0)
                gpu_.startNextConnection();
            boot_ = buildBoot();
            return true;
        } catch (const std::exception& error) {
            boot_.reset();
            std::fprintf(
                stderr,
                restarts == 0 ? "Kernarium: startup failed: %s\n" :
                                "Kernarium: unable to restart the guest: %s\n",
                error.what());
            return false;
        } catch (...) {
            boot_.reset();
            std::fputs(restarts == 0 ? "Kernarium: startup failed\n" :
                                       "Kernarium: unable to restart the guest\n",
                stderr);
            return false;
        }
    }

    int enter() override
    {
        return uml_emulator_run(
            emulator_,
            static_cast<int>(boot_->pointers.size()),
            boot_->pointers.data(),
            nullptr);
    }

    bool runEnd(kernarium::restart::RunEnd& end) override
    {
        uml_emulator_run_end information {.struct_size = sizeof(information)};
        if (uml_emulator_get_run_end(emulator_, &information) !=
            UML_EMULATOR_OK)
            return false;
        information.restart_command[sizeof(information.restart_command) - 1U] =
            '\0';
        using Kind = kernarium::restart::RunEnd::Kind;
        switch (information.kind) {
        case UML_EMULATOR_RUN_END_POWER_OFF:
            end.kind = Kind::powerOff;
            break;
        case UML_EMULATOR_RUN_END_RESTART:
            end.kind = Kind::restart;
            break;
        case UML_EMULATOR_RUN_END_STOPPED:
            end.kind = Kind::stopped;
            break;
        case UML_EMULATOR_RUN_END_FAILED:
            end.kind = Kind::failed;
            break;
        case UML_EMULATOR_RUN_END_NONE:
        default:
            end.kind = Kind::none;
            break;
        }
        end.command = information.restart_command;
        return true;
    }

    void stopBoot() noexcept override { boot_.reset(); }

    void resetLink() noexcept override { network_.resetLink(); }

    bool stopRequested() noexcept override
    {
        return lifecycle.stopRequested();
    }

    bool bootHeld() noexcept override { return lifecycle.held(); }

    void waitWhileHeld(std::chrono::milliseconds slice) override
    {
        lifecycle.waitWhileHeld(slice);
    }

    void pause(std::chrono::milliseconds delay) override
    {
        std::this_thread::sleep_for(delay);
    }

    kernarium::restart::Clock::time_point now() noexcept override
    {
        return kernarium::restart::Clock::now();
    }

    void say(const std::string& message) noexcept override
    {
        std::printf("Kernarium: %s\n", message.c_str());
    }

    void restarting(unsigned restarts) noexcept override
    {
#ifndef UML_APP_STORE_RUNTIME
        if (FootprintReporter::enabled())
            reportMemory("ended");
#endif
        publishRestart(KERNARIUM_RUNTIME_RESTART_BEGAN, restarts);
    }

    bool consoleAnswers() noexcept override
    {
        /* The guest boots with trimming off, and the application's last
         * request is what it should have, so asking for that changes nothing
         * the application did not ask for. */
        return uml_emulator_set_page_cache_trimming(
                   emulator_,
                   pageCacheTrimmingRequested.load(
                       std::memory_order_acquire) ? 1 : 0) == UML_EMULATOR_OK;
    }

    void booted(unsigned restarts, bool answered) noexcept override
    {
        std::printf(
            "Kernarium: restart %u: the new guest's console %s\n",
            restarts,
            answered ? "answers" : "has not answered; sending the event anyway");
#ifndef UML_APP_STORE_RUNTIME
        if (FootprintReporter::enabled())
            reportMemory("booted");
#endif
        publishRestart(KERNARIUM_RUNTIME_RESTART_BOOTED, restarts);
    }

private:
    struct Boot final {
        std::unique_ptr<uml::input::virtio::InProcessInputService> touchscreen;
        std::unique_ptr<uml::input::virtio::InProcessInputService> keyboard;
        std::unique_ptr<uml::gpu::InProcessVhostUserService> audio;
        std::unique_ptr<uml::gpu::InProcessVhostUserService> video;
        std::unique_ptr<uml::gpu::InProcessVhostUserService> videoEncoder;
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        std::unique_ptr<uml::gpu::InProcessVhostUserService> battery;
#endif
        /* The vsock device, the last Virtio device of the boot. */
        std::optional<uml::vsock::InProcessService> vsock;
        std::optional<ConsoleBridge> console;
        std::vector<std::string> arguments;
        std::vector<const char*> pointers;
#ifndef UML_APP_STORE_RUNTIME
        std::optional<ActiveConsoleRegistration> consoleRegistration;
#endif
        std::optional<ActiveInputRegistration> inputRegistration;
        std::optional<ActiveVsockRegistration> vsockRegistration;
    };

    [[nodiscard]] std::unique_ptr<Boot> buildBoot()
    {
        auto boot = std::make_unique<Boot>();
        boot->touchscreen =
            std::make_unique<uml::input::virtio::InProcessInputService>(
                uml::input::virtio::DeviceKind::touchscreen);
        boot->keyboard =
            std::make_unique<uml::input::virtio::InProcessInputService>(
                uml::input::virtio::DeviceKind::keyboard);
        boot->audio = uml::audio::makeInProcessService(
            {.microphone = microphoneSwitch()});
        if (!uml::platform::videoDecodeCapabilities().empty())
            boot->video = uml::video::makeInProcessService(
                sharedMemoryRegions_);
        if (!uml::platform::videoEncodeCapabilities().empty())
            boot->videoEncoder = uml::video::makeInProcessEncoderService(
                sharedMemoryRegions_);
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        boot->battery = uml::battery::makeInProcessService();
#endif
        boot->console.emplace();

        auto& arguments = boot->arguments;
        arguments = {
            "uml-kernarium",
            std::string {"--execution-engine="} + selectedExecutionEngine(),
            std::string {"--guest-hibernation="} + selectedGuestHibernation(),
            "mem=" + std::to_string(request_.guestMemoryMiB) + "M",
            "ncpus=" + std::to_string(request_.guestProcessorCount),
            /* iOS can reclaim the application without running any more of its
             * code, and only the guest can write back its own page cache. The
             * host asks for that through UML's management console, which needs
             * magic SysRq to act rather than silently ignore the request;
             * Android's init turns SysRq off, so state it here. */
            "sysrq_always_enabled",
        };
        if (!request_.sessionRecordPath.empty())
            arguments.emplace_back(
                "--session-record=" + request_.sessionRecordPath);
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        /* Android tags heap pointers, so the guest must run with the top byte
         * ignored exactly as it does on the macOS Android product. */
        arguments.emplace_back("--guest-top-byte-ignore");
        arguments.emplace_back(boot->console->kernelArgument());
#else
        arguments.emplace_back(request_.initrdArgument);
        arguments.emplace_back(uml::boot::missingInitArgument);
        arguments.emplace_back("console=tty0");
        arguments.emplace_back("console=ttyS0");
        arguments.emplace_back(fbconFontArgument(request_.dotsPerInch));
        arguments.emplace_back(boot->console->kernelArgument());
        arguments.emplace_back("ubd1=" + request_.overlayPath);
#endif
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            gpu_.takeConnectionName(),
            uml::gpu::protocol::virtioGpuDeviceId));
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        /* Android's vendor policy labels the battery's sysfs path by its
         * position among the Virtio devices
         * (android/lineage-24/overlay/sepolicy/vendor/genfs_contexts). Straight
         * after the GPU it is always virtio-uml.1/virtio1, whatever optional
         * devices follow. */
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            boot->battery->takeConnectionName(),
            uml::battery::protocol::deviceId));
#endif
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            boot->touchscreen->takeConnectionName(),
            uml::input::virtio::protocol::deviceId));
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            boot->keyboard->takeConnectionName(),
            uml::input::virtio::protocol::deviceId));
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            boot->audio->takeConnectionName(),
            uml::audio::protocol::deviceId));
        if (boot->video) {
            arguments.emplace_back(uml::gpu::virtioDeviceArgument(
                boot->video->takeConnectionName(), uml::video::deviceId));
        }
        if (boot->videoEncoder) {
            arguments.emplace_back(uml::gpu::virtioDeviceArgument(
                boot->videoEncoder->takeConnectionName(),
                uml::video::encoderDeviceId));
        }
#if KERNARIUM_AUTOMATION_HARNESS && !KERNARIUM_RUNTIME_ANDROID_GUEST
        if (request_.automation != nullptr) {
            if (auto relay = request_.automation->relayDeviceArgument();
                !relay.empty())
                arguments.emplace_back(std::move(relay));
        }
#endif
        /* The last Virtio device, so that no other device's index moves. */
        boot->vsock.emplace(uml::vsock::makeInProcessService());
        arguments.emplace_back(uml::gpu::virtioDeviceArgument(
            boot->vsock->service->takeConnectionName(),
            uml::vsock::protocol::deviceId));
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        /* The release's read-only system disk and the instance's data disk are
         * the guest's only block devices. The selected ramdisk is exposed from
         * the system disk without a standalone image. */
        for (auto& argument :
             request_.androidStorage->kernelArguments(androidBoot_))
            arguments.push_back(std::move(argument));
        appendKernelCommandLine(
            arguments,
            *request_.androidStorage,
            request_.dotsPerInch,
            request_.guestProcessorCount,
            request_.adbPublicKey,
            request_.timeZone);
#else
        /* A sealed ext4 derivative is attached read-only; the per-run /init
         * mounts either kind of image beneath the overlay. */
        if (request_.derivativeRootfs)
            arguments.emplace_back("ubd0cr=" + request_.rootfsPath);
        arguments.emplace_back("systemd.unit=" + request_.systemdUnit);
        arguments.emplace_back("plymouth.enable=0");
#endif
        boot->pointers.reserve(arguments.size());
        for (const auto& argument : arguments)
            boot->pointers.push_back(argument.c_str());
        requireKernelCommandLineFits(boot->pointers);
#ifndef UML_APP_STORE_RUNTIME
        boot->consoleRegistration.emplace(*boot->console);
#endif
        boot->inputRegistration.emplace(*boot->touchscreen, *boot->keyboard);
        boot->vsockRegistration.emplace(boot->vsock->endpoint);
        return boot;
    }

    /* The kernel exits the process when its arguments do not fit its command
     * line, so a launch whose arguments, as the emulator completes them, do
     * not is refused here, with the reason. */
    void requireKernelCommandLineFits(
        const std::vector<const char*>& pointers) const
    {
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        const std::string_view rootfs;
#else
        const std::string_view rootfs = request_.rootfsPath;
#endif
        const auto prepared = uml::emulator::prepare_emulator_options(
            static_cast<int>(pointers.size()), pointers.data(), rootfs);
        /* The run reports why it is invalid itself. */
        if (!prepared.valid)
            return;
        const auto length =
            kernarium::adb::kernelCommandLineLength(prepared.arguments);
        if (!kernarium::adb::fitsKernelCommandLine(length))
            throw std::runtime_error(kernarium::adb::commandLineTooLong(length));
    }

#if KERNARIUM_RUNTIME_ANDROID_GUEST
    /* The first boot asks for the mode the application chose. The ramdisk of
     * every restart is picked again from the misc partition in automatic mode,
     * so that `reboot recovery` boots recovery and anything else boots
     * normally, and a bootloader request is consumed. */
    void chooseAndroidBoot(unsigned restarts)
    {
        if (restarts == 0) {
            if (request_.androidStorage->consumeBootloaderCommand())
                sayBootloaderConsumed();
            androidBoot_ = request_.androidStorage->selectBoot(
                request_.androidBootMode);
            return;
        }
        const auto next = request_.androidStorage->selectRestartBoot();
        if (next.bootloaderConsumed)
            sayBootloaderConsumed();
        androidBoot_ = next.boot;
    }

    uml::android::BootSelection androidBoot_ {};
#endif

    const GuestRequest request_;
    uml::gpu::InProcessGpuService& gpu_;
    const std::shared_ptr<uml::virtio::SharedMemoryRegions>
        sharedMemoryRegions_;
    AppNetwork& network_;
    uml_emulator* emulator_ {};
    std::unique_ptr<Boot> boot_;
};

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;

/* The host GPU's budget for a guest of `guestMemoryMiB` and
 * `guestProcessorCount` vCPUs with the launch parameters `boot` states, which
 * is what both the application's warning and the run use. */
uml::emulator::GpuBudgetPlan planGpuBudget(
    const kernarium_runtime_boot* boot,
    std::uint32_t guestMemoryMiB,
    std::uint32_t guestProcessorCount)
{
    auto plan = uml::emulator::plan_gpu_budget(
        static_cast<std::uint64_t>(guestMemoryMiB) * mebibyte,
        /* The host sets the limit; a launch states neither it nor the
         * gadget cache. */
        0,
        0,
        guestProcessorCount,
        {
            .application = boot->gpu_budget_application_mib * mebibyte,
            .engine_rest = boot->gpu_budget_engine_mib * mebibyte,
            .headroom = boot->gpu_budget_headroom_mib * mebibyte,
        });
#if UML_GPU_SOFTWARE_RENDERER
    /* The Simulator's renderer draws on the CPU and holds no device memory,
     * so no GPU budget applies and a limit, which the Simulator never
     * states, cannot leave the GPU short: plan as if there were none. */
    plan.process_limit = 0;
    plan.budget = uml::platform::compute_gpu_budget({
        .process_limit = 0,
        .guest_memory = plan.guest_memory,
    });
#endif
    return plan;
}

int runtimePlanGpuBudget(
    std::uint32_t guestMemoryMiB,
    std::uint32_t guestProcessorCount,
    std::uint32_t applicationMiB,
    std::uint32_t engineMiB,
    std::uint32_t headroomMiB,
    kernarium_runtime_gpu_budget* plan)
{
    if (plan == nullptr || plan->struct_size < sizeof(*plan) ||
        guestMemoryMiB == 0 || guestProcessorCount == 0)
        return -UML_EMULATOR_INVALID_ARGUMENT;
    kernarium_runtime_boot parameters {};
    parameters.gpu_budget_application_mib = applicationMiB;
    parameters.gpu_budget_engine_mib = engineMiB;
    parameters.gpu_budget_headroom_mib = headroomMiB;
    const auto planned = planGpuBudget(
        &parameters, guestMemoryMiB, guestProcessorCount);
    const std::uint32_t size = plan->struct_size;
    *plan = {};
    plan->struct_size = size;
    plan->short_of_floor = planned.budget.short_of_floor ? 1U : 0U;
    plan->budget_bytes = planned.budget.bytes;
    plan->process_limit_bytes = planned.process_limit;
    plan->available_bytes = planned.budget.available;
    plan->guest_memory_bytes = planned.guest_memory;
    plan->engine_bytes = planned.engine;
    plan->application_bytes = planned.parameters.application;
    plan->headroom_bytes = planned.parameters.headroom;
    return 0;
}

/* What a request states before the adb key was added to it. A request this
 * large states everything but the key. */
constexpr std::size_t bootSizeBeforeAdbKey =
    offsetof(kernarium_runtime_boot, android_adb_public_key);

/* What a request states before the time zone was added to it: everything but
 * the time zone, the adb key included. */
constexpr std::size_t bootSizeBeforeTimeZone =
    offsetof(kernarium_runtime_boot, android_time_zone);

/* What a request states before the session record was added to it. */
constexpr std::size_t bootSizeBeforeSessionRecord =
    offsetof(kernarium_runtime_boot, session_record_path);

int runtimeRun(
    KernariumMetalView view,
    const kernarium_runtime_boot* boot)
{
    try {
        /* A pre-existing Swift worker need not inherit the initial main-thread
         * mask. Keep the launcher itself ineligible for ITIMER_REAL throughout
         * the managed run; every runtime worker it creates inherits this mask.
         */
        ScopedGuestTimerBlock guestTimerBlock;
        ActiveRuntime active;
        if (view == nullptr || boot == nullptr ||
            boot->struct_size < bootSizeBeforeAdbKey) {
            std::fputs("Kernarium: invalid boot request\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        /* Read only from a request large enough to hold it, and no further
         * than the longest key: the string is the caller's. */
        std::string adbPublicKey;
        if (boot->struct_size >= bootSizeBeforeTimeZone &&
            boot->android_adb_public_key != nullptr) {
            const std::size_t length = ::strnlen(
                boot->android_adb_public_key,
                kernarium::adb::maximumKeyLength + 1U);
            if (!kernarium::adb::validPublicKey(
                    {boot->android_adb_public_key, length})) {
                std::fputs("Kernarium: invalid adb public key\n", stderr);
                return -UML_EMULATOR_INVALID_ARGUMENT;
            }
            adbPublicKey.assign(boot->android_adb_public_key, length);
        }
        /* The same for the time zone, which differs in one respect: a name
         * that is not valid is dropped, because the guest keeps its own zone
         * without one and no boot should fail over it. */
        std::string timeZone;
        if (boot->struct_size >= bootSizeBeforeSessionRecord &&
            boot->android_time_zone != nullptr) {
            const std::size_t length = ::strnlen(
                boot->android_time_zone,
                kernarium::timezone::maximumLength + 1U);
            if (kernarium::timezone::validIdentifier(
                    {boot->android_time_zone, length}))
                timeZone.assign(boot->android_time_zone, length);
            else
                std::fputs(
                    "Kernarium: ignoring an invalid time zone name\n",
                    stderr);
        }
        const uint32_t width = boot->width;
        const uint32_t height = boot->height;
        const uint32_t dotsPerInch = boot->dots_per_inch;
        const uint32_t guestMemoryMiB = boot->guest_memory_mib;
        const uint32_t guestProcessorCount = boot->guest_processor_count;
        if (width == 0 || height == 0 ||
            width > uml::gpu::protocol::maximumEdidDimension ||
            height > uml::gpu::protocol::maximumEdidDimension ||
            dotsPerInch == 0) {
            std::fputs("Kernarium: invalid display geometry\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        if (boot->auto_resize_guest > 1U) {
            std::fputs("Kernarium: invalid display resize policy\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        const NSUInteger hostProcessorCount =
            [NSProcessInfo processInfo].processorCount;
        const uint32_t maximumGuestProcessorCount = hostProcessorCount == 0
            ? 1U : static_cast<uint32_t>(hostProcessorCount);
        if (guestProcessorCount == 0 ||
            guestProcessorCount > maximumGuestProcessorCount ||
            guestMemoryMiB < 512U || guestMemoryMiB > 4096U ||
            guestMemoryMiB % 64U != 0) {
            std::fputs(
                "Kernarium: invalid guest CPU or memory configuration\n",
                stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        GuestRequest request;
        request.adbPublicKey = std::move(adbPublicKey);
        request.timeZone = std::move(timeZone);
        if (boot->struct_size >= sizeof(kernarium_runtime_boot) &&
            boot->session_record_path != nullptr) {
            const std::size_t length =
                ::strnlen(boot->session_record_path, PATH_MAX);
            if (length == 0 || length == PATH_MAX) {
                std::fputs("Kernarium: invalid session record path\n",
                    stderr);
                return -UML_EMULATOR_INVALID_ARGUMENT;
            }
            request.sessionRecordPath.assign(boot->session_record_path,
                length);
        }
        request.dotsPerInch = dotsPerInch;
        request.guestMemoryMiB = guestMemoryMiB;
        request.guestProcessorCount = guestProcessorCount;
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        if (boot->android_data_disk_path == nullptr ||
            boot->android_release_path == nullptr) {
            std::fputs("Kernarium: incomplete Android bundle\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        request.androidBootMode =
            static_cast<uml::android::BootMode>(boot->android_boot_mode);
        try {
            const std::filesystem::path release = boot->android_release_path;
            const std::filesystem::path dataDisk = boot->android_data_disk_path;
            const auto prepared = uml::android::prepareDataDisk(
                uml::android::readLayout(release / uml::android::releaseLayout),
                dataDisk,
                boot->android_data_gib);
            if (prepared.change != uml::android::DataDiskChange::none)
                std::printf(
                    "Kernarium: %s the data disk, %u GiB\n",
                    prepared.change == uml::android::DataDiskChange::created
                        ? "created" : "grew",
                    prepared.capacityGiB);
            request.androidStorage.emplace(release, dataDisk);
        } catch (const std::exception& error) {
            std::fprintf(
                stderr, "Kernarium: cannot prepare the Android disks: %s\n",
                error.what());
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
#else
        const char* rootfsPath = boot->rootfs_path;
        const char* overlayPath = boot->overlay_path;
        if (!usableFile(rootfsPath) || !immutableExt4Rootfs(rootfsPath) ||
            !usableFile(overlayPath)) {
            std::fputs("Kernarium: invalid rootfs or overlay path\n", stderr);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        /* Both are named in UBD kernel arguments, which cannot carry a path
         * with whitespace, a quote, a comma or a colon as itself. */
        if (!uml::block::usable_ubd_path(rootfsPath) ||
            !uml::block::usable_ubd_path(overlayPath)) {
            std::fprintf(
                stderr,
                "Kernarium: %s cannot be named on the kernel command line; "
                "rename it without spaces, quotes, commas or colons\n",
                uml::block::usable_ubd_path(rootfsPath) ? overlayPath
                                                        : rootfsPath);
            return -UML_EMULATOR_INVALID_ARGUMENT;
        }
        request.rootfsPath = rootfsPath;
        request.overlayPath = overlayPath;
        request.derivativeRootfs = ext4Rootfs(rootfsPath);
#endif
        /* A stop asked for before this run is not carried into it, and the
         * disposition every later run of the library returns to is set before
         * the first. */
        lifecycle.begin();
        ignoreBrokenPipes();
        (void)setvbuf(stdout, nullptr, _IONBF, 0);
#ifndef UML_APP_STORE_RUNTIME
        const auto bootingAt = std::chrono::steady_clock::now();
#endif
        std::printf(
            "Kernarium: booting %ux%u guest at %u DPI with mem=%uM "
            "ncpus=%u\n",
            width,
            height,
            dotsPerInch,
            guestMemoryMiB,
            guestProcessorCount);

        AppNetwork network;
        auto networkCallbacks = network.callbacks();
        const auto destroy = &uml_emulator_destroy;
        std::unique_ptr<uml_emulator, decltype(destroy)> emulator {nullptr, destroy};
        int result = 0;
        {
            /* The host-visible shared-memory region belongs to the GPU backend,
             * and the video decoder fills capture buffers that live in it, so
             * both backends share one index of the ranges published there. */
            auto sharedMemoryRegions =
                std::make_shared<uml::virtio::SharedMemoryRegions>();
            /* GPU memory is charged to the application like guest RAM: the GPU's
             * budget is what the application's memory limit leaves after the
             * guest, the gadget cache and the launch's allowances, at least 384 MiB
             * and at most half of the guest's allocation. The application has
             * already warned the user when the limit leaves less than the floor
             * (runtimePlanGpuBudget); the guest boots with the floor. */
            const auto gpuBudget = planGpuBudget(
                boot, guestMemoryMiB, guestProcessorCount);
            const std::uint64_t deviceMemoryLimit = gpuBudget.budget.bytes;
            if (gpuBudget.budget.short_of_floor) {
                std::fprintf(
                    stderr,
                    "Kernarium: %s\n",
                    uml::emulator::describe_gpu_budget_shortfall(gpuBudget)
                        .c_str());
            }
#if UML_GPU_SOFTWARE_RENDERER
            std::puts(
                "Kernarium: no GPU budget: the software renderer holds no "
                "device memory");
#else
            std::printf(
                "Kernarium: GPU budget %llu MiB of a %llu MiB limit\n",
                static_cast<unsigned long long>(deviceMemoryLimit >> 20),
                static_cast<unsigned long long>(gpuBudget.process_limit >> 20));
#endif
            /* The display outlives the guest: the GPU service keeps the view and
             * the frame mirror, and hands each boot its own connection. */
            uml::gpu::InProcessGpuService gpuService(
                uml::gpu::InProcessGpuOptions {
                    .displayMode = uml::gpu::DisplayMode::native,
                    .width = width,
                    .height = height,
                    .dotsPerInch = dotsPerInch,
                    .nativeView = (__bridge void*)view,
                    .autoResizeGuest = boot->auto_resize_guest != 0,
                    .sharedMemoryRegions = sharedMemoryRegions,
                    .deviceMemoryLimit = deviceMemoryLimit,
                });
            /* Discovery of the hardware encoder takes test encodes, so it starts
             * now and the capability check at each boot finds it done or nearly. */
            uml::platform::prepareVideoEncodeCapabilities();
#if !KERNARIUM_RUNTIME_ANDROID_GUEST
#if KERNARIUM_AUTOMATION_HARNESS
            const auto automation =
                kernarium::harness::AutomationBoot::fromEnvironment(
                    kernarium::console::output().stdoutMutex());
            if (automation)
                std::fprintf(
                    stderr,
                    "Kernarium: automation boot with a %zu-byte payload and "
                    "target %s\n",
                    automation->payload().size(),
                    automation->systemdUnit().empty()
                        ? "graphical.target"
                        : automation->systemdUnit().c_str());
            const RunInitrd initrd {
                request.derivativeRootfs,
                automation ? automation->payload()
                           : std::span<const std::byte> {}};
            request.automation = automation.get();
            request.systemdUnit =
                automation && !automation->systemdUnit().empty()
                ? automation->systemdUnit()
                : std::string {"graphical.target"};
#else
            const RunInitrd initrd {request.derivativeRootfs, {}};
            request.systemdUnit = "graphical.target";
#endif
            request.initrdArgument = initrd.kernelArgument();
#endif
            /* Every boot's device services end before guest RAM is released. */
            RestartableGuest guest {
                std::move(request),
                gpuService,
                sharedMemoryRegions,
                network};
            const uml_emulator_config config {
                .abi_version = UML_EMULATOR_ABI_VERSION,
                .struct_size = sizeof(uml_emulator_config),
#if KERNARIUM_RUNTIME_ANDROID_GUEST
                .rootfs_path = nullptr,
#else
                .rootfs_path = rootfsPath,
#endif
                .kernel_image = uml_elf_start,
                .kernel_image_size = static_cast<std::size_t>(
                    uml_elf_end - uml_elf_start),
                .network = &networkCallbacks,
            };
            uml_emulator* created = nullptr;
            const int createResult = uml_emulator_create(&config, &created);
            if (createResult != UML_EMULATOR_OK) {
                std::fprintf(
                    stderr, "Kernarium: emulator creation failed: %d\n",
                    createResult);
                return createResult;
            }
            emulator.reset(created);
            guest.attach(emulator.get());
#ifndef UML_APP_STORE_RUNTIME
            /* The host devices, the GPU driver among them, start before the
             * guest does; the footprint log times that stretch. */
            if (FootprintReporter::enabled())
                std::printf("Kernarium: emulator created %.1f ms after booting\n",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - bootingAt).count());
#endif
            {
                /* The handle and the GPU stay registered between boots: a sync
                 * flushes what the last guest left open, a hold is applied to the
                 * next guest's GPU, and a stop reaches the loop. */
                ActiveEmulatorRegistration registration {emulator.get()};
                ActiveGpuRegistration gpuRegistration {gpuService};
#ifndef UML_APP_STORE_RUNTIME
                const FootprintReporter footprintReporter;
#endif
                result = kernarium::restart::run(guest);
            }
        }
#if KERNARIUM_RUNTIME_ANDROID_GUEST
        // Only Android's own clean power-off may return a reusable process.
        if (result == 0) {
            uml_emulator_run_end end {.struct_size = sizeof(end)};
            if (uml_emulator_get_run_end(emulator.get(), &end) != UML_EMULATOR_OK ||
                end.kind != UML_EMULATOR_RUN_END_POWER_OFF) {
                result = -UML_EMULATOR_RUNTIME_ERROR;
            } else {
                result = uml_emulator_release_guest(emulator.get());
                for (unsigned attempt = 0;
                     result == -UML_EMULATOR_RUNTIME_ERROR &&
                     attempt < uml::emulator::restart_run_retries; ++attempt) {
                    std::this_thread::sleep_for(uml::emulator::restart_run_retry_delay);
                    result = uml_emulator_release_guest(emulator.get());
                }
                if (result != 0)
                    std::fprintf(stderr,
                        "Kernarium: clean shutdown could not release the guest: %d\n", result);
            }
        }
#endif
        std::printf("Kernarium: guest exited with status %d\n", result);
        return result;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Kernarium: startup failed: %s\n", error.what());
        return -UML_EMULATOR_RUNTIME_ERROR;
    } catch (...) {
        std::fputs("Kernarium: startup failed\n", stderr);
        return -UML_EMULATOR_RUNTIME_ERROR;
    }
}

void runtimeRequestStop(void)
{
    /* The handle drops a stop made while it is idle, which is what the gap
     * between two boots is, so the loop keeps the request itself and looks at
     * it before every run. */
    lifecycle.requestStop();
    std::lock_guard lock(activeEmulatorMutex);
    if (activeEmulator != nullptr)
        uml_emulator_request_stop(activeEmulator);
}

/*
 * iOS can reclaim a suspended application without running anything else in it,
 * so the guest's writable images have to reach stable storage while the app is
 * still scheduled. This flushes what the guest has already handed to the host;
 * pages still dirty inside the guest are written back by the guest itself.
 */
int runtimeSync(void)
{
    std::lock_guard lock(emulatorLifecycleMutex);
    if (activeEmulator == nullptr)
        return 0;
    return uml_emulator_sync(activeEmulator);
}

/*
 * Flushing alone is not enough once the application stops being scheduled at a
 * predictable moment: the guest keeps running afterwards and dirties what was
 * just written back. Freezing it as well means the disk it leaves behind is
 * the disk that was flushed.
 */
int quiesceGuest(bool resident)
{
    /* A guest that restarts between the quiesce and the resume waits for the
     * resume before it boots: the library's quiesce of an idle handle only
     * flushes, and a guest booted behind it would run while the application
     * is away. */
    lifecycle.quiesce();
    std::lock_guard lock(emulatorLifecycleMutex);
    if (activeEmulator == nullptr)
        return 0;
    return resident ? uml_emulator_quiesce_resident(activeEmulator)
                    : uml_emulator_quiesce(activeEmulator);
}

int runtimeQuiesce(void)
{
    return quiesceGuest(false);
}

/* The quiesce that keeps the guest's RAM in the process, for a host short of
 * the space a hibernation image takes. */
int runtimeQuiesceResident(void)
{
    return quiesceGuest(true);
}

void runtimeResume(void)
{
    {
        std::lock_guard lock(emulatorLifecycleMutex);
        if (activeEmulator != nullptr)
            uml_emulator_resume(activeEmulator);
    }
    /* After the library has restored what it hibernated. */
    lifecycle.resume();
}

int runtimeSetMicrophone(int enabled)
{
    namespace audio = uml::audio;
    const auto microphone = microphoneSwitch();
    if (enabled == 0) {
        microphone->disable();
        return KERNARIUM_RUNTIME_MICROPHONE_OFF;
    }
    /* Never asks: the application asks, and this only carries its answer. */
    switch (microphone->restore()) {
    case audio::MicrophoneSwitch::State::off:
        return KERNARIUM_RUNTIME_MICROPHONE_OFF;
    case audio::MicrophoneSwitch::State::unasked:
    case audio::MicrophoneSwitch::State::asking:
        // The runtime never asks, so a question is never open here.
        return KERNARIUM_RUNTIME_MICROPHONE_UNASKED;
    case audio::MicrophoneSwitch::State::denied:
        return KERNARIUM_RUNTIME_MICROPHONE_DENIED;
    case audio::MicrophoneSwitch::State::on:
        return KERNARIUM_RUNTIME_MICROPHONE_ON;
    }
    return KERNARIUM_RUNTIME_MICROPHONE_OFF;
}

int runtimeSetPageCacheTrimming(int enabled)
{
    /* Remembered even when no console answers yet: the restarted guest's
     * console is asked for this. */
    pageCacheTrimmingRequested.store(enabled != 0, std::memory_order_release);
    std::lock_guard lock(emulatorLifecycleMutex);
    if (activeEmulator == nullptr)
        return -UML_EMULATOR_UNSUPPORTED;
    return uml_emulator_set_page_cache_trimming(activeEmulator, enabled);
}

static_assert(
    KERNARIUM_RUNTIME_TOUCH_MAXIMUM ==
    uml::input::virtio::protocol::touchscreenAbsoluteMaximum);

static_assert(
    KERNARIUM_RUNTIME_TOUCH_CONTACTS ==
    uml::input::virtio::protocol::touchscreenContactCount);

void runtimeSendTouch(
    uint32_t slot,
    uint32_t x,
    uint32_t y,
    int pressed)
{
    std::lock_guard lock(activeInputMutex);
    if (activeTouchscreen != nullptr) {
        const auto report = touchscreenContacts.touch(slot, x, y, pressed != 0);
        activeTouchscreen->send(report.span());
    }
}

void runtimeCancelTouch(uint32_t slot)
{
    std::lock_guard lock(activeInputMutex);
    if (activeTouchscreen != nullptr) {
        const auto report = touchscreenContacts.cancel(slot);
        activeTouchscreen->send(report.span());
    }
}

void runtimeSendKey(uint16_t code, int pressed)
{
    namespace input = uml::input::virtio::protocol;
    const std::array events {
        input::InputEvent {
            .type = input::eventKey,
            .code = code,
            .value = pressed != 0 ? 1 : 0,
        },
        input::InputEvent {
            .type = input::eventSynchronize,
            .code = input::synchronizeReport,
            .value = 0,
        },
    };
    {
        std::lock_guard lock(activeInputMutex);
        if (activeKeyboard == nullptr)
            return;
        activeKeyboard->send(events);
    }
    /* A guest that suspended itself, as Android does once its display is off,
     * holds the key until something wakes it. A key-down wakes it, as a phone
     * wakes to its keyboard, and a touch does not; see KeyWakePolicy. The wake
     * can wait on the guest's console, so it leaves this thread. */
    if (!keyWakePolicy.shouldWake(
            code == input::keyPower, pressed != 0,
            std::chrono::steady_clock::now()))
        return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::lock_guard lifecycle(emulatorLifecycleMutex);
        if (activeEmulator != nullptr)
            uml_emulator_wake(activeEmulator);
    });
}

int runtimeSetGpuHold(int held)
{
    std::lock_guard lock(activeGpuMutex);
    gpuHoldRequested = held != 0;
    if (activeGpu == nullptr)
        return -1;
    if (held == 0) {
        activeGpu->releaseRequests();
        /* A restarted guest waits for this. */
        lifecycle.changed();
        return 1;
    }
    return activeGpu->holdRequests(gpuHoldBudget) ? 1 : 0;
}

int runtimeGuestDisplayActive()
{
    std::lock_guard lock(activeGpuMutex);
    if (activeGpu == nullptr)
        return -1;
    const auto active = activeGpu->guestScanoutActive();
    return active.has_value() ? (*active ? 1 : 0) : -1;
}

int runtimeReadStatistics(kernarium_runtime_statistics* statistics)
{
    if (statistics == nullptr || statistics->struct_size < sizeof(*statistics))
        return -EINVAL;
    uml_emulator_statistics guest {.struct_size = sizeof(guest)};
    int result;
    {
        std::lock_guard lock(activeEmulatorMutex);
        if (activeEmulator == nullptr)
            return -ENODEV;
        result = uml_emulator_get_statistics(activeEmulator, &guest);
    }
    if (const int error = kernarium::statistics::statisticsResult(result))
        return error;
    const auto timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::optional<kernarium::statistics::DisplayFigures> display;
    std::optional<bool> scanout;
    std::optional<std::uint64_t> memoryRefusals;
    std::optional<kernarium::statistics::GpuMemoryFigures> gpuMemory;
    {
        std::lock_guard lock(activeGpuMutex);
        if (activeGpu != nullptr) {
            if (const auto frames = activeGpu->displayStatistics())
                display = kernarium::statistics::DisplayFigures {
                    .presentedFrames = frames->presentedFrames,
                    .offeredFrames = frames->guestFrames,
                };
            scanout = activeGpu->guestScanoutActive();
            memoryRefusals = activeGpu->memoryRefusals();
            if (const auto memory = activeGpu->deviceMemory())
                gpuMemory = kernarium::statistics::GpuMemoryFigures {
                    .budgetBytes = memory->budgetBytes,
                    .usedBytes = memory->usageBytes,
                };
        }
    }
    *statistics = kernarium::statistics::reading(
        guest,
        display,
        scanout,
        bootIndex.load(std::memory_order_acquire),
        timestamp,
        memoryRefusals,
        gpuMemory);
    return 0;
}

int runtimeReadGraphicsMemory(kernarium_runtime_graphics_memory* memory)
{
    if (memory == nullptr || memory->struct_size < sizeof(*memory))
        return -EINVAL;
    std::optional<uml::gpu::GraphicsMemory> figures;
    std::optional<std::uint64_t> refusals;
    {
        std::lock_guard lock(activeGpuMutex);
        if (activeGpu == nullptr)
            return -ENODEV;
        figures = activeGpu->graphicsMemory(
            KERNARIUM_RUNTIME_GRAPHICS_HOLDERS,
            std::chrono::milliseconds {memory->minimum_idle_ms},
            memory->minimum_holder_bytes);
        refusals = activeGpu->memoryRefusals();
    }
    if (!figures)
        return -ENODEV;
    std::array<kernarium::statistics::GraphicsHolderFigures,
        KERNARIUM_RUNTIME_GRAPHICS_HOLDERS> holders;
    const auto count = std::min<std::size_t>(
        figures->holders.size(), holders.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& holder = figures->holders[index];
        holders[index] = {
            .pid = holder.pid,
            .contexts = holder.contexts,
            .bytes = holder.bytes,
            .idleMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    holder.idle).count()),
            .name = holder.name,
        };
    }
    *memory = kernarium::statistics::graphicsReading({
        .budgetBytes = figures->budgetBytes,
        .usageBytes = figures->usageBytes,
        .contextBytes = figures->contextBytes,
        .memoryRefusals = refusals.value_or(0U),
        .guestReclaims = figures->guestReclaims,
        .holders = std::span {holders.data(), count},
    });
    return 0;
}

int runtimeReadGuestMemory(kernarium_runtime_guest_memory* memory)
{
    if (memory == nullptr || memory->struct_size < sizeof(*memory))
        return -EINVAL;
    std::optional<bool> scanout;
    {
        std::lock_guard lock(activeGpuMutex);
        if (activeGpu != nullptr)
            scanout = activeGpu->guestScanoutActive();
    }
    /* A quiesce holds the lifecycle mutex for seconds, and a request would
     * end the suspend it makes, so a busy mutex is not waited for. */
    std::unique_lock lock(emulatorLifecycleMutex, std::try_to_lock);
    const bool free = lock.owns_lock();
    if (const int refusal = kernarium::statistics::memoryRefusal(
            free,
            free && activeEmulator != nullptr,
            lifecycle.quiesced(),
            scanout))
        return refusal;
    uml_emulator_guest_memory figures {.struct_size = sizeof(figures)};
    const int result = uml_emulator_get_guest_memory(activeEmulator, &figures);
    lock.unlock();
    if (const int error = kernarium::statistics::guestMemoryResult(result))
        return error;
    memory->total_bytes = figures.total_bytes;
    memory->available_bytes = figures.available_bytes;
    return 0;
}

int runtimeSetBackgroundThrottle(int enabled)
{
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return -1;
    int changed = 0;
    for (mach_msg_type_number_t index = 0; index < count; ++index) {
        thread_extended_info_data_t info {};
        mach_msg_type_number_t infoCount = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(
                threads[index],
                THREAD_EXTENDED_INFO,
                reinterpret_cast<thread_info_t>(&info),
                &infoCount) == KERN_SUCCESS &&
            std::strncmp(info.pth_name, "uml-vcpu-", 9) == 0) {
            thread_background_policy_data_t policy {
                .priority = enabled != 0 ?
                    THREAD_BACKGROUND_POLICY_DARWIN_BG :
                    0,
            };
            if (thread_policy_set(
                    threads[index],
                    THREAD_BACKGROUND_POLICY,
                    reinterpret_cast<thread_policy_t>(&policy),
                    THREAD_BACKGROUND_POLICY_COUNT) == KERN_SUCCESS) {
                ++changed;
            }
        }
        (void)mach_port_deallocate(mach_task_self(), threads[index]);
    }
    (void)vm_deallocate(
        mach_task_self(),
        reinterpret_cast<vm_address_t>(threads),
        count * sizeof(thread_act_t));
    return changed;
}

#ifdef UML_APP_STORE_RUNTIME
/* An App Store runtime has no frame mirror; the table keeps the entry. */
int runtimeSetFrameSink(
    kernarium_runtime_frame_sink,
    void*,
    std::uint32_t,
    std::uint32_t)
{
    return -ENOTSUP;
}
#else
int runtimeSetFrameSink(
    kernarium_runtime_frame_sink sink,
    void* context,
    std::uint32_t maximumWidth,
    std::uint32_t framesPerSecond)
{
    if (sink == nullptr) {
        uml::gpu::setFrameTap(nullptr, nullptr);
        return 0;
    }
    if (maximumWidth == 0 || framesPerSecond == 0)
        return -EINVAL;
    uml::gpu::setFrameTap(
        sink,
        context,
        {.maximumWidth = maximumWidth, .framesPerSecond = framesPerSecond});
    return 0;
}
#endif

void runtimeSetConsoleOutputCallback(
    kernarium_runtime_console_output_callback callback,
    void* context)
{
    kernarium::console::output().setCallback(callback, context);
}

#ifdef UML_APP_STORE_RUNTIME
/* An App Store runtime takes no console input; the table keeps the entry. */
int runtimeSendConsoleInput(const void*, std::size_t)
{
    return -ENOTSUP;
}
#else
int runtimeSendConsoleInput(const void* buffer, std::size_t length)
{
    const int error = consoleInputs.send(buffer, length);
    return error == 0 ? 0 : -error;
}
#endif

int runtimeConnectVsock(uint32_t port, uint32_t timeoutMilliseconds)
{
    std::shared_ptr<uml::vsock::Endpoint> endpoint;
    {
        std::lock_guard lock(activeVsockMutex);
        endpoint = activeVsock;
    }
    if (endpoint == nullptr)
        return -ENODEV;
    /* A guest that suspended itself, as Android does once its display is off,
     * answers nothing until something wakes it, as network traffic does. The
     * wake does nothing to a guest the host quiesced, which then does not
     * answer either, and the open times out. It can wait on the guest's
     * console, which is why this call leaves the main thread. */
    {
        std::lock_guard lifecycle(emulatorLifecycleMutex);
        if (activeEmulator != nullptr)
            uml_emulator_wake(activeEmulator);
    }
    return endpoint->connect(
        port, std::chrono::milliseconds(timeoutMilliseconds));
}

void runtimeSetRestartCallback(
    kernarium_runtime_restart_callback callback,
    void* context)
{
    std::lock_guard lock(restartCallbackMutex);
    restartCallback = callback;
    restartContext = context;
}

const kernarium_runtime_api runtimeApi {
    .abi_version = KERNARIUM_RUNTIME_API_VERSION,
    .struct_size = sizeof(kernarium_runtime_api),
    .guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE,
    .guest_flavor = KERNARIUM_RUNTIME_GUEST_FLAVOR,
    .run = runtimeRun,
    .request_stop = runtimeRequestStop,
    .sync = runtimeSync,
    .quiesce = runtimeQuiesce,
    .resume = runtimeResume,
    .send_touch = runtimeSendTouch,
    .send_key = runtimeSendKey,
    .set_console_output_callback = runtimeSetConsoleOutputCallback,
    .send_console_input = runtimeSendConsoleInput,
    .set_gpu_hold = runtimeSetGpuHold,
    .set_frame_sink = runtimeSetFrameSink,
    .set_background_throttle = runtimeSetBackgroundThrottle,
    .set_page_cache_trimming = runtimeSetPageCacheTrimming,
    .set_microphone = runtimeSetMicrophone,
    .set_restart_callback = runtimeSetRestartCallback,
    .guest_display_active = runtimeGuestDisplayActive,
    .plan_gpu_budget = runtimePlanGpuBudget,
    .connect_vsock = runtimeConnectVsock,
    .read_statistics = runtimeReadStatistics,
    .read_guest_memory = runtimeReadGuestMemory,
    .cancel_touch = runtimeCancelTouch,
    .quiesce_resident = runtimeQuiesceResident,
    .read_graphics_memory = runtimeReadGraphicsMemory,
};

} // namespace

#if KERNARIUM_AUTOMATION_JOB_AT_START
/* An automation runtime tells the application whether the launch asked for the
 * background job to start with the guest (see KernariumRuntime.h). The
 * application finds this symbol only in a runtime built for automation. */
extern "C" KERNARIUM_RUNTIME_EXPORT int
kernarium_runtime_automation_job_at_start(void)
{
    using kernarium::automation::JobAtStart;
    const char* value =
        std::getenv(kernarium::automation::jobAtStartVariable);
    switch (kernarium::automation::parseJobAtStart(value)) {
    case JobAtStart::on:
        return 1;
    case JobAtStart::invalid:
        std::fprintf(
            stderr,
            "Kernarium: %s must be 0 or 1, not '%s'; no job is started\n",
            kernarium::automation::jobAtStartVariable,
            value);
        return 0;
    case JobAtStart::off:
        break;
    }
    return 0;
}

/* The delay an automation launch asked for before the guest's GPU is held in
 * the background (see KernariumRuntime.h). */
extern "C" KERNARIUM_RUNTIME_EXPORT int
kernarium_runtime_automation_hold_delay_ms(void)
{
    const char* value =
        std::getenv(kernarium::automation::holdDelayVariable);
    const auto delay = kernarium::automation::parseHoldDelayMs(value);
    if (!delay.valid) {
        std::fprintf(
            stderr,
            "Kernarium: %s must be a whole number of milliseconds from 0 to "
            "%d, not '%s'; the GPU is held at once\n",
            kernarium::automation::holdDelayVariable,
            kernarium::automation::holdDelayMaximumMs,
            value);
        return 0;
    }
    return delay.milliseconds;
}
#endif

extern "C" const kernarium_runtime_api* kernarium_runtime_get_api_v28(void)
{
    return &runtimeApi;
}
