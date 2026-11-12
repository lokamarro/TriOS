// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "runtime/context.hpp"

#include <array>
#include <cstdint>
#include <cstring>

using namespace uml::emulator::tests;

namespace {

void select_execution_engine(uml::runtime::ExecutionEngine engine)
{
	auto *context = uml::runtime::current_context();
	require(context != nullptr && context->set_execution_engine(engine) == 0);
}

void public_boundary_reports_precise_metadata_for_every_engine()
{
	constexpr unsigned long generation = 702;
	constexpr std::uint64_t base = 0x81'0000;
	constexpr std::uint32_t divide_by_zero = 1U << 1U;
	constexpr std::uint32_t enable = divide_by_zero << 8U;
	constexpr std::uint32_t syndrome =
		DARWIN_UML_FLOATING_POINT_SYNDROME_BASE | divide_by_zero;
	constexpr std::uint32_t one = 0x3f80'0000U;
	JitBacking backing{1};
	backing.put32(0, 0x1e22'1820U); // fdiv s0, s1, s2
	backing.put32(4, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};

	const std::array engines{
		uml::runtime::ExecutionEngine::gadgets,
		uml::runtime::ExecutionEngine::jit,
	};
	for (const auto engine : engines) {
		if (engine == uml::runtime::ExecutionEngine::jit &&
		    !jit_available())
			continue;
		select_execution_engine(engine);
		darwin_uml_guest_registers registers{};
		uml::testing::GuestFpTail fp{};
		unsigned long tls = 0;
		registers.pc = base;
		fp.fpcr = enable;
		fp.fpsr = 1U << 27U;
		std::memset(fp.vectors[0], 0x5a, sizeof(fp.vectors[0]));
		std::memcpy(fp.vectors[1], &one, sizeof(one));
		const std::array<unsigned char, 16> original_destination = [] {
			std::array<unsigned char, 16> value{};
			value.fill(0x5a);
			return value;
		}();

		require(darwin_uml_enter_guest(
				&registers, &fp, sizeof(fp), generation, &tls) ==
			DARWIN_UML_EVENT_FLOATING_POINT_TRAP);
		require(registers.pc == base);
		require(registers.fault.address == base);
		require(static_cast<std::uint32_t>(
				registers.fault.error_code) == syndrome);
		require(registers.fault.trap_number == 0);
		require(fp.fpcr == enable);
		require(fp.fpsr == (1U << 27U));
		require(std::memcmp(fp.vectors[0],
			original_destination.data(),
			original_destination.size()) == 0);
	}
	select_execution_engine(uml::runtime::ExecutionEngine::automatic);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	public_boundary_reports_precise_metadata_for_every_engine();
	return 0;
}
