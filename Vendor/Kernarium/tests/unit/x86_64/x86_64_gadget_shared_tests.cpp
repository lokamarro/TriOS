/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Executors that run one address space share its translations
 * (DARWIN_UML_GADGET_SHARED_BODIES): its bodies, the link records and indirect
 * sites their streams name, and the target tables, held once, with each
 * executor keeping only its own slots.
 *
 *   bodies      what sharing does: a second executor adopts a translation
 *               instead of building it, a code change reaches every executor's
 *               links, one executor letting go leaves the others their
 *               bodies, executors built for different exceptions share
 *               nothing, a shared space is never swept, and a block entered
 *               through a link into a body only the address space holds
 *               reports its exits right.
 *   store       the writer's protocol: a store over a block's source makes it
 *               stale for every executor and resets the link another executor
 *               armed, a build a store landed across is not kept, and no link
 *               is armed to a stale body. Dropping the stale check at arming,
 *               the store count at publication or the reset at the store each
 *               fails it.
 *   concurrent  executors on their own threads running one address space
 *               through rounds of code patched by an engine store.
 *   bound       what bounds a shared space: one that holds mostly freed arena
 *               (executors let go of it down to the bodies of one, by being
 *               destroyed or by the MMU destroying the address space) is
 *               renewed for every executor attached and a smaller one is left
 *               alone, the process's byte budget reaches every executor of a
 *               space it asks to shed, and executors on their own threads
 *               keep running while spaces are shed and renewed under them.
 */
#include "cpu/gadget_cache_budget.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/exception_checks.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "memory/guest_address_space.hpp"
#include "require.hpp"
#include "x86_64_code_change_history.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace uml::cpu::x86_64;
using uml::cpu::EngineEvent;
using uml::testing::require;

using uml::test::x86_64_code_change_history::Sequences;
using uml::test::x86_64_code_change_history::read_code_changes;
using uml::test::x86_64_code_change_history::read_sequences;

class Memory final : public uml::memory::GuestAddressSpace {
public:
	explicit Memory(std::size_t size = 16384)
		: bytes_(size), versions_(bytes_.size() / 4096U + 1U, 1)
	{
	}

	void set_sequences(Sequences *sequences) noexcept
	{
		sequences_ = sequences;
	}

	/* Bytes placed by the test, and the publication a writer makes after
	 * them. */
	void load(std::uint64_t address, std::initializer_list<std::uint8_t> bytes)
	{
		const auto first = address;
		for (const auto byte : bytes)
			bytes_[address++] = static_cast<std::byte>(byte);
		bump(first, bytes.size());
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (address > bytes_.size() ||
		    destination.size() > bytes_.size() - address) {
			fault_ = {address, uml::memory::MemoryAccess::read};
			faulted_ = true;
			return false;
		}
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (!read(address, destination))
			return false;
		if (on_fetch_ && address == fetch_trigger_) {
			auto hook = std::move(on_fetch_);
			on_fetch_ = nullptr;
			hook();
		}
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (address > bytes_.size() || source.size() > bytes_.size() - address) {
			fault_ = {address, uml::memory::MemoryAccess::write};
			faulted_ = true;
			return false;
		}
		std::memcpy(bytes_.data() + address, source.data(), source.size());
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, uml::memory::MemoryAccess access) override
	{
		if (address > bytes_.size() || size > bytes_.size() - address) {
			fault_ = {address, access};
			faulted_ = true;
			return nullptr;
		}
		return bytes_.data() + address;
	}

	[[nodiscard]] bool is_executable(std::uint64_t) noexcept override
	{
		return true;
	}

	[[nodiscard]] bool code_page_stamp(std::uint64_t address, void *,
		uml::memory::CodePageStamp &stamp) noexcept override
	{
		const auto page = static_cast<std::size_t>(address / 4096U);
		if (page >= versions_.size())
			return false;
		stamp = {page + 1U, versions_[page]};
		return true;
	}

