// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" const std::uint32_t uml_arm64_neon_inventory_begin[];
extern "C" const std::uint32_t uml_arm64_neon_inventory_end[];
extern "C" void uml_arm64_gadget_illegal();

namespace {

constexpr std::uint64_t pc = 0x7a0'0000;

using uml::testing::require;

void complete_baseline_inventory()
{
	const auto illegal = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_illegal);
	unsigned failures = 0;
	for (auto encoded = uml_arm64_neon_inventory_begin;
	     encoded != uml_arm64_neon_inventory_end; ++encoded) {
		const std::array stream{*encoded, 0xd400'0001U};
		const auto built = gadgets::Builder{}.build(pc, stream);
		if (!built.complete ||
		    built.program.instruction_count() != stream.size() ||
		    built.program.words().empty() ||
		    built.program.words().front() == illegal) {
			std::fprintf(stderr,
				"production builder rejected baseline %#x "
				"(complete=%u instructions=%zu words=%zu)\n",
				*encoded, static_cast<unsigned>(built.complete),
				built.program.instruction_count(),
				built.program.words().size());
			++failures;
		}
	}
	require(failures == 0);
}

} // namespace

int main()
{
	complete_baseline_inventory();
	return 0;
}
