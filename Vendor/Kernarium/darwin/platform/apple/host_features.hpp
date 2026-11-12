// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/host_capabilities.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/sysctl.h>

namespace uml::platform::apple {

enum class HostCapabilityResult : std::uint8_t {
	unsupported,
	supported,
	unavailable,
};

namespace detail {

[[nodiscard]] inline bool
native_jscvt_subnormal_flags_correct() noexcept
{
	constexpr std::uint64_t positive_subnormal = 1;
	constexpr std::array<std::uint64_t, 9> controls{
		0U, 1U, 2U, std::uint64_t{1} << 24U,
		2U | (std::uint64_t{1} << 24U),
		std::uint64_t{1} << 25U, std::uint64_t{1} << 26U,
		4U, 3U << 22U};
	std::uint64_t saved_fpcr = 0;
	std::uint64_t saved_fpsr = 0;
	std::uint64_t saved_nzcv = 0;
	asm volatile(
		"mrs %[saved_fpcr], fpcr\n\t"
		"mrs %[saved_fpsr], fpsr\n\t"
		"mrs %[saved_nzcv], nzcv\n\t"
		: [saved_fpcr] "=&r"(saved_fpcr),
		  [saved_fpsr] "=&r"(saved_fpsr),
		  [saved_nzcv] "=&r"(saved_nzcv)
		:
		: "memory");
	bool correct = true;
	for (const auto control : controls) {
		std::uint64_t flags = 0;
		asm volatile(
			"msr fpcr, %[control]\n\t"
			"msr fpsr, xzr\n\t"
			"msr nzcv, xzr\n\t"
			"fmov d0, %[input]\n\t"
			".inst 0x1e7e0000\n\t"
			"mrs %[flags], nzcv\n\t"
			: [flags] "=r"(flags)
			: [control] "r"(control),
			  [input] "r"(positive_subnormal)
			: "x0", "v0", "cc", "memory");
		correct = correct && (flags & 0xf000'0000U) == 0U;
	}
	asm volatile(
		"msr nzcv, %[saved_nzcv]\n\t"
		"msr fpcr, %[saved_fpcr]\n\t"
		"msr fpsr, %[saved_fpsr]\n\t"
		:
		: [saved_nzcv] "r"(saved_nzcv),
		  [saved_fpcr] "r"(saved_fpcr),
		  [saved_fpsr] "r"(saved_fpsr)
		: "cc", "memory");
	return correct;
}

} // namespace detail

[[nodiscard]] inline HostCapabilityResult query_host_arm_feature_sysctl(
	const char *name) noexcept
{
	int enabled = 0;
	std::size_t size = sizeof(enabled);
	if (sysctlbyname(name, &enabled, &size, nullptr, 0) != 0 ||
	    size != sizeof(enabled))
		return HostCapabilityResult::unavailable;
	return enabled != 0 ? HostCapabilityResult::supported
			    : HostCapabilityResult::unsupported;
}

[[nodiscard]] inline bool query_host_arm_feature(
	const char *name, bool architectural_minimum = false) noexcept
{
	const auto queried = query_host_arm_feature_sysctl(name);
	if (queried != HostCapabilityResult::unavailable)
		return queried == HostCapabilityResult::supported;
	/*
	 * FP and Advanced SIMD are required by Apple's arm64 process ABI.
	 * Sandboxes may deny sysctl while those instructions remain available.
	 * Every optional extension fails closed.
	 */
	return architectural_minimum;
}

[[nodiscard]] inline bool query_host_arm_feature(
	ArmFeature feature) noexcept
{
	if (feature == ArmFeature::floating_point)
		return query_host_arm_feature(
			"hw.optional.floatingpoint", true);
	if (feature == ArmFeature::advanced_simd)
		return query_host_arm_feature("hw.optional.neon", true);

	const auto label =
		arm_feature_labels[static_cast<std::size_t>(feature)];
	std::array<char, 64> name{};
	const int length = std::snprintf(
		name.data(), name.size(), "hw.optional.arm.FEAT_%.*s",
		static_cast<int>(label.size()), label.data());
	if (length <= 0 || static_cast<std::size_t>(length) >= name.size())
		return false;

	return query_host_arm_feature_sysctl(name.data()) ==
		HostCapabilityResult::supported;
}

/* Whether the virtual CPU implements SME on a host that has the profile, at
 * any streaming vector length: on by default, now that both engines
 * implement the profile (cpu/engine_factory.cpp checks that). Setting
 * DARWIN_UML_GUEST_SME to exactly "0" forces it off. It is a run policy, not
 * a test hook, and it never enables anything the host lacks. */
enum class GuestSmePolicy : std::uint8_t {
	off,
	on,
};

inline constexpr const char *guest_sme_policy_variable = "DARWIN_UML_GUEST_SME";

[[nodiscard]] inline GuestSmePolicy parse_guest_sme_policy(
	const char *value) noexcept
{
	return value != nullptr && std::strcmp(value, "0") == 0 ?
		GuestSmePolicy::off : GuestSmePolicy::on;
}

/* Read once, so every engine, the identification registers and the kernel
 * bridge see one answer for the whole process. */
[[nodiscard]] inline GuestSmePolicy guest_sme_policy() noexcept
{
#ifdef UML_APP_STORE_RUNTIME
	static const GuestSmePolicy policy = parse_guest_sme_policy(nullptr);
#else
	static const GuestSmePolicy policy =
		parse_guest_sme_policy(std::getenv(guest_sme_policy_variable));
#endif
	return policy;
}

/* hw.optional.arm.sme_max_svl_b: the host's streaming vector length in
 * bytes. Zero when the sysctl is missing, has an unexpected size, or holds
 * a value no SME implementation can have (a power of two from 16 to 256). */
[[nodiscard]] inline std::uint16_t query_host_sme_svl_bytes() noexcept
{
	std::uint64_t value = 0;
	std::size_t size = sizeof(value);
	/* An int today; either width fills the low bytes of the zeroed
	 * little-endian value. */
	if (sysctlbyname("hw.optional.arm.sme_max_svl_b", &value, &size,
		    nullptr, 0) != 0 ||
	    (size != sizeof(std::uint32_t) && size != sizeof(value)))
		return 0;
	if (value < 16 || value > 256 || (value & (value - 1)) != 0)
		return 0;
	return static_cast<std::uint16_t>(value);
}

/* The SME tokens, which only the guest-SME policy lets the prober report. */
inline constexpr std::array sme_policy_features{ArmFeature::sme,
	ArmFeature::sme2, ArmFeature::sme_f64f64, ArmFeature::sme_i16i64};

/* Platform probing ends here. Runtime copies this immutable neutral profile
 * into EngineConfiguration; CPU translation code never depends on sysctl or
 * any Apple header. With the guest-SME policy off, or without a valid
 * streaming vector length, no SME token is reported and sme_svl_bytes is 0;
 * sme_hardware reports FEAT_SME either way. Any SVE token the host's own
 * sysctls claim is cleared: the virtual CPU's profile is not the host's.
 */
[[nodiscard]] inline ::uml::platform::HostCapabilities probe_host_capabilities(
	GuestSmePolicy sme_policy) noexcept
{
	::uml::platform::HostCapabilities result;
	for (std::size_t index = 0; index < arm_feature_count; ++index)
		result.supported.set(index, query_host_arm_feature(
			static_cast<ArmFeature>(index)));
	result.floating_point = result.has(ArmFeature::floating_point);
	result.advanced_simd = result.has(ArmFeature::advanced_simd);
	result.fp16 = result.has(ArmFeature::fp16);
	result.cssc = result.has(ArmFeature::cssc);
	result.aes = result.has(ArmFeature::aes);
	result.pmull = result.has(ArmFeature::pmull);
	result.sha1 = result.has(ArmFeature::sha1);
	result.sha2 = result.has(ArmFeature::sha256);
	result.crc32 = result.has(ArmFeature::crc32);
	result.sha3 = result.has(ArmFeature::sha3);
	result.sha512 = result.has(ArmFeature::sha512);
	if (result.has(ArmFeature::jscvt))
		result.jscvt_subnormal_flags_correct =
			detail::native_jscvt_subnormal_flags_correct();
	result.sme_hardware = result.has(ArmFeature::sme);
	if (sme_policy == GuestSmePolicy::on && result.sme_hardware)
		result.sme_svl_bytes = query_host_sme_svl_bytes();
	if (result.sme_svl_bytes == 0)
		for (const auto feature : sme_policy_features)
			result.supported.reset(static_cast<std::size_t>(feature));
	return ::uml::platform::without_sve(result);
}

[[nodiscard]] inline const ::uml::platform::HostCapabilities &
probed_host_capabilities() noexcept
{
	static const ::uml::platform::HostCapabilities features =
		probe_host_capabilities(guest_sme_policy());
	return features;
}

} // namespace uml::platform::apple