	/* A publication moves the page's stamp and the code sequence. */
	void invalidate_code(std::uint64_t address) override
	{
		bump(address, 1);
		if (sequences_ != nullptr)
			sequences_->change(address & ~std::uint64_t{4095}, 4096);
	}

	[[nodiscard]] uml::memory::MemoryFault last_fault() const noexcept override
	{
		return fault_;
	}
	[[nodiscard]] bool has_fault() const noexcept override { return faulted_; }

	/* A write by another vCPU that lands while a block is being decoded,
	 * right after the fetch that starts at `trigger`. */
	void on_fetch_at(std::uint64_t trigger, std::function<void()> hook)
	{
		fetch_trigger_ = trigger;
		on_fetch_ = std::move(hook);
	}

	[[nodiscard]] std::uint8_t byte(std::uint64_t address) const noexcept
	{
		return static_cast<std::uint8_t>(bytes_[address]);
	}

private:
	void bump(std::uint64_t address, std::size_t size) noexcept
	{
		if (size == 0)
			return;
		const auto first = static_cast<std::size_t>(address / 4096U);
		const auto last = static_cast<std::size_t>((address + size - 1U) / 4096U);
		for (auto page = first; page <= last && page < versions_.size(); ++page)
			++versions_[page];
	}

	std::vector<std::byte> bytes_;
	std::vector<std::uint64_t> versions_;
	Sequences *sequences_{};
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
	std::uint64_t fetch_trigger_{};
	std::function<void()> on_fetch_;
};

constexpr std::uint64_t generation = 77;
constexpr std::uint64_t entry_block = 0x000;
constexpr std::uint64_t target_block = 0x040;
constexpr std::uint64_t writer_block = 0x080;

/* X at 0 jumps to Y at 0x40, which loads EAX with a value and makes a system
 * call: the three instructions run as one native chain once the link between
 * the blocks is armed, and X alone otherwise. */
void load_chain(Memory &memory, std::uint8_t value)
{
	memory.load(entry_block, {0xe9, 0x3b, 0x00, 0x00, 0x00});
	memory.load(target_block, {0xb8, value, 0x00, 0x00, 0x00, 0x0f, 0x05});
}

/* W at 0x80 stores BL at [RDX], then makes a system call. */
void load_writer(Memory &memory)
{
	memory.load(writer_block, {0x88, 0x1a, 0x0f, 0x05});
}

void prepare(gadgets::Executor &executor, Memory &memory, Sequences &sequences)
{
	executor.prepare(memory, generation, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_sequences,
		read_code_changes);
}

[[nodiscard]] gadgets::ProgramSelection select(gadgets::Executor &executor,
	Memory &memory, std::uint64_t address)
{
	const auto selected = executor.select_program(
		memory, address, ExecutionMode::long64, 64);
	require(selected.status == gadgets::BuildStatus::built &&
		selected.program != nullptr, "a block did not build");
	return selected;
}

struct Run final {
	EngineEvent event;
	std::size_t retired;
	std::uint64_t rax;
	std::uint64_t rip;
};

/* Runs a selected block with the executor's own links. */
[[nodiscard]] Run run(gadgets::Executor &executor, Memory &memory,
	const gadgets::Program &program, std::uint64_t rbx = 0,
	std::uint64_t rdx = 0)
{
	CpuState cpu{};
	cpu.rip = program.address();
	cpu.gpr(Register::rbx) = rbx;
	cpu.gpr(Register::rdx) = rdx;
	const auto result = gadgets::execute(program, cpu, memory, executor, 64);
	return {result.event, result.retired_instructions,
		cpu.gpr(Register::rax), cpu.rip};
}

[[nodiscard]] bool chained(const Run &result)
{
	return result.event == EngineEvent::system_call && result.retired == 3;
}

[[nodiscard]] bool stopped_at_the_jump(const Run &result)
{
	return result.event == EngineEvent::continued && result.retired == 1 &&
		result.rip == target_block;
}

