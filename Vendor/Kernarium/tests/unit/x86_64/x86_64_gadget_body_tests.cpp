/* SPDX-License-Identifier: GPL-2.0-or-later */
/* What a cached translation is: a body in its address space's arena, with its
 * stream behind it and its saved source apart, and a program that is a view of
 * them. A program a build returns owns its pieces; the one the cache stores
 * owns nothing but what a replay decoded, and publishes that once however many
 * executors replay it.
 *
 * Checked here: a stored program's place in its body, the stream and header it
 * carries, its source, a view made by hand, the move of an owning program, a
 * replay on a stored program and the same replay raced by several threads.
 */
#include "cpu/gadget_cache_budget.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "cpu/x86_64/gadgets/translation_body.hpp"
#include "require.hpp"
#include "x86_64_flat_memory.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace uml::cpu::x86_64;
using uml::testing::require;

using Memory = uml::test::x86_64_flat_memory::FlatMemory;

/* Five instructions and a syscall, the block every test below stores. */
constexpr std::uint8_t block[] = {
	0x48, 0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1
	0x48, 0xff, 0xc0,                         // inc rax
	0x48, 0x83, 0xc0, 0x02,                   // add rax, 2
	0x48, 0x01, 0xc0,                         // add rax, rax
	0x0f, 0x05,                               // syscall
};

void load_block(Memory &memory)
{
	for (std::size_t at = 0; at < sizeof(block); ++at)
		memory.load(at, {block[at]});
}

const gadgets::Program &store_block(gadgets::Executor &executor,
	Memory &memory)
{
	executor.prepare(memory, 1, 1, 1);
	const auto selected = executor.select_program(
		memory, 0, ExecutionMode::long64, 64);
	require(selected.program != nullptr &&
		selected.program->instruction_count() == 5,
		"the block did not build");
	return *selected.program;
}

/* The body a stored program belongs to: its stream is the allocation right
 * behind the body, and the program is the body's own member. */
const gadgets::Body &body_of(const gadgets::Program &program)
{
	const auto *body = reinterpret_cast<const gadgets::Body *>(
		program.stream_allocation()) - 1;
	require(&body->program == &program,
		"a stored program is not the program of the body in front of "
		"its stream");
	return *body;
}

void test_stored_program_is_a_view()
{
	Memory memory;
	load_block(memory);
	gadgets::Executor executor;
	const auto &program = store_block(executor, memory);
	const auto &body = body_of(program);
	require(reinterpret_cast<std::uintptr_t>(&body) % 16 == 0 &&
		reinterpret_cast<std::uintptr_t>(
			program.native_stream().data()) % 16 == 0,
		"a body or its stream is not sixteen-byte aligned");
	require(body.allocation_bytes() ==
		gadgets::Body::allocation_bytes(program.native_stream().size()),
		"a body does not know the size of its allocation");

	/* The guest bounds are the two words in front of the stream. */
	const auto *stream = program.native_stream().data();
	require(stream[-2] == program.address() &&
		stream[-1] == program.address() + program.byte_count(),
		"a stored stream does not carry its guest bounds");

	/* The saved source is the bytes the block was read from, and is not in
	 * the body's allocation. */
	const auto source = program.source();
	require(source.size() == sizeof(block) &&
		std::memcmp(source.data(), block, sizeof(block)) == 0,
		"a stored program lost its source");
	const auto *begin = reinterpret_cast<const std::byte *>(&body);
	const auto *end = begin + body.allocation_bytes();
	require(source.data() < begin || source.data() >= end,
		"the saved source is in the hot allocation of its body");
	require(body.instruction_limit != 0,
		"a body did not keep the instruction limit it was built under");

	/* The block runs from its stored stream. */
	CpuState cpu{};
	const auto ran = gadgets::execute(program, cpu, memory, 5);
	require(ran.event == uml::cpu::EngineEvent::system_call &&
		cpu.gpr(Register::rax) == 8,
		"a stored program did not run");
}

void test_view_made_by_hand()
{
	Memory memory;
	load_block(memory);
	auto built = gadgets::build(memory, 0);
	require(built.status == gadgets::BuildStatus::built,
		"the block did not build");
	const auto &owner = built.program;
	const auto words = owner.native_stream().size();
	alignas(16) std::uintptr_t stream[64];
	std::byte source[64];
	require(gadgets::Program::stream_bytes(words) <= sizeof(stream) &&
		owner.source().size() <= sizeof(source),
		"the block does not fit the buffers of the test");
	{
		const auto view = owner.view_in(stream, source);
		require(view.native_stream().data() ==
			stream + gadgets::stream_header_words &&
			view.source().data() == source &&
			view.stream_allocation() == stream,
			"a view does not live in the storage it was made in");
		require(view.address() == owner.address() &&
			view.byte_count() == owner.byte_count() &&
			view.instruction_count() == owner.instruction_count() &&
			view.terminal() == owner.terminal() &&
			view.execution_mode() == owner.execution_mode(),
			"a view differs from the program it was made from");
		require(std::memcmp(view.native_stream().data(),
			owner.native_stream().data(),
			owner.native_stream().size_bytes()) == 0 &&
			stream[0] == owner.address() &&
			stream[1] == owner.address() + owner.byte_count() &&
			std::memcmp(source, owner.source().data(),
				owner.source().size()) == 0,
			"a view's words are not the program's");
		/* A view frees nothing it does not own; the sanitizers see a
		 * free of the buffers above. */
	}
	require(owner.native_stream().size() == words,
		"making a view disturbed its owner");
}

