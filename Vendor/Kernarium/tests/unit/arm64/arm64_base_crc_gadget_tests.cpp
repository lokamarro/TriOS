// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <set>
#include <string_view>

namespace uml::emulator::tests::base_crc {

namespace arm64 = uml::cpu::arm64;

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t encode(
	unsigned operation, unsigned accumulator, unsigned source,
	unsigned destination, bool wrong_width = false) noexcept
{
	const bool wide = (operation & 3U) == 3U;
	return 0x1ac0'4000U | ((wide != wrong_width) ? 0x8000'0000U : 0U) |
		((operation & 7U) << 10) | ((source & 31U) << 16) |
		((accumulator & 31U) << 5) | (destination & 31U);
}

} // namespace uml::emulator::tests::base_crc

namespace a64 = uml::testing::a64;

extern "C" {
#define UML_DECLARE_CHECKSUM_SOURCES(prefix) \
	void uml_arm64_gadget_crc32_##prefix##_rr(); \
	void uml_arm64_gadget_crc32_##prefix##_same(); \
	void uml_arm64_gadget_crc32_##prefix##_rz(); \
	void uml_arm64_gadget_crc32_##prefix##_zr(); \
	void uml_arm64_gadget_crc32_##prefix##_zz(); \
	void uml_arm64_gadget_crc32_software_##prefix##_rr(); \
	void uml_arm64_gadget_crc32_software_##prefix##_same(); \
	void uml_arm64_gadget_crc32_software_##prefix##_rz(); \
	void uml_arm64_gadget_crc32_software_##prefix##_zr(); \
	void uml_arm64_gadget_crc32_software_##prefix##_zz();
UML_DECLARE_CHECKSUM_SOURCES(b)
UML_DECLARE_CHECKSUM_SOURCES(h)
UML_DECLARE_CHECKSUM_SOURCES(w)
UML_DECLARE_CHECKSUM_SOURCES(x)
UML_DECLARE_CHECKSUM_SOURCES(cb)
UML_DECLARE_CHECKSUM_SOURCES(ch)
UML_DECLARE_CHECKSUM_SOURCES(cw)
UML_DECLARE_CHECKSUM_SOURCES(cx)
#undef UML_DECLARE_CHECKSUM_SOURCES
}

