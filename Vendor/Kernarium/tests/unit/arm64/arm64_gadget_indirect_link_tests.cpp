// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory_resource>
#include <utility>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_link_exit();
extern "C" void uml_arm64_gadget_branch_register_cached();
extern "C" void uml_arm64_gadget_branch_link_register_cached();
extern "C" void uml_arm64_gadget_return_cached();
extern "C" void uml_arm64_gadget_return_x30_cached();

namespace {

using uml::testing::require;
using uml::emulator::tests::address;

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

	void resolve(const gadgets::BuildResult &target) noexcept
	{
		link.stream = target.program.words().data();
		link.instruction_count = target.program.instruction_count();
	}

	void reset() noexcept
	{
		link.stream = &fallback_stream;
		link.instruction_count = 0;
	}

	std::uintptr_t fallback_stream;
	gadgets::BlockLink link;
};
static_assert(offsetof(LinkTarget, link) == sizeof(std::uintptr_t));

gadgets::BuildResult build_target(std::uint64_t pc, std::uint16_t value)
{
	const std::array instructions{
		0xd280'0000U | (static_cast<std::uint32_t>(value) << 5),
		0xd400'0001U,
	};
	return gadgets::Builder{}.build(pc, instructions);
}

/* Cached calls also name their return block's link: the return ring pushes
 * it, and the call reads the return address it sets out of it. So the fixture
 * resolves each direct target to an unlinked record of its own, as the
 * executor does. */
const gadgets::BlockLink *unlinked(std::uint64_t pc)
{
	static std::map<std::uint64_t, LinkTarget> records;
	return &records.try_emplace(pc, pc).first->second.link;
}

gadgets::BuildResult build_cached(std::uint64_t pc,
	std::uint32_t instruction, gadgets::IndirectLink &site)
{
	const std::array instructions{instruction};
	return gadgets::Builder{}.build(
		pc, instructions,
		[](std::uint64_t target) { return unlinked(target); },
		std::pmr::get_default_resource(),
		[&](std::uint64_t source_pc) {
			require(source_pc == pc);
			return &site;
		});
}

void initialize(gadgets::IndirectLink &site, const LinkTarget &empty)
{
	for (auto &entry : site.entries) {
		entry.guest_pc = ~std::uint64_t{};
		entry.link = &empty.link;
	}
}

std::uint32_t branch_register(unsigned source)
{
	return 0xd61f'0000U | (source << 5);
}

std::uint32_t branch_link_register(unsigned source)
{
	return 0xd63f'0000U | (source << 5);
}

std::uint32_t return_register(unsigned source)
{
	return 0xd65f'0000U | (source << 5);
}

} // namespace

