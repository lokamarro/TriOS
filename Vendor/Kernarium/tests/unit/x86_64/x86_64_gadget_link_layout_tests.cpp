/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The layout translated code reads a cached link by: a link names a stream
 * and an instruction count as one aligned pair, and the guest bounds of the
 * code the stream belongs to are the two words in front of the stream. The
 * engine loads the pair, then the bounds at [stream, #-16], so retargeting a
 * link is one paired store and the bounds can never disagree with the stream
 * they were read for.
 *
 * Checked here: the header every program's stream carries, the records an
 * executor links to (a cached block's and the vacant one), a reader that
 * follows a link while writers retarget it among real programs, and, as the
 * control that shows the reader can see a torn link, the layout this one
 * replaced, which kept the bounds in the record and stored them separately.
 */
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/native_symbols.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "require.hpp"
#include "wait_until.hpp"
#include "x86_64_flat_memory.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using namespace uml::cpu::x86_64;
using uml::testing::require;

using Memory = uml::test::x86_64_flat_memory::FlatMemory;

/* mov eax, 1 ; mov ebx, 2 ; mov ecx, 3 ; syscall, every block starting at 0. */
void load_blocks(Memory &memory)
{
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0xbb, 0x02, 0x00, 0x00, 0x00,
		0xb9, 0x03, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
}

[[nodiscard]] std::uint64_t word_before(const std::uintptr_t *stream,
	std::size_t back) noexcept
{
	return stream[-static_cast<std::ptrdiff_t>(back)];
}

void check_header(const gadgets::Program &program, const char *what)
{
	const auto *stream = program.native_stream().data();
	require(reinterpret_cast<std::uintptr_t>(stream) % 16 == 0, what);
	require(program.stream_allocation() ==
		stream - gadgets::stream_header_words, what);
	require(word_before(stream, 2) == program.address(), what);
	require(word_before(stream, 1) ==
		program.address() + program.byte_count(), what);
}

void test_program_headers()
{
	Memory memory;
	load_blocks(memory);
	for (const auto mode : {ExecutionMode::long64, ExecutionMode::compat32}) {
		for (const std::size_t limit : {1U, 2U, 3U, 4U}) {
			const auto built = gadgets::build(memory, 0, mode, limit);
			require(built.status == gadgets::BuildStatus::built,
				"a block did not build");
			require(built.program.instruction_count() != 0 &&
				built.program.instruction_count() <= limit,
				"a block ignored its instruction limit");
			check_header(built.program, "a program's header is not its bounds");
		}
	}
	/* Blocks of different lengths at one address differ in their end alone,
	 * which is what a link retargeted between them must carry along. */
	const auto shorter = gadgets::build(memory, 0, ExecutionMode::long64, 1);
	const auto longer = gadgets::build(memory, 0, ExecutionMode::long64, 4);
	require(shorter.program.byte_count() != longer.program.byte_count(),
		"the blocks do not differ in length");
	require(word_before(shorter.program.native_stream().data(), 1) !=
		word_before(longer.program.native_stream().data(), 1),
		"two lengths share an end");
}

void check_link(const gadgets::BlockLink &link, std::uint64_t begin,
	std::uint64_t end, const char *what)
{
	require(reinterpret_cast<std::uintptr_t>(&link.stream) % 16 == 0, what);
	require(word_before(link.stream, 2) == begin, what);
	require(word_before(link.stream, 1) == end, what);
}

void test_executor_links()
{
	Memory memory;
	load_blocks(memory);
	gadgets::Executor executor;
	executor.prepare(memory, 1, 1, 1);
	const auto selected = executor.select_program(
		memory, 0, ExecutionMode::long64, 4);
	require(selected.program != nullptr, "the block did not build");
	const auto &program = *selected.program;
	check_header(program, "a cached program's header is not its bounds");

	const auto *table = executor.target_links(ExecutionMode::long64);
	require(table != nullptr, "the executor has no target links");
	const auto &claimed = table[gadgets::target_link_index(0)];
	require(claimed.guest_rip == 0 && claimed.link != nullptr,
		"a cached block did not announce itself");
	require(claimed.link->stream == program.native_stream().data() &&
		claimed.link->instruction_count == program.instruction_count() &&
		claimed.link->guest_rip == 0,
		"a block's link does not name it");
	check_link(*claimed.link, program.address(),
		program.address() + program.byte_count(),
		"a block's link carries other bounds than its program");

	/* Entries nothing claimed share the vacant record: its stream is its
	 * own link_exit word, behind a {RIP, RIP} header, and the RIP link_exit
	 * leaves with is two words past that word. */
	const auto &vacant = table[gadgets::target_link_index(0) ^ 1U];
	require(vacant.guest_rip == ~std::uint64_t{} && vacant.link != nullptr,
		"an unclaimed entry is not vacant");
	const auto *fallback = vacant.link->stream;
	require(vacant.link->instruction_count == 0 &&
		*fallback == reinterpret_cast<std::uintptr_t>(
			gadgets::native::uml_x86_64_gadget_link_exit) &&
		reinterpret_cast<const std::uintptr_t *>(vacant.link) ==
			fallback + 1 &&
		vacant.link->guest_rip == ~std::uint64_t{},
		"the vacant link does not exit to its own RIP");
	check_link(*vacant.link, ~std::uint64_t{}, ~std::uint64_t{},
		"the vacant link's bounds are not its RIP");
}

/* The paired store and load a retargeting writer and a following reader use.
 */
inline void store_pair(void *at, std::uint64_t first, std::uint64_t second)
{
	asm volatile("stp %x1, %x2, [%x0]"
		: : "r"(at), "r"(first), "r"(second) : "memory");
}

struct Pair final {
	std::uint64_t first;
	std::uint64_t second;
};

inline Pair load_pair(const void *at)
{
	Pair pair;
	asm volatile("ldp %x0, %x1, [%x2]"
		: "=r"(pair.first), "=r"(pair.second) : "r"(at) : "memory");
	return pair;
}

/* The record before this layout: the link's stream and count, then the
 * bounds as a second pair of the same record, stored separately. */
struct alignas(16) SeparateBounds final {
	std::uint64_t owner;
	std::uint64_t unlinked_begin;
	std::uint64_t unlinked_end;
	std::uintptr_t fallback_stream;
	gadgets::BlockLink link;
	std::uint64_t begin;
	std::uint64_t end;
};

constexpr auto run_time = std::chrono::milliseconds(750);

/* Waits until `count` threads have begun their first pass, so the run time
 * that follows is spent with all of them going: a loaded host can schedule
 * them only after it is over. */
void await_started(const std::atomic<unsigned> &started, unsigned count)
{
	require(uml::testing::waitUntil([&] {
			return started.load(std::memory_order_acquire) == count;
		}, std::chrono::seconds{10}),
		"the threads of the run never started");
}

struct Candidates final {
	gadgets::BuildResult shorter;
	gadgets::BuildResult longer;
};

void test_retargeting_reader()
{
	Memory memory;
	load_blocks(memory);
	Candidates candidates{
		gadgets::build(memory, 0, ExecutionMode::long64, 1),
		gadgets::build(memory, 0, ExecutionMode::long64, 4),
	};
	const gadgets::Program *programs[2] = {
		&candidates.shorter.program, &candidates.longer.program};
	gadgets::UnlinkedBlockLink record{
		0, 0, 0,
		reinterpret_cast<std::uintptr_t>(
			gadgets::native::uml_x86_64_gadget_link_exit),
		{&record.fallback_stream, 0, 0},
	};
	record.unlinked_begin = record.unlinked_end = record.link.guest_rip = 0;
	require(reinterpret_cast<std::uintptr_t>(&record.link) % 16 == 0,
		"the link pair is not aligned");

	std::atomic<bool> stop{false};
	std::atomic<unsigned> started{0};
	std::atomic<std::uint64_t> torn{0};
	std::atomic<std::uint64_t> reads{0};
	std::atomic<std::uint64_t> retargets{0};

	const auto writer = [&](unsigned seed) {
		std::uint64_t state = seed;
		std::uint64_t done = 0;
		started.fetch_add(1, std::memory_order_release);
		/* The first pass is made whatever `stop` says. */
		do {
			state = state * 6364136223846793005ULL + 1442695040888963407ULL;
			const auto choice = (state >> 33) % 3U;
			if (choice == 2)
				store_pair(&record.link.stream,
					reinterpret_cast<std::uintptr_t>(
						&record.fallback_stream), 0);
			else
				store_pair(&record.link.stream,
					reinterpret_cast<std::uintptr_t>(
						programs[choice]->native_stream().data()),
					programs[choice]->instruction_count());
			++done;
		} while (!stop.load(std::memory_order_relaxed));
		retargets.fetch_add(done);
	};

	const auto reader = [&] {
		std::uint64_t seen = 0;
		std::uint64_t bad = 0;
		started.fetch_add(1, std::memory_order_release);
		do {
			const auto link = load_pair(&record.link.stream);
			const auto bounds = load_pair(
				reinterpret_cast<const std::byte *>(link.first) - 16);
			bool good = false;
			if (link.first == reinterpret_cast<std::uintptr_t>(
					&record.fallback_stream)) {
				good = link.second == 0 && bounds.first == 0 &&
					bounds.second == 0;
			} else {
				for (const auto *program : programs)
					if (link.first == reinterpret_cast<std::uintptr_t>(
							program->native_stream().data()))
						good = link.second == program->instruction_count() &&
							bounds.first == program->address() &&
							bounds.second == program->address() +
								program->byte_count();
			}
			bad += good ? 0 : 1;
			++seen;
		} while (!stop.load(std::memory_order_relaxed));
		reads.fetch_add(seen);
		torn.fetch_add(bad);
	};

	std::vector<std::thread> threads;
	for (unsigned index = 0; index < 2; ++index)
		threads.emplace_back(writer, 17U + index);
	for (unsigned index = 0; index < 2; ++index)
		threads.emplace_back(reader);
	await_started(started, static_cast<unsigned>(threads.size()));
	std::this_thread::sleep_for(run_time);
	stop.store(true);
	for (auto &thread : threads)
		thread.join();
	std::printf("link layout: %llu reads, %llu retargets, %llu torn\n",
		static_cast<unsigned long long>(reads.load()),
		static_cast<unsigned long long>(retargets.load()),
		static_cast<unsigned long long>(torn.load()));
	require(reads.load() != 0 && retargets.load() != 0,
		"the readers or the writers made no progress");
	require(torn.load() == 0, "a reader followed a link to bounds that "
		"belong to another stream");
}

/* The control: bounds kept in the record and stored after the pair, as the
 * layout this replaced stored them, tear for a reader that loads the pair and
 * then the bounds. A writer that pauses between the two stores makes the
 * window wide enough to see within the run. */
void test_separate_bounds_tear()
{
	Memory memory;
	load_blocks(memory);
	const auto shorter = gadgets::build(memory, 0, ExecutionMode::long64, 1);
	const auto longer = gadgets::build(memory, 0, ExecutionMode::long64, 4);
	const gadgets::Program *programs[2] = {&shorter.program, &longer.program};
	SeparateBounds record{};
	record.link.stream = &record.fallback_stream;

	std::atomic<bool> stop{false};
	std::atomic<unsigned> started{0};
	std::atomic<std::uint64_t> torn{0};

	const auto writer = [&] {
		std::uint64_t flip = 0;
		started.fetch_add(1, std::memory_order_release);
		do {
			const auto *program = programs[flip++ & 1U];
			store_pair(&record.link.stream,
				reinterpret_cast<std::uintptr_t>(
					program->native_stream().data()),
				program->instruction_count());
			for (volatile int pause = 0; pause < 200; pause = pause + 1) {
			}
			store_pair(&record.begin, program->address(),
				program->address() + program->byte_count());
		} while (!stop.load(std::memory_order_relaxed));
	};
	const auto reader = [&] {
		std::uint64_t bad = 0;
		started.fetch_add(1, std::memory_order_release);
		do {
			const auto link = load_pair(&record.link.stream);
			const auto bounds = load_pair(&record.begin);
			for (const auto *program : programs)
				if (link.first == reinterpret_cast<std::uintptr_t>(
						program->native_stream().data()) &&
				    bounds.second != program->address() +
						program->byte_count())
					++bad;
		} while (!stop.load(std::memory_order_relaxed));
		torn.fetch_add(bad);
	};
	std::vector<std::thread> threads;
	threads.emplace_back(writer);
	threads.emplace_back(reader);
	threads.emplace_back(reader);
	await_started(started, static_cast<unsigned>(threads.size()));
	std::this_thread::sleep_for(run_time);
	stop.store(true);
	for (auto &thread : threads)
		thread.join();
	require(torn.load() != 0,
		"bounds stored after the pair did not tear: the reader cannot see "
		"a torn link");
}

} // namespace

int main()
{
	test_program_headers();
	test_executor_links();
	test_retargeting_reader();
	test_separate_bounds_tear();
	return 0;
}