void share(bool shared)
{
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", shared ? "1" : "0",
		1) == 0, "the environment did not take the setting");
}

/* A fixture: the chain loaded, and executors prepared on one address space. */
struct World final {
	Memory memory;
	Sequences sequences;
	std::vector<std::unique_ptr<gadgets::Executor>> executors;

	explicit World(std::uint8_t value = 1, std::size_t size = 16384)
		: memory{size}
	{
		memory.set_sequences(&sequences);
		load_chain(memory, value);
	}

	gadgets::Executor &add(unsigned checked = 0)
	{
		executors.push_back(std::make_unique<gadgets::Executor>());
		auto &executor = *executors.back();
		if (checked != 0)
			executor.check_exceptions(checked);
		prepare(executor, memory, sequences);
		return executor;
	}

	void prepare_all()
	{
		for (auto &executor : executors)
			prepare(*executor, memory, sequences);
	}

	/* A writer's store over Y's immediate and the publication after it,
	 * made by `by`, which cools and marks what the store overlaps. */
	void store_over_target(gadgets::Executor &by, std::uint8_t value)
	{
		const std::byte byte{value};
		require(memory.write(target_block + 1, {&byte, 1}),
			"the store did not land");
		by.invalidate_cached_code(target_block + 1, 1);
		memory.invalidate_code(target_block + 1);
	}
};

/* ---- bodies ---- */

void test_a_second_executor_adopts()
{
	World world;
	auto &first = world.add();
	auto &second = world.add();
	const auto x = select(first, world.memory, entry_block);
	const auto y = select(first, world.memory, target_block);
	require(first.cache_diagnostics().translations == 2 && !x.cache_hit,
		"the first executor did not build both blocks");

	const auto adopted_x = select(second, world.memory, entry_block);
	const auto adopted_y = select(second, world.memory, target_block);
	require(adopted_x.cache_hit && adopted_y.cache_hit &&
		second.cache_diagnostics().translations == 0,
		"a second executor built what the address space held");
	require(adopted_x.program == x.program && adopted_y.program == y.program,
		"a second executor did not take the very translation");
	/* The adopted block runs, and chains through the shared link. */
	require(chained(run(second, world.memory, *adopted_x.program)),
		"an adopted block did not run through the shared link");
	require(chained(run(first, world.memory, *x.program)),
		"the block the address space shares stopped running for the "
		"executor that built it");
}

void test_a_code_change_reaches_every_executors_links()
{
	World world;
	auto &first = world.add();
	auto &second = world.add();
	const auto x = select(first, world.memory, entry_block);
	(void)select(first, world.memory, target_block);
	const auto other_x = select(second, world.memory, entry_block);
	require(chained(run(second, world.memory, *other_x.program)),
		"the chain was not armed");

	/* Something that is not either executor's own store -- a mapping, the
	 * kernel, a JIT -- changes Y and publishes. The first executor to see
	 * the sequence move resets the shared links of the blocks the change
	 * covers; the other has not looked yet, and must not follow the link
	 * into the old bytes. */
	const std::byte value{2};
	require(world.memory.write(target_block + 1, {&value, 1}),
		"the write did not land");
	world.memory.invalidate_code(target_block + 1);
	prepare(first, world.memory, world.sequences);
	require(stopped_at_the_jump(run(second, world.memory, *other_x.program)),
		"a link another executor armed survived a code change that "
		"covered its target");

	/* Each executor then revalidates, builds the new block once and
	 * shares it. */
	prepare(second, world.memory, world.sequences);
	const auto y = select(first, world.memory, target_block);
	const auto adopted = select(second, world.memory, target_block);
	require(adopted.cache_hit && adopted.program == y.program &&
		second.cache_diagnostics().translations == 0,
		"the executor that saw the change second built the new block "
		"again");
	const auto after = run(second, world.memory, *x.program);
	require(chained(after) && after.rax == 2,
		"the chain did not run the changed block");
	(void)x;
}