namespace uml::emulator::tests::base_crc {
namespace {

namespace gadgets = arm64::gadgets;
using uml::emulator::tests::Gadget;

#define UML_NATIVE_CHECKSUM_SOURCES(prefix) std::array<Gadget, 5>{ \
	uml_arm64_gadget_crc32_##prefix##_rr, \
	uml_arm64_gadget_crc32_##prefix##_same, \
	uml_arm64_gadget_crc32_##prefix##_rz, \
	uml_arm64_gadget_crc32_##prefix##_zr, \
	uml_arm64_gadget_crc32_##prefix##_zz}
#define UML_SOFTWARE_CHECKSUM_SOURCES(prefix) std::array<Gadget, 5>{ \
	uml_arm64_gadget_crc32_software_##prefix##_rr, \
	uml_arm64_gadget_crc32_software_##prefix##_same, \
	uml_arm64_gadget_crc32_software_##prefix##_rz, \
	uml_arm64_gadget_crc32_software_##prefix##_zr, \
	uml_arm64_gadget_crc32_software_##prefix##_zz}

constexpr std::array native_gadgets{
	UML_NATIVE_CHECKSUM_SOURCES(b),
	UML_NATIVE_CHECKSUM_SOURCES(h),
	UML_NATIVE_CHECKSUM_SOURCES(w),
	UML_NATIVE_CHECKSUM_SOURCES(x),
	UML_NATIVE_CHECKSUM_SOURCES(cb),
	UML_NATIVE_CHECKSUM_SOURCES(ch),
	UML_NATIVE_CHECKSUM_SOURCES(cw),
	UML_NATIVE_CHECKSUM_SOURCES(cx),
};
constexpr std::array software_gadgets{
	UML_SOFTWARE_CHECKSUM_SOURCES(b),
	UML_SOFTWARE_CHECKSUM_SOURCES(h),
	UML_SOFTWARE_CHECKSUM_SOURCES(w),
	UML_SOFTWARE_CHECKSUM_SOURCES(x),
	UML_SOFTWARE_CHECKSUM_SOURCES(cb),
	UML_SOFTWARE_CHECKSUM_SOURCES(ch),
	UML_SOFTWARE_CHECKSUM_SOURCES(cw),
	UML_SOFTWARE_CHECKSUM_SOURCES(cx),
};

#undef UML_SOFTWARE_CHECKSUM_SOURCES
#undef UML_NATIVE_CHECKSUM_SOURCES

using uml::emulator::tests::address;

[[nodiscard]] gadgets::BuildResult build(
	std::uint32_t instruction, bool force_software)
{
	constexpr auto base = engine_test_code_base;
	constexpr std::uint32_t svc = 0xd400'0001U;
	gadgets::BuildOptions options{};
	options.force_software_checksum = force_software;
	return gadgets::Builder{}.build(base, std::array{instruction, svc},
		{}, std::pmr::get_default_resource(), {}, options);
}

void exact_translation_selection_and_stream_shape(bool force_software)
{
	constexpr auto base = engine_test_code_base;
	constexpr std::uint32_t svc = 0xd400'0001U;
	const auto nop = gadgets::Builder{}.build(
		base, std::array{std::uint32_t{0xd503'201fU}, svc});
	require(nop.complete, "NOP production program did not build");

	const bool native = !force_software && ::uml::platform::apple::probed_host_capabilities().crc32;
	const auto &expected = native ? native_gadgets : software_gadgets;
	std::set<std::uintptr_t> entries;
	for (unsigned operation = 0; operation < 8; ++operation) {
		const std::array patterns{
			std::array{7U, 11U},
			std::array{7U, 7U},
			std::array{7U, 31U},
			std::array{31U, 11U},
			std::array{31U, 31U},
		};
		for (unsigned pattern = 0; pattern < patterns.size(); ++pattern) {
			const auto accumulator = patterns[pattern][0];
			const auto source = patterns[pattern][1];
			const auto instruction =
				encode(operation, accumulator, source, 23);
			const auto built = build(instruction, force_software);
			require(built.complete,
				"checksum production program did not build");
			const auto words = built.program.words();
			require(words[0] == address(expected[operation][pattern]),
				"checksum implementation was not selected in translation");
			const auto destination_offset =
				offsetof(arm64::CpuState, registers) + 23U * 8U;
			const auto expected_operand =
				gadgets::selection::arithmetic_binary_operand(instruction);
			require(words.size() == 4U &&
					words[1] == expected_operand &&
					(expected_operand & 0xffffU) == destination_offset,
				"checksum fields were not packed during translation");
			entries.insert(words[0]);

			const auto discard = build(
				(instruction & ~31U) | 31U, force_software);
			require(discard.complete &&
					discard.program.words().size() == 3U &&
					discard.program.words()[0] ==
						nop.program.words()[0] &&
					discard.program.words()[1] ==
						nop.program.words()[1],
				"discarded checksum was not folded to NOP");
		}
	}
	require(entries.size() == 40U,
		"checksum operation/source modes did not select 40 live gadgets");
}

[[nodiscard]] constexpr bool direct_branch(std::uint32_t instruction)
{
	return (instruction & 0xfc00'0000U) == 0x1400'0000U;
}

[[nodiscard]] constexpr bool native_checksum(std::uint32_t instruction)
{
	return (instruction & 0x7fe0'e000U) == 0x1ac0'4000U;
}

struct PathAudit final {
	unsigned native_operations{};
	unsigned direct_branches{};
	std::uint32_t native_instruction{};
};

[[nodiscard]] PathAudit audit_path(Gadget gadget)
{
	PathAudit audit{};
	const auto shape = a64::body_shape(
		reinterpret_cast<std::uintptr_t>(gadget),
		{.window = 512U, .follow_branches = true},
		[&](std::uint32_t instruction) {
			require(!a64::conditional_branch(instruction),
				"checksum gadget retained a conditional runtime branch");
			if (native_checksum(instruction)) {
				++audit.native_operations;
				audit.native_instruction = instruction;
			}
			audit.direct_branches += direct_branch(instruction);
		});
	if (!shape.reached_dispatch)
		fail("checksum gadget did not reach threaded dispatch");
	return audit;
}

void native_hot_paths_and_software_fallbacks_are_distinct()
{
	std::set<std::uintptr_t> native_entries;
	std::set<std::uintptr_t> software_entries;
	for (unsigned operation = 0; operation < native_gadgets.size();
	     ++operation)
		for (const auto gadget : native_gadgets[operation]) {
			const auto audit = audit_path(gadget);
			require(audit.native_operations == 1U &&
					audit.direct_branches == 0U &&
					audit.native_instruction ==
						encode(operation, 14U, 15U, 16U),
				"native checksum hot path was not direct and branch-free");
			native_entries.insert(address(gadget));
		}
	for (const auto &operation : software_gadgets)
		for (const auto gadget : operation) {
			const auto audit = audit_path(gadget);
			require(audit.native_operations == 0U &&
					audit.direct_branches == 1U,
				"software checksum fallback retained a native operation");
			software_entries.insert(address(gadget));
		}
	require(native_entries.size() == 40U &&
			software_entries.size() == 40U,
		"checksum gadget matrices contain duplicate entries");
	for (const auto entry : native_entries)
		require(!software_entries.contains(entry),
			"native and software checksum gadgets share an entry");
}

} // namespace
} // namespace uml::emulator::tests::base_crc

int main()
{
	uml::emulator::tests::base_crc::
		exact_translation_selection_and_stream_shape(false);
	uml::emulator::tests::base_crc::
		exact_translation_selection_and_stream_shape(true);
	uml::emulator::tests::base_crc::
		native_hot_paths_and_software_fallbacks_are_distinct();
	return 0;
}