void test_moving_an_owner()
{
	Memory memory;
	load_block(memory);
	auto built = gadgets::build(memory, 0);
	require(built.status == gadgets::BuildStatus::built,
		"the block did not build");
	const auto *stream = built.program.native_stream().data();
	const auto *source = built.program.source().data();
	gadgets::Program moved{std::move(built.program)};
	require(moved.native_stream().data() == stream &&
		moved.source().data() == source,
		"a move copied what it should hand over");
	/* A program that holds nothing has no allocation to offset into, so it
	 * is asked by that. */
	require(built.program.stream_allocation() == nullptr &&
		built.program.source().empty() &&
		built.program.instruction_count() == 0,
		"a moved-from program still holds the program");
	gadgets::Program assigned;
	assigned = std::move(moved);
	require(assigned.native_stream().data() == stream &&
		moved.stream_allocation() == nullptr,
		"a move assignment did not hand over the program");
	/* The program that was assigned over is released; the owner of the
	 * block's pieces frees them once. */
	assigned = gadgets::Program{};
	require(assigned.stream_allocation() == nullptr,
		"assigning an empty program did not release the program");
}

constexpr std::size_t decoded_bytes = 5 * sizeof(gadgets::Gadget);

void test_replay_on_a_stored_program()
{
	Memory memory;
	load_block(memory);
	gadgets::Executor executor;
	const auto &program = store_block(executor, memory);
	require(program.replay_bytes() == 0,
		"a stored translation holds decoded instructions before a replay");
	CpuState cpu{};
	const auto first = gadgets::execute(program, cpu, memory, 2);
	require(first.event == uml::cpu::EngineEvent::continued &&
		first.instruction_budget_exhausted && cpu.rip == 10 &&
		cpu.gpr(Register::rax) == 2,
		"a replay on a stored program did not stop at its budget");
	require(program.replay_bytes() == decoded_bytes,
		"a stored program did not keep what its replay decoded");
	CpuState again{};
	const auto second = gadgets::execute(program, again, memory, 3);
	require(second.instruction_budget_exhausted && again.rip == 14 &&
		again.gpr(Register::rax) == 4 &&
		program.replay_bytes() == decoded_bytes,
		"a second replay did not read the kept instructions");
}

/* Executors that share a program replay it at once: each decodes for itself
 * and the first to publish keeps its copy, so every caller sees one set of
 * instructions, and the loser's storage is freed. */
void test_replays_race_to_one_decode()
{
	Memory memory;
	load_block(memory);
	for (int round = 0; round < 64; ++round) {
		gadgets::Executor executor;
		const auto &program = store_block(executor, memory);
		constexpr std::size_t threads = 4;
		std::atomic<std::size_t> ready{0};
		std::atomic<bool> go{false};
		std::vector<const gadgets::Gadget *> seen(threads, nullptr);
		std::vector<std::size_t> counts(threads, 0);
		std::vector<std::thread> pool;
		for (std::size_t thread = 0; thread < threads; ++thread)
			pool.emplace_back([&, thread] {
				ready.fetch_add(1);
				while (!go.load(std::memory_order_acquire)) {}
				const auto kept = program.replay_instructions();
				seen[thread] = kept.data();
				counts[thread] = kept.size();
			});
		while (ready.load() != threads) {}
		go.store(true, std::memory_order_release);
		for (auto &thread : pool)
			thread.join();
		for (std::size_t thread = 0; thread < threads; ++thread)
			require(seen[thread] != nullptr && counts[thread] == 5 &&
				seen[thread] == seen[0],
				"racing replays did not share one decode");
		require(program.replay_bytes() == decoded_bytes,
			"racing replays kept the wrong amount");
	}
}

/* Everything a store charged goes back when the executor does. */
void test_charges_are_given_back()
{
	namespace budget = uml::cpu::gadget_cache;
	Memory memory;
	load_block(memory);
	const auto base = budget::ledger_state().total;
	{
		gadgets::Executor executor;
		const auto &program = store_block(executor, memory);
		require(budget::ledger_state().total > base,
			"a stored translation was not charged");
		CpuState cpu{};
		(void)gadgets::execute(program, cpu, memory, 2);
	}
	require(budget::ledger_state().total == base,
		"an executor that went kept its charges");
}

} // namespace

int main()
{
	test_stored_program_is_a_view();
	test_view_made_by_hand();
	test_moving_an_owner();
	test_replay_on_a_stored_program();
	test_replays_race_to_one_decode();
	test_charges_are_given_back();
	return 0;
}