void test_an_unrelated_executor_letting_go_keeps_the_others()
{
	World world;
	auto &first = world.add();
	auto &second = world.add();
	const auto x = select(first, world.memory, entry_block);
	(void)select(first, world.memory, target_block);
	(void)select(second, world.memory, entry_block);
	(void)select(second, world.memory, target_block);

	/* The address space is destroyed as far as the first executor knows. */
	first.leave_gone_generations(1,
		[](unsigned long) noexcept { return 0; });
	require(first.resident_generations() == 0,
		"the executor kept an address space it let go of");
	/* What the second holds is untouched, and still runs. */
	const auto kept = select(second, world.memory, entry_block);
	require(kept.cache_hit && kept.program == x.program,
		"an executor lost a block when another let go of the address "
		"space");
	require(chained(run(second, world.memory, *kept.program)),
		"a block the other executors held stopped running");

	/* The first, preparing again, finds the blocks where they were. */
	prepare(first, world.memory, world.sequences);
	const auto back = select(first, world.memory, entry_block);
	require(back.cache_hit && first.cache_diagnostics().translations == 2,
		"an executor that came back to the address space built its "
		"blocks again");
}

void test_executors_built_for_different_exceptions_share_nothing()
{
	World world;
	auto &plain = world.add();
	auto &checked = world.add(gadgets::check_x87);
	const auto x = select(plain, world.memory, entry_block);
	const auto c = select(checked, world.memory, entry_block);
	require(!c.cache_hit && c.program != x.program &&
		checked.cache_diagnostics().translations == 1,
		"executors built for different exceptions shared a translation");
}

void test_executors_keep_their_own_when_asked()
{
	share(false);
	{
		World world;
		auto &first = world.add();
		auto &second = world.add();
		const auto x = select(first, world.memory, entry_block);
		const auto y = select(second, world.memory, entry_block);
		require(!y.cache_hit && y.program != x.program &&
			second.cache_diagnostics().translations == 1,
			"executors kept apart shared a translation");
	}
	share(true);
}

/* How many sweeps an executor ran over translations that name ever more
 * records, and replaced blocks leave behind. */
[[nodiscard]] std::uint64_t sweeps_after_churn()
{
	World world;
	auto &executor = world.add();
	constexpr std::size_t rounds = 6000;
	constexpr std::uint64_t base = 0x2000;
	for (std::size_t round = 0; round < rounds; ++round) {
		const auto source = base + 8U * (round % 1024U);
		const auto target = 0x10'0000 + 16U * round;
		const auto displacement =
			static_cast<std::uint32_t>(target - (source + 5U));
		world.memory.load(source, {0xe9,
			static_cast<std::uint8_t>(displacement),
			static_cast<std::uint8_t>(displacement >> 8),
			static_cast<std::uint8_t>(displacement >> 16),
			static_cast<std::uint8_t>(displacement >> 24)});
		world.sequences.change(source, 5);
		prepare(executor, world.memory, world.sequences);
		(void)select(executor, world.memory, source);
	}
	return executor.cache_diagnostics().link_sweeps;
}

void test_a_shared_space_is_never_swept()
{
	/* The control: an address space one executor keeps to itself is swept
	 * under the same churn. */
	share(false);
	const auto private_sweeps = sweeps_after_churn();
	share(true);
	require(private_sweeps != 0,
		"the churn did not make a private address space sweep");
	require(sweeps_after_churn() == 0, "a shared address space was swept");
}

/* A block entered through a link into a body this executor does not hold:
 * Y faults, and the exit reports Y's instruction. */
