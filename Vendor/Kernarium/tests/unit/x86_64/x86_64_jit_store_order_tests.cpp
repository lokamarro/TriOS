// SPDX-License-Identifier: GPL-2.0-or-later
/* x86's store order on the x86-64 JIT, between two host threads that each
 * drive an engine as a vCPU thread does: message passing (MP) through plain
 * moves must never show the flag without the data in a space that orders
 * stores, and does show it in one that does not, which proves the test can
 * see the reordering it guards against. The data and flag are reached
 * through a general register (mp), or through RSP by the reader (mp-stack-
 * load) or the writer (mp-stack-store), which an ordered space orders too.
 *
 * Every round the reader clears its pair, publishes the round with XCHG,
 * spins until it reads y = 1 and then judges the x it read with that y;
 * the writer waits for the round, takes y's line, stores x = 1 and y = 1
 * and counts the round with LOCK INC. The pairs rotate through 16 slots on
 * separate lines. The programs below were assembled from:
 *
 *	reader:	xor %r8d,%r8d; mov $1,%ecx
 *	1:	mov %rcx,%rax; and $15,%eax; shl $8,%eax
 *		lea 4096(%rdi,%rax),%rdx
 *		movq $0,(%rdx); movq $0,128(%rdx); mov %rcx,40(%rdx)
 *		[reader_stack: mov %rdx,%rsp]
 *		mov %rcx,%rax; xchg %rax,(%rdi)
 *	2:	mov 128(%rdx),%r9; mov (%rdx),%r10	[stack: (%rsp)]
 *		test %r9,%r9; jz 2b
 *	3:	mov 64(%rdi),%rax; cmp %rcx,%rax; jb 3b
 *		mfence; test %r10,%r10; jnz 4f; inc %r8
 *	4:	inc %rcx; cmp %rsi,%rcx; jbe 1b
 *		mov %r8,128(%rdi); xor %eax,%eax; syscall
 *
 *	writer:	mov $1,%ecx
 *	1:	mov (%rdi),%rax; cmp %rcx,%rax; jb 1b
 *		mov %rcx,%rax; and $15,%eax; shl $8,%eax
 *		lea 4096(%rdi,%rax),%rdx		[writer_stack: %rsp]
 *		mov %rcx,152(%rdx); movq $1,(%rdx); movq $1,128(%rdx)
 *		lock incq 64(%rdi)
 *		inc %rcx; cmp %rsi,%rcx; jbe 1b
 *		xor %eax,%eax; syscall
 *
 * RDI holds the data page and RSI the rounds.
 *
 * mp-cycle runs MP on one address space and the same two engines four
 * times, unordered, ordered, unordered and ordered, as the guest kernel's
 * mark comes and goes with a sharer: each run must show what its ordering
 * promises, so the engine runs relaxed code again once a space it ran
 * ordered stops being shared. */
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "memory/guest_address_space.hpp"
#include "platform/apple/host_features.hpp"
#include "watchdog.hpp"

#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <thread>

