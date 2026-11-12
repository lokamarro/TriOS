// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

extern "C" void
uml_arm64_guest_code_synchronize(std::uint64_t address) noexcept;