void test_a_fault_in_a_block_only_the_address_space_holds()
{
	World world;
	auto &first = world.add();
	auto &second = world.add();
	/* Y: mov eax, [rbx]. */
	world.memory.load(target_block, {0x8b, 0x03, 0x0f, 0x05});
	const auto x = select(first, world.memory, entry_block);
	(void)select(first, world.memory, target_block);
	const auto bad = std::uint64_t{0x20'0000};
	const auto expected = run(first, world.memory, *x.program, bad);

	/* The second executor holds X only, and enters Y through the link the
	 * first armed. */
	const auto mine = select(second, world.memory, entry_block);
	require(mine.cache_hit && second.cache_diagnostics().translations == 0,
		"the second executor built the entry block");
	const auto result = run(second, world.memory, *mine.program, bad);
	require(expected.event == EngineEvent::memory_fault &&
		expected.rip == target_block && expected.retired == 1,
		"the fault in the linked block was not reported where it is");
	require(result.event == expected.event && result.rip == expected.rip &&
		result.retired == expected.retired,
		"an executor that does not hold the faulting block reported its "
		"fault differently");
}

/* ---- store ---- */

void test_a_store_resets_a_link_another_executor_armed()
{
	World world;
	auto &writer = world.add();
	auto &reader = world.add();
	(void)select(writer, world.memory, entry_block);
	(void)select(writer, world.memory, target_block);
	const auto x = select(reader, world.memory, entry_block);
	require(chained(run(reader, world.memory, *x.program)),
		"the link was not armed");

	/* The writer stores over Y's source. The reader has not looked at the
	 * sequence, and the link it follows is the one it armed. */
	world.store_over_target(writer, 3);
	require(stopped_at_the_jump(run(reader, world.memory, *x.program)),
		"a store left a link armed to the block it overwrote");
}

void test_a_build_a_store_landed_across_is_not_kept()
{
	World world;
	auto &writer = world.add();
	auto &builder = world.add();
	/* The store lands after the builder's decoder has read Y, over the
	 * bytes it read, and changes nothing the builder could tell by
	 * comparing them. */
	world.memory.on_fetch_at(target_block, [&] {
		writer.invalidate_cached_code(target_block + 1, 1);
	});
	const auto built = select(builder, world.memory, target_block);
	require(!built.cache_hit &&
		builder.cache_diagnostics().translations == 1,
		"the build did not run");
	/* Nobody may take it from the address space. */
	const auto other = select(writer, world.memory, target_block);
	require(!other.cache_hit &&
		writer.cache_diagnostics().translations == 1,
		"an executor took a translation that a store landed across");
	/* The builder, asking again, takes the writer's clean build, not the
	 * one it made across the store. */
	const auto again = select(builder, world.memory, target_block);
	require(again.cache_hit && again.program == other.program &&
		builder.cache_diagnostics().translations == 1,
		"the builder kept a translation that a store landed across");
}

void test_no_link_is_armed_to_a_stale_body()
{
	World world;
	auto &writer = world.add();
	auto &other = world.add();
	const auto x = select(writer, world.memory, entry_block);
	(void)select(writer, world.memory, target_block);
	require(chained(run(writer, world.memory, *x.program)),
		"the link was not armed");
	/* A store over Y's immediate with the same value: the bytes still
	 * match, so only the store's own mark says the block is stale. */
	world.store_over_target(writer, 1);
	/* The writer's slot for Y is current, so it is selected and activated
	 * again at once, which must not re-arm the link. */
	const auto y = select(writer, world.memory, target_block);
	require(y.cache_hit, "the writer lost its slot");
	require(stopped_at_the_jump(run(writer, world.memory, *x.program)),
		"a link was armed to a body a store had made stale");
	/* And no executor adopts it. */
	prepare(other, world.memory, world.sequences);
	const auto adopted = select(other, world.memory, target_block);
	require(!adopted.cache_hit &&
		other.cache_diagnostics().translations == 1,
		"an executor adopted a stale translation");
}

/* ---- concurrent ---- */

/* The threads meet here between rounds. A lock and a condition variable, which
 * the sanitizers understand, rather than std::barrier, whose waits they do
 * not. */
class Rendezvous final {
public:
	explicit Rendezvous(unsigned count) : count_{count} {}

	void arrive_and_wait()
	{
		std::unique_lock lock{mutex_};
		const auto phase = phase_;
		if (++arrived_ == count_) {
			arrived_ = 0;
			++phase_;
			changed_.notify_all();
			return;
		}
		changed_.wait(lock, [&] { return phase_ != phase; });
	}

private:
	std::mutex mutex_;
	std::condition_variable changed_;
	const unsigned count_;
	unsigned arrived_{};
	unsigned phase_{};
};

void test_executors_on_threads_follow_patched_code()
{
	constexpr unsigned runners = 4;
	constexpr unsigned rounds = 40;
	World world;
	load_writer(world.memory);
	std::vector<gadgets::Executor *> executors;
	for (unsigned index = 0; index <= runners; ++index)
		executors.push_back(&world.add());
	auto &writer = *executors[runners];

	Rendezvous sync{runners + 1};
	std::atomic<std::uint64_t> expected{1};
	std::atomic<bool> failed{};
	std::vector<std::jthread> threads;
	for (unsigned index = 0; index < runners; ++index)
		threads.emplace_back([&, index] {
			auto &executor = *executors[index];
			for (unsigned round = 0; round < rounds; ++round) {
				executor.begin_run();
				prepare(executor, world.memory, world.sequences);
				for (unsigned pass = 0; pass < 200; ++pass) {
					(void)select(executor, world.memory, target_block);
					const auto x = select(
						executor, world.memory, entry_block);
					const auto result = run(
						executor, world.memory, *x.program);
					if (!chained(result) ||
					    result.rax != expected.load()) {
						if (!failed.exchange(true))
							std::fprintf(stderr,
								"runner %u round %u pass %u: event %d "
								"retired %zu rax %llu rip %llx "
								"expected %llu\n", index, round, pass,
								static_cast<int>(result.event),
								result.retired,
								static_cast<unsigned long long>(
									result.rax),
								static_cast<unsigned long long>(
									result.rip),
								static_cast<unsigned long long>(
									expected.load()));
					}
				}
				executor.end_run();
				sync.arrive_and_wait();
				sync.arrive_and_wait();
			}
		});
	for (unsigned round = 0; round < rounds; ++round) {
		sync.arrive_and_wait();
		/* Every runner has stopped: the writer's own store, through the
		 * engine, patches the immediate Y returns. */
		const auto value = static_cast<std::uint8_t>(2 + round);
		writer.begin_run();
		prepare(writer, world.memory, world.sequences);
		const auto w = select(writer, world.memory, writer_block);
		const auto stored = run(writer, world.memory, *w.program, value,
			target_block + 1);
		writer.end_run();
		if (stored.event != EngineEvent::system_call ||
		    world.memory.byte(target_block + 1) != value)
			failed = true;
		expected = value;
		sync.arrive_and_wait();
	}
	threads.clear();
	require(!failed.load(),
		"an executor ran a block other than the one the address space "
		"held after a code change");
}

} // namespace

