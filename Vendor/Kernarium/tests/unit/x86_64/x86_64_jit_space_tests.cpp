// SPDX-License-Identifier: GPL-2.0-or-later
/* The x86-64 JIT's spaces. One vCPU's engine keeps a space per address
 * space it runs, and a process that ends leaves its space behind. When the
 * engine needs a space for another address space it makes a gone one over
 * rather than building a context, and the space must then translate the new
 * address space's bytes, not the old one's. The program is "mov $value,
 * %eax; syscall" at one address, with other bytes for every address space
 * and no change published between them, so a space that kept a translation
 * returns the previous value. A space the engine lets go of, or an engine
 * that dies with its spaces, returns everything FEXCore mapped for its
 * context, which FEXCore's block-link buffers once did not. */
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "memory/guest_address_space.hpp"
#include "platform/apple/host_features.hpp"

#include "mach_vm_region_test_support.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace {

namespace x86_64 = uml::cpu::x86_64;
using uml::memory::MemoryAccess;

constexpr std::uint64_t page_size = 4096;
constexpr std::uint64_t code_address = 0x1'0000;

class OnePage final : public uml::memory::GuestAddressSpace {
public:
	void set_value(std::uint32_t value) noexcept
	{
		const std::uint8_t program[] = {
			0xb8, static_cast<std::uint8_t>(value),
			static_cast<std::uint8_t>(value >> 8),
			static_cast<std::uint8_t>(value >> 16),
			static_cast<std::uint8_t>(value >> 24), 0x0f, 0x05,
		};
		std::memcpy(page_, program, sizeof(program));
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		auto *source = locate(address, destination.size());
		if (source == nullptr)
			return false;
		std::memcpy(destination.data(), source, destination.size());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t,
		std::span<const std::byte>) override
	{
		return false;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, MemoryAccess access) override
	{
		return access == MemoryAccess::write ? nullptr :
			locate(address, size);
	}

	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override
	{
		return address >= code_address && address < code_address + page_size;
	}

private:
	[[nodiscard]] std::byte *locate(std::uint64_t address,
		std::size_t size) noexcept
	{
		if (address >= code_address && address < code_address + page_size &&
			size <= code_address + page_size - address)
			return page_ + (address - code_address);
		return nullptr;
	}

	alignas(page_size) std::byte page_[page_size]{};
};

/* Every address space but the one running is gone, or all are there. */
std::atomic<unsigned long> running{};
std::atomic<bool> all_live{};

bool generation_live(unsigned long generation) noexcept
{
	return all_live.load() || generation == running.load();
}

int failures;

void expect(bool condition, const char *what)
{
	if (condition)
		return;
	std::printf("FAIL %s\n", what);
	++failures;
}

/* The engine's first dispatch in an address space: the runtime asks it to
 * make the space and tells it no change, as the runtime does. */
std::uint64_t run_program(x86_64::jit::FexEngine &engine, OnePage &memory,
	unsigned long generation, bool &finished)
{
	running = generation;
	std::uint64_t rax_in = 0;
	std::uint64_t seen_mapping = 0;
	std::uint64_t seen_code = 0;
	if (!engine.consumed_sequences(generation, x86_64::ExecutionMode::long64,
		seen_mapping, seen_code))
		engine.apply_changes(generation, x86_64::ExecutionMode::long64,
			{.mapping_sequence = 1, .code_sequence = 1}, false);
	uml::cpu::InterruptToken interrupt{};
	x86_64::CpuState cpu{};
	cpu.rip = code_address;
	cpu.rflags = 0x202;
	cpu.gpr(x86_64::Register::rax) = rax_in;
	const auto result = engine.run(cpu, memory, {
		.generation = generation,
		.instruction_budget = 1000,
		.execution_context = nullptr,
		.interrupt = &interrupt,
		.orders_stores = false,
	});
	finished = result.execution.reason == uml::cpu::ExitReason::system_call;
	return cpu.gpr(x86_64::Register::rax);
}

