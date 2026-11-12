/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu_budget_plan.hpp"

#include "execution_engine.hpp"
#include "guest_memory_argument.hpp"
#include "options.hpp"
#include "platform/host_environment.hpp"

#include <cstdio>
#include <limits>
#include <string>

namespace uml::emulator {

namespace {

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;

} // namespace

std::size_t configured_option_bytes(
	const std::vector<std::string> &arguments, std::string_view option)
{
	std::size_t bytes = 0;
	for (const auto &argument : arguments)
		if (argument.starts_with(option))
			bytes = ::uml::runtime::parse_translation_cache_quantity(
				argument.c_str() + option.size());
	return bytes;
}

std::string default_guest_memory_argument(
	const std::vector<std::string> &arguments, std::string_view fallback)
{
	auto limit = configured_option_bytes(arguments, "--memory-limit=");
	if (limit == 0)
		limit = static_cast<std::size_t>(
			::uml::platform::configured_process_memory_limit_bytes());
	if (limit == 0)
		return std::string{fallback};
	return "mem=" + std::to_string(
		::uml::runtime::guest_memory::default_guest_memory_mib(
			limit, launch_guest_cpu_count(arguments))) + "M";
}

bool parse_gpu_budget_bytes(
	std::string_view text, std::uint64_t &bytes) noexcept
{
	std::uint64_t scale = 1;
	if (!text.empty()) {
		switch (text.back()) {
		case 'k': case 'K': scale = 1ULL << 10; break;
		case 'm': case 'M': scale = 1ULL << 20; break;
		case 'g': case 'G': scale = 1ULL << 30; break;
		default: break;
		}
		if (scale != 1)
			text.remove_suffix(1);
	}
	if (text.empty())
		return false;
	std::uint64_t count = 0;
	for (const char character : text) {
		if (character < '0' || character > '9')
			return false;
		const auto digit = static_cast<std::uint64_t>(character - '0');
		if (count > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
			return false;
		count = count * 10U + digit;
	}
	if (count > std::numeric_limits<std::uint64_t>::max() / scale)
		return false;
	bytes = count * scale;
	return true;
}

GpuBudgetPlan plan_gpu_budget(std::uint64_t guest_memory,
	std::size_t memory_limit_option, std::size_t gadget_cache_option,
	unsigned guest_cpus, const GpuBudgetParameters &parameters)
{
	/* The run publishes the option the same way before it derives the cache
	 * budget; an option left out leaves the variable, then the host's own
	 * limit, in force. */
	if (memory_limit_option != 0)
		::uml::runtime::set_process_memory_limit(memory_limit_option);
	GpuBudgetPlan plan;
	plan.process_limit = ::uml::platform::process_memory_limit_bytes();
	plan.guest_memory = guest_memory;
	plan.guest_cpus = guest_cpus;
	plan.parameters = parameters;
	plan.engine = ::uml::runtime::guest_memory::engine_memory(
		::uml::runtime::gadget_cache_budget_for(
			static_cast<std::size_t>(guest_memory),
			gadget_cache_option, guest_cpus),
		guest_cpus, parameters.engine_rest);
	plan.budget = ::uml::platform::compute_gpu_budget({
		.process_limit = plan.process_limit,
		.guest_memory = plan.guest_memory,
		.application = parameters.application,
		.engine = plan.engine,
		.headroom = parameters.headroom,
	});
	return plan;
}

GpuBudgetPlan plan_gpu_budget(const std::vector<std::string> &arguments,
	const GpuBudgetParameters &parameters)
{
	return plan_gpu_budget(configured_guest_memory_bytes(arguments),
		configured_option_bytes(arguments, "--memory-limit="),
		configured_option_bytes(arguments, "--gadget-cache-budget="),
		configured_guest_cpu_count(arguments), parameters);
}

std::string describe_gpu_budget_shortfall(const GpuBudgetPlan &plan)
{
	char text[512];
	std::snprintf(text, sizeof(text),
		"warning: the process limit of %llu MiB leaves %llu MiB for the "
		"GPU after the guest's %llu MiB, %llu MiB of gadget cache and "
		"other guest-side memory, %llu MiB for the application and %llu "
		"MiB of headroom, below the %llu MiB it needs; the GPU budget "
		"is %llu MiB and the process is likely to be ended",
		static_cast<unsigned long long>(plan.process_limit / mebibyte),
		static_cast<unsigned long long>(plan.budget.available / mebibyte),
		static_cast<unsigned long long>(plan.guest_memory / mebibyte),
		static_cast<unsigned long long>(plan.engine / mebibyte),
		static_cast<unsigned long long>(
			plan.parameters.application / mebibyte),
		static_cast<unsigned long long>(plan.parameters.headroom / mebibyte),
		static_cast<unsigned long long>(
			::uml::platform::gpu_budget_floor_bytes / mebibyte),
		static_cast<unsigned long long>(plan.budget.bytes / mebibyte));
	return text;
}

} // namespace uml::emulator