namespace {

namespace x86_64 = uml::cpu::x86_64;
using uml::memory::MemoryAccess;

constexpr std::uint64_t page_size = 4096;
constexpr std::uint64_t code_address = 0x1'0000;
constexpr std::uint64_t data_address = 0x2'0000;
constexpr std::uint64_t data_pages = 2;
constexpr std::uint64_t result_offset = 128;
constexpr std::uint64_t rounds = 1'000'000;
constexpr std::size_t dispatch_budget = 64 * 1024;
constexpr auto time_limit = std::chrono::seconds{120};

constexpr std::uint8_t reader_plain[] = {
	0x45, 0x31, 0xc0, 0xb9, 0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0xc8, 0x83,
	0xe0, 0x0f, 0xc1, 0xe0, 0x08, 0x48, 0x8d, 0x94, 0x07, 0x00, 0x10, 0x00,
	0x00, 0x48, 0xc7, 0x02, 0x00, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x82, 0x80,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x28, 0x48,
	0x89, 0xc8, 0x48, 0x87, 0x07, 0x4c, 0x8b, 0x8a, 0x80, 0x00, 0x00, 0x00,
	0x4c, 0x8b, 0x12, 0x4d, 0x85, 0xc9, 0x74, 0xf1, 0x48, 0x8b, 0x47, 0x40,
	0x48, 0x39, 0xc8, 0x72, 0xf7, 0x0f, 0xae, 0xf0, 0x4d, 0x85, 0xd2, 0x75,
	0x03, 0x49, 0xff, 0xc0, 0x48, 0xff, 0xc1, 0x48, 0x39, 0xf1, 0x76, 0xa8,
	0x4c, 0x89, 0x87, 0x80, 0x00, 0x00, 0x00, 0x31, 0xc0, 0x0f, 0x05,
};
constexpr std::uint8_t reader_stack[] = {
	0x45, 0x31, 0xc0, 0xb9, 0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0xc8, 0x83,
	0xe0, 0x0f, 0xc1, 0xe0, 0x08, 0x48, 0x8d, 0x94, 0x07, 0x00, 0x10, 0x00,
	0x00, 0x48, 0xc7, 0x02, 0x00, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x82, 0x80,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x28, 0x48,
	0x89, 0xd4, 0x48, 0x89, 0xc8, 0x48, 0x87, 0x07, 0x4c, 0x8b, 0x8c, 0x24,
	0x80, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x14, 0x24, 0x4d, 0x85, 0xc9, 0x74,
	0xef, 0x48, 0x8b, 0x47, 0x40, 0x48, 0x39, 0xc8, 0x72, 0xf7, 0x0f, 0xae,
	0xf0, 0x4d, 0x85, 0xd2, 0x75, 0x03, 0x49, 0xff, 0xc0, 0x48, 0xff, 0xc1,
	0x48, 0x39, 0xf1, 0x76, 0xa3, 0x4c, 0x89, 0x87, 0x80, 0x00, 0x00, 0x00,
	0x31, 0xc0, 0x0f, 0x05,
};
constexpr std::uint8_t writer_plain[] = {
	0xb9, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x07, 0x48, 0x39, 0xc8, 0x72,
	0xf8, 0x48, 0x89, 0xc8, 0x83, 0xe0, 0x0f, 0xc1, 0xe0, 0x08, 0x48, 0x8d,
	0x94, 0x07, 0x00, 0x10, 0x00, 0x00, 0x48, 0x89, 0x8a, 0x98, 0x00, 0x00,
	0x00, 0x48, 0xc7, 0x02, 0x01, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x82, 0x80,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x48, 0xff, 0x47, 0x40,
	0x48, 0xff, 0xc1, 0x48, 0x39, 0xf1, 0x76, 0xc1, 0x31, 0xc0, 0x0f, 0x05,
};
constexpr std::uint8_t writer_stack[] = {
	0xb9, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x07, 0x48, 0x39, 0xc8, 0x72,
	0xf8, 0x48, 0x89, 0xc8, 0x83, 0xe0, 0x0f, 0xc1, 0xe0, 0x08, 0x48, 0x8d,
	0xa4, 0x07, 0x00, 0x10, 0x00, 0x00, 0x48, 0x89, 0x8c, 0x24, 0x98, 0x00,
	0x00, 0x00, 0x48, 0xc7, 0x04, 0x24, 0x01, 0x00, 0x00, 0x00, 0x48, 0xc7,
	0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x48,
	0xff, 0x47, 0x40, 0x48, 0xff, 0xc1, 0x48, 0x39, 0xf1, 0x76, 0xbe, 0x31,
	0xc0, 0x0f, 0x05,
};

constexpr std::uint64_t reader_plain_entry = 0x000;
constexpr std::uint64_t reader_stack_entry = 0x100;
constexpr std::uint64_t writer_plain_entry = 0x200;
constexpr std::uint64_t writer_stack_entry = 0x300;

/* One code page and two data pages, reached by both engines' translations
 * through host pointers, as the static MMU's are. */
class LitmusMemory final : public uml::memory::GuestAddressSpace {
public:
	LitmusMemory()
	{
		std::memset(code_, 0xcc, sizeof(code_));
		std::memset(data_, 0, sizeof(data_));
		load(reader_plain_entry, reader_plain);
		load(reader_stack_entry, reader_stack);
		load(writer_plain_entry, writer_plain);
		load(writer_stack_entry, writer_stack);
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

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		auto *destination = locate(address, source.size());
		if (destination == nullptr || address < data_address)
			return false;
		std::memcpy(destination, source.data(), source.size());
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, MemoryAccess access) override
	{
		if (access == MemoryAccess::write && address < data_address)
			return nullptr;
		return locate(address, size);
	}

	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override
	{
		return address >= code_address && address < code_address + page_size;
	}

	[[nodiscard]] bool code_page_stamp(std::uint64_t address, void *,
		uml::memory::CodePageStamp &stamp) noexcept override
	{
		if (!is_executable(address))
			return false;
		stamp = {1, 0};
		return true;
	}

	[[nodiscard]] std::uint64_t result() const noexcept
	{
		std::uint64_t value{};
		std::memcpy(&value, data_ + result_offset, sizeof(value));
		return value;
	}

	/* Clears the round counters and pairs for another run, between runs. */
	void reset_data() noexcept { std::memset(data_, 0, sizeof(data_)); }

private:
	void load(std::uint64_t offset, std::span<const std::uint8_t> bytes) noexcept
	{
		std::memcpy(code_ + offset, bytes.data(), bytes.size());
	}

	[[nodiscard]] std::byte *locate(std::uint64_t address,
		std::size_t size) noexcept
	{
		if (address >= code_address && address < code_address + page_size &&
			size <= code_address + page_size - address)
			return code_ + (address - code_address);
		if (address >= data_address &&
			address < data_address + data_pages * page_size &&
			size <= data_address + data_pages * page_size - address)
			return data_ + (address - data_address);
		return nullptr;
	}