void test_a_gone_address_space_leaves_its_space_to_the_next()
{
	OnePage memory;
	x86_64::jit::FexEngine engine{
		uml::platform::apple::probed_host_capabilities(),
		{.generation_live = &generation_live}};
	all_live = false;
	for (unsigned long generation = 1; generation <= 6; ++generation) {
		const std::uint32_t value = 0x1000 + static_cast<std::uint32_t>(generation) * 7;
		memory.set_value(value);
		bool finished = false;
		const auto rax = run_program(engine, memory, generation, finished);
		expect(finished, "the program reaches its system call");
		expect(rax == value, "a recycled space runs the new bytes");
	}
	expect(engine.recycled_spaces() == 5,
		"each address space after the first takes the last one's space");
}

void test_a_live_address_space_keeps_its_space()
{
	OnePage memory;
	x86_64::jit::FexEngine engine{
		uml::platform::apple::probed_host_capabilities(),
		{.generation_live = &generation_live}};
	all_live = true;
	memory.set_value(0x2222);
	for (int round = 0; round < 3; ++round)
		for (unsigned long generation = 10; generation <= 11; ++generation) {
			bool finished = false;
			expect(run_program(engine, memory, generation, finished) == 0x2222 &&
				finished, "both address spaces run their program");
		}
	expect(engine.recycled_spaces() == 0,
		"a space of an address space that is there is never taken");
}

void test_an_engine_without_a_query_never_recycles()
{
	OnePage memory;
	x86_64::jit::FexEngine engine{
		uml::platform::apple::probed_host_capabilities()};
	memory.set_value(0x3333);
	for (unsigned long generation = 20; generation <= 24; ++generation) {
		bool finished = false;
		expect(run_program(engine, memory, generation, finished) == 0x3333 &&
			finished, "the program runs");
	}
	expect(engine.recycled_spaces() == 0, "nothing is gone to take");
}

/* The bytes of untagged read-write anonymous memory this task maps. FEXCore
 * maps its own buffers directly, so they carry no tag, and a context that
 * dies without returning one adds to the total for good. */
std::uint64_t untagged_bytes()
{
	std::uint64_t total = 0;
	mach_vm_address_t address = 0;
	natural_t depth = 0;
	for (;;) {
		mach_vm_size_t size = 0;
		vm_region_submap_info_data_64_t information{};
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
		if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth,
			reinterpret_cast<vm_region_recurse_info_t>(&information),
			&count) != KERN_SUCCESS)
			return total;
		if (information.is_submap) {
			++depth;
			continue;
		}
		if (information.user_tag == 0 &&
			information.protection == (VM_PROT_READ | VM_PROT_WRITE) &&
			information.share_mode == SM_PRIVATE)
			total += size;
		address += size;
	}
}

void test_a_destroyed_engine_returns_its_mappings()
{
	OnePage memory;
	memory.set_value(0x4444);
	const auto make_and_destroy = [&](int engines) {
		for (int round = 0; round < engines; ++round) {
			x86_64::jit::FexEngine engine{
				uml::platform::apple::probed_host_capabilities()};
			bool finished = false;
			expect(run_program(engine, memory, 30, finished) == 0x4444 &&
				finished, "the program runs");
		}
	};
	/* Whatever FEXCore and the allocator keep for the process comes first. */
	make_and_destroy(4);
	const auto before = untagged_bytes();
	make_and_destroy(48);
	const auto after = untagged_bytes();
	/* A context's block links alone take a host page of their own, so
	 * the engines left more than 48 pages behind. */
	expect(after < before + 8 * uml::testing::host_page(),
		"engines that are gone leave no mappings behind");
	if (after >= before + 8 * uml::testing::host_page())
		std::printf("untagged bytes %llu before, %llu after\n",
			static_cast<unsigned long long>(before),
			static_cast<unsigned long long>(after));
}

} // namespace

int main()
{
	test_a_gone_address_space_leaves_its_space_to_the_next();
	test_a_live_address_space_keeps_its_space();
	test_an_engine_without_a_query_never_recycles();
	test_a_destroyed_engine_returns_its_mappings();
	if (failures != 0)
		return 1;
	std::puts("x86-64 JIT space tests passed");
	return 0;
}