/* ---- bound ---- */

namespace budget = uml::cpu::gadget_cache;

constexpr std::size_t block_stride = 32;

/* Block `index` is at index * block_stride: EAX = index + 1, then a system
 * call. Its translation is its own, so each is a body of the address space. */
void load_blocks(Memory &memory, std::size_t count)
{
	for (std::size_t index = 0; index < count; ++index) {
		const auto value = static_cast<std::uint32_t>(index + 1U);
		memory.load(index * block_stride,
			{0xb8, static_cast<std::uint8_t>(value),
				static_cast<std::uint8_t>(value >> 8),
				static_cast<std::uint8_t>(value >> 16),
				static_cast<std::uint8_t>(value >> 24), 0x0f, 0x05});
	}
}

[[nodiscard]] int alive(unsigned long) noexcept { return 1; }

[[nodiscard]] int gone(unsigned long) noexcept { return 0; }

/* How executor A, which translated a great many blocks, stops holding them. */
enum class Departure { destroyed, retired };

/* Executor A translates a great many blocks and B adopts one in sixty-four,
 * so that every arena chunk keeps a body; A then stops holding what it
 * translated, and the bodies only it held are freed. What is left is a space
 * whose arena is mostly freed, which is closed for B to let go of. */
void a_mostly_freed_space_is_renewed_for_every_executor(Departure how)
{
	constexpr std::size_t blocks = 12000;
	World world{1, blocks * block_stride + 4096};
	load_blocks(world.memory, blocks);
	auto &a = world.add();
	auto &b = world.add();
	for (std::size_t index = 0; index < blocks; ++index)
		(void)select(a, world.memory, index * block_stride);
	for (std::size_t index = 0; index < blocks; index += 64)
		require(select(b, world.memory, index * block_stride).cache_hit,
			"an executor built what the address space held");
	require(b.cache_diagnostics().translations == 0,
		"the executors did not share one space");

	switch (how) {
	case Departure::destroyed:
		world.executors.erase(world.executors.begin());
		break;
	case Departure::retired:
		/* The MMU destroyed the address space as far as A knows. */
		a.leave_gone_generations(1, &gone);
		break;
	}
	/* B lets go of the space at its next run, if it was closed, and starts
	 * again in a new one. */
	const auto before = b.cache_diagnostics().translations;
	b.leave_gone_generations(0, &alive);
	require(b.resident_generations() == 0,
		"a space that held mostly freed arena was not closed");
	prepare(b, world.memory, world.sequences);
	const auto again = select(b, world.memory, 0);
	require(!again.cache_hit && b.cache_diagnostics().translations ==
		before + 1,
		"an executor that let go of a closed space did not start a new one");
	const auto ran = run(b, world.memory, *again.program);
	require(ran.event == EngineEvent::system_call && ran.rax == 1,
		"a block translated into the renewed space ran wrongly");
}

