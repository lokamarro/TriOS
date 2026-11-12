// SPDX-License-Identifier: GPL-2.0-or-later
/* The virtual CPU implements no SVE outside streaming mode and so presents
 * none: the prober reports no SVE token whatever the host claims, neither
 * engine presents one, ID_AA64PFR0_EL1.SVE and ID_AA64ZFR0_EL1 read zero,
 * the SVE bits of AT_HWCAP and AT_HWCAP2 are clear, and the bridge carries no
 * SVE state or vector length. arm64-advertised-features pins the words and
 * registers of named hosts; this checks the rules. */
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/engine_factory.hpp"
#include "cpu/host_capabilities.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"
#include "uml_darwin_cpu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>

namespace {

using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;
using ::uml::testing::require;
namespace apple = ::uml::platform::apple;
namespace arm64 = ::uml::cpu::arm64;
namespace detail = ::uml::cpu::arm64::detail;

/* AT_HWCAP's SVE bit, and AT_HWCAP2's SVE2, SVEAES, SVEPMULL, SVESHA3,
 * SVEI8MM and SVEBF16 bits (arm64 Linux's uapi hwcap.h). */
constexpr unsigned long sve_hwcap = 1UL << 22;
constexpr unsigned long sve_hwcap2 = (1UL << 1) | (1UL << 2) | (1UL << 3) |
	(1UL << 5) | (1UL << 9) | (1UL << 12);

constexpr std::array sve_tokens{ArmFeature::sve, ArmFeature::sve2,
	ArmFeature::sve_aes, ArmFeature::sve_pmull128, ArmFeature::sve_sha3};

/* A host that claims every part of the removed profile and the AdvSIMD
 * forms it would have been lowered onto. */
[[nodiscard]] HostCapabilities claiming_sve()
{
	HostCapabilities host;
	for (const auto bit : {ArmFeature::fp16, ArmFeature::dotprod,
		     ArmFeature::aes, ArmFeature::pmull, ArmFeature::sha3,
		     ArmFeature::i8mm, ArmFeature::bf16})
		host.supported.set(static_cast<std::size_t>(bit));
	for (const auto token : sve_tokens)
		host.supported.set(static_cast<std::size_t>(token));
	host.fp16 = true;
	return host;
}

[[nodiscard]] bool reports_any_sve(const HostCapabilities &host)
{
	for (const auto token : sve_tokens)
		if (host.has(token))
			return true;
	return false;
}

[[nodiscard]] std::uint64_t read_id(const HostCapabilities &host,
	std::uint32_t id_register)
{
	const ::uml::cpu::HostCapabilitiesScope scope{host};
	require(detail::emulated_id_register(id_register));
	return detail::emulated_id_register_value(id_register);
}

/* The prober reports no SVE token: DARWIN_UML_GUEST_SVE no longer exists,
 * and a token the host's own sysctls claim is not the virtual CPU's. */
void prober_reports_no_sve()
{
	require(::setenv("DARWIN_UML_GUEST_SVE", "1", 1) == 0);
	for (const auto policy :
	     {apple::GuestSmePolicy::off, apple::GuestSmePolicy::on})
		require(!reports_any_sve(apple::probe_host_capabilities(policy)));
	require(!reports_any_sve(apple::probed_host_capabilities()));

	const auto claimed = claiming_sve();
	require(reports_any_sve(claimed));
	const auto cleared = ::uml::platform::without_sve(claimed);
	require(!reports_any_sve(cleared));
	auto expected = claimed;
	for (const auto token : sve_tokens)
		expected.supported.reset(static_cast<std::size_t>(token));
	require(cleared == expected, "without_sve changed more than the tokens");
	require(::uml::platform::without_sve(cleared) == cleared);
}

/* Neither engine presents an SVE token, whatever the host it is given, and
 * the rest of the presented profile is the SME profile's rule alone. */
void engines_present_no_sve()
{
	const auto claimed = claiming_sve();
	for (const auto kind : {::uml::cpu::EngineKind::gadgets,
		     ::uml::cpu::EngineKind::jit}) {
		const auto presented =
			::uml::cpu::engine_host_capabilities(kind, claimed);
		require(!reports_any_sve(presented));
		require(presented == arm64::sme_host_for_support(
			::uml::platform::without_sve(claimed),
			kind == ::uml::cpu::EngineKind::jit ?
				arm64::jit::sme_support :
				arm64::gadgets::sme_support));
	}
}

/* ID_AA64PFR0_EL1.SVE and ID_AA64ZFR0_EL1 are zero and no SVE HWCAP bit is
 * set, for a host that claims the whole removed profile as for one that
 * claims nothing. */
void identification_has_no_sve()
{
	for (const auto &host : {HostCapabilities{}, claiming_sve()}) {
		require(read_id(host, detail::id_aa64zfr0_el1) == 0);
		require((read_id(host, detail::id_aa64pfr0_el1) >> 32 & 15U) == 0);
		const auto hwcaps = detail::guest_hwcaps(host);
		require((hwcaps[0] & sve_hwcap) == 0 &&
			(hwcaps[1] & sve_hwcap2) == 0);
	}
}

/* The bridge carries no SVE state and imports no SVE query, and userspace
 * learns of SVE from the HWCAPs, which say none. */
void guest_kernel_is_told_no_sve()
{
	require((darwin_uml_guest_hwcap() & sve_hwcap) == 0);
	require((darwin_uml_guest_hwcap2() & sve_hwcap2) == 0);
}

} // namespace

int main()
{
	prober_reports_no_sve();
	engines_present_no_sve();
	identification_has_no_sve();
	guest_kernel_is_told_no_sve();
	return 0;
}