int main()
{
	constexpr std::uint64_t source_pc = 0x4000;
	LinkTarget empty{~std::uint64_t{}};

	/* A cold site exits exactly once and reports its private cache cell to the
	 * executor without dereferencing either nonmatching entry. */
	{
		gadgets::IndirectLink site{};
		initialize(site, empty);
		auto source = build_cached(
			source_pc, branch_register(5), site);
		require(source.program.words()[0] ==
			address(uml_arm64_gadget_branch_register_cached));
		arm64::CpuState cpu{};
		cpu.registers[5] = 0x5000;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(source.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x5000);
		require(cpu.gadget_indirect_miss == &site);
		require(executed == 1);
	}

	/* Both ways chain directly into their translated target and preserve the
	 * combined instruction count. */
	{
		auto first_target = build_target(0x5000, 41);
		auto second_target = build_target(0x6000, 42);
		LinkTarget first{0x5000};
		LinkTarget second{0x6000};
		first.resolve(first_target);
		second.resolve(second_target);
		gadgets::IndirectLink site{};
		site.entries[0] = {first.link.guest_pc, &first.link};
		site.entries[1] = {second.link.guest_pc, &second.link};
		auto source = build_cached(
			source_pc, branch_register(5), site);
		for (const auto &[target, value] :
		     std::array<std::pair<std::uint64_t, std::uint64_t>, 2>{
			     {{0x5000, 41}, {0x6000, 42}}}) {
			arm64::CpuState cpu{};
			cpu.registers[5] = target;
			::uml::memory::Tlb tlb;
			std::size_t executed = 0;
			require(gadgets::execute(source.program, cpu, tlb, executed) ==
				::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] == value);
			require(cpu.pc == target + 8);
			require(cpu.gadget_indirect_miss == nullptr);
			require(executed == 3);
		}

		/* Target eviction resets the stable BlockLink, so a still-matching PIC
		 * entry takes the safe executor fallback rather than a stale stream. */
		second.reset();
		arm64::CpuState cpu{};
		cpu.registers[5] = 0x6000;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(source.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x6000);
		require(cpu.gadget_indirect_miss == nullptr);
		require(executed == 1);
	}

	/* BLR specializes the return address at translation and RET can use an
	 * arbitrary architectural source register while taking the same PIC path. */
	{
		auto target_program = build_target(0x7000, 77);
		LinkTarget target{0x7000};
		target.resolve(target_program);
		gadgets::IndirectLink site{};
		initialize(site, empty);
		site.entries[0] = {target.link.guest_pc, &target.link};

		auto call = build_cached(
			source_pc, branch_link_register(9), site);
		require(call.program.words()[0] ==
			address(uml_arm64_gadget_branch_link_register_cached));
		arm64::CpuState call_cpu{};
		call_cpu.registers[9] = 0x7000;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(call.program, call_cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(call_cpu.registers[30] == source_pc + 4);
		require(call_cpu.registers[0] == 77);
		require(executed == 3);

		auto ret = build_cached(
			source_pc, return_register(12), site);
		require(ret.program.words()[0] ==
			address(uml_arm64_gadget_return_cached));
		arm64::CpuState return_cpu{};
		return_cpu.registers[12] = 0x7000;
		executed = 0;
		require(gadgets::execute(ret.program, return_cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(return_cpu.registers[0] == 77);
		require(executed == 3);
	}

	/* RET through X30 bakes the register: the stream is the entry and the
	 * site, and a hit, a miss and the return ring behave as for any RET. */
	{
		auto target_program = build_target(0x7000, 77);
		LinkTarget target{0x7000};
		target.resolve(target_program);
		gadgets::IndirectLink site{};
		initialize(site, empty);
		site.entries[0] = {target.link.guest_pc, &target.link};

		auto ret = build_cached(source_pc, return_register(30), site);
		require(ret.program.words().size() == 2);
		require(ret.program.words()[0] ==
			address(uml_arm64_gadget_return_x30_cached));
		require(ret.program.words()[1] ==
			reinterpret_cast<std::uintptr_t>(&site));
		arm64::CpuState hit_cpu{};
		hit_cpu.registers[30] = 0x7000;
		hit_cpu.registers[12] = 0x9000;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(ret.program, hit_cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(hit_cpu.registers[0] == 77);
		require(executed == 3);

		arm64::CpuState miss_cpu{};
		miss_cpu.registers[30] = 0x8000;
		miss_cpu.registers[12] = 0x7000;
		executed = 0;
		require(gadgets::execute(ret.program, miss_cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(miss_cpu.pc == 0x8000);
		require(miss_cpu.gadget_indirect_miss == &site);

		/* Any other register keeps the general entry and its operand. */
		auto other = build_cached(source_pc, return_register(29), site);
		require(other.program.words().size() == 3);
		require(other.program.words()[0] ==
			address(uml_arm64_gadget_return_cached));
	}

	/* A call pushes the return ring, whose record answers a baked RET whose
	 * site has not seen the target. */
	{
		const std::uint64_t callee_pc = 0x7000;
		LinkTarget callee{callee_pc};
		LinkTarget resume{source_pc + 4};
		auto resume_program = build_target(source_pc + 4, 91);
		resume.resolve(resume_program);
		gadgets::IndirectLink return_site{};
		initialize(return_site, empty);
		const std::array return_code{return_register(30)};
		auto callee_program = gadgets::Builder{}.build(
			callee_pc, return_code, {},
			std::pmr::get_default_resource(),
			[&](std::uint64_t) { return &return_site; });
		callee.resolve(callee_program);
		require(callee_program.program.words()[0] ==
			address(uml_arm64_gadget_return_x30_cached));

		gadgets::IndirectLink call_site{};
		initialize(call_site, empty);
		call_site.entries[0] = {callee_pc, &callee.link};
		const std::array call_code{branch_link_register(9)};
		auto call = gadgets::Builder{}.build(
			source_pc, call_code,
			[&](std::uint64_t) { return &resume.link; },
			std::pmr::get_default_resource(),
			[&](std::uint64_t) { return &call_site; });
		arm64::CpuState cpu{};
		cpu.registers[9] = callee_pc;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(call.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == 91);
		require(cpu.gadget_indirect_miss == nullptr);
		require(executed == 4);
		require(return_site.entries[0].guest_pc == source_pc + 4 &&
			return_site.entries[0].link == &resume.link);
	}

	/* A target the site's cells do not hold is followed through the address
	 * space's target table, which also fills the site's empty cell; without
	 * a matching record, or with no table published, the site exits as
	 * before. A site whose cells are both filled is left alone, so vCPUs
	 * sharing it do not keep rewriting it. */
	{
		auto first_target = build_target(0x5000, 41);
		auto second_target = build_target(0x6000, 42);
		auto third_target = build_target(0x9000, 43);
		LinkTarget first{0x5000};
		LinkTarget second{0x6000};
		LinkTarget third{0x9000};
		first.resolve(first_target);
		second.resolve(second_target);
		third.resolve(third_target);
		std::array<gadgets::IndirectLinkEntry, gadgets::target_tier_entries>
			table;
		table.fill({~std::uint64_t{}, &empty.link});
		table[gadgets::target_tier_index(0x9000)] = {0x9000, &third.link};
		/* Targets 4 KiB apart land in different records. */
		require(gadgets::target_tier_index(0x9000) !=
			gadgets::target_tier_index(0xa000));

		gadgets::IndirectLink site{};
		initialize(site, empty);
		site.entries[0] = {first.link.guest_pc, &first.link};
		site.next_replacement = 1;
		auto source = build_cached(source_pc, branch_register(5), site);
		const auto run = [&](std::uint64_t target, const void *published,
				std::size_t &executed) {
			arm64::CpuState cpu{};
			cpu.registers[5] = target;
			cpu.gadget_target_tier = published;
			::uml::memory::Tlb tlb;
			executed = 0;
			const auto event = gadgets::execute(
				source.program, cpu, tlb, executed);
			return std::pair{event, cpu};
		};

		std::size_t executed = 0;
		auto [unpublished, unpublished_cpu] = run(0x9000, nullptr, executed);
		require(unpublished == ::uml::cpu::EngineEvent::continued);
		require(unpublished_cpu.pc == 0x9000);
		require(unpublished_cpu.gadget_indirect_miss == &site);
		require(site.entries[1].guest_pc == ~std::uint64_t{});

		auto [absent, absent_cpu] = run(0xa000, table.data(), executed);
		require(absent == ::uml::cpu::EngineEvent::continued);
		require(absent_cpu.pc == 0xa000);
		require(absent_cpu.gadget_indirect_miss == &site);

		auto [hit, hit_cpu] = run(0x9000, table.data(), executed);
		require(hit == ::uml::cpu::EngineEvent::system_call);
		require(hit_cpu.registers[0] == 43);
		require(hit_cpu.pc == 0x9008);
		require(hit_cpu.gadget_indirect_miss == nullptr);
		require(executed == 3);
		require(site.entries[1].guest_pc == 0x9000 &&
			site.entries[1].link == &third.link);
		require(site.entries[0].guest_pc == 0x5000);
		require(site.next_replacement == 1);

		/* The trained cell now answers without the table. */
		auto [cell, cell_cpu] = run(0x9000, nullptr, executed);
		require(cell == ::uml::cpu::EngineEvent::system_call);
		require(cell_cpu.registers[0] == 43);

		/* A full site follows the table without being rewritten. */
		site.entries[1] = {second.link.guest_pc, &second.link};
		auto [full, full_cpu] = run(0x9000, table.data(), executed);
		require(full == ::uml::cpu::EngineEvent::system_call);
		require(full_cpu.registers[0] == 43);
		require(full_cpu.gadget_indirect_miss == nullptr);
		require(site.entries[0].guest_pc == 0x5000 &&
			site.entries[1].guest_pc == 0x6000);

		/* A record whose translation was retired leaves through its
		 * unlinked exit with the target PC and no miss to train. */
		third.reset();
		site.entries[1] = {second.link.guest_pc, &second.link};
		auto [retired, retired_cpu] = run(0x9000, table.data(), executed);
		require(retired == ::uml::cpu::EngineEvent::continued);
		require(retired_cpu.pc == 0x9000);
		require(retired_cpu.gadget_indirect_miss == nullptr);
		require(executed == 1);
	}
}
