// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_cpu.h"

#include "cpu/arm64/id_registers.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/guest_identity.hpp"
#include "platform/apple/host_features.hpp"
#include "runtime/context.hpp"

/* The HWCAPs follow the running engine's profile (guest_host_capabilities),
 * as its identification registers do. */
extern "C" unsigned long darwin_uml_guest_hwcap(void)
{
	return ::uml::cpu::arm64::detail::guest_hwcaps(
		::uml::runtime::current_guest_host_capabilities())[0];
}

/* The AT_HWCAP and AT_HWCAP2 words of a 32-bit task, from the AArch32
 * profile of the running engine's host, as the AArch64 words follow its
 * profile. A non-zero word is the single source of truth that this run
 * executes AArch32 EL0, and the first word always names the baseline. */
extern "C" unsigned long darwin_uml_guest_compat_hwcap(void)
{
	return ::uml::cpu::arm64::detail::guest_compat_hwcaps(
		::uml::runtime::current_guest_host_capabilities())[0];
}

extern "C" unsigned long darwin_uml_guest_compat_hwcap2(void)
{
	return ::uml::cpu::arm64::detail::guest_compat_hwcaps(
		::uml::runtime::current_guest_host_capabilities())[1];
}

/* Whether the run ignores the top byte of a data address, as every run does
 * unless the host passes --guest-no-top-byte-ignore. The kernel reads it at
 * boot to decide whether to offer the tagged-address ABI. Outside a run no
 * engine executes a guest, so nothing ignores a tag. */
extern "C" unsigned long darwin_uml_guest_top_byte_ignore(void)
{
	const auto *context = ::uml::runtime::current_context();
	return context != nullptr && context->top_byte_ignore() ? 1UL : 0UL;
}

extern "C" const char *darwin_uml_guest_cpu_model_name(void)
{
	/*
	 * The kernel prints what the emulator decides this CPU is, rather than
	 * naming a CPU of its own. An arm64 slice and the x86-64 userspace
	 * slice therefore report the same machine, and on x86-64 this is the
	 * same name the CPUID brand string carries.
	 */
	return ::uml::cpu::guest_cpu_model_name_storage;
}

extern "C" unsigned long darwin_uml_guest_hwcap2(void)
{
	return ::uml::cpu::arm64::detail::guest_hwcaps(
		::uml::runtime::current_guest_host_capabilities())[1];
}

/* Matches the register file every thread's execution state sizes. */
extern "C" unsigned long darwin_uml_guest_sme_vector_length(void)
{
#if DARWIN_UML_X86_64_USERSPACE_ONLY
	return 0;
#else
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	return ::uml::cpu::arm64::implements_sme(host) ? host.sme_svl_bytes : 0;
#endif
}

extern "C" int darwin_uml_enter_guest(void *registers, void *fp_registers,
				       unsigned long fp_size,
				       unsigned long generation,
				       unsigned long *tls)
{
	auto *context = ::uml::runtime::current_context();
	if (context == nullptr)
		return DARWIN_UML_EVENT_FATAL;
	const auto entry = context->guest_entry();
	return entry(registers, fp_registers, fp_size, generation, tls);
}
