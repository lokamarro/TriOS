// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_bfloat_arithmetic_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

using uml::testing::require;

} // namespace arm64_bfloat_arithmetic_test
