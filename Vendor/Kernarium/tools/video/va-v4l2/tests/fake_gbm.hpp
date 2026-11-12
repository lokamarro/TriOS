// SPDX-License-Identifier: GPL-2.0-or-later
// A stand-in for Mesa's DRI gbm backend: it allocates the single-plane colour
// formats Mesa does, as memory files, and counts what it is asked, so the
// planar adapter and the module that carries it can be tested without a GPU.
#pragma once

#include <cstdint>
#include <vector>

#include "../gbm/abi.hpp"

namespace uml::gbm::test {

struct FakeCreate {
    unsigned width;
    unsigned height;
    std::uint32_t format;
    std::uint32_t usage;
    unsigned modifiers;
};

struct FakeState {
    // Buffer objects and devices that exist now.
    int liveBos {};
    int liveDevices {};
    std::vector<FakeCreate> creates;
    // The backend allocates NV12 itself, as a newer Mesa may.
    bool nativeNv12 {};
    // The backend refuses this one format (allocation and support query).
    std::uint32_t refuseFormat {};
    // Allocation of this format fails with ENOSPC, after the query said yes.
    std::uint32_t failCreate {};
    // The version the fake backend speaks.
    std::uint32_t backendVersion {abi::kBackendVersion};
};

// The state of the copy of the fake linked into this object.
FakeState& fakeState();
// The backend, as libgbm gets it from gbmint_get_backend().
const abi::Backend* fakeBackend();

// What a buffer object of the fake holds, for a test that wants to look.
int fakeBoFile(abi::Bo* bo);

}

extern "C" {
// The same fake built as a module (dri_gbm.so beside the module under test)
// also exports gbmint_get_backend(), which libgbm calls.
uml::gbm::test::FakeState* uml_fake_gbm_state();
}