	alignas(page_size) std::byte code_[page_size];
	alignas(page_size) std::byte data_[data_pages * page_size];
};

/* One agent's program on its own engine, as one vCPU thread runs it; the
 * engine keeps its spaces from one run to the next. */
struct Agent final {
	explicit Agent(std::uint64_t program) : entry{program} {}

	std::uint64_t entry{};
	x86_64::jit::FexEngine engine{
		uml::platform::apple::probed_host_capabilities()};
	bool synced{};
	bool finished{};
	bool stores_ordered{};
	const char *failure{};

	void run(LitmusMemory &memory, unsigned long generation,
		bool ordered) noexcept
	{
		uml::cpu::InterruptToken interrupt{};
		if (!synced) {
			engine.apply_changes(generation,
				x86_64::ExecutionMode::long64, {
				.mapping_sequence = 1,
				.code_sequence = 1,
				.all_mappings = true,
				.all_code = true,
			}, ordered);
			synced = true;
		}
		finished = false;
		failure = nullptr;
		x86_64::CpuState cpu{};
		cpu.rip = code_address + entry;
		cpu.rflags = 0x202;
		cpu.gpr(x86_64::Register::rdi) = data_address;
		cpu.gpr(x86_64::Register::rsi) = rounds;
		for (;;) {
			const auto result = engine.run(cpu, memory, {
				.generation = generation,
				.instruction_budget = dispatch_budget,
				.execution_context = nullptr,
				.interrupt = &interrupt,
				.orders_stores = ordered,
			});
			stores_ordered = result.stores_ordered;
			if (result.execution.reason == uml::cpu::ExitReason::system_call) {
				finished = true;
				return;
			}
			if (result.execution.reason !=
				uml::cpu::ExitReason::instruction_budget) {
				failure = "the program stopped short of its system call";
				return;
			}
		}
	}
};

struct Case final {
	const char *name;
	std::uint64_t reader_entry;
	std::uint64_t writer_entry;
};

/* Judges one run of the case and returns 0 when it shows what its ordering
 * promises. */
int judge(const Case &test, const LitmusMemory &memory, const Agent &reader,
	const Agent &writer, bool ordered)
{
	const char *mode = ordered ? "ordered" : "unordered";
	for (const auto *agent : {&reader, &writer}) {
		if (!agent->finished) {
			std::printf("FAIL %s %s: %s\n", test.name, mode,
				agent->failure);
			return 1;
		}
		if (agent->stores_ordered != ordered) {
			std::printf("FAIL %s %s: the engine ran a space that %s\n",
				test.name, mode,
				ordered ? "keeps no order" : "orders stores");
			return 1;
		}
	}
	const auto forbidden = memory.result();
	const bool passed = ordered ? forbidden == 0 : forbidden != 0;
	std::printf("%s %s %s: %llu forbidden outcome(s) in %llu rounds\n",
		passed ? "PASS" : "FAIL", test.name, mode,
		static_cast<unsigned long long>(forbidden),
		static_cast<unsigned long long>(rounds));
	return passed ? 0 : 1;
}

/* Runs the case once per ordering in `orders` on one address space, each
 * program on its own engine and host thread for every run, as a vCPU thread
 * keeps its engine. */
int run_case(const Case &test, std::initializer_list<bool> orders,
	unsigned long generation)
{
	uml::testing::Watchdog watchdog{test.name, time_limit};
	LitmusMemory memory;
	Agent reader{test.reader_entry};
	Agent writer{test.writer_entry};
	std::barrier phase{2};
	std::thread writer_thread{[&] {
		for (const bool ordered : orders) {
			phase.arrive_and_wait();
			writer.run(memory, generation, ordered);
			phase.arrive_and_wait();
		}
	}};
	int failures = 0;
	for (const bool ordered : orders) {
		memory.reset_data();
		phase.arrive_and_wait();
		reader.run(memory, generation, ordered);
		phase.arrive_and_wait();
		failures += judge(test, memory, reader, writer, ordered);
	}
	writer_thread.join();
	return failures;
}

} // namespace

int main()
{
	constexpr Case cases[] = {
		{"mp", reader_plain_entry, writer_plain_entry},
		{"mp-stack-load", reader_stack_entry, writer_plain_entry},
		{"mp-stack-store", reader_plain_entry, writer_stack_entry},
	};
	int failures = 0;
	unsigned long generation = 0;
	for (const auto &test : cases) {
		failures += run_case(test, {false}, ++generation);
		failures += run_case(test, {true}, ++generation);
	}
	failures += run_case({"mp-cycle", reader_plain_entry, writer_plain_entry},
		{false, true, false, true}, ++generation);
	return failures == 0 ? 0 : 1;
}
