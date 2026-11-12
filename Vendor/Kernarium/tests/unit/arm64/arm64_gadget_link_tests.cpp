// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <utility>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_link_exit();
extern "C" void uml_arm64_gadget_fp_link_entry();

namespace {

using uml::testing::require;
using uml::emulator::tests::address;

gadgets::BuildResult build_target(std::uint64_t pc, std::uint16_t value)
{
	const std::array instructions{
		0xd280'0000U | (static_cast<std::uint32_t>(value) << 5),
		0xd400'0001U,
	};
	return gadgets::Builder{}.build(pc, instructions);
}

/* Laid out as the executor's link record: the unlinked stream is one
 * link_exit word, which takes the PC from the BlockLink behind it. */
struct LinkTarget final {
	explicit LinkTarget(std::uint64_t pc) noexcept
		: fallback_stream{address(uml_arm64_gadget_link_exit)},
		  link{
			.stream = &fallback_stream,
			.instruction_count = 0,
			.guest_pc = pc,
		  }
	{
	}

	LinkTarget(const LinkTarget &) = delete;
	LinkTarget &operator=(const LinkTarget &) = delete;

	void resolve(const gadgets::BuildResult &target) noexcept
	{
		link.stream = target.program.words().data();
		link.instruction_count = target.program.instruction_count();
	}

	/* As the executor stores an FP/SIMD block: its stream behind one
	 * fp_link_entry word, which the link names. */
	void resolve_fp(const gadgets::BuildResult &target) noexcept
	{
		require(target.program.accesses_fp_state());
		const auto words = target.program.words();
		require(words.size() < fp_stream.size());
		fp_stream[0] = address(uml_arm64_gadget_fp_link_entry);
		std::copy(words.begin(), words.end(), fp_stream.begin() + 1);
		link.stream = fp_stream.data();
		link.instruction_count = target.program.instruction_count();
	}

	void reset() noexcept
	{
		link.stream = &fallback_stream;
		link.instruction_count = 0;
	}

	std::uintptr_t fallback_stream;
	gadgets::BlockLink link;
	std::array<std::uintptr_t, 64> fp_stream{};
};
static_assert(offsetof(LinkTarget, link) == sizeof(std::uintptr_t));

std::uint32_t direct_branch(std::uint64_t pc, std::uint64_t target,
			    bool link = false)
{
	const auto displacement =
		static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
	require((displacement & 3) == 0);
	const auto immediate = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(displacement >> 2) & 0x03ff'ffffU);
	return (link ? 0x9400'0000U : 0x1400'0000U) | immediate;
}

std::uint32_t conditional_branch(std::uint64_t pc, std::uint64_t target,
				 unsigned condition)
{
	const auto displacement =
		static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
	require((displacement & 3) == 0);
	const auto immediate = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(displacement >> 2) & 0x7ffffU);
	return 0x5400'0000U | (immediate << 5) | (condition & 15U);
}

std::uint32_t compare_branch(std::uint64_t pc, std::uint64_t target,
			     bool nonzero)
{
	const auto displacement =
		static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
	require((displacement & 3) == 0);
	const auto immediate = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(displacement >> 2) & 0x7ffffU);
	/* CBZ/CBNZ X0. */
	return (nonzero ? 0xb500'0000U : 0xb400'0000U) | (immediate << 5);
}

std::uint32_t test_branch(std::uint64_t pc, std::uint64_t target,
			  bool nonzero, unsigned bit)
{
	const auto displacement =
		static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
	require((displacement & 3) == 0);
	require(bit < 64);
	const auto immediate = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(displacement >> 2) & 0x3fffU);
	return 0x3600'0000U |
		(static_cast<std::uint32_t>(bit >> 5) << 31) |
		(static_cast<std::uint32_t>(bit & 31U) << 19) |
		(static_cast<std::uint32_t>(nonzero) << 24) |
		(immediate << 5);
}

} // namespace

