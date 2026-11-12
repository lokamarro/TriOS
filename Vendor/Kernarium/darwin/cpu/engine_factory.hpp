/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/engine.hpp"

#include <cstddef>

namespace uml::cpu {

[[nodiscard]] std::unique_ptr<CpuEngine> make_cpu_engine(
	EngineKind kind, EngineConfiguration configuration = {});

[[nodiscard]] bool cpu_engine_available(EngineKind kind) noexcept;

/* The host an engine of `kind` presents the virtual CPU for: `host` less
 * the parts of the SME profile the engine does not implement
 * (arm64::sme_host_for_support) and less every non-streaming SVE token
 * (platform::without_sve), since no engine implements SVE. The runtime
 * configures the engine and derives the guest's HWCAPs from it. */
[[nodiscard]] HostCapabilities engine_host_capabilities(
	EngineKind kind, HostCapabilities host) noexcept;

/* Publishes the whole-process budget for translated-code caches and the
 * private per-executor cap. The first valid configuration owns process-wide
 * geometry; identical repeats succeed and incompatible later runs fail. See
 * arm64/jit/jit_cache_geometry.hpp for what the budget buys.
 */
[[nodiscard]] bool set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept;

/* Serializes JIT-capable emulator runs before any guest vCPU can allocate an
 * executor slot. Other engine kinds do not take this process-wide session.
 */
[[nodiscard]] bool acquire_translation_cache_run_session() noexcept;
void release_translation_cache_run_session() noexcept;

/* Parses a byte count with an optional K/M/G suffix. Zero if unparsable. */
[[nodiscard]] std::size_t parse_translation_cache_quantity(
	const char *text) noexcept;

/* Materialize any lazily resident architectural state before runtime exports
 * a guest task frame. Runtime must not know which concrete engine held it. */
void materialize_architectural_state(arm64::CpuState &cpu) noexcept;

} // namespace uml::cpu
