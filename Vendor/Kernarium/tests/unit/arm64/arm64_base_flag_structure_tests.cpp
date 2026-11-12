// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace uml::emulator::tests::base_flag {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::uint32_t cfinv_encoding = 0xd500'401fU;
inline constexpr std::uint32_t xaflag_encoding = 0xd500'403fU;
inline constexpr std::uint32_t axflag_encoding = 0xd500'405fU;

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t encode_rmif(
	unsigned source, unsigned rotation, unsigned mask) noexcept
{
	return 0xba00'0400U | ((rotation & 63U) << 15U) |
		((source & 31U) << 5U) | (mask & 15U);
}

[[nodiscard]] constexpr std::uint32_t encode_setf(
	bool sixteen, unsigned source) noexcept
{
	return 0x3a00'080dU | (sixteen ? 0x4000U : 0U) |
		((source & 31U) << 5U);
}

} // namespace uml::emulator::tests::base_flag

namespace a64 = uml::testing::a64;

namespace uml::emulator::tests::base_flag {
namespace {

namespace gadgets = arm64::gadgets;

extern "C" {
void uml_arm64_gadget_cfinv();
void uml_arm64_gadget_axflag();
void uml_arm64_gadget_axflag_native();
void uml_arm64_gadget_xaflag();
void uml_arm64_gadget_xaflag_native();
void uml_arm64_gadget_rmif_fast();
void uml_arm64_gadget_rmif_zero();
void uml_arm64_gadget_setf_fast_8();
void uml_arm64_gadget_setf_fast_16();
void uml_arm64_gadget_setf_native_8();
void uml_arm64_gadget_setf_native_16();
}

using uml::emulator::tests::address;

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction,
	bool force_software = false)
{
	constexpr std::array terminal{std::uint32_t{0xd400'0001U}};
	const std::array code{instruction, terminal.front()};
	gadgets::BuildOptions options{};
	options.force_software_flags = force_software;
	return gadgets::Builder{}.build(engine_test_code_base, code, {},
		std::pmr::get_default_resource(), {}, options);
}

[[nodiscard]] unsigned conditional_branches(std::uintptr_t entry)
{
	const auto shape = a64::body_shape(entry, {.window = 64U});
	if (!shape.reached_dispatch)
		fail("flag gadget did not terminate in its inspection window");
	return shape.conditional_branches;
}

void fixed_translation()
{
	const auto cfinv = build(cfinv_encoding);
	require(cfinv.complete && cfinv.program.words().size() == 3U &&
			cfinv.program.words().front() ==
				address(uml_arm64_gadget_cfinv),
		"CFINV did not select its minimal software gadget");

	const auto ax_software = build(axflag_encoding, true);
	const auto xa_software = build(xaflag_encoding, true);
	require(ax_software.complete &&
			ax_software.program.words().front() ==
				address(uml_arm64_gadget_axflag),
		"forced AXFLAG software gadget was not selected");
	require(xa_software.complete &&
			xa_software.program.words().front() ==
				address(uml_arm64_gadget_xaflag),
		"forced XAFLAG software gadget was not selected");

	const bool native =
		::uml::platform::apple::probed_host_capabilities().has(ArmFeature::flag_m2);
	const auto ax_default = build(axflag_encoding);
	const auto xa_default = build(xaflag_encoding);
	require(ax_default.program.words().front() ==
			address(native ? uml_arm64_gadget_axflag_native :
				uml_arm64_gadget_axflag),
		"AXFLAG did not resolve host support during translation");
	require(xa_default.program.words().front() ==
			address(native ? uml_arm64_gadget_xaflag_native :
				uml_arm64_gadget_xaflag),
		"XAFLAG did not resolve host support during translation");

	const std::array entries{
		address(uml_arm64_gadget_cfinv),
		address(uml_arm64_gadget_axflag),
		address(uml_arm64_gadget_xaflag),
	};
	for (const auto entry : entries)
		require(conditional_branches(entry) == 0U,
			"portable fixed flag gadget retained a conditional branch");
	if (native) {
		require(conditional_branches(
				address(uml_arm64_gadget_axflag_native)) == 0U &&
				conditional_branches(
					address(uml_arm64_gadget_xaflag_native)) == 0U,
			"native fixed flag gadget retained a conditional branch");
	}
}

void rmif_translation()
{
	const auto nop = build(0xd503'201fU);
	const auto no_mask = build(encode_rmif(7, 29, 0));
	require(no_mask.complete && no_mask.program.words().size() == 3U &&
			no_mask.program.words().front() ==
				nop.program.words().front(),
		"zero-mask RMIF was not folded to NOP");

	const auto zero = build(encode_rmif(31, 63, 0xb));
	require(zero.complete && zero.program.words().size() == 4U &&
			zero.program.words()[0] ==
				address(uml_arm64_gadget_rmif_zero) &&
			zero.program.words()[1] == 0xb000'0000U,
		"zero-source RMIF retained source/rotation decoding");

	const auto ordinary = build(encode_rmif(19, 47, 0xd));
	require(ordinary.complete &&
			ordinary.program.words().size() == 4U &&
			ordinary.program.words()[0] ==
				address(uml_arm64_gadget_rmif_fast) &&
			ordinary.program.words()[1] ==
				(gadgets::selection::packed_u16_operands(
					offsetof(arm64::CpuState, registers) + 19U * 8U,
					47U) |
				 (std::uintptr_t{0xd000'0000U} << 32)),
		"RMIF fields were not packed during translation");
	require(conditional_branches(
			address(uml_arm64_gadget_rmif_fast)) == 0U &&
			conditional_branches(
				address(uml_arm64_gadget_rmif_zero)) == 0U,
		"RMIF hot path retained a conditional branch");
}

void setf_translation()
{
	const bool native =
		::uml::platform::apple::probed_host_capabilities().has(ArmFeature::flag_m);
	for (const bool sixteen : {false, true}) {
		const auto ordinary = build(encode_setf(sixteen, 23));
		const auto software = build(encode_setf(sixteen, 23), true);
		const auto expected_native = sixteen ?
			address(uml_arm64_gadget_setf_native_16) :
			address(uml_arm64_gadget_setf_native_8);
		const auto expected_software = sixteen ?
			address(uml_arm64_gadget_setf_fast_16) :
			address(uml_arm64_gadget_setf_fast_8);
		require(ordinary.complete &&
				ordinary.program.words().size() == 4U &&
				ordinary.program.words()[0] ==
					(native ? expected_native :
						expected_software) &&
				ordinary.program.words()[1] ==
					offsetof(arm64::CpuState, registers) +
						23U * 8U,
			"SETF host choice/source was not resolved in translation");
		require(software.complete &&
				software.program.words()[0] == expected_software,
			"forced SETF software gadget was not selected");
		require(conditional_branches(expected_software) == 0U,
			"portable SETF gadget retained a conditional branch");
		if (native)
			require(conditional_branches(expected_native) == 0U,
				"native SETF gadget retained a conditional branch");
	}
}

} // namespace
} // namespace uml::emulator::tests::base_flag

int main()
{
	uml::emulator::tests::base_flag::fixed_translation();
	uml::emulator::tests::base_flag::rmif_translation();
	uml::emulator::tests::base_flag::setf_translation();
	return 0;
}
