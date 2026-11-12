// SPDX-License-Identifier: GPL-2.0-or-later

#include "uml/emulator.h"
#include "emulator_test_support.hpp"
#include "require.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

extern "C" int darwin_uml_net_open(
    unsigned char mac[6], unsigned int* mtu);
extern "C" int darwin_uml_net_irq_fd(void);
extern "C" int darwin_uml_net_read(void* buffer, unsigned long capacity);
extern "C" int darwin_uml_net_write(
    const void* buffer, unsigned long length);

namespace {

struct CallbackFailure final {};

using uml::testing::make_emulator;
using uml::testing::require;

std::atomic_uint kernelEntries;

int kernelThatFailsOnce(int, char* const[], char* const[])
{
    if (kernelEntries.fetch_add(1, std::memory_order_relaxed) == 0)
        throw CallbackFailure{};
    return 47;
}

void kernelExceptionsBecomeApiErrorsAndLeaveARetryableHandle()
{
    kernelEntries.store(0, std::memory_order_relaxed);
    auto emulator = make_emulator(kernelThatFailsOnce);
    uml_emulator_capabilities before{
        .struct_size = sizeof(uml_emulator_capabilities),
    };
    require(uml_emulator_get_capabilities(emulator.get(), &before) ==
            UML_EMULATOR_OK,
        "could not query a fresh C API boundary handle");
    const char* arguments[]{"uml-c-api-boundary"};
    require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) ==
            -UML_EMULATOR_RUNTIME_ERROR,
        "a kernel exception escaped the public C run boundary");

    unsigned char mac[6]{};
    unsigned int mtu{};
    require(darwin_uml_net_open(mac, &mtu) == -ENETDOWN,
        "an exceptional run retained its public network route");
    require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 47,
        "a handle could not retry after a contained kernel exception");

    uml_emulator_capabilities capabilities{
        .struct_size = sizeof(uml_emulator_capabilities),
    };
    require(uml_emulator_get_capabilities(emulator.get(), &capabilities) ==
                UML_EMULATOR_OK &&
            capabilities.abi_version == before.abi_version &&
            capabilities.userspace_execution == before.userspace_execution &&
            capabilities.can_run == before.can_run &&
            capabilities.sandbox_safe == before.sandbox_safe &&
            capabilities.platform == before.platform,
        "a contained run exception corrupted public handle capabilities");
}

int throwingOpen(void*, unsigned char[6], unsigned int*)
{
    throw CallbackFailure{};
}

int throwingIrq(void*)
{
    throw CallbackFailure{};
}

int throwingRead(void*, void*, std::size_t)
{
    throw CallbackFailure{};
}

int throwingWrite(void*, const void*, std::size_t)
{
    throw CallbackFailure{};
}

int kernelThatSamplesFailingNetwork(int, char* const[], char* const[])
{
    unsigned char mac[6]{};
    unsigned int mtu{};
    return darwin_uml_net_open(mac, &mtu) == -EIO &&
            darwin_uml_net_irq_fd() == -1 &&
            darwin_uml_net_read(nullptr, 0) == -EIO &&
            darwin_uml_net_write(nullptr, 0) == -EIO
        ? 53
        : 91;
}

void networkExceptionsBecomeDeviceErrorsAndLeaveConsistentState()
{
    const uml_emulator_network_callbacks callbacks{
        .struct_size = sizeof(uml_emulator_network_callbacks),
        .open = throwingOpen,
        .irq_fd = throwingIrq,
        .read = throwingRead,
        .write = throwingWrite,
    };
    auto emulator =
        make_emulator(kernelThatSamplesFailingNetwork, &callbacks);
    const char* arguments[]{"uml-c-api-network-boundary"};
    require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 53,
        "a network callback exception escaped its C bridge");
    require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 53,
        "network callback containment poisoned a later run");
}

void nullSafeOperationsRemainNoThrow()
{
    uml_emulator_destroy(nullptr);
    uml_emulator_request_stop(nullptr);
    uml_emulator_capabilities capabilities{
        .struct_size = sizeof(uml_emulator_capabilities),
    };
    require(uml_emulator_get_capabilities(nullptr, &capabilities) ==
            -UML_EMULATOR_INVALID_ARGUMENT,
        "a null capability handle was accepted");
}

} // namespace

int main()
{
    try {
        kernelExceptionsBecomeApiErrorsAndLeaveARetryableHandle();
        networkExceptionsBecomeDeviceErrorsAndLeaveConsistentState();
        nullSafeOperationsRemainNoThrow();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(
            stderr, "emulator C API boundary test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