void test_a_space_under_the_floor_is_left_alone()
{
	constexpr std::size_t blocks = 500;
	World world{1, blocks * block_stride + 4096};
	load_blocks(world.memory, blocks);
	auto &a = world.add();
	auto &b = world.add();
	for (std::size_t index = 0; index < blocks; ++index)
		(void)select(a, world.memory, index * block_stride);
	for (std::size_t index = 0; index < blocks; index += 64)
		(void)select(b, world.memory, index * block_stride);
	world.executors.erase(world.executors.begin());
	b.leave_gone_generations(0, &alive);
	require(b.resident_generations() == 1,
		"an executor let go of a space nobody closed");
}

/* The process's byte budget, past which the largest space is asked to shed,
 * reaches every executor that holds it. */
void test_the_budget_reaches_every_executor_of_a_space()
{
	constexpr std::size_t blocks = 3000;
	constexpr std::size_t kib = 1024;
	const auto base = budget::ledger_state();
	World world{1, blocks * block_stride + 4096};
	load_blocks(world.memory, blocks);
	auto &a = world.add();
	auto &b = world.add();
	budget::set_budget(512 * kib);
	std::size_t translated = 0;
	for (std::size_t pass = 0; pass < 2; ++pass)
		for (std::size_t index = 0; index < blocks; ++index) {
			a.leave_gone_generations(0, &alive);
			prepare(a, world.memory, world.sequences);
			const auto selected =
				select(a, world.memory, index * block_stride);
			translated += selected.cache_hit ? 0U : 1U;
			const auto ran = run(a, world.memory, *selected.program);
			require(ran.event == EngineEvent::system_call &&
				ran.rax == index + 1,
				"a block ran wrongly under a budget");
		}
	const auto after = budget::ledger_state();
	budget::set_budget(budget::fallback_budget);
	require(after.reliefs > base.reliefs && translated > blocks,
		"a space past the budget was not asked to shed");
	/* B has not run since: it lets go of the space A's stores shed. */
	b.leave_gone_generations(0, &alive);
	require(b.resident_generations() == 0,
		"an executor kept a space the budget asked to shed");
	world.executors.clear();
	require(budget::ledger_state().total == base.total,
		"executors that went kept their charges");
}

