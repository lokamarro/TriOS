/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine_factory.hpp"

#include "cpu/arm64/engine_adapters.hpp"
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <utility>

namespace uml::cpu {

/* The guest-SME policy is on by default (platform/apple/host_features.hpp),
 * so a guest on an SME host is told of every form of the profile but SME2's,
 * which neither engine may then take as illegal. */
static_assert(arm64::gadgets::sme_support >= arm64::SmeSupport::complete &&
	arm64::jit::sme_support >= arm64::SmeSupport::complete);

std::unique_ptr<CpuEngine> make_cpu_engine(
	EngineKind kind, EngineConfiguration configuration)
{
	switch (kind) {
	case EngineKind::gadgets:
		return arm64::detail::make_gadget_engine(configuration);
	case EngineKind::jit:
		return arm64::detail::make_jit_engine(configuration);
	}
	return nullptr;
}

bool cpu_engine_available(EngineKind kind) noexcept
{
	switch (kind) {
	case EngineKind::gadgets:
		return true;
	case EngineKind::jit:
		return arm64::detail::native_jit_engine_available();
	}
	return false;
}

HostCapabilities engine_host_capabilities(
	EngineKind kind, HostCapabilities host) noexcept
{
	const bool jit = kind == EngineKind::jit;
	return ::uml::platform::without_sve(
		arm64::sme_host_for_support(std::move(host),
			jit ? arm64::jit::sme_support :
			      arm64::gadgets::sme_support));
}

void materialize_architectural_state(arm64::CpuState &cpu) noexcept
{
	arm64::restore_native_fp_environment(cpu);
}

bool set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept
{
	return arm64::set_jit_cache_budget(
		total_bytes, executors, per_executor_bytes);
}

bool acquire_translation_cache_run_session() noexcept
{
	return arm64::acquire_jit_cache_run_session();
}

void release_translation_cache_run_session() noexcept
{
	arm64::release_jit_cache_run_session();
}

std::size_t parse_translation_cache_quantity(const char *text) noexcept
{
	return arm64::parse_byte_quantity(text);
}

} // namespace uml::cpu
