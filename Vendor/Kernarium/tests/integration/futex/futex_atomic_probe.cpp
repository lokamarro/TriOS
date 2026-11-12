// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
void require(bool condition, const char* message)
{
    if (condition)
        return;
    std::fprintf(stderr, "futex regression: %s (errno=%d)\n", message, errno);
    std::_Exit(1);
}

void wakeOperationRace()
{
    alignas(4) std::uint32_t counter {};
    std::uint32_t waitWord {};
    std::atomic_bool ready {};
    constexpr std::uint32_t iterations = 100000;
    std::thread userspace([&] {
        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (std::uint32_t i = 0; i < iterations; ++i) {
            __atomic_fetch_add(&counter, 1U, __ATOMIC_SEQ_CST);
            if ((i & 31U) == 0)
                std::this_thread::yield();
        }
    });
    ready.store(true, std::memory_order_release);
    for (std::uint32_t i = 0; i < iterations; ++i) {
        require(syscall(SYS_futex, &waitWord, FUTEX_WAKE_OP_PRIVATE, 0, 0,
                    &counter, FUTEX_OP(FUTEX_OP_ADD, 1, FUTEX_OP_CMP_EQ, 0)) == 0,
                "FUTEX_WAKE_OP failed");
    }
    userspace.join();
    require(counter == iterations * 2, "kernel/userspace atomic additions lost updates");
    std::printf("PASS concurrent userspace/FUTEX_WAKE_OP additions: %u\n", counter);
}

void priorityInheritanceRace()
{
    pthread_mutexattr_t attributes;
    pthread_mutex_t mutex;
    require(pthread_mutexattr_init(&attributes) == 0, "mutex attributes");
    require(pthread_mutexattr_setprotocol(&attributes, PTHREAD_PRIO_INHERIT) == 0,
            "priority inheritance attributes");
    require(pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST) == 0,
            "robust mutex attributes");
    require(pthread_mutex_init(&mutex, &attributes) == 0, "PI mutex initialization");
    pthread_mutexattr_destroy(&attributes);
    std::atomic_bool ready {};
    std::uint32_t counter {};
    std::array<std::thread, 4> workers;
    constexpr std::uint32_t iterations = 10000;
    for (auto& worker : workers) {
        worker = std::thread([&] {
            while (!ready.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (std::uint32_t i = 0; i < iterations; ++i) {
                timespec deadline {};
                require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "read deadline");
                deadline.tv_sec += 5;
                require(pthread_mutex_timedlock(&mutex, &deadline) == 0,
                        "robust PI mutex acquisition timed out or failed");
                const auto previous = counter;
                if ((i & 31U) == 0)
                    std::this_thread::yield();
                counter = previous + 1;
                require(pthread_mutex_unlock(&mutex) == 0, "PI mutex unlock failed");
            }
        });
    }
    ready.store(true, std::memory_order_release);
    for (auto& worker : workers)
        worker.join();
    require(counter == iterations * workers.size(), "PI mutex lost mutual exclusion");
    require(pthread_mutex_destroy(&mutex) == 0, "PI mutex left owned or with waiters");
    std::printf("PASS robust PI mutex contention: %u acquisitions\n", counter);
}
} // namespace

int main()
{
    alarm(90);
    wakeOperationRace();
    priorityInheritanceRace();
    std::puts("UML_FUTEX_ATOMIC: PASS");
    return 0;
}