/* Executors on their own threads select and run blocks of one address space
 * while the budget has the space shed and the arena has it renewed. */
void test_executors_on_threads_keep_running_while_spaces_are_replaced()
{
	constexpr std::size_t blocks = 2500;
	constexpr std::size_t threads = 4;
	constexpr std::size_t rounds = 600;
	constexpr std::size_t kib = 1024;
	const auto base = budget::ledger_state();
	World world{1, blocks * block_stride + 4096};
	load_blocks(world.memory, blocks);
	for (std::size_t thread = 0; thread < threads; ++thread)
		(void)world.add();
	budget::set_budget(768 * kib);
	std::atomic<unsigned> failures{0};
	std::vector<std::thread> pool;
	for (std::size_t thread = 0; thread < threads; ++thread)
		pool.emplace_back([&, thread] {
			auto &executor = *world.executors[thread];
			std::size_t cursor = thread * (blocks / threads);
			for (std::size_t round = 0; round < rounds; ++round) {
				executor.leave_gone_generations(0, &alive);
				prepare(executor, world.memory, world.sequences);
				for (std::size_t step = 0; step < 16; ++step) {
					const auto index = cursor++ % blocks;
					const auto selected = executor.select_program(
						world.memory, index * block_stride,
						ExecutionMode::long64, 64);
					if (selected.status !=
						    gadgets::BuildStatus::built ||
					    selected.program == nullptr) {
						failures.fetch_add(1);
						return;
					}
					const auto ran = run(executor, world.memory,
						*selected.program);
					if (ran.event != EngineEvent::system_call ||
					    ran.rax != index + 1)
						failures.fetch_add(1);
				}
			}
		});
	for (auto &thread : pool)
		thread.join();
	const auto after = budget::ledger_state();
	budget::set_budget(budget::fallback_budget);
	require(failures.load() == 0,
		"a block ran wrongly while its address space was replaced");
	require(after.reliefs > base.reliefs,
		"the budget never asked the shared space to shed");
	world.executors.clear();
	require(budget::ledger_state().total == base.total,
		"executors that went kept their charges");
}

int main(int argc, char **argv)
{
	share(true);
	const std::string_view mode = argc > 1 ? argv[1] : "bodies";
	if (mode == "bodies") {
		test_a_second_executor_adopts();
		test_a_code_change_reaches_every_executors_links();
		test_an_unrelated_executor_letting_go_keeps_the_others();
		test_executors_built_for_different_exceptions_share_nothing();
		test_executors_keep_their_own_when_asked();
		test_a_shared_space_is_never_swept();
		test_a_fault_in_a_block_only_the_address_space_holds();
	} else if (mode == "store") {
		test_a_store_resets_a_link_another_executor_armed();
		test_a_build_a_store_landed_across_is_not_kept();
		test_no_link_is_armed_to_a_stale_body();
	} else if (mode == "concurrent") {
		test_executors_on_threads_follow_patched_code();
	} else if (mode == "bound") {
		a_mostly_freed_space_is_renewed_for_every_executor(
			Departure::destroyed);
		a_mostly_freed_space_is_renewed_for_every_executor(
			Departure::retired);
		test_a_space_under_the_floor_is_left_alone();
		test_the_budget_reaches_every_executor_of_a_space();
		test_executors_on_threads_keep_running_while_spaces_are_replaced();
	} else {
		std::fprintf(stderr, "x86 gadget shared: unknown mode %s\n", argv[1]);
		return 2;
	}
	return 0;
}
