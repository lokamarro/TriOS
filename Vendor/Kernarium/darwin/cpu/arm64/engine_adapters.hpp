/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/engine.hpp"

#include <memory>

namespace uml::cpu::arm64::detail {

[[nodiscard]] std::unique_ptr<CpuEngine> make_gadget_engine(
	EngineConfiguration configuration);
[[nodiscard]] std::unique_ptr<CpuEngine> make_jit_engine(
	EngineConfiguration configuration);
[[nodiscard]] bool native_jit_engine_available() noexcept;

} // namespace uml::cpu::arm64::detail