int main()
{
	/* An unresolved direct edge selects the data-only fallback stream and
	 * returns the target PC without executing arbitrary memory. */
	{
		LinkTarget target{0x4040};
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto built = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) -> const gadgets::BlockLink * {
				require(pc == target.link.guest_pc);
				return &target.link;
			});
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(built.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x4040);
		require(executed == 1);
	}

	/* A resolved edge enters the successor stream without returning through
	 * C++; its terminal event and combined instruction count are preserved. */
	{
		auto successor = build_target(0x4040, 42);
		LinkTarget target{0x4040};
		target.resolve(successor);
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &target.link; });
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == 42);
		require(cpu.pc == 0x4048);
		require(executed == 3);
	}

	/* An FP/SIMD target owns a translation-selected entry prefix. The hot
	 * loaded case marks the frame dirty without C++, while a fresh deferred
	 * kernel frame is materialized exactly before the linked target. */
	{
		const std::array target_instructions{
			0x9e66'0000U, // fmov x0, d0
			0xd400'0001U, // svc #0
		};
		auto successor = gadgets::Builder{}.build(
			0x4040, target_instructions);
		require(successor.program.accesses_fp_state());
		LinkTarget target{0x4040};
		target.resolve_fp(successor);
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &target.link; });
		::uml::memory::Tlb tlb;

		const std::uint64_t loaded_value =
			0x0123'4567'89ab'cdefULL;
		arm64::CpuState loaded{};
		std::memcpy(loaded.vectors[0].bytes.data(), &loaded_value,
			sizeof(loaded_value));
		std::size_t executed = 0;
		require(gadgets::execute(first.program, loaded, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(loaded.registers[0] == loaded_value);
		require(loaded.fp_loaded && loaded.fp_dirty);
		require(loaded.pc == 0x4048);
		require(executed == 3);

		const std::uint64_t deferred_value =
			0xfedc'ba98'7654'3210ULL;
		std::array<arm64::VectorRegister, 32> deferred{};
		std::memcpy(deferred[0].bytes.data(), &deferred_value,
			sizeof(deferred_value));
		arm64::CpuState lazy{};
		lazy.defer_fp_load(deferred.data(), 0x0040'0000U, 0x0800'0010U,
			arm64::fpcr_writable_mask);
		executed = 0;
		require(gadgets::execute(first.program, lazy, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(lazy.registers[0] == deferred_value);
		require(lazy.fpcr == 0x0040'0000U);
		require(lazy.fpsr == 0x0800'0010U);
		require(lazy.fp_loaded && lazy.fp_dirty);
		require(lazy.pc == 0x4048);
		require(executed == 3);
	}

	/* Materializing a lazy frame does not hide a fault in the linked FP/SIMD
	 * target. The architectural PC remains at the faulting vector load. */
	{
		const std::array target_instructions{
			0x3dc0'0000U, // ldr q0, [x0]
			0xd400'0001U, // svc #0
		};
		auto successor = gadgets::Builder{}.build(
			0x4040, target_instructions);
		require(successor.program.accesses_fp_state());
		LinkTarget target{0x4040};
		target.resolve_fp(successor);
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &target.link; });

		std::array<arm64::VectorRegister, 32> deferred{};
		arm64::CpuState cpu{};
		cpu.defer_fp_load(deferred.data(), 0, 0,
			arm64::fpcr_writable_mask);
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::memory_fault);
		require(cpu.fp_loaded && cpu.fp_dirty);
		/* The vector load reports its position in the successor, as every
		 * transfer without a PC word does. */
		gadgets::resolve_fault_position(successor.program, cpu.pc);
		require(cpu.pc == target.link.guest_pc);
		require(executed == 3);
	}

	/* B.cond selects target links directly. Exercise both NZCV outcomes so a
	 * raw guest address can never be mistaken for a host stream pointer. */
	for (const bool taken : {false, true}) {
		auto branch_target = build_target(0x4040, 7);
		auto fallthrough = build_target(0x4004, 9);
		LinkTarget target{0x4040};
		LinkTarget next{0x4004};
		target.resolve(branch_target);
		next.resolve(fallthrough);
		const std::array instructions{
			conditional_branch(0x4000, target.link.guest_pc, 0)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) {
				return pc == target.link.guest_pc ?
					&target.link : &next.link;
			});
		arm64::CpuState cpu{};
		cpu.nzcv = taken ? 0x4000'0000U : 0;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == (taken ? 7U : 9U));
		require(executed == 3);
	}

	/* BL preserves its fixed return address while using the same deferred-PC
	 * linked edge as B. The return address is the return block's link's own,
	 * so that address resolves to a record of its own, as in the executor. */
	{
		auto successor = build_target(0x4040, 17);
		LinkTarget target{0x4040};
		target.resolve(successor);
		LinkTarget return_site{0x4004};
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc, true)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) {
				return pc == target.link.guest_pc ?
					&target.link : &return_site.link;
			});
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[30] == 0x4004);
		require(cpu.registers[0] == 17);
		require(cpu.pc == 0x4048);
		require(executed == 3);
	}

	/* Both architecturally-always B.cond encodings take the target without
	 * reading NZCV. */
	for (const unsigned condition : {14U, 15U}) {
		auto successor = build_target(0x4040, 18 + condition);
		LinkTarget target{0x4040};
		LinkTarget fallthrough{0x4004};
		target.resolve(successor);
		const std::array instructions{
			conditional_branch(0x4000, target.link.guest_pc, condition)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) {
				return pc == target.link.guest_pc ?
					&target.link : &fallthrough.link;
			});
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == 18 + condition);
		require(executed == 3);
	}

	/* CBZ/CBNZ and TBZ/TBNZ each select the correct linked successor for both
	 * outcomes. */
	for (const bool nonzero : {false, true}) {
		for (const bool condition_true : {false, true}) {
			auto taken_target = build_target(0x4040, 31);
			auto not_taken_target = build_target(0x4004, 32);
			LinkTarget target{0x4040};
			LinkTarget fallthrough{0x4004};
			target.resolve(taken_target);
			fallthrough.resolve(not_taken_target);
			const std::array instructions{
				compare_branch(0x4000, target.link.guest_pc,
					nonzero)};
			auto first = gadgets::Builder{}.build(
				0x4000, instructions,
				[&](std::uint64_t pc) {
					return pc == target.link.guest_pc ?
						&target.link : &fallthrough.link;
				});
			arm64::CpuState cpu{};
			cpu.registers[0] =
				condition_true == nonzero ? 1U : 0U;
			::uml::memory::Tlb tlb;
			std::size_t executed = 0;
			require(gadgets::execute(first.program, cpu, tlb, executed) ==
				::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] ==
				(condition_true ? 31U : 32U));
			require(executed == 3);
		}

		for (const bool condition_true : {false, true}) {
			auto taken_target = build_target(0x4040, 41);
			auto not_taken_target = build_target(0x4004, 42);
			LinkTarget target{0x4040};
			LinkTarget fallthrough{0x4004};
			target.resolve(taken_target);
			fallthrough.resolve(not_taken_target);
			const std::array instructions{
				test_branch(0x4000, target.link.guest_pc,
					nonzero, 5)};
			auto first = gadgets::Builder{}.build(
				0x4000, instructions,
				[&](std::uint64_t pc) {
					return pc == target.link.guest_pc ?
						&target.link : &fallthrough.link;
				});
			arm64::CpuState cpu{};
			cpu.registers[0] =
				condition_true == nonzero ? (1U << 5) : 0U;
			::uml::memory::Tlb tlb;
			std::size_t executed = 0;
			require(gadgets::execute(first.program, cpu, tlb, executed) ==
				::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] ==
				(condition_true ? 41U : 42U));
			require(executed == 3);
		}
	}

	/* A sequential block trailer links to its translated fallthrough too. */
	{
		auto successor = build_target(0x4004, 51);
		LinkTarget fallthrough{0x4004};
		fallthrough.resolve(successor);
		const std::array instructions{0xd503'201fU}; // nop
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) {
				require(pc == fallthrough.link.guest_pc);
				return &fallthrough.link;
			});
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == 51);
		require(cpu.pc == 0x400c);
		require(executed == 3);
	}

	/* A live link can be retargeted and then reset to its private fallback
	 * stream without rebuilding its predecessor. */
	{
		auto first_target = build_target(0x4040, 61);
		auto second_target = build_target(0x4080, 62);
		LinkTarget target{0x4040};
		target.resolve(first_target);
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto predecessor = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &target.link; });
		::uml::memory::Tlb tlb;

		for (const auto expected : {61U, 62U}) {
			arm64::CpuState cpu{};
			std::size_t executed = 0;
			require(gadgets::execute(
				predecessor.program, cpu, tlb, executed) ==
				::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] == expected);
			require(executed == 3);
			target.link.stream = second_target.program.words().data();
			target.link.instruction_count =
				second_target.program.instruction_count();
		}

		target.reset();
		arm64::CpuState cpu{};
		std::size_t executed = 0;
		require(gadgets::execute(
			predecessor.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == target.link.guest_pc);
		require(executed == 1);
	}

	/* A self-linked hot loop is forced out at the 4K chain boundary so the
	 * executor can observe interrupts and enforce its 64K budget. */
	{
		LinkTarget self{0x4000};
		self.link.instruction_count = 1;
		const std::array instructions{0x1400'0000U}; // b .
		auto loop = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &self.link; });
		self.link.stream = loop.program.words().data();
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(loop.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x4000);
		require(executed == 4096);
	}

	/* A caller that precharges the counter above the chain limit must leave at
	 * the first linked edge and retain its exact count. This also covers values
	 * above 8,191, where testing bit 12 alone would wrap back to "below". */
	{
		LinkTarget self{0x4000};
		self.link.instruction_count = 1;
		const std::array instructions{0x1400'0000U}; // b .
		auto built = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &self.link; });
		gadgets::ProgramWords words{std::pmr::get_default_resource()};
		for (const auto word : built.program.words())
			words.push_back(word);
		gadgets::Program precharged{
			0x4000, 8192, std::move(words), false};
		self.link.stream = precharged.words().data();
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(precharged, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x4000);
		require(executed == 8193);
	}

	return 0;
}
