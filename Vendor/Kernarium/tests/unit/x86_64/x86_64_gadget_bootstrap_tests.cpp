/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "allocation_failure.hpp"
#include "cpu/gadget_cache_budget.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/cpuid.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "cpu/x86_64/gadgets/exception_checks.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/native_baked.hpp"
#include "cpu/x86_64/gadgets/native_entry.hpp"
#include "cpu/x86_64/gadgets/native_symbols.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "cpu/x86_64/gadgets/x87_math.hpp"
#include "cpu/x86_64/segments.hpp"
#include "memory/guest_address_space.hpp"
#include "require.hpp"
#include "wait_until.hpp"
#include "x86_64_code_change_history.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <malloc/malloc.h>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using uml::testing::fail;
using uml::testing::require;

class TestMemory : public uml::memory::GuestAddressSpace {
public:
	/* Most tests need a few pages. The repeat-budget tests need enough to
	 * run past a whole executor entry's worth of copying, which is far more
	 * than is worth giving every other test, so the span is a parameter and
	 * the storage is on the heap.
	 */
	explicit TestMemory(std::uint64_t base = 0, std::size_t span = 16384)
		: base_{base}, bytes_(span),
		  code_stamp_versions_(span / 4096U + 1U, 1) {}

	/* Memory at the top of the 32-bit address space continues, past 4 GiB,
	 * with the addresses from zero, as a compatibility-mode program sees it:
	 * with a base just below 4 GiB, an address below the base is the one that
	 * many bytes past the top. */
	void set_wrap32(bool wrap) noexcept { wrap32_ = wrap; }

	void load(std::uint64_t address,
		std::initializer_list<std::uint8_t> bytes) noexcept
	{
		address = fold(address);
		const auto first = address;
		for (const auto byte : bytes)
			bytes_[address++ - base_] = static_cast<std::byte>(byte);
		bump_code_stamps(first, bytes.size());
	}

	void fill(std::uint64_t address,
		std::span<const std::byte> source) noexcept
	{
		address = fold(address);
		std::memcpy(bytes_.data() + (address - base_), source.data(),
			source.size());
		bump_code_stamps(address, source.size());
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		address = fold(address);
		if (address < base_ || address - base_ > bytes_.size() ||
			destination.size() > bytes_.size() - (address - base_)) {
			fault_ = {address, uml::memory::MemoryAccess::read};
			faulted_ = true;
			return false;
		}
		std::memcpy(destination.data(), bytes_.data() + (address - base_),
			destination.size());
		return true;
	}

	[[nodiscard]] bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		++fetches_;
		if (fetches_fail_) {
			fault_ = {address, uml::memory::MemoryAccess::execute};
			faulted_ = true;
			return false;
		}
		if (!read(address, destination)) {
			fault_.access = uml::memory::MemoryAccess::execute;
			return false;
		}
		if (fetch_trigger_armed_ && address == fetch_trigger_) {
			fetch_trigger_armed_ = false;
			auto target = fetch_deferred_address_;
			for (const auto byte : fetch_deferred_bytes_)
				bytes_[target++ - base_] = static_cast<std::byte>(byte);
			if (fetch_deferred_publishes_)
				bump_code_stamps(fetch_deferred_address_,
					fetch_deferred_bytes_.size());
		}
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		address = fold(address);
		if (address < base_ || address - base_ > bytes_.size() ||
			source.size() > bytes_.size() - (address - base_)) {
			fault_ = {address, uml::memory::MemoryAccess::write};
			faulted_ = true;
			return false;
		}
		std::memcpy(bytes_.data() + (address - base_),
			source.data(), source.size());
		bump_code_stamps(address, source.size());
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, uml::memory::MemoryAccess access) override
	{
		atomic_pointers_.fetch_add(1, std::memory_order_relaxed);
		if (!atomic_pointer_available_)
			return nullptr;
		address = fold(address);
		if (address < base_ || address - base_ > bytes_.size() ||
			size > bytes_.size() - (address - base_)) {
			fault_ = {address, access};
			faulted_ = true;
			return nullptr;
		}
		return bytes_.data() + (address - base_);
	}

	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override
	{
		return every_page_executable_ ||
			(address & ~std::uint64_t{4095}) == executable_page_;
	}

	[[nodiscard]] bool code_page_stamp(std::uint64_t address, void *,
		uml::memory::CodePageStamp &stamp) noexcept override
	{
		++code_stamp_queries_;
		address = fold(address);
		if (deferred_query_ != 0 &&
		    code_stamp_queries_ == deferred_query_) {
			deferred_query_ = 0;
			auto target = deferred_address_;
			for (const auto byte : deferred_bytes_)
				bytes_[target++ - base_] = static_cast<std::byte>(byte);
			bump_code_stamps(deferred_address_, deferred_bytes_.size());
		}
		if (!code_stamps_available_ || address < base_)
			return false;
		const auto page = static_cast<std::size_t>((address - base_) / 4096U);
		if (page >= code_stamp_versions_.size())
			return false;
		stamp = {page + 1U, code_stamp_versions_[page]};
		return true;
	}

	/* A publication is where the MMU moves a page's code stamp; an ordinary
	 * guest store does not move it. A reader validating on the stamp alone
	 * therefore keeps its translation of a page nobody published. */
	void invalidate_code(std::uint64_t address) override
	{
		++invalidations_;
		if (code_changes_ != nullptr)
			++*code_changes_;
		bump_code_stamps(address, 1);
	}
	void set_executable_page(std::uint64_t address) noexcept
	{
		executable_page_ = address & ~std::uint64_t{4095};
	}
	/* What READ_IMPLIES_EXEC makes of a process: data pages that qualify as
	 * code too. */
	void set_every_page_executable(bool value) noexcept
	{
		every_page_executable_ = value;
	}

	[[nodiscard]] uml::memory::MemoryFault last_fault() const noexcept override
	{
		return fault_;
	}

	[[nodiscard]] bool has_fault() const noexcept override { return faulted_; }

	[[nodiscard]] std::uint64_t qword(std::uint64_t address) const noexcept
	{
		address = fold(address);
		std::uint64_t value{};
		std::memcpy(&value, bytes_.data() + (address - base_), sizeof(value));
		return value;
	}

	[[nodiscard]] std::uint32_t dword(std::uint64_t address) const noexcept
	{
		address = fold(address);
		std::uint32_t value{};
		std::memcpy(&value, bytes_.data() + (address - base_), sizeof(value));
		return value;
	}

	[[nodiscard]] std::uint8_t byte(std::uint64_t address) const noexcept
	{
		address = fold(address);
		return static_cast<std::uint8_t>(bytes_[address - base_]);
	}

	[[nodiscard]] std::size_t fetches() const noexcept { return fetches_; }
	void reset_fetches() noexcept { fetches_ = 0; }
	[[nodiscard]] std::size_t atomic_pointers() const noexcept
	{
		return atomic_pointers_.load(std::memory_order_relaxed);
	}
	void reset_atomic_pointers() noexcept
	{
		atomic_pointers_.store(0, std::memory_order_relaxed);
	}
	void set_atomic_pointer_available(bool available) noexcept
	{
		atomic_pointer_available_ = available;
	}
	void set_code_stamps_available(bool available) noexcept
	{
		code_stamps_available_ = available;
	}
	/* An instruction fetch that faults everywhere, as one does after the
	 * block's page was unmapped. */
	void set_fetches_fail(bool fail) noexcept { fetches_fail_ = fail; }
	[[nodiscard]] std::size_t code_stamp_queries() const noexcept
	{
		return code_stamp_queries_;
	}
	void reset_code_stamp_queries() noexcept { code_stamp_queries_ = 0; }
	/* A guest write by another vCPU that lands in the middle of one
	 * validation, immediately before the numbered page-stamp query this
	 * memory answers. A page stamp certifies only bytes read after it, so a
	 * write armed for the query that once followed the source comparison
	 * must not be able to hide behind the stamp that validation records. */
	void write_before_code_page_stamp(std::size_t query,
		std::uint64_t address, std::initializer_list<std::uint8_t> bytes)
	{
		deferred_query_ = query;
		deferred_address_ = address;
		deferred_bytes_.assign(bytes.begin(), bytes.end());
	}
	/* A guest write by another vCPU that lands while this block is being
	 * decoded: it is applied immediately after the fetch that starts at
	 * `trigger`, so the decoded stream predates it and everything the
	 * translation reads afterwards -- the source bytes it saves above all --
	 * does not. `trigger` is a fetch address, and the decoder fetches an
	 * instruction's bytes in one page-bounded window, so it is the address
	 * the instruction covering the raced byte begins at, or the page
	 * boundary the window refills at. It fires once, and the numbered stamp
	 * query it happens to fall between is not part of the arrangement, so
	 * the same arming means the same interleaving however many stamps a
	 * translation takes.
	 *
	 * `publishes` false stores the bytes without moving the page stamp,
	 * which is where a real writer is between its store and its call into
	 * the address space; the test moves the stamp afterwards with
	 * `publish_code_page`, as that writer eventually does. */
	void write_after_fetching(std::uint64_t trigger, std::uint64_t address,
		std::initializer_list<std::uint8_t> bytes, bool publishes = true)
	{
		fetch_trigger_ = trigger;
		fetch_trigger_armed_ = true;
		fetch_deferred_address_ = address;
		fetch_deferred_publishes_ = publishes;
		fetch_deferred_bytes_.assign(bytes.begin(), bytes.end());
	}
	[[nodiscard]] bool write_after_fetching_fired() const noexcept
	{
		return !fetch_trigger_armed_;
	}
	/* The stamp move a writer makes after its bytes have landed. */
	void publish_code_page(std::uint64_t address) noexcept
	{
		bump_code_stamps(address, 1);
	}
	void set_code_changes(std::uint64_t *code_changes) noexcept
	{
		code_changes_ = code_changes;
	}
	[[nodiscard]] std::size_t invalidations() const noexcept { return invalidations_; }

private:
	[[nodiscard]] std::uint64_t fold(std::uint64_t address) const noexcept
	{
		return wrap32_ && address < base_ ? address + (std::uint64_t{1} << 32)
			: address;
	}

	void bump_code_stamps(std::uint64_t address, std::size_t size) noexcept
	{
		address = fold(address);
		if (!code_stamps_available_ || size == 0 || address < base_)
			return;
		const auto first = static_cast<std::size_t>((address - base_) / 4096U);
		const auto last = static_cast<std::size_t>(
			(address - base_ + size - 1U) / 4096U);
		for (auto page = first;
		     page <= last && page < code_stamp_versions_.size(); ++page)
			++code_stamp_versions_[page];
	}

	std::uint64_t base_{};
	bool wrap32_{};
	std::vector<std::byte> bytes_;
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
	std::size_t fetches_{};
	std::size_t invalidations_{};
	std::atomic<std::size_t> atomic_pointers_{};
	bool atomic_pointer_available_{true};
	bool fetches_fail_{};
	std::size_t code_stamp_queries_{};
	std::vector<std::uint64_t> code_stamp_versions_;
	bool code_stamps_available_{};
	std::uint64_t executable_page_{~std::uint64_t{}};
	bool every_page_executable_{};
	std::uint64_t *code_changes_{};
	std::size_t deferred_query_{};
	std::uint64_t deferred_address_{};
	std::vector<std::uint8_t> deferred_bytes_;
	std::uint64_t fetch_trigger_{};
	bool fetch_trigger_armed_{};
	bool fetch_deferred_publishes_{true};
	std::uint64_t fetch_deferred_address_{};
	std::vector<std::uint8_t> fetch_deferred_bytes_;
};

struct TestCodeSequences final {
	std::uint64_t mapping_changes{1};
	std::uint64_t code_changes{1};
};

[[nodiscard]] uml::cpu::x86_64::gadgets::ProgramCacheSequences
read_test_code_sequences(void *opaque) noexcept
{
	const auto &sequences = *static_cast<TestCodeSequences *>(opaque);
	return {
		.mapping_changes = sequences.mapping_changes,
		.code_changes = sequences.code_changes,
		.interrupted = false,
	};
}

/* Another vCPU publishing a code change between every two observations, which
 * is what a guest whose JIT heap or executable stack shares a page with hot
 * data does to every other vCPU of its address space. */
[[nodiscard]] uml::cpu::x86_64::gadgets::ProgramCacheSequences
read_moving_code_sequences(void *opaque) noexcept
{
	auto &sequences = *static_cast<TestCodeSequences *>(opaque);
	return {
		.mapping_changes = sequences.mapping_changes,
		.code_changes = ++sequences.code_changes,
		.interrupted = false,
	};
}

/* What a build made, required to be a block: a failure names the caller's
 * line and `message`. */
[[nodiscard]] uml::cpu::x86_64::gadgets::BuildResult require_built(
	uml::cpu::x86_64::gadgets::BuildResult result, std::string_view message,
	const std::source_location location = std::source_location::current())
{
	require(
		result.status == uml::cpu::x86_64::gadgets::BuildStatus::built,
		message, location);
	return result;
}

/* Selects and runs blocks from RIP until one ends in SYSCALL, at most eight;
 * `what` names the long-mode program in a failure. */
void run_to_system_call(uml::cpu::x86_64::gadgets::Executor &executor,
	TestMemory &memory, uml::cpu::x86_64::CpuState &cpu, std::string_view what,
	const std::source_location location = std::source_location::current())
{
	using namespace uml::cpu::x86_64;
	for (unsigned step = 0; step < 8U; ++step) {
		const auto selected = executor.select_program(
			memory, cpu.rip, ExecutionMode::long64);
		if (selected.status != gadgets::BuildStatus::built ||
			selected.program == nullptr)
			fail(std::string{what} + " did not build",
				location);
		const auto result = gadgets::execute(
			*selected.program, cpu, memory, executor, 64);
		if (result.event == uml::cpu::EngineEvent::system_call)
			return;
		if (result.event != uml::cpu::EngineEvent::continued)
			fail(std::string{what} + " left unexpectedly",
				location);
	}
	fail(std::string{what} + " did not reach SYSCALL",
		location);
}

/* Runs `body` on `count` host threads at once, standing for that many vCPUs.
 * Each is handed one shared flag to clear when its vCPU goes wrong; returns
 * whether none did. */
template<typename Body>
[[nodiscard]] bool on_vcpus(unsigned count, Body body)
{
	std::atomic<bool> healthy{true};
	std::vector<std::thread> threads;
	threads.reserve(count);
	for (unsigned index = 0; index < count; ++index)
		threads.emplace_back([&] { body(healthy); });
	for (auto &thread : threads)
		thread.join();
	return healthy.load(std::memory_order_relaxed);
}

void cache_reuses_and_revalidates_programs()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	memory.reset_fetches();
	const auto first = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto cold_fetches = memory.fetches();
	require(first.status == gadgets::BuildStatus::built &&
		first.program != nullptr && !first.cache_hit,
		"cold program selection did not populate the cache");

	memory.reset_fetches();
	const auto second = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(second.status == gadgets::BuildStatus::built &&
		second.program != nullptr && second.cache_hit,
		"unchanged program did not hit the cache");
	require(cold_fetches != 0 && memory.fetches() == 0,
		"unchanged cache hit fetched instruction bytes");

	/* A code epoch can advance without changing this block. As in the arm64
	 * hierarchy, exact validation retains it and records the new epoch once.
	 */
	executor.prepare(memory, 42, 1, 2);
	memory.reset_fetches();
	memory.reset_atomic_pointers();
	const auto revalidated = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(revalidated.status == gadgets::BuildStatus::built &&
		revalidated.program != nullptr && revalidated.cache_hit,
		"identical source was not retained after a code epoch");
	require(memory.atomic_pointers() != 0 && memory.fetches() == 0,
		"exact validation copied directly mapped source bytes");

	/* Keep exact validation available to address-space implementations that
	 * cannot expose a stable page mapping to the cache. */
	memory.set_atomic_pointer_available(false);
	executor.prepare(memory, 42, 1, 3);
	memory.reset_fetches();
	memory.reset_atomic_pointers();
	const auto fallback = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(fallback.status == gadgets::BuildStatus::built &&
		fallback.program != nullptr && fallback.cache_hit,
		"fetch fallback did not retain identical source");
	require(memory.atomic_pointers() != 0 && memory.fetches() != 0,
		"unavailable direct mapping skipped the fetch fallback");
	memory.set_atomic_pointer_available(true);

	memory.load(1, {0x02});
	executor.prepare(memory, 42, 1, 4);
	memory.reset_fetches();
	const auto changed = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(changed.status == gadgets::BuildStatus::built &&
		changed.program != nullptr && !changed.cache_hit,
		"changed source bytes reused a stale cached program");
	CpuState cpu{};
	const auto executed = gadgets::execute(
		*changed.program, cpu, memory, executor, 64);
	const auto &diagnostics = executor.cache_diagnostics();
	require(executed.event == uml::cpu::EngineEvent::system_call,
		"rebuilt cache program did not reach SYSCALL");
	require(cpu.gpr(Register::rax) == 2,
		"rebuilt cache program retained the old immediate");
	require(diagnostics.translations == 2 &&
		diagnostics.validation_attempts == 3 &&
		diagnostics.validation_hits == 2 &&
		diagnostics.validation_mismatches == 1,
		"cache diagnostics did not describe reuse and replacement");
}

void cache_validates_inline_source_spans()
{
	using namespace uml::cpu::x86_64;
	constexpr std::array<std::size_t, 13> sizes{
		1, 2, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 64};
	TestMemory memory;
	gadgets::Executor executor;
	std::uint64_t changes = 1;
	const auto before = executor.cache_diagnostics();
	for (std::size_t index = 0; index < sizes.size(); ++index) {
		const auto size = sizes[index];
		const auto address = 0x100U + index * 0x100U;
		std::vector<std::byte> source(size, std::byte{0x90});
		if (size == 1) {
			source[0] = std::byte{0xcc};
		} else {
			source[size - 2] = std::byte{0x0f};
			source[size - 1] = std::byte{0x05};
		}
		require(memory.write(address,
				std::span<const std::byte>{source}),
			"validation-span source did not fit test memory");

		executor.prepare(memory, 42, 1, changes++);
		const auto cold = executor.select_program(
			memory, address, ExecutionMode::long64);
		require(cold.status == gadgets::BuildStatus::built &&
				cold.program != nullptr && !cold.cache_hit,
			"validation-span source did not populate the cache");

		executor.prepare(memory, 42, 1, changes++);
		const auto identical = executor.select_program(
			memory, address, ExecutionMode::long64);
		require(identical.status == gadgets::BuildStatus::built &&
				identical.program != nullptr && identical.cache_hit,
			"inline source comparison rejected identical bytes");

		source[0] = size == 1 ? std::byte{0xc3} : std::byte{0x91};
		require(memory.write(address,
				std::span<const std::byte>{source}),
			"changed validation-span source did not fit test memory");
		executor.prepare(memory, 42, 1, changes++);
		const auto changed = executor.select_program(
			memory, address, ExecutionMode::long64);
		require(changed.status == gadgets::BuildStatus::built &&
				changed.program != nullptr && !changed.cache_hit,
			"inline source comparison accepted changed bytes");
	}
	const auto &after = executor.cache_diagnostics();
	require(after.validation_attempts - before.validation_attempts ==
			2 * sizes.size() &&
			after.validation_hits - before.validation_hits == sizes.size() &&
			after.validation_mismatches - before.validation_mismatches ==
				sizes.size(),
		"inline source comparison diagnostics are incomplete");
}

/* Revalidation abandons a block whose source it cannot read, and a check it
 * made while the address space's mappings moved; each outcome is counted. */
void cache_counts_validation_faults_and_races()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	TestCodeSequences sequences;
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes, 1, &sequences,
		read_test_code_sequences);
	const auto cold = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(cold.status == gadgets::BuildStatus::built &&
			!cold.cache_hit,
		"validation-outcome source did not populate the cache");
	const auto before = executor.cache_diagnostics();

	/* The page stamps still match, but the mapping sequence the reader
	 * reports is not the one the executor was prepared for. */
	++sequences.mapping_changes;
	executor.prepare(memory, 42, 1, 2, &sequences,
		read_test_code_sequences);
	const auto raced = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto races = executor.cache_diagnostics().validation_sequence_races -
		before.validation_sequence_races;
	require(raced.status == gadgets::BuildStatus::built &&
			!raced.cache_hit && races == 1,
		"a moving mapping did not abandon and count the validation");

	/* Without stamps or a direct mapping, validation fetches the source,
	 * and the fetch faults. */
	executor.prepare(memory, 42, sequences.mapping_changes, 3, &sequences,
		read_test_code_sequences);
	memory.set_code_stamps_available(false);
	memory.set_atomic_pointer_available(false);
	memory.set_fetches_fail(true);
	const auto faulted = executor.select_program(
		memory, 0, ExecutionMode::long64);
	memory.set_fetches_fail(false);
	memory.set_atomic_pointer_available(true);
	memory.set_code_stamps_available(true);
	const auto faults = executor.cache_diagnostics().validation_faults -
		before.validation_faults;
	require(faulted.status == gadgets::BuildStatus::fetch_fault &&
			faults == 1,
		"an unreadable cached source was not counted as a fault");
}

void cache_rejects_unrelated_code_pages_by_stamp()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(4096, {0x90});
	memory.set_code_stamps_available(true);
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto cold = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(cold.status == gadgets::BuildStatus::built &&
		cold.program != nullptr && !cold.cache_hit,
		"code-stamp source did not populate the cache");

	/* A publication to a different physical code page must update the cache
	 * epoch without rereading this program's unchanged bytes. */
	memory.load(4096, {0x91});
	executor.prepare(memory, 42, 1, 2);
	memory.reset_fetches();
	memory.reset_atomic_pointers();
	memory.reset_code_stamp_queries();
	const auto unrelated = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(unrelated.status == gadgets::BuildStatus::built &&
		unrelated.program != nullptr && unrelated.cache_hit,
		"unrelated code-page publication discarded a cached program");
	require(memory.code_stamp_queries() != 0 &&
		memory.atomic_pointers() == 0 && memory.fetches() == 0,
		"matching code-page stamp still reread source bytes");

	/* A publication to the program's own page still falls through to exact
	 * validation and rebuilds when the saved bytes differ. */
	memory.load(1, {0x02});
	executor.prepare(memory, 42, 1, 3);
	memory.reset_atomic_pointers();
	const auto changed = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto &diagnostics = executor.cache_diagnostics();
	require(changed.status == gadgets::BuildStatus::built &&
		changed.program != nullptr && !changed.cache_hit,
		"changed code-page stamp reused stale translated bytes");
	require(memory.atomic_pointers() != 0,
		"changed code-page stamp skipped exact source validation");
	require(diagnostics.validation_stamp_hits == 1 &&
		diagnostics.validation_mismatches == 1,
		"code-page stamp diagnostics did not describe both paths");
}

/* One vCPU publishing code changes at speed must not cost every other vCPU of
 * the address space a fresh translation of every block it runs. Validation
 * used to re-read the code-change sequence after comparing a program and throw
 * the program away when it had moved, which under a fast publisher is every
 * time. What keeps a program is its pages' stamps and its own saved bytes; a
 * publication that did not touch it moves neither.
 */
void a_moving_code_sequence_keeps_unchanged_programs()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.set_code_stamps_available(true);
	TestCodeSequences sequences;
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_moving_code_sequences);
	const auto cold = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(cold.status == gadgets::BuildStatus::built &&
		cold.program != nullptr && !cold.cache_hit,
		"moving code-change sequence did not populate the cache");

	const auto translated = executor.cache_diagnostics().translations;
	for (int round = 0; round < 16; ++round) {
		executor.prepare(memory, 42, sequences.mapping_changes,
			++sequences.code_changes, &sequences,
			read_moving_code_sequences);
		const auto again = executor.select_program(
			memory, 0, ExecutionMode::long64);
		require(again.program != nullptr && again.cache_hit,
			"a code change published elsewhere rebuilt an untouched "
			"program");
	}
	const auto &diagnostics = executor.cache_diagnostics();
	require(diagnostics.translations == translated,
		"a moving code-change sequence retranslated an untouched program");
	require(diagnostics.validation_stamp_hits != 0,
		"a moving code-change sequence never reached the page stamp");
}

/* A native program owes one code-change publication per store to an
 * executable page that holds cached code, to the page it published last or to
 * another: the page stamp another vCPU validates against moves only with a
 * publication, and that vCPU may have revalidated between two stores.
 */
void slow_executable_writes_publish_every_code_page()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.set_every_page_executable(true);
	std::array<std::byte, 16> bytes{};
	bytes.fill(std::byte{0x55});
	gadgets::NativeExecutionContext context{
		.memory = &memory,
	};
	const auto first = gadgets::uml_x86_64_gadget_bulk_store(
		&context, 0x800, bytes.data(), bytes.size());
	const auto second = gadgets::uml_x86_64_gadget_bulk_store(
		&context, 0x1800, bytes.data(), bytes.size());
	/* And back inside the page just published, which owes its own record. */
	const auto repeated = gadgets::uml_x86_64_gadget_bulk_store(
		&context, 0x1820, bytes.data(), bytes.size());
	require(first == 1 && second == 1 && repeated == 1,
		"slow executable stores did not complete");
	require(memory.invalidations() == 3 &&
		context.code_publications == 3,
		"a slow store to a code page published nothing");
	require(memory.byte(0x800) == 0x55 && memory.byte(0x1800) == 0x55 &&
		memory.byte(0x1820) == 0x55,
		"slow executable stores lost guest bytes");
}

/* One store, two pages: a `rep movs` of code over a page boundary, or a
 * relocating loader's wide write. Both halves need their own record. */
void a_slow_store_across_a_page_boundary_publishes_both_pages()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.set_every_page_executable(true);
	std::array<std::byte, 16> bytes{};
	bytes.fill(std::byte{0x5a});
	gadgets::NativeExecutionContext context{
		.memory = &memory,
	};
	const auto stored = gadgets::uml_x86_64_gadget_bulk_store(
		&context, 0x1000 - 8, bytes.data(), bytes.size());
	require(stored == 1, "crossing slow store did not complete");
	require(memory.invalidations() == 2 &&
		context.code_publications == 2,
		"a store spanning two code pages published only one");
	require(memory.byte(0xff8) == 0x5a && memory.byte(0x1007) == 0x5a,
		"crossing slow store lost guest bytes");
}

/* The same rule on the engine's own path, where the write translation carries
 * the decision: cold, through the miss handler, and warm, where the write
 * translation already knows the page holds code. The middle store is a vector
 * form, which the ordinary bank serves, so the register-baked bank and the
 * hand-written one each have to cross a page here.
 */
void native_executable_stores_publish_every_code_page()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* MOV byte ptr [RAX], 0x7f; MOVD dword ptr [RCX], XMM0;
	 * MOV byte ptr [RAX], 0x6e; SYSCALL -- page 0, page 1, page 0 again. */
	memory.load(0, {
		0xc6, 0x00, 0x7f,
		0x66, 0x0f, 0x7e, 0x01,
		0xc6, 0x00, 0x6e,
		0x0f, 0x05,
	});
	memory.load(0x1000, {0x0f, 0x05});
	memory.load(0x1800, {0xff, 0xff, 0xff, 0xff});
	memory.set_every_page_executable(true);
	gadgets::Executor writer;
	writer.prepare(memory, 21, 1, 1);
	gadgets::Executor reader;
	reader.prepare(memory, 21, 1, 1);
	const auto other = reader.select_program(
		memory, 0x1000, ExecutionMode::long64);
	const auto block = writer.select_program(
		memory, 0, ExecutionMode::long64);
	require(other.program != nullptr && block.program != nullptr,
		"two-page publication setup did not cache both pages");

	const auto run = [&] {
		CpuState cpu{};
		cpu.gpr(Register::rax) = 0x800;
		cpu.gpr(Register::rcx) = 0x1800;
		return gadgets::execute(*block.program, cpu, memory, writer, 64);
	};
	const auto cold = run();
	require(cold.event == uml::cpu::EngineEvent::system_call &&
			cold.retired_instructions == 4,
		"two-page publication block did not run");
	require(memory.invalidations() == 3,
		"a native store to a second code page published nothing");
	const auto warm = run();
	require(warm.event == uml::cpu::EngineEvent::system_call &&
		warm.retired_instructions == 4,
		"warm two-page publication block did not run");
	require(memory.invalidations() == 6,
		"a warm native store to a second code page published nothing");
	require(memory.byte(0x800) == 0x6e && memory.dword(0x1800) == 0,
		"two-page publication block lost its guest stores");
}

/* A store owes a publication, and a followed link settles the publications of
 * the stores before it with the address space and clears what is owed. A store
 * after the link owes its own, however the page stood before it.
 */
void a_settled_link_owes_its_page_a_fresh_publication()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* MOV byte ptr [RAX], 0x7f; JMP +8 -- then the same store again. */
	memory.load(0, {0xc6, 0x00, 0x7f, 0xe9, 0x08, 0x00, 0x00, 0x00});
	memory.load(16, {0xc6, 0x00, 0x6e, 0x0f, 0x05});
	memory.set_executable_page(0);
	TestCodeSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	executor.prepare(memory, 25, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	const auto target = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto source = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(target.program != nullptr && source.program != nullptr,
		"settled-link setup did not cache both blocks");

	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x800;
	const auto result = gadgets::execute(
		*source.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		result.retired_instructions == 4,
		"settled-link blocks did not run as one native chain");
	require(memory.byte(0x800) == 0x6e,
		"settled-link blocks lost a guest store");
	require(memory.invalidations() == 2,
		"a store after a settled link inherited the page's record");
}

/* What the missing record costs another vCPU. A publication moves the page's
 * stamp; an ordinary guest store does not. Without one, the executor holding a
 * translation of the second page revalidates on the sequence the first store
 * moved, gets a stamp hit for its own page, and keeps a program whose bytes
 * have just been rewritten.
 */
void an_unpublished_code_page_strands_another_translation()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* The second store lands on the immediate of the other page's block. */
	memory.load(0, {0xc6, 0x00, 0x7f, 0xc6, 0x01, 0x6e, 0x0f, 0x05});
	memory.load(0x1000, {0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	memory.set_every_page_executable(true);
	memory.set_code_stamps_available(true);
	TestCodeSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor writer;
	writer.prepare(memory, 23, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	gadgets::Executor reader;
	reader.prepare(memory, 23, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	const auto cached = reader.select_program(
		memory, 0x1000, ExecutionMode::long64);
	const auto block = writer.select_program(
		memory, 0, ExecutionMode::long64);
	require(cached.program != nullptr && block.program != nullptr,
		"stranded-translation setup did not cache both pages");

	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x800;
	cpu.gpr(Register::rcx) = 0x1001;
	const auto result = gadgets::execute(
		*block.program, cpu, memory, writer, 64);
	require(result.event == uml::cpu::EngineEvent::system_call,
			"stranded-translation writer did not run");
	require(memory.byte(0x1001) == 0x6e,
		"stranded-translation writer lost its second store");

	reader.prepare(memory, 23, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	const auto again = reader.select_program(
		memory, 0x1000, ExecutionMode::long64);
	require(again.program != nullptr && !again.cache_hit,
		"another executor kept a translation of an unpublished code page");
	CpuState other{};
	other.rip = 0x1000;
	const auto ran = gadgets::execute(
		*again.program, other, memory, reader, 64);
	require(ran.event == uml::cpu::EngineEvent::system_call &&
		other.gpr(Register::rax) == 0x6e,
		"rebuilt translation did not execute the rewritten bytes");
}

/* A masked store publishes once for its whole range, from its base. When that
 * base is on one page and a selected element lies wholly on the next, the
 * record the store owes is the next page's: another executor holding a
 * translation there must see its stamp move and rebuild. */
void a_masked_store_publishes_the_page_its_elements_reach()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* VPMASKMOVD [RAX], XMM1, XMM0; SYSCALL. */
	memory.load(0, {0xc4, 0xe2, 0x71, 0x8e, 0x00, 0x0f, 0x05});
	memory.load(0x2000, {0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	memory.set_every_page_executable(true);
	memory.set_code_stamps_available(true);
	TestCodeSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor writer;
	writer.prepare(memory, 29, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	gadgets::Executor reader;
	reader.prepare(memory, 29, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	const auto cached = reader.select_program(
		memory, 0x2000, ExecutionMode::long64);
	const auto block = writer.select_program(
		memory, 0, ExecutionMode::long64);
	require(cached.program != nullptr && block.program != nullptr,
		"masked-store publication setup did not cache both blocks");

	/* Only element 2, the dword at 0x2000, is selected; the base and the
	 * two elements before it are on the page below. */
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x1ff8;
	const std::array<std::uint8_t, 4> replacement{0xb8, 0x02, 0x00, 0x00};
	for (std::size_t index = 0; index < replacement.size(); ++index)
		cpu.vectors[0][8 + index] = std::byte{replacement[index]};
	cpu.vectors[1][11] = std::byte{0x80};
	memory.load(0x1ff8, {0x5a});
	const auto result = gadgets::execute(
		*block.program, cpu, memory, writer, 64);
	require(result.event == uml::cpu::EngineEvent::system_call,
			"masked-store writer did not run");
	require(memory.byte(0x2001) == 0x02 && memory.byte(0x1ff8) == 0x5a,
		"masked store did not write only its selected element");

	reader.prepare(memory, 29, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	const auto again = reader.select_program(
		memory, 0x2000, ExecutionMode::long64);
	require(again.program != nullptr && !again.cache_hit,
		"a masked store left another executor's translation current");
	CpuState other{};
	other.rip = 0x2000;
	const auto ran = gadgets::execute(
		*again.program, other, memory, reader, 64);
	require(ran.event == uml::cpu::EngineEvent::system_call &&
		other.gpr(Register::rax) == 2,
		"rebuilt translation did not execute the masked store's bytes");
}

/* The same publisher must not be able to keep a program whose own bytes have
 * been rewritten. */
void a_moving_code_sequence_rebuilds_changed_programs()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.set_code_stamps_available(true);
	TestCodeSequences sequences;
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_moving_code_sequences);
	require(executor.select_program(memory, 0,
			ExecutionMode::long64).program != nullptr,
		"changed-program setup did not populate the cache");

	memory.load(1, {0x02});
	executor.prepare(memory, 42, sequences.mapping_changes,
		++sequences.code_changes, &sequences, read_moving_code_sequences);
	const auto changed = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(changed.program != nullptr && !changed.cache_hit,
		"a moving code-change sequence kept a rewritten program");
	CpuState cpu{};
	const auto result = gadgets::execute(
		*changed.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		cpu.gpr(Register::rax) == 2,
		"rebuilt program did not execute the written bytes");
}

/* A validation records the stamp it took before reading the source, never one
 * taken after. A write that lands between the two would otherwise be covered
 * by the recorded stamp while the recorded source predates it, and the next
 * validation would accept the stale program on a stamp match.
 */
void a_recorded_code_stamp_is_no_newer_than_its_source()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(2048, {0x00});
	memory.set_code_stamps_available(true);
	TestCodeSequences sequences;
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	require(executor.select_program(memory, 0,
			ExecutionMode::long64).program != nullptr,
		"stamp-order setup did not populate the cache");

	/* Move the page's stamp without touching the program, so the next
	 * validation has to compare the saved source, and arm a write to the
	 * program for the stamp query that used to follow that comparison. */
	memory.load(2048, {0x01});
	memory.reset_code_stamp_queries();
	memory.write_before_code_page_stamp(2, 1, {0x02});
	executor.prepare(memory, 42, sequences.mapping_changes,
		++sequences.code_changes, &sequences, read_test_code_sequences);
	require(executor.select_program(memory, 0,
			ExecutionMode::long64).program != nullptr,
		"stamp-order validation lost its program");

	/* Whether that write landed inside the validation or at the start of this
	 * one, the cached program no longer matches the guest. */
	executor.prepare(memory, 42, sequences.mapping_changes,
		++sequences.code_changes, &sequences, read_test_code_sequences);
	const auto after = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(after.program != nullptr && !after.cache_hit,
		"a stamp recorded after its source accepted a stale program");
	CpuState cpu{};
	const auto result = gadgets::execute(
		*after.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		cpu.gpr(Register::rax) == 2,
		"stamp-order rebuild did not execute the written bytes");
}

/* A write that lands while a block is being decoded must not survive in the
 * cache. What makes that decidable is that the slot's saved source is the
 * bytes the decoder read: the write moves the page's stamp, and the comparison
 * that follows puts those very bytes against guest memory. A translation that
 * fetched its source separately kept a program older than its own source, and
 * then every validation agreed with it -- a stamp match accepted the program
 * outright, and a moved stamp compared the newer saved source against the
 * newer memory and accepted it too. The code-change replay did not repair it
 * either, because retiring a changed range only unlinks the slot and leaves
 * that same validation to decide.
 */
void a_write_racing_a_first_translation_does_not_survive()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.set_code_stamps_available(true);
	TestCodeSequences sequences;
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);

	/* Another vCPU rewrites the immediate just after the decoder has read
	 * it -- the window fetched at 0 carries it -- while this translation is
	 * still being built. */
	memory.write_after_fetching(0, 1, {0x02});
	const auto racing = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(racing.program != nullptr && !racing.cache_hit,
		"a raced first translation did not produce a program");
	require(memory.write_after_fetching_fired(),
		"the racing write never landed inside the build");

	executor.prepare(memory, 42, sequences.mapping_changes,
		++sequences.code_changes, &sequences, read_test_code_sequences);
	const auto after = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(after.program != nullptr && !after.cache_hit,
		"a write that raced the decoder survived as a valid translation");
	CpuState cpu{};
	const auto result = gadgets::execute(
		*after.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		cpu.gpr(Register::rax) == 2,
		"the translation after a raced build did not execute the written "
		"bytes");
	require(executor.cache_diagnostics().translations == 2,
		"a raced first translation was served from the cache");
}

/* The same race on the second page of a block that spans two. Every page a
 * block can span is held and stamped before the decoder reads it, so the page
 * a block only reaches at its tail is covered exactly like the page it starts
 * on.
 */
void a_write_racing_a_second_page_does_not_survive()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* mov eax, imm32 straddling the page boundary, then syscall. */
	memory.load(4092, {
		0x90, 0x90,
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.set_code_stamps_available(true);
	TestCodeSequences sequences;
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);

	memory.write_after_fetching(4096, 4096, {0x02});
	const auto racing = executor.select_program(
		memory, 4092, ExecutionMode::long64);
	require(racing.program != nullptr && !racing.cache_hit,
		"a raced second-page translation did not produce a program");
	require(memory.write_after_fetching_fired(),
		"the racing second-page write never landed inside the build");

	executor.prepare(memory, 42, sequences.mapping_changes,
		++sequences.code_changes, &sequences, read_test_code_sequences);
	const auto after = executor.select_program(
		memory, 4092, ExecutionMode::long64);
	require(after.program != nullptr && !after.cache_hit,
		"a write to a block's second page survived as a valid "
		"translation");
	CpuState cpu{};
	cpu.rip = 4092;
	const auto result = gadgets::execute(
		*after.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		cpu.gpr(Register::rax) == 0x201,
		"the translation after a raced second page did not execute the "
		"written bytes");
}

/* A writer stores its bytes well before it publishes them. The store lands in
 * the gadget; the page stamp moves several calls later, once the writer has
 * walked its own cache and entered the address space, and the code-change
 * sequence moves after that. So a translation that read its block a second
 * time to fill its saved source could fetch that source after the store and
 * still find the page stamp unmoved on both sides of the fetch -- and the slot
 * it kept, program older than source, was then accepted by every validation
 * that followed, for as long as it lived. The window is not narrow: it is
 * whatever the host lets pass between a store and its publication.
 *
 * One read of the block leaves the late stamp move nothing to hide behind. The
 * saved source is the decoder's own bytes, so the comparison the moved stamp
 * forces sees the write.
 */
void a_write_stamped_after_a_first_translation_does_not_survive()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.set_code_stamps_available(true);
	TestCodeSequences sequences;
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);

	/* The store alone: the writer has not reached its page stamp yet, so
	 * nothing this translation can read afterwards has moved. */
	memory.write_after_fetching(0, 1, {0x02}, false);
	const auto racing = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(racing.program != nullptr && !racing.cache_hit,
		"an unstamped racing write did not produce a program");
	require(memory.write_after_fetching_fired(),
		"the unstamped racing write never landed inside the build");

	/* The rest of that writer: the page stamp, and then the sequence which
	 * brings this vCPU back to validate. */
	memory.publish_code_page(0);
	executor.prepare(memory, 42, sequences.mapping_changes,
		++sequences.code_changes, &sequences, read_test_code_sequences);
	const auto after = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(after.program != nullptr && !after.cache_hit,
		"a write that landed before its page stamp survived as a valid "
		"translation");
	CpuState cpu{};
	const auto result = gadgets::execute(
		*after.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		cpu.gpr(Register::rax) == 2,
		"the translation after a late-stamped write did not execute the "
		"written bytes");
}

/* Refusing a raced translation must stay confined to the race. An undisturbed
 * block is cached whether it spans one page or two, and a write to the other
 * page a block could have spanned but did not is no business of that block --
 * a block in the last bytes of a text page next to hot data would otherwise
 * never be cached at all.
 */
void an_undisturbed_first_translation_is_still_cached()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(4092, {
		0x90, 0x90,
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(4000, {
		0xb8, 0x03, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.set_code_stamps_available(true);
	TestCodeSequences sequences;
	gadgets::Executor executor;
	struct Case final {
		std::uint64_t address;
		bool disturb_neighbour;
		const char *message;
	};
	for (const auto &item : std::initializer_list<Case>{
		{0, false, "an undisturbed single-page translation was not cached"},
		{4092, false, "an undisturbed two-page translation was not cached"},
		{4000, true, "a write to a page the block does not cover refused "
			"its translation"},
	}) {
		executor.prepare(memory, 42, sequences.mapping_changes,
			++sequences.code_changes, &sequences, read_test_code_sequences);
		if (item.disturb_neighbour) {
			/* On the next page, which this block's hold covers because a
			 * block starting here could have reached it, but which its
			 * seven bytes never touch. */
			memory.write_after_fetching(
				item.address, 4096 + 64U, {0x11});
		}
		const auto cold = executor.select_program(
			memory, item.address, ExecutionMode::long64);
		require(cold.program != nullptr && !cold.cache_hit,
			"undisturbed translation setup did not build a program");
		require(!item.disturb_neighbour ||
			memory.write_after_fetching_fired(),
			"the neighbouring write never landed inside the build");

		executor.prepare(memory, 42, sequences.mapping_changes,
			++sequences.code_changes, &sequences, read_test_code_sequences);
		const auto again = executor.select_program(
			memory, item.address, ExecutionMode::long64);
		require(again.program != nullptr && again.cache_hit,
			item.message);
	}
	require(executor.cache_diagnostics().validation_stamp_hits != 0,
		"an undisturbed translation never recorded a usable page stamp");
}

/* The decoder fetches an instruction's bytes in one window and the window
 * stops at the guest page, so it can only fault for a page the instruction has
 * already reached. An instruction ending inside the last mapped page still
 * decodes, and one that runs past it reports the fault at the first byte the
 * decoder could not read rather than at the address its window began at.
 */
void a_decode_at_a_page_edge_faults_where_it_stopped()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t edge = 16384;
	TestMemory memory;
	/* SYSCALL ending exactly on the last mapped byte. */
	memory.load(edge - 2U, {0x0f, 0x05});
	const auto ending = gadgets::build(memory, edge - 2U);
	require(ending.status == gadgets::BuildStatus::built &&
		ending.program.instruction_count() == 1,
		"an instruction ending on the last mapped byte did not decode");
	/* MOV EAX, imm32 whose immediate runs into the unmapped page. */
	memory.load(edge - 2U, {0xb8, 0x01});
	const auto crossing = gadgets::build(memory, edge - 2U);
	require(crossing.status == gadgets::BuildStatus::fetch_fault,
		"an instruction running past the last mapped page decoded");
	require(crossing.fault_address == edge,
		"a fetch fault was not reported at the first byte the "
		"decoder could not read");
}

/* One fetch window serves a run of instructions, and a block can outrun it.
 * Every instruction after a refill has to decode from the bytes at its own
 * address, so a window that forgets where it starts, or one that is refilled
 * from the wrong address, shows up as the wrong immediate here.
 */
void a_block_longer_than_the_fetch_window_decodes()
{
	using namespace uml::cpu::x86_64;
	constexpr unsigned count = 40;
	TestMemory memory;
	std::array<std::byte, count * 5U + 2U> code{};
	for (unsigned index = 0; index < count; ++index) {
		/* MOV EAX, index + 1 */
		code[index * 5U] = std::byte{0xb8};
		code[index * 5U + 1U] =
			static_cast<std::byte>(index + 1U);
	}
	code[count * 5U] = std::byte{0x0f};
	code[count * 5U + 1U] = std::byte{0x05};
	memory.fill(0, code);
	const auto built = gadgets::build(memory, 0);
	require(built.status == gadgets::BuildStatus::built &&
		built.program.instruction_count() == count + 1U,
		"a block longer than one fetch window did not decode whole");
	CpuState cpu{};
	const auto result = gadgets::execute(built.program, cpu, memory, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		cpu.gpr(Register::rax) == count,
		"a block decoded across a window refill did not run its own "
		"instructions");
}

void cache_retires_the_oldest_generation()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05});
	gadgets::Executor executor;
	for (std::uint64_t generation = 1; generation <= 9; ++generation) {
		executor.prepare(memory, generation, 1, 1);
		const auto selected = executor.select_program(
			memory, 0, ExecutionMode::long64);
		require(selected.status == gadgets::BuildStatus::built &&
			selected.program != nullptr && !selected.cache_hit,
			"new address space unexpectedly reused another's program");
	}
	require(executor.resident_generations() == 8,
		"x86 cache did not enforce the arm64 eight-generation limit");
	executor.prepare(memory, 1, 1, 1);
	const auto retired = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(retired.status == gadgets::BuildStatus::built &&
		retired.program != nullptr && !retired.cache_hit,
		"least-recently-current address space was not retired");
}

/* A slot names its address space by an id the cache hands out and takes back
 * when the space is retired, so the id of a retired space is the id of the
 * next to arrive. Every translation of the retired space must be gone by then
 * however many times the ids go round, or the newcomer would be served the
 * old space's program at the same address. */
void a_recycled_address_space_id_serves_no_old_translation()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05});
	gadgets::Executor executor;
	constexpr std::uint64_t spaces = 3 * 8 + 1;
	for (std::uint64_t generation = 1; generation <= spaces; ++generation) {
		executor.prepare(memory, generation, 1, 1);
		const auto fresh = executor.select_program(
			memory, 0, ExecutionMode::long64);
		require(fresh.status == gadgets::BuildStatus::built &&
			fresh.program != nullptr && !fresh.cache_hit,
			"a new address space was served another's program");
		const auto again = executor.select_program(
			memory, 0, ExecutionMode::long64);
		require(again.program != nullptr && again.cache_hit,
			"an address space did not find its own program");
	}
	/* The eight most recent are resident and the rest are gone. */
	executor.prepare(memory, spaces - 7, 1, 1);
	const auto kept = executor.select_program(
		memory, 0, ExecutionMode::long64);
	executor.prepare(memory, 1, 1, 1);
	const auto retired = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(kept.program != nullptr && kept.cache_hit,
		"a resident address space lost its program");
	require(retired.program != nullptr && !retired.cache_hit,
		"a retired address space was served its old program");
}

/* What the MMU says of a generation: the cache is told by function, since a
 * cache without an MMU of its own has none to ask. Bit g of a mask answers
 * for generation g. */
std::uint64_t destroyed_generations;
std::uint64_t unknown_generations;

int mmu_says(unsigned long generation) noexcept
{
	if ((unknown_generations >> generation) & 1U)
		return -1;
	return ((destroyed_generations >> generation) & 1U) != 0 ? 0 : 1;
}

/* One program at address 0 in every address space, and whether it was found
 * resident or had to be built. */
struct Lookup final {
	bool built;
	bool hit;
};

[[nodiscard]] Lookup look_up(uml::cpu::x86_64::gadgets::Executor &executor,
	TestMemory &memory, std::uint64_t generation)
{
	using namespace uml::cpu::x86_64;
	executor.prepare(memory, generation, 1, 1);
	const auto selected = executor.select_program(
		memory, 0, ExecutionMode::long64);
	return {selected.status == gadgets::BuildStatus::built &&
			selected.program != nullptr,
		selected.cache_hit};
}

/* An address space whose generation the MMU destroyed is let go of the next
 * time the executor looks after the count moved, and not before; one that is
 * live, or that the MMU cannot answer for, keeps its programs. */
void a_destroyed_generation_is_let_go_of_when_the_count_moves()
{
	using namespace uml::cpu::x86_64;
	destroyed_generations = unknown_generations = 0;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05});
	gadgets::Executor executor;
	for (std::uint64_t generation = 1; generation <= 3; ++generation)
		require(look_up(executor, memory, generation).built,
			"a new address space was not built");
	destroyed_generations = 1U << 1;
	unknown_generations = 1U << 2;
	executor.leave_gone_generations(0, &mmu_says);
	require(executor.resident_generations() == 3,
		"an executor looked before the count moved");
	executor.leave_gone_generations(1, &mmu_says);
	require(executor.resident_generations() == 2,
		"a destroyed address space was kept, or a live one let go");
	executor.leave_gone_generations(1, &mmu_says);
	destroyed_generations |= 1U << 3;
	executor.leave_gone_generations(1, &mmu_says);
	require(executor.resident_generations() == 2,
		"an executor looked again at the same count");
	const auto live = look_up(executor, memory, 3);
	const auto unknown = look_up(executor, memory, 2);
	require(live.hit && unknown.hit,
		"an address space the MMU cannot answer for lost its program");
	executor.leave_gone_generations(2, &mmu_says);
	require(executor.resident_generations() == 1,
		"the last destroyed address space was kept");
	const auto again = look_up(executor, memory, 1);
	require(again.built && !again.hit,
		"a let-go address space was served its old program");
}

/* Fill `memory`, `blocks * stride` bytes from zero, with `blocks` blocks
 * `stride` bytes apart, each 63 `add rax, 1` and a SYSCALL. */
void fill_add_blocks(TestMemory &memory, std::size_t blocks, std::size_t stride)
{
	std::vector<std::byte> block(stride, std::byte{0x90});
	for (std::size_t add = 0; add < 63; ++add) {
		block[add * 4] = std::byte{0x48};
		block[add * 4 + 1] = std::byte{0x83};
		block[add * 4 + 2] = std::byte{0xc0};
		block[add * 4 + 3] = std::byte{0x01};
	}
	block[252] = std::byte{0x0f};
	block[253] = std::byte{0x05};
	for (std::size_t index = 0; index < blocks; ++index)
		memory.fill(index * stride, block);
}

/* The process's gadget-cache budget bounds what an executor keeps of its
 * address spaces, whatever its slots would hold. A workload of large blocks,
 * each translated once, holds some megabytes under the default budget and is
 * never asked to shed; under a quarter of that, the address space is asked to
 * shed whenever a store takes the executor's address spaces past the budget,
 * the executor lets go of it at its next run, as of a destroyed one, and
 * translates afresh into a new one, so what it keeps stays within the budget
 * and its blocks still run as they should. An executor that goes refunds what
 * it held. */
void the_budget_bounds_what_an_executor_keeps()
{
	using namespace uml::cpu::x86_64;
	namespace budget = uml::cpu::gadget_cache;
	constexpr std::size_t blocks = 3000;
	constexpr std::size_t stride = 256;
	constexpr std::size_t kib = 1024;
	/* What one store and its links may add before the next store's check:
	 * a translation, a record table's growth and a target table. */
	constexpr std::size_t one_store = 128 * kib;
	destroyed_generations = unknown_generations = 0;
	TestMemory memory{0, blocks * stride + 4096};
	fill_add_blocks(memory, blocks, stride);
	const auto open = [] {
		const auto state = budget::ledger_state();
		return state.total - state.shedding;
	};
	/* Select every block once, each at the start of a run, as the runtime
	 * starts one; return the most the open address spaces held after any
	 * store, and count the translations. */
	const auto run = [&](gadgets::Executor &executor,
				 std::size_t &translated) {
		std::size_t peak = 0;
		for (std::size_t index = 0; index < blocks; ++index) {
			executor.leave_gone_generations(0, &mmu_says);
			executor.prepare(memory, 7, 1, 1);
			const auto selected = executor.select_program(memory,
				index * stride, ExecutionMode::long64);
			if (selected.status != gadgets::BuildStatus::built ||
			    selected.program == nullptr)
				return std::size_t{0};
			if (!selected.cache_hit)
				++translated;
			peak = std::max(peak, open());
		}
		return peak;
	};
	const auto base = budget::ledger_state();
	require(base.budget == budget::fallback_budget,
		"the default budget was not in effect");
	std::size_t control = 0;
	{
		gadgets::Executor executor;
		std::size_t translated = 0;
		control = std::max(run(executor, translated),
			run(executor, translated));
		require(budget::ledger_state().reliefs == base.reliefs &&
			translated == blocks && control >= blocks * 63 * 8,
			"the workload did not fit the default budget");
	}
	require(budget::ledger_state().total == base.total,
		"an executor that went kept its charges");
	const auto small = std::max(control / 4, 4 * one_store);
	budget::set_budget(small);
	{
		gadgets::Executor executor;
		std::size_t translated = 0;
		const auto peak = std::max(run(executor, translated),
			run(executor, translated));
		const auto after = budget::ledger_state();
		std::printf("x86 gadget budget: the control held %zu KiB; under "
			    "%zu KiB the address spaces held at most %zu KiB "
			    "in %llu reliefs\n", control / kib, small / kib,
			peak / kib, static_cast<unsigned long long>(
				after.reliefs - base.reliefs));
		CpuState cpu{};
		cpu.rip = 0;
		run_to_system_call(executor, memory, cpu, "a shed block");
		require(peak != 0 && peak <= small + one_store,
			"the budget did not bound the address spaces");
		require(after.reliefs > base.reliefs &&
			translated > blocks + blocks / 2,
			"an address space past the budget was not shed");
		require(cpu.gpr(Register::rax) == 63,
			"a block translated afresh ran wrongly");
	}
	budget::set_budget(budget::fallback_budget);
	require(budget::ledger_state().total == base.total,
		"an executor that went kept its charges");
}

/* An address space the budget asked to shed is let go of at the executor's
 * next look. A look that finds no room for its id leaves the address space
 * held and asked for again, so the one after it lets go of it. The room is
 * the look's first allocation, whether the executor keeps its translations to
 * itself or shares them (in_both_modes). */
void a_shed_address_space_is_asked_for_again_after_a_failed_allocation()
{
	using namespace uml::cpu::x86_64;
	namespace budget = uml::cpu::gadget_cache;
	constexpr std::size_t blocks = 1500;
	constexpr std::size_t stride = 256;
	constexpr std::size_t small = 512 * 1024;
	destroyed_generations = unknown_generations = 0;
	TestMemory memory{0, blocks * stride + 4096};
	fill_add_blocks(memory, blocks, stride);
	const auto base = budget::ledger_state();
	budget::set_budget(small);
	bool failed = false;
	bool asked_again = true;
	{
		gadgets::Executor executor;
		for (std::size_t index = 0; index < blocks && asked_again; ++index) {
			if (budget::ledger_state().shedding != 0) {
				uml::testing::fail_next_allocation();
				executor.leave_gone_generations(0, &mmu_says);
				failed = !uml::testing::cancel_pending_allocation_failure() ||
					failed;
				executor.leave_gone_generations(0, &mmu_says);
				asked_again = budget::ledger_state().shedding == 0;
			}
			executor.prepare(memory, 7, 1, 1);
			const auto selected = executor.select_program(
				memory, index * stride, ExecutionMode::long64);
			if (selected.status != gadgets::BuildStatus::built ||
			    selected.program == nullptr)
				asked_again = false;
		}
	}
	budget::set_budget(budget::fallback_budget);
	require(failed, "no look at a shedding address space met a failed "
		"allocation");
	require(asked_again, "an address space that could not be let go of "
		"was not asked for again");
	require(budget::ledger_state().total == base.total,
		"an executor that went kept its charges");
}

/* The address space the cache last prepared may be the one let go of: the
 * executor must prepare again before it is used, and the next address space
 * must work. */
void the_prepared_generation_can_be_let_go_of()
{
	using namespace uml::cpu::x86_64;
	destroyed_generations = unknown_generations = 0;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05});
	gadgets::Executor executor;
	require(look_up(executor, memory, 7).built,
		"a new address space was not built");
	destroyed_generations = 1U << 7;
	executor.leave_gone_generations(1, &mmu_says);
	require(executor.resident_generations() == 0,
		"the prepared destroyed address space was kept");
	/* Until it prepares again the executor translates without caching, as
	 * after a failed prepare, rather than bind the address space it let go
	 * of. */
	const auto unbound = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(unbound.status == gadgets::BuildStatus::built &&
			executor.resident_generations() == 0,
		"an executor cached into the address space it let go of");
	const auto next = look_up(executor, memory, 8);
	const auto repeat = look_up(executor, memory, 8);
	require(next.built && !next.hit && repeat.hit,
		"an address space after one let go of did not cache");
}

/* A generation number the MMU gave out again after destroying it (which the
 * kernel never does) names new code: the old address space's program must be
 * gone, not found resident under the same code-change count. */
void a_number_given_out_again_runs_its_new_code()
{
	using namespace uml::cpu::x86_64;
	destroyed_generations = unknown_generations = 0;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05});
	gadgets::Executor executor;
	require(look_up(executor, memory, 5).built, "address space not built");
	require(look_up(executor, memory, 6).built, "address space not built");
	destroyed_generations = 1U << 5;
	executor.leave_gone_generations(1, &mmu_says);
	destroyed_generations = 0;
	memory.load(0, {0x90, 0x0f, 0x05});
	executor.prepare(memory, 5, 1, 1);
	const auto selected = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(selected.status == gadgets::BuildStatus::built &&
			selected.program != nullptr && !selected.cache_hit &&
			selected.program->source().size() == 3,
		"a number given out again was served the old address space's program");
}

/* A destroyed address space keeps its place in the order the window retires
 * in until the least-recently-used rule reaches it, so the live address
 * spaces kept are the ones kept without the rule. Generations 1..8 fill the
 * window and 3 is destroyed. Entering a ninth retires 1, the least recently
 * used; with the place given up nothing would be retired and 1 would still
 * hit. Entering 1 retires 2, and a tenth reaches 3's place and drops it, so 4
 * still hits; a place at the back of the order would have retired 4 instead. */
void a_destroyed_generation_keeps_its_window_place()
{
	using namespace uml::cpu::x86_64;
	destroyed_generations = unknown_generations = 0;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05});
	gadgets::Executor executor;
	for (std::uint64_t generation = 1; generation <= 8; ++generation)
		require(look_up(executor, memory, generation).built,
			"a new address space was not built");
	destroyed_generations = 1U << 3;
	executor.leave_gone_generations(1, &mmu_says);
	require(executor.resident_generations() == 7,
		"the destroyed address space was kept");
	require(look_up(executor, memory, 9).built, "address space not built");
	const auto first = look_up(executor, memory, 1);
	require(first.built && !first.hit,
		"the least recently used address space survived a full window");
	require(look_up(executor, memory, 10).built, "address space not built");
	const auto fourth = look_up(executor, memory, 4);
	require(fourth.hit,
		"a live address space was retired in place of the tombstone");
}

/* Letting go of an address space frees its id for the next to arrive, and the
 * room for that is found before anything changes: when it cannot be, the
 * address space is still held, the executor prepares afresh, and the next look
 * lets go of it as if nothing had happened. Every allocation of the look is
 * failed in turn, with an executor that keeps its translations to itself, for
 * which the room is the only one, and with one that shares them
 * (in_both_modes), whose bodies also queue and are reclaimed behind a snapshot
 * of the executors: a failure there only leaves them for a later reclaim. */
void letting_go_of_an_address_space_survives_a_failed_allocation()
{
	using namespace uml::cpu::x86_64;
	bool held = false;
	for (std::size_t allowed = 0;; ++allowed) {
		require(allowed < 256, "the failure point never moved past "
			"the end of letting go of an address space");
		destroyed_generations = unknown_generations = 0;
		TestMemory memory;
		memory.load(0, {0x0f, 0x05});
		gadgets::Executor executor;
		for (std::uint64_t generation = 1; generation <= 3; ++generation)
			require(look_up(executor, memory, generation).built,
				"a new address space was not built");
		destroyed_generations = 1U << 2;
		uml::testing::fail_after_allocations(allowed);
		executor.leave_gone_generations(1, &mmu_says);
		/* True once the look made fewer allocations than allowed. */
		const bool swept = uml::testing::cancel_pending_allocation_failure();
		held = held || executor.resident_generations() == 3;
		/* The same count: a look that failed is made again, and one that
		 * did not has nothing left to do. */
		executor.leave_gone_generations(1, &mmu_says);
		require(executor.resident_generations() == 2,
			"a destroyed address space was not let go of at the next look");
		const auto first = look_up(executor, memory, 1);
		const auto second = look_up(executor, memory, 2);
		const auto third = look_up(executor, memory, 3);
		require(first.hit && third.hit,
			"a live address space lost its program to a failed look");
		require(second.built && !second.hit,
		"a let-go address space was served its old program");
		if (swept) {
			require(held, "no failure left an address space held "
				"for want of room for its id");
			return;
		}
	}
}

/* An address space entering a full window retires the least recently used,
 * which needs room for its id. Whichever allocation of that fails, the
 * executor goes on working: it prepares afresh, translates, and caches. Run
 * in both modes, as above. */
void retiring_the_oldest_address_space_survives_a_failed_allocation()
{
	using namespace uml::cpu::x86_64;
	destroyed_generations = unknown_generations = 0;
	for (std::size_t allowed = 0;; ++allowed) {
		require(allowed < 256, "the failure point never moved past "
			"the entry of a ninth address space");
		TestMemory memory;
		memory.load(0, {0x0f, 0x05});
		gadgets::Executor executor;
		for (std::uint64_t generation = 1; generation <= 8; ++generation)
			require(look_up(executor, memory, generation).built,
				"a new address space was not built");
		uml::testing::fail_after_allocations(allowed);
		executor.prepare(memory, 9, 1, 1);
		/* True once the prepare made fewer allocations than allowed, so
		 * that every one of its allocations has failed in turn. */
		const bool swept =
			uml::testing::cancel_pending_allocation_failure();
		const auto ninth = look_up(executor, memory, 9);
		const auto again = look_up(executor, memory, 9);
		require(ninth.built && again.hit &&
				executor.resident_generations() <= 8,
			"an executor did not recover from a failed allocation "
			"entering a ninth address space");
		if (swept)
			return;
	}
}

/* A slot is current when the epoch it carries is its space's, and the epoch
 * counts code changes in sixteen bits. A slot the space has not run for a whole
 * epoch cycle must not be taken for current when the count comes round to its
 * own again: the cache clears the epochs a wrap would repeat.
 *
 * Block A is selected once and its bytes rewritten behind the cache's back.
 * Block B then runs through every code change that returns the epoch to A's,
 * and A, never run in between, must be validated against the guest's bytes and
 * rebuilt instead of served stale. */
void a_validation_does_not_outlive_the_epochs_it_counts()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	memory.load(0x100, {0xb8, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x05});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto cold = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(cold.status == gadgets::BuildStatus::built &&
		cold.program != nullptr && !cold.cache_hit,
		"the first program was not built");
	memory.load(1, {0x09});
	std::uint64_t changes = 1;
	for (unsigned change = 0; change < 65'535; ++change) {
		executor.prepare(memory, 42, 1, ++changes);
		const auto other = executor.select_program(
			memory, 0x100, ExecutionMode::long64);
		require(other.program != nullptr,
			"the second program was not selected");
	}
	const auto before = executor.cache_diagnostics();
	const auto rebuilt = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(rebuilt.status == gadgets::BuildStatus::built &&
		rebuilt.program != nullptr && !rebuilt.cache_hit,
		"a stale program survived a whole epoch cycle");
	CpuState cpu{};
	const auto executed = gadgets::execute(
		*rebuilt.program, cpu, memory, executor, 64);
	const auto after = executor.cache_diagnostics();
	require(executed.event == uml::cpu::EngineEvent::system_call &&
		cpu.gpr(Register::rax) == 9,
		"the rebuilt program ran the old immediate");
	require(after.validation_attempts == before.validation_attempts + 1 &&
		after.validation_mismatches ==
			before.validation_mismatches + 1,
		"the program was not validated against the guest's bytes");
}

void cache_links_common_successors()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05, 0x0f, 0x05});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto first = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto successor = executor.select_program(
		memory, 2, ExecutionMode::long64);
	const auto returned = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto before = executor.cache_diagnostics().successor_hits;
	const auto linked = executor.select_program(
		memory, 2, ExecutionMode::long64);
	require(first.program != nullptr && successor.program != nullptr &&
		returned.program != nullptr && linked.program != nullptr,
		"successor-link setup did not select both programs");
	require(linked.cache_hit &&
		executor.cache_diagnostics().successor_hits == before + 1,
		"common successor did not bypass the main cache probe");
}

void cache_chains_direct_branches()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xeb, 0x0e});
	memory.load(16, {
		0xb8, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	/* CMP EAX,EAX; JE 16 exercises the conditional linked entry against the
	 * same active target as the direct JMP above.
	 */
	memory.load(32, {0x39, 0xc0, 0x74, 0xec});
	/* CALL 16 from 48; the return address is 53. */
	memory.load(48, {0xe8, 0xdb, 0xff, 0xff, 0xff});

	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto target = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto direct = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto conditional = executor.select_program(
		memory, 32, ExecutionMode::long64);
	const auto call = executor.select_program(
		memory, 48, ExecutionMode::long64);
	require(target.program != nullptr && direct.program != nullptr &&
			conditional.program != nullptr && call.program != nullptr,
		"direct-link setup did not cache all programs");

	CpuState cpu{};
	const auto chained = gadgets::execute(
		*direct.program, cpu, memory, executor, 64);
	require(chained.event == uml::cpu::EngineEvent::system_call &&
			chained.retired_instructions == 3 &&
			cpu.gpr(Register::rax) == 42 && cpu.rip == 23,
		"cached direct JMP did not enter its native successor");

	cpu = CpuState{};
	cpu.rip = 32;
	const auto conditionally_chained = gadgets::execute(
		*conditional.program, cpu, memory, executor, 64);
	require(conditionally_chained.event ==
			uml::cpu::EngineEvent::system_call &&
			conditionally_chained.retired_instructions == 4 &&
			cpu.gpr(Register::rax) == 42 && cpu.rip == 23,
		"cached JE did not enter its native successor");

	cpu = CpuState{};
	cpu.rip = 48;
	cpu.gpr(Register::rsp) = 512;
	const auto call_chained = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	require(call_chained.event == uml::cpu::EngineEvent::system_call &&
			call_chained.retired_instructions == 3 && cpu.rip == 23 &&
			cpu.gpr(Register::rsp) == 504 && memory.qword(504) == 53,
		"cached direct CALL did not push its return address and enter its "
		"native callee");

	cpu = CpuState{};
	const auto bounded = gadgets::execute(
		*direct.program, cpu, memory, executor, 1);
	require(bounded.event == uml::cpu::EngineEvent::continued &&
			bounded.retired_instructions == 1 &&
			bounded.instruction_budget_exhausted && cpu.rip == 16,
		"native direct link crossed the exact instruction budget");

	/* A code epoch resets active native links before exact validation. The
	 * source may be retained immediately, but it must stop at the reset target
	 * until that target is itself validated and activated again.
	 */
	executor.prepare(memory, 42, 1, 2);
	const auto source_after_change = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(source_after_change.program != nullptr &&
			source_after_change.cache_hit,
		"unchanged direct-link source did not survive exact validation");
	cpu = CpuState{};
	const auto reset = gadgets::execute(
		*source_after_change.program, cpu, memory, executor, 64);
	require(reset.event == uml::cpu::EngineEvent::continued &&
			reset.retired_instructions == 1 && cpu.rip == 16,
		"code epoch left a stale native target active");

	const auto target_after_change = executor.select_program(
		memory, 16, ExecutionMode::long64);
	cpu = CpuState{};
	const auto restored = gadgets::execute(
		*source_after_change.program, cpu, memory, executor, 64);
	require(target_after_change.program != nullptr &&
			target_after_change.cache_hit,
			"unchanged native target did not survive exact validation");
	require(restored.event == uml::cpu::EngineEvent::system_call &&
		restored.retired_instructions == 3 && cpu.rip == 23,
		"validated native target was not reactivated");
}

/* Every indirect branch in an address space shares one tier keyed on the
 * target, and a block publishes itself into that tier when the cache hands it
 * out. So a RET enters a return site it has never returned to, and a site that
 * has already spent both of its private entries still reaches a third target
 * without leaving the stream -- neither of which a tier keyed on the branch
 * can do, because the addresses a function returns to belong to its callers.
 * The private entries stay the first tier and still carry a site's own targets
 * when the shared one evicts them.
 */
void cache_chains_indirect_transfers()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xff, 0xe0});
	memory.load(16, {
		0xb8, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(24, {
		0xb8, 0x2b, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(32, {0xc3});
	memory.load(40, {
		0xb8, 0x2c, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(48, {0xff, 0xd0});
	/* Folds onto the same shared-tier entry as 16. */
	memory.load(0x1011, {
		0xb8, 0x2d, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(512, {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto jump = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto ret = executor.select_program(
		memory, 32, ExecutionMode::long64);
	const auto call = executor.select_program(
		memory, 48, ExecutionMode::long64);
	require(jump.program != nullptr && ret.program != nullptr &&
			call.program != nullptr,
		"indirect-link setup did not cache its branch programs");

	CpuState cpu{};
	cpu.gpr(Register::rax) = 16;
	const auto cold = gadgets::execute(
		*jump.program, cpu, memory, executor, 64);
	require(cold.event == uml::cpu::EngineEvent::continued &&
			cold.retired_instructions == 1 && cpu.rip == 16,
		"indirect JMP entered a target no block had run at");

	const auto target_a = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto target_b = executor.select_program(
		memory, 24, ExecutionMode::long64);
	const auto target_c = executor.select_program(
		memory, 40, ExecutionMode::long64);
	require(target_a.program != nullptr && target_b.program != nullptr &&
			target_c.program != nullptr,
		"indirect-link setup did not cache all three targets");

	/* Three targets through a two-entry site, revisited: the third would have
	 * evicted one of the first two from the site alone. */
	struct Visit final {
		std::uint64_t target;
		std::uint64_t result;
		std::uint64_t next;
	};
	static constexpr Visit visits[] = {
		{16, 42, 23}, {24, 43, 31}, {40, 44, 47}, {16, 42, 23},
		{24, 43, 31},
	};
	for (const auto &visit : visits) {
		cpu = CpuState{};
		cpu.gpr(Register::rax) = visit.target;
		const auto entered = gadgets::execute(
			*jump.program, cpu, memory, executor, 64);
		require(entered.event == uml::cpu::EngineEvent::system_call &&
				entered.retired_instructions == 3 &&
				cpu.gpr(Register::rax) == visit.result &&
				cpu.rip == visit.next,
			"indirect JMP left the stream for a target that had run");
	}

	/* This RET has never returned anywhere, and it still enters its return
	 * site, because the site announced itself when the cache handed it out. */
	cpu = CpuState{};
	cpu.rip = 32;
	cpu.gpr(Register::rsp) = 512;
	const auto returned = gadgets::execute(
		*ret.program, cpu, memory, executor, 64);
	require(returned.event == uml::cpu::EngineEvent::system_call &&
			returned.retired_instructions == 3 && cpu.rip == 23 &&
			cpu.gpr(Register::rsp) == 520,
		"RET left the stream for a return site that had run");

	cpu = CpuState{};
	cpu.gpr(Register::rax) = 16;
	const auto bounded = gadgets::execute(
		*jump.program, cpu, memory, executor, 1);
	require(bounded.event == uml::cpu::EngineEvent::continued &&
			bounded.retired_instructions == 1 &&
			bounded.instruction_budget_exhausted && cpu.rip == 16,
		"native indirect link crossed the exact instruction budget");

	cpu = CpuState{};
	cpu.rip = 48;
	cpu.gpr(Register::rax) = 16;
	cpu.gpr(Register::rsp) = 1024;
	const auto called = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	require(called.event == uml::cpu::EngineEvent::system_call &&
			called.retired_instructions == 3 && cpu.rip == 23,
		"indirect CALL left the stream for a target that had run");
	require(cpu.gpr(Register::rsp) == 1016 &&
		memory.qword(1016) == 50,
		"indirect CALL lost its architectural stack write");

	/* Evict the shared entry 16 answers to. The JMP trained its own site on
	 * the cold miss above, so its private tier still carries the target. */
	const auto conflicting = executor.select_program(
		memory, 0x1011, ExecutionMode::long64);
	require(conflicting.program != nullptr,
		"conflicting target did not cache");
	cpu = CpuState{};
	cpu.gpr(Register::rax) = 16;
	const auto retained = gadgets::execute(
		*jump.program, cpu, memory, executor, 64);
	require(retained.event == uml::cpu::EngineEvent::system_call &&
			retained.retired_instructions == 3 &&
			cpu.gpr(Register::rax) == 42 && cpu.rip == 23,
		"a shared-tier eviction cost a site its own trained target");
}

/* No other test here binds an execution context, so `code_link_safe` has never
 * been reachable: without one it refuses every link and the engine leaves the
 * stream. The two call tests below are about exactly that decision, so they
 * supply the sequence contract a real address space provides.
 */
using uml::test::x86_64_code_change_history::Sequences;
using uml::test::x86_64_code_change_history::read_code_changes;
using uml::test::x86_64_code_change_history::read_sequences;

/* A call whose return-address push lands on an executable page publishes that
 * write like any other store, and then follows its link. What makes that safe
 * is not the page but the bytes: publication deactivates every cached block the
 * write actually overlapped and resets its link, and the link itself still has
 * to pass `code_link_safe`. Leaving the stream merely because some executable
 * page was written is what an ordinary store already declines to do, and a call
 * that did it anyway left the engine paying a C++ round trip for the majority
 * of the control transfers a real program performs.
 */
/* A guest code write says nothing on its own about the blocks this cache
 * holds. Cooling every link in the address space for one is what made a JIT or
 * a fresh executable mapping cost a C++ round trip for every edge afterwards,
 * however far the write was from anything translated. The address space knows
 * which bytes moved, and retiring exactly the translations covering them is
 * the same selective retirement the cache already performs for a write it made
 * itself.
 */
void an_unrelated_code_change_leaves_a_link_armed()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe9, 0x0b, 0x00, 0x00, 0x00});
	memory.load(16, {0x0f, 0x05});
	Sequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	const auto prepare = [&](gadgets::Executor &executor) {
		executor.prepare(memory, 42, sequences.mapping_changes,
			sequences.code_changes, &sequences, read_sequences,
			read_code_changes);
	};
	/* Only the jump is selected before each run. Selecting the target would
	 * re-arm its link, which is what the dispatch loop does and what would
	 * hide the difference this test is about. */
	const auto seed = [&](gadgets::Executor &executor) {
		return executor.select_program(memory, 16,
			ExecutionMode::long64).program != nullptr;
	};
	const auto run = [&](gadgets::Executor &executor) {
		const auto jump = executor.select_program(
			memory, 0, ExecutionMode::long64);
		if (jump.program == nullptr)
			return gadgets::ExecuteResult{};
		CpuState cpu{};
		return gadgets::execute(*jump.program, cpu, memory, executor, 64);
	};

	gadgets::Executor executor;
	prepare(executor);
	require(seed(executor), "linked jump target did not cache");
	const auto armed = run(executor);
	require(armed.event == uml::cpu::EngineEvent::system_call &&
			armed.retired_instructions == 2,
		"linked jump did not enter its target");

	/* A page neither block was translated from. */
	sequences.change(0x2000, 4096);
	prepare(executor);
	const auto unrelated = run(executor);
	require(unrelated.event == uml::cpu::EngineEvent::system_call &&
			unrelated.retired_instructions == 2,
		"a code change on an untranslated page retired a link");

	/* The page the target was translated from. */
	sequences.change(0, 4096);
	prepare(executor);
	const auto covered = run(executor);
	require(covered.event == uml::cpu::EngineEvent::continued &&
			covered.retired_instructions == 1,
		"a code change covering the target left its link armed");

	/* And a history that cannot describe the span retires everything. */
	gadgets::Executor blind;
	prepare(blind);
	require(seed(blind) &&
			run(blind).event == uml::cpu::EngineEvent::system_call,
		"linked jump did not enter its target for the second executor");
	sequences.recorded = sequences.history.size() + 1;
	sequences.change(0x2000, 4096);
	prepare(blind);
	require(run(blind).event == uml::cpu::EngineEvent::continued,
		"an undescribed code change left a link armed");
}

/* A link sweep releases the records only replaced blocks named, as the arm64
 * cache's does, and keeps what the resident blocks and a trained site need.
 *
 * One jump is rewritten to a new target thousands of times, each through a
 * code change the address space can describe, so only the jump is retired and
 * rebuilt, and every earlier target is named by nothing but, for a round, an
 * indirect site's cells. Those records pile up past the sweep floor and a
 * store sweeps them; the site, asked again for the target it learned last
 * round, must find its cell emptied rather than naming a released record. A
 * pair linked before any sweep still links after them, and a resident block
 * nothing links to keeps the record its slot remembers and that a block
 * translated later links through.
 */
void a_link_sweep_releases_what_only_replaced_blocks_named()
{
	using namespace uml::cpu::x86_64;
	constexpr std::size_t rounds = 6000;
	constexpr std::uint64_t far_targets = 0x10'0000;
	TestMemory memory;
	const auto jump_to = [&](std::uint64_t source, std::uint64_t target) {
		const auto displacement =
			static_cast<std::uint32_t>(target - (source + 5U));
		memory.load(source, {0xe9,
			static_cast<std::uint8_t>(displacement),
			static_cast<std::uint8_t>(displacement >> 8),
			static_cast<std::uint8_t>(displacement >> 16),
			static_cast<std::uint8_t>(displacement >> 24)});
	};
	const auto target = [](std::size_t round) {
		return far_targets + 16U * round;
	};
	jump_to(0, target(0));
	memory.load(0x100, {0xff, 0xe0});
	memory.load(0x200, {0xb8, 0x4d, 0x00, 0x00, 0x00, 0x0f, 0x05});
	jump_to(0x300, 0x200);
	memory.load(0x400, {0xb8, 0x4e, 0x00, 0x00, 0x00, 0x0f, 0x05});
	jump_to(0x500, 0x400);
	Sequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	const auto prepare = [&] {
		executor.prepare(memory, 42, sequences.mapping_changes,
			sequences.code_changes, &sequences, read_sequences,
			read_code_changes);
	};
	const auto run = [&](std::uint64_t rip, std::uint64_t rax = 0) {
		CpuState cpu{};
		cpu.rip = rip;
		cpu.gpr(Register::rax) = rax;
		const auto selected = executor.select_program(
			memory, rip, ExecutionMode::long64);
		if (selected.program == nullptr)
			return std::pair{gadgets::ExecuteResult{}, cpu};
		const auto result = gadgets::execute(
			*selected.program, cpu, memory, executor, 64);
		return std::pair{result, cpu};
	};
	const auto linked = [&](std::uint64_t rip, std::uint64_t value) {
		const auto [result, cpu] = run(rip);
		return result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 3 &&
			cpu.gpr(Register::rax) == value;
	};

	prepare();
	require(executor.select_program(memory, 0x200,
			ExecutionMode::long64).program != nullptr &&
			executor.select_program(memory, 0x400,
				ExecutionMode::long64).program != nullptr &&
			linked(0x300, 0x4d),
		"link-sweep setup did not link its resident pair");
	for (std::size_t round = 0; round < rounds; ++round) {
		if (round != 0) {
			jump_to(0, target(round));
			/* Each prepare reads only the newest change. */
			sequences.recorded = 0;
			sequences.change(0, 5);
			prepare();
		}
		const auto [jumped, cpu] = run(0);
		require(jumped.event == uml::cpu::EngineEvent::continued &&
				jumped.retired_instructions == 1 &&
				cpu.rip == target(round),
			"a rewritten jump did not leave for its new target");
		if (round == 0)
			continue;
		for (const auto asked : {target(round - 1), target(round)}) {
			const auto [indirect, landed] = run(0x100, asked);
			require(indirect.event ==
					uml::cpu::EngineEvent::continued &&
					landed.rip == asked,
				"an indirect site lost a target a sweep released");
		}
	}
	const auto &diagnostics = executor.cache_diagnostics();
	require(linked(0x300, 0x4d),
			"a sweep reset a link between resident blocks");
	require(executor.select_program(memory, 0x400,
			ExecutionMode::long64).program != nullptr &&
		linked(0x500, 0x4e),
		"a sweep released the record of a resident block");
	require(diagnostics.link_sweeps != 0 &&
		diagnostics.released_links >= rounds / 2 &&
		diagnostics.released_sites == 0,
		"replaced jumps' targets were not swept");
}

void linked_call_crosses_a_disjoint_executable_stack_write()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe8, 0x0b, 0x00, 0x00, 0x00});
	memory.load(16, {0x0f, 0x05});
	memory.set_executable_page(0);
	Sequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_sequences);
	const auto target = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto call = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(target.program != nullptr && call.program != nullptr,
		"executable-stack CALL did not populate its direct link");

	CpuState cpu{};
	cpu.gpr(Register::rsp) = 0x800;
	const auto result = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 2 && cpu.rip == 18,
		"direct CALL left the stream for a stack write that changed "
		"no instruction it was about to run");
	require(cpu.gpr(Register::rsp) == 0x7f8 &&
		memory.qword(0x7f8) == 5 && memory.invalidations() == 1,
		"code-changing CALL lost its architectural stack write");
}

/* The same call, with the push aimed at the callee's own instructions. Nothing
 * about the call gadget stops this one; publication deactivates the block the
 * write overlapped and resets the link it was reached through, so following
 * that link finds the fallback and returns to have the callee retranslated.
 */
void linked_call_stops_when_the_stack_write_hits_its_target()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe8, 0x0b, 0x00, 0x00, 0x00});
	memory.load(16, {0x0f, 0x05});
	memory.set_executable_page(0);
	Sequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_sequences);
	const auto target = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto call = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(target.program != nullptr && call.program != nullptr,
		"overlapping-stack CALL did not populate its direct link");

	CpuState cpu{};
	/* The eight-byte push lands on 16, which is the callee. */
	cpu.gpr(Register::rsp) = 24;
	const auto result = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1 && cpu.rip == 16,
		"direct CALL entered a callee its own stack write had just "
		"overwritten");
	require(cpu.gpr(Register::rsp) == 16 &&
		memory.qword(16) == 5 && memory.invalidations() == 1,
		"overlapping-stack CALL lost its architectural stack write");
}

void short_blocks_retain_only_their_translated_storage()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05});
	const auto built = gadgets::build(memory, 0);
	require(built.status == gadgets::BuildStatus::built &&
		built.program.instruction_count() == 1,
		"short allocation-regression block did not build");
	/* Allow allocator rounding and its smallest bucket, but not a
	 * maximum-size decoder reservation, and nothing outside the two
	 * allocations a translation is: the native stream, and the guest bytes
	 * the decoder read. Inspect the actual allocations rather than an
	 * implementation counter. */
	require(built.program.source().size() == 2,
		"short cached block did not save its own two bytes");
	require(built.program.replay_bytes() == 0,
		"a translation nothing replayed carries decoded "
		"instructions");
	require(malloc_size(built.program.stream_allocation()) != 0 &&
		malloc_size(built.program.stream_allocation()) <=
		2 * (built.program.native_stream().size_bytes() +
			gadgets::stream_header_words * sizeof(std::uintptr_t)),
		"short cached block retained excess native stream capacity");
	require(malloc_size(built.program.source().data()) <=
		2 * built.program.source().size() + 32,
		"short cached block retained the decoder's spare capacity");
}

/* What a block keeps, and when. A translation is built with its native stream
 * and its saved source and nothing else; the first budget-limited replay
 * decodes the block out of that source and the block keeps the result, because
 * the dispatches that end inside a block keep ending inside it.
 */
void a_replay_keeps_what_it_decoded()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x48, 0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1
		0x48, 0xff, 0xc0,                         // inc rax
		0x48, 0x83, 0xc0, 0x02,                   // add rax, 2
		0x48, 0x01, 0xc0,                         // add rax, rax
		0x0f, 0x05,                               // syscall
	});
	const auto built = gadgets::build(memory, 0);
	require(built.status == gadgets::BuildStatus::built &&
		built.program.instruction_count() == 5 &&
		built.program.replay_bytes() == 0,
		"a fresh translation did not build empty of decoded "
		"instructions");
	/* A block entered with the budget to finish it never asks. */
	CpuState whole{};
	const auto ran = gadgets::execute(built.program, whole, memory, 5);
	require(ran.event == uml::cpu::EngineEvent::system_call &&
		built.program.replay_bytes() == 0,
		"a block that ran to its end decoded itself again");
	const auto kept = 5 * sizeof(gadgets::Gadget);
	CpuState cpu{};
	const auto first = gadgets::execute(built.program, cpu, memory, 2);
	require(first.event == uml::cpu::EngineEvent::continued &&
		first.instruction_budget_exhausted && cpu.rip == 10 &&
		cpu.gpr(Register::rax) == 2,
		"a first replay did not stop at its budget");
	require(built.program.replay_bytes() == kept,
		"a replayed block did not keep what it decoded");
	CpuState second{};
	const auto again = gadgets::execute(built.program, second, memory, 3);
	require(again.event == uml::cpu::EngineEvent::continued &&
		again.instruction_budget_exhausted && second.rip == 14 &&
		second.gpr(Register::rax) == 4,
		"a second replay did not read the kept instructions");
	require(built.program.replay_bytes() == kept,
		"a second replay decoded the block again");
}

/* What a budget shorter than the block replays, against the block itself. A
 * translation keeps no decoded instructions, so the replay decodes them back
 * out of the bytes the build saved, and what it must reproduce is every
 * instruction boundary of the original block. */
void budget_limited_replay_matches_the_whole_block()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* Seven instructions of four different lengths, so a replay that
	 * mistakes one boundary lands on a different instruction. */
	memory.load(0, {
		0x48, 0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1
		0x48, 0xff, 0xc0,                         // inc rax
		0x48, 0x83, 0xc0, 0x02,                   // add rax, 2
		0x48, 0x01, 0xc0,                         // add rax, rax
		0x48, 0xff, 0xc0,                         // inc rax
		0x48, 0x83, 0xc0, 0x04,                   // add rax, 4
		0x0f, 0x05,                               // syscall
	});
	const auto built = gadgets::build(memory, 0);
	require(built.status == gadgets::BuildStatus::built &&
		built.program.instruction_count() == 7 &&
		built.program.byte_count() == 26 &&
		built.program.source().size() == 26,
		"mixed-length replay block did not build");
	require(built.program.terminal(),
		"a block ending at SYSCALL is not terminal");
	static constexpr std::uint64_t boundaries[] = {0, 7, 10, 14, 17, 20, 24};
	static constexpr std::uint64_t reached[] = {1, 2, 4, 8, 9, 13};
	for (std::size_t budget = 1; budget < 7; ++budget) {
		CpuState cpu{};
		const auto result = gadgets::execute(
			built.program, cpu, memory, budget);
		require(result.event == uml::cpu::EngineEvent::continued &&
			result.instruction_budget_exhausted &&
			result.retired_instructions == budget,
			"a budget-limited replay did not stop at its budget");
		require(cpu.rip == boundaries[budget] &&
			cpu.gpr(Register::rax) == reached[budget - 1],
			"a budget-limited replay lost an instruction "
			"boundary");
	}
	const auto cut = gadgets::build(memory, 0, ExecutionMode::long64, 3);
	require(cut.status == gadgets::BuildStatus::built &&
		cut.program.instruction_count() == 3 && !cut.program.terminal(),
		"a block cut at its instruction limit reported itself terminal");
}

/* A replay decodes the bytes its block was built from, not the bytes at its
 * address now. Guest memory is free to change under a translation -- deciding
 * whether a cached one survives that is the validation's job -- and a block
 * already entered has to finish as itself. */
void budget_limited_replay_ignores_rewritten_guest_code()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x48, 0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1
		0x48, 0xff, 0xc0,                         // inc rax
		0x48, 0x83, 0xc0, 0x02,                   // add rax, 2
		0x48, 0x01, 0xc0,                         // add rax, rax
		0x0f, 0x05,                               // syscall
	});
	const auto built = gadgets::build(memory, 0);
	require(built.status == gadgets::BuildStatus::built &&
		built.program.instruction_count() == 5,
		"replay-source block did not build");
	/* Six DEC RAX in place of the block. A replay reading guest memory
	 * decodes those instead, and both the register it leaves and the RIP it
	 * stops at say which bytes it read. */
	memory.load(0, {
		0x48, 0xff, 0xc8, 0x48, 0xff, 0xc8, 0x48, 0xff, 0xc8,
		0x48, 0xff, 0xc8, 0x48, 0xff, 0xc8, 0x48, 0xff, 0xc8,
	});
	CpuState cpu{};
	const auto result = gadgets::execute(built.program, cpu, memory, 3);
	require(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 3 &&
		result.instruction_budget_exhausted,
		"replay after a code rewrite did not stop at its budget");
	require(cpu.rip == 14 && cpu.gpr(Register::rax) == 4,
		"a replay decoded the guest's newer bytes");
}

/* A fault inside a replayed prefix reports exactly what the same fault in the
 * whole block reports: the faulting instruction's own RIP, nothing retired for
 * it, and the prefix before it retired. */
void budget_limited_replay_faults_where_the_block_does()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x48, 0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1
		0x48, 0xff, 0xc0,                         // inc rax
		0x48, 0x8b, 0x1b,                         // mov rbx, [rbx]
		0x48, 0xff, 0xc0,                         // inc rax
		0x48, 0xff, 0xc0,                         // inc rax
		0x0f, 0x05,                               // syscall
	});
	const auto built = gadgets::build(memory, 0);
	require(built.status == gadgets::BuildStatus::built &&
		built.program.instruction_count() == 6,
		"faulting replay block did not build");
	CpuState cpu{};
	cpu.gpr(Register::rbx) = 16383;
	const auto result = gadgets::execute(built.program, cpu, memory, 4);
	require(result.event == uml::cpu::EngineEvent::memory_fault &&
		result.fault.address == 16383 &&
		result.fault.access == uml::memory::MemoryAccess::read,
		"a replayed prefix lost its precise fault");
	require(result.retired_instructions == 2 && cpu.rip == 10 &&
		cpu.gpr(Register::rax) == 2,
		"a replayed prefix did not stop at the faulting "
		"instruction");
}

/* Both stores of a block publish, though they share a page and nothing
 * separates them: another vCPU that revalidated a block of the page after the
 * first would keep the bytes it read until the second moved the stamp again.
 */
void executable_stores_publish_a_code_change_each()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* Two MOV byte ptr stores followed by SYSCALL. Both destinations share the
	 * code page but do not overlap this translated block.
	 */
	memory.load(0, {0xc6, 0x00, 0x7f, 0xc6, 0x40, 0x01, 0x6e, 0x0f, 0x05});
	memory.set_executable_page(0);
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x800;
	gadgets::Executor executor;
	executor.prepare(memory, 1, 1, 1);
	/* Cached, so the page is on the shared code-page list and its stores
	 * have a translation to protect. */
	const auto selected = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(selected.program != nullptr,
		"executable-store block did not build");
	const auto result = gadgets::execute(
		*selected.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		result.retired_instructions == 3 && cpu.rip == 9,
		"non-overlapping executable stores did not stay in one native block");
	require(memory.invalidations() == 2,
		"a second executable store to a code page published "
		"nothing");
	require(memory.byte(0x800) == 0x7f && memory.byte(0x801) == 0x6e,
		"batched executable stores did not update guest memory");
}

/* A store publishes a code change to protect cached translations, and only
 * the executors know where those are. With every page executable, a store to
 * a page no executor has translated publishes nothing; once another executor
 * caches code on that page, this executor's next store to it publishes even
 * though its own write translation was filled before the page gained code.
 */
void store_publishes_only_where_some_executor_caches_code()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* Page 0: MOV byte ptr [RAX], 0x7f; SYSCALL. Page 1: SYSCALL. */
	memory.load(0, {0xc6, 0x00, 0x7f, 0x0f, 0x05});
	memory.load(0x1000, {0x0f, 0x05});
	memory.set_every_page_executable(true);
	gadgets::Executor writer;
	writer.prepare(memory, 7, 1, 1);
	const auto block = writer.select_program(
		memory, 0, ExecutionMode::long64);
	require(block.program != nullptr, "code-page registry block did not build");
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x1800;
	auto result = gadgets::execute(*block.program, cpu, memory, writer, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		memory.byte(0x1800) == 0x7f,
		"store to an untranslated executable page did not run");
	require(memory.invalidations() == 0,
		"store to a page holding no cached code published a change");

	gadgets::Executor reader;
	reader.prepare(memory, 7, 1, 1);
	const auto target = reader.select_program(
		memory, 0x1000, ExecutionMode::long64);
	require(target.program != nullptr, "code-page registry target did not build");
	cpu = {};
	cpu.rip = 0;
	cpu.gpr(Register::rax) = 0x1801;
	result = gadgets::execute(*block.program, cpu, memory, writer, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		memory.byte(0x1801) == 0x7f,
		"store to a newly translated page did not run");
	require(memory.invalidations() == 1,
		"store to a page another executor translated did not publish");

	/* Retiring the reader's translations takes the page off the list again. */
	reader = gadgets::Executor{};
	cpu = {};
	cpu.rip = 0;
	cpu.gpr(Register::rax) = 0x1802;
	result = gadgets::execute(*block.program, cpu, memory, writer, 64);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		memory.byte(0x1802) == 0x7f,
		"store after the other executor retired did not run");
	require(memory.invalidations() == 1,
		"store to a page that lost its cached code still published");
}

void overlapping_executable_store_stops_immediately()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* MOV byte ptr [RAX], 0x90; SYSCALL. Replacing the first SYSCALL byte
	 * must return before the stale translated instruction can execute.
	 */
	memory.load(0, {0xc6, 0x00, 0x90, 0x0f, 0x05});
	memory.set_executable_page(0);
	const auto built = require_built(gadgets::build(memory, 0),
		"overlapping executable-store block did not build");
	CpuState cpu{};
	cpu.gpr(Register::rax) = 3;
	gadgets::Executor executor;
	executor.prepare(memory, 1, 1, 1);
	const auto result = gadgets::execute(
		built.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 1 && cpu.rip == 3,
		"overlapping executable store ran stale translated code");
	require(memory.invalidations() == 1,
		"overlapping executable store did not publish its change");
	require(memory.byte(3) == 0x90,
		"overlapping executable store did not update guest memory");
}

void executable_store_links_only_to_unchanged_code()
{
	using namespace uml::cpu::x86_64;
	const auto run = [](std::uint64_t destination, bool should_chain,
		bool concurrent_change = false) {
		TestMemory memory;
		/* MOV byte ptr [RAX], 0x90; JMP 16. The cached target is a
		 * SYSCALL and both mappings deliberately share one executable page.
		 */
		memory.load(0, {
			0xc6, 0x00, 0x90,
			0xe9, 0x08, 0x00, 0x00, 0x00,
		});
		memory.load(16, {0x0f, 0x05});
		memory.set_executable_page(0);
		TestCodeSequences sequences;
		memory.set_code_changes(&sequences.code_changes);
		gadgets::Executor executor;
		executor.prepare(memory, 42, sequences.mapping_changes,
			sequences.code_changes, &sequences,
			read_test_code_sequences);
		const auto target = executor.select_program(
			memory, 16, ExecutionMode::long64);
		const auto source = executor.select_program(
			memory, 0, ExecutionMode::long64);
		require(target.program != nullptr && source.program != nullptr,
			"code-write link setup did not cache both programs");
		if (concurrent_change)
			++sequences.code_changes;

		CpuState cpu{};
		cpu.gpr(Register::rax) = destination;
		const auto result = gadgets::execute(
			*source.program, cpu, memory, executor, 64);
		if (should_chain) {
			require(result.event == uml::cpu::EngineEvent::system_call &&
				result.retired_instructions == 3 && cpu.rip == 18,
				"non-overlapping code write did not retain its safe link");
			require(memory.invalidations() == 1 &&
				sequences.code_changes == 2,
				"safe code-write link published the wrong sequence");
			return;
		}
		require(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 2 && cpu.rip == 16,
			"unsafe code write entered a cached target");
		require(memory.invalidations() == 1,
			"unsafe code-write link did not publish exactly once");
		require(destination != 16 || memory.byte(16) == 0x90,
			"overlapping target write was not committed");
	};
	run(0x800, true);
	run(16, false);
	run(0x800, false, true);
}

/* A store into a cached block far outside the running program is caught by
 * publication, not by the program-range check, which only sees the block being
 * executed. Reselecting a block revalidates it against its source bytes, so the
 * stale case needs an already-linked edge that is followed without reselection:
 * run once with a store that changes nothing so the link forms, then again with
 * a store that rewrites the target.
 */
void distant_code_write_cools_its_linked_block()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t target = 0x800;
	TestMemory memory;
	/* MOV [RBX], AL; JMP target. */
	memory.load(0, {0x88, 0x03, 0xe9, 0xf9, 0x07, 0x00, 0x00});
	/* MOV EAX, 1; SYSCALL. The second run rewrites the immediate to 2. */
	memory.load(target, {0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	memory.set_executable_page(0);
	TestCodeSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	require(executor.select_program(memory, target,
			ExecutionMode::long64).program != nullptr &&
		executor.select_program(memory, 0,
			ExecutionMode::long64).program != nullptr,
		"distant code-write setup did not cache both programs");

	const auto run = [&](std::uint8_t value) {
		const auto source = executor.select_program(
			memory, 0, ExecutionMode::long64);
		if (source.program == nullptr)
			return CpuState{};
		CpuState cpu{};
		cpu.gpr(Register::rbx) = target + 1;
		cpu.gpr(Register::rax) = value;
		(void)gadgets::execute(*source.program, cpu, memory, executor, 64);
		return cpu;
	};

	/* Writing back the byte already there leaves the target unchanged, so the
	 * edge is safe to keep and the link is established.
	 */
	const auto warm = run(0x01);
	require(memory.byte(target + 1) == 0x01,
			"link-forming run did not perform its store");
	require(warm.gpr(Register::rax) == 1,
		"link-forming run did not reach the cached target");

	const auto after = run(0x02);
	require(memory.byte(target + 1) == 0x02,
		"distant code write was not committed");
	if (after.rip == target)
		/* Stopping at the boundary is correct: the next selection rebuilds. */
		return;
	require(after.gpr(Register::rax) == 2,
		"a linked edge entered a stale translation of a written block");
}

void code_write_resets_unvalidated_successors()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* Source writes over the second target and branches to an unchanged
	 * intermediary. The intermediary's cached edge must be reset even though
	 * the immediate source-to-intermediary edge is safe to reactivate.
	 */
	memory.load(0, {
		0xc6, 0x00, 0x90,
		0xe9, 0x08, 0x00, 0x00, 0x00,
	});
	memory.load(16, {
		0xe9, 0x0b, 0x00, 0x00, 0x00,
	});
	memory.load(32, {0x0f, 0x05});
	memory.set_executable_page(0);
	TestCodeSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	const auto final_target = executor.select_program(
		memory, 32, ExecutionMode::long64);
	const auto intermediary = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto source = executor.select_program(
		memory, 0, ExecutionMode::long64);
	require(final_target.program != nullptr &&
		intermediary.program != nullptr && source.program != nullptr,
		"code-write successor reset setup did not cache every program");

	CpuState cpu{};
	cpu.gpr(Register::rax) = 32;
	const auto result = gadgets::execute(
		*source.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 3 && cpu.rip == 32,
		"code write followed an unvalidated stale successor");
	require(memory.byte(32) == 0x90 && memory.invalidations() == 1,
		"code-write successor reset lost the guest store");
}

void slow_executable_writes_publish_every_store()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.set_executable_page(0);
	std::array<std::byte, 16> first{};
	std::array<std::byte, 16> second{};
	first.fill(std::byte{0x55});
	second.fill(std::byte{0xaa});
	gadgets::NativeExecutionContext context{
		.memory = &memory,
	};
	const auto first_result = gadgets::uml_x86_64_gadget_bulk_store(
		&context, 0x800, first.data(), first.size());
	const auto second_result = gadgets::uml_x86_64_gadget_bulk_store(
		&context, 0x810, second.data(), second.size());
	require(first_result == 1 && second_result == 1,
		"slow executable stores did not complete");
	require(memory.invalidations() == 2 &&
		context.code_publications == 2,
		"a second slow store to a published page published nothing");
	require(memory.byte(0x800) == 0x55 && memory.byte(0x810) == 0xaa,
		"slow executable stores lost guest bytes");
}

void decode_scaled_address()
{
	TestMemory memory;
	memory.load(0, {0x48, 0x8b, 0x44, 0x8d, 0xf0});
	const auto result = uml::cpu::x86_64::decode(memory, 0);
	const auto &source = result.instruction.source;
	require(result.status == uml::cpu::x86_64::DecodeStatus::decoded,
		"scaled load did not decode");
	require(result.instruction.length == 5, "scaled load length is wrong");
	require(source.kind == uml::cpu::x86_64::OperandKind::memory,
		"scaled load source is not memory");
	require(source.memory.base == 5 && source.memory.index == 1 &&
		source.memory.scale == 4 && source.memory.displacement == -16,
		"ModR/M plus SIB address was not normalized");
}

/* A REX prefix counts only immediately before the opcode: any legacy prefix
 * after it voids it. ADD [rax], ebx behind REX.B and each prefix decodes with
 * the base RAX, and with the prefix first it keeps REX.B and so the base R8. */
void a_legacy_prefix_after_rex_voids_it()
{
	using namespace uml::cpu::x86_64;
	{
		TestMemory memory;
		memory.load(0, {0x66, 0x41, 0x01, 0x18});
		const auto kept = decode(memory, 0);
		require(kept.status == DecodeStatus::decoded &&
				kept.instruction.destination.kind == OperandKind::memory &&
				kept.instruction.destination.memory.base == 8,
			"a REX prefix before the opcode was not honoured");
	}
	for (const std::uint8_t prefix : {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65,
		     0x66, 0x67, 0xf0, 0xf2, 0xf3}) {
		TestMemory memory;
		memory.load(0, {0x41, prefix, 0x01, 0x18});
		const auto voided = decode(memory, 0);
		require(voided.status == DecodeStatus::decoded &&
				voided.instruction.destination.kind ==
					OperandKind::memory &&
				voided.instruction.destination.memory.base == 0 &&
				voided.instruction.length == 4,
			"a legacy prefix after REX did not void it");
	}
}

void execute_scalar_control_flow()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x48, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x48, 0x83, 0xc0, 0x03,
		0x48, 0x83, 0xf8, 0x08,
		0x74, 0x07,
		0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x00,
		0x48, 0xc7, 0xc7, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	CpuState cpu{};
	const auto first = require_built(gadgets::build(memory, 0),
		"scalar block did not build");
	const auto first_run = gadgets::execute(first.program, cpu, memory, 64);
	require(first_run.event == uml::cpu::EngineEvent::continued,
		"conditional block returned the wrong event");
	require(first_run.retired_instructions == 4,
		"conditional block retirement is wrong");
	require(cpu.gpr(Register::rax) == 8 && cpu.rip == 27,
		"arithmetic or equal branch result is wrong");
	require(cpu.deferred_flags.operation ==
			DeferredFlagOperation::subtract,
		"equal branch eagerly materialized the deferred flags");

	const auto second = require_built(gadgets::build(memory, cpu.rip),
		"syscall block did not build");
	const auto second_run = gadgets::execute(second.program, cpu, memory, 64);
	require(second_run.event == uml::cpu::EngineEvent::system_call,
		"SYSCALL did not exit the gadget stream");
	require(second_run.retired_instructions == 2,
		"SYSCALL block retirement is wrong");
	require(cpu.gpr(Register::rdi) == 42 && cpu.rip == 36,
		"SYSCALL state is wrong");
	require(cpu.gpr(Register::rcx) == 36 &&
		cpu.gpr(Register::r11) == cpu.rflags,
		"SYSCALL did not publish RCX/R11");
}

void execute_absolute_memory_test_branch()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t base = 0x7ffe0000;
	constexpr std::uint64_t code = base + 0x100;
	constexpr std::uint64_t shared_data = base + 0x308;
	TestMemory memory{base};
	memory.load(code, {
		0xf6, 0x04, 0x25, 0x08, 0x03, 0xfe, 0x7f, 0x01,
		0x75, 0x05,
	});
	memory.load(shared_data, {0x01});
	memory.load(code + 15, {
		0xb8, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	const auto block = require_built(gadgets::build(memory, code),
		"absolute TEST/JNE block did not build");

	CpuState cpu{};
	cpu.rip = code;
	const auto result = gadgets::execute(block.program, cpu, memory, 2);
	require(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 2,
		"absolute TEST/JNE block did not complete");
	require(cpu.rip == code + 15,
		"absolute TEST/JNE ignored a set tested bit");
	require(cpu.deferred_flags.operation == DeferredFlagOperation::logical &&
		cpu.deferred_flags.result == 1,
		"absolute TEST recorded the wrong deferred result");

	/* Wine's syscall stubs use this exact TEST/JNE sequence.  Exercise it
	 * through the production cache too, with the taken target already linked,
	 * so a direct build cannot hide a conditional-link regression.
	 */
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto target = executor.select_program(
		memory, code + 15, ExecutionMode::long64);
	const auto source = executor.select_program(
		memory, code, ExecutionMode::long64);
	require(target.program != nullptr && source.program != nullptr,
		"absolute TEST/JNE cache setup did not build both blocks");
	cpu = CpuState{};
	cpu.rip = code;
	const auto linked = gadgets::execute(
		*source.program, cpu, memory, executor, 64);
	require(linked.event == uml::cpu::EngineEvent::system_call &&
			linked.retired_instructions == 4 &&
			cpu.gpr(Register::rax) == 42 && cpu.rip == code + 22,
			"cached absolute TEST/JNE did not enter its linked target");
}

void execute_lazy_zero_branches()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x74, 0x05});
	memory.load(16, {0x75, 0x05});
	const auto equal = gadgets::build(memory, 0);
	const auto not_equal = gadgets::build(memory, 16);
	require(equal.status == gadgets::BuildStatus::built &&
			not_equal.status == gadgets::BuildStatus::built,
		"zero-condition branches did not build");

	const auto run_branch = [&](const gadgets::Program &program, CpuState cpu,
		std::uint64_t expected_rip,
		DeferredFlagOperation expected_operation,
		const char *message) {
		cpu.rip = program.address();
		const auto result = gadgets::execute(program, cpu, memory, 1);
		require(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1 && cpu.rip == expected_rip &&
			cpu.deferred_flags.operation == expected_operation, message);
	};

	CpuState cpu{};
	cpu.rflags &= ~rflags_zero;
	cpu.deferred_flags.result = 0;
	cpu.deferred_flags.operation = DeferredFlagOperation::subtract;
	cpu.deferred_flags.width = 64;
	run_branch(equal.program, cpu, 7,
			DeferredFlagOperation::subtract,
			"JE did not take a deferred zero result");
	run_branch(not_equal.program, cpu, 18,
		DeferredFlagOperation::subtract,
		"JNE took a deferred zero result");

	cpu.rflags |= rflags_zero;
	cpu.deferred_flags.result = 1;
	run_branch(equal.program, cpu, 2,
			DeferredFlagOperation::subtract,
			"JE took a deferred nonzero result");
	run_branch(not_equal.program, cpu, 23,
		DeferredFlagOperation::subtract,
		"JNE did not take a deferred nonzero result");

	cpu = CpuState{};
	cpu.rflags |= rflags_zero;
	run_branch(equal.program, cpu, 7, DeferredFlagOperation::none,
			"JE ignored an architectural zero flag");
	run_branch(not_equal.program, cpu, 18,
		DeferredFlagOperation::none,
		"JNE ignored an architectural zero flag");

	cpu.rflags &= ~rflags_zero;
	run_branch(equal.program, cpu, 2, DeferredFlagOperation::none,
			"JE ignored a clear architectural zero flag");
	run_branch(not_equal.program, cpu, 23,
		DeferredFlagOperation::none,
		"JNE ignored a clear architectural zero flag");
}

void zero_conditional_moves_preserve_pending_flags()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	for (const auto mode : {ExecutionMode::long64, ExecutionMode::compat32}) {
		for (const unsigned width : {16U, 32U, 64U}) {
			if (mode == ExecutionMode::compat32 && width == 64)
				continue;
			for (const bool not_equal : {false, true}) {
				const auto opcode = static_cast<std::uint8_t>(not_equal ? 0x45 : 0x44);
				if (width == 16)
					memory.load(0, {0x66, 0x0f, opcode, 0xc3});
				else if (width == 64)
					memory.load(0, {0x48, 0x0f, opcode, 0xc3});
				else
					memory.load(0, {0x0f, opcode, 0xc3});
				const auto block = require_built(
					gadgets::build(memory, 0, mode, 1),
					"zero conditional move did not build");
				for (unsigned state = 0; state != 6; ++state) {
					CpuState cpu{};
					cpu.execution_mode = mode;
					cpu.gpr(Register::rax) = 0x76543210;
					cpu.gpr(Register::rbx) = 0xfedcba987654abcdULL;
					cpu.rflags |= rflags_carry | rflags_overflow;
					if (state & 1U)
						cpu.rflags |= rflags_zero;
					if (state >= 2) {
						cpu.deferred_flags = {0, (state & 1U),
							(state & 1U) ? 0xffffU : 0U,
							DeferredFlagOperation::subtract, 2};
					}
					if (state >= 4) {
						cpu.deferred_flags.override_mask = rflags_zero;
						cpu.deferred_flags.override_value =
							(state & 1U) ? rflags_zero : 0;
					}
					auto reference = cpu;
					materialize_flags(reference);
					const bool take = ((reference.rflags & rflags_zero) != 0) != not_equal;
					const auto mask = width == 64 ? ~std::uint64_t{} :
						(std::uint64_t{1} << width) - 1;
					const auto expected = take ?
						(cpu.gpr(Register::rax) & ~mask) |
						(cpu.gpr(Register::rbx) & mask) : cpu.gpr(Register::rax);
					const auto pending = cpu.deferred_flags;
					const auto result = gadgets::execute(block.program, cpu, memory, 1);
					require(result.retired_instructions == 1 &&
						cpu.gpr(Register::rax) == expected &&
						cpu.deferred_flags.operation == pending.operation &&
						cpu.deferred_flags.override_mask == pending.override_mask,
						"conditional move changed pending flags or selected the wrong operand");
					materialize_flags(cpu);
					require(cpu.rflags == reference.rflags,
						"conditional move lost flags needed by a later consumer");
				}
			}
		}
	}
}

/* glibc's i386 futex wait rejects a positive syscall result with this exact
 * TEST EAX,EAX; JG sequence.  The kernel bridge stores syscall returns in the
 * 64-bit backing slot, so retain upper garbage here and prove that compat32
 * flags and the signed branch use only the architectural EAX value.
 */
void materialized_flags_match_the_reference()
{
	using namespace uml::cpu::x86_64;
	/* The engine folds the recorded operation and width into one body per
	 * pair, so every pair has to agree with the independent C++
	 * implementation rather than only with the pairs a corpus happens to
	 * reach. PUSHFQ is the one instruction that exposes the whole result.
	 */
	TestMemory memory;
	memory.load(0, {0x9c});
	const auto block = require_built(gadgets::build(memory, 0),
		"PUSHFQ block did not build");

	constexpr std::uint64_t stack = 0x800;
	constexpr std::array<DeferredFlagOperation, 7> operations{
		DeferredFlagOperation::add,
		DeferredFlagOperation::subtract,
		DeferredFlagOperation::logical,
		DeferredFlagOperation::increment,
		DeferredFlagOperation::decrement,
		DeferredFlagOperation::add_carry,
		DeferredFlagOperation::subtract_borrow,
	};
	constexpr std::array<std::uint8_t, 4> widths{1, 2, 4, 8};
	constexpr std::array<std::uint64_t, 8> values{
		0, 1, 0x7f, 0x80, 0xff, 0x7fffffff,
		0x8000000000000000ULL, 0xffffffffffffffffULL,
	};
	constexpr std::array<std::uint64_t, 2> entry_flags{0x2, 0xfff};

	for (const auto operation : operations)
		for (const auto width : widths)
			for (const auto left : values)
				for (const auto right : values)
					for (const std::uint8_t carry : {0, 1})
						/* Only ADC and SBB record an incoming carry; the
						 * recording macro zeroes the byte for every other
						 * operation, so a carry there is not a reachable state.
						 */
						if (carry == 0 ||
							operation == DeferredFlagOperation::add_carry ||
							operation ==
								DeferredFlagOperation::subtract_borrow)
						for (const auto entry : entry_flags)
							for (const int sense : {0, 1}) {
		CpuState cpu{};
		cpu.rip = 0;
		cpu.rflags = entry;
		cpu.gpr(Register::rsp) = stack;
		cpu.deferred_flags.left = left;
		cpu.deferred_flags.right = right;
		cpu.deferred_flags.result =
			sense == 0 ? left + right + carry : left - right - carry;
		cpu.deferred_flags.operation = operation;
		cpu.deferred_flags.width = width;
		cpu.deferred_flags.carry = carry;

		CpuState reference = cpu;
		materialize_flags(reference);

		const auto run = gadgets::execute(block.program, cpu, memory, 1);
		require(run.event == uml::cpu::EngineEvent::continued &&
			run.retired_instructions == 1,
			"PUSHFQ did not retire");
		std::uint64_t pushed = 0;
		std::byte raw[sizeof(pushed)]{};
		require(memory.read(stack - sizeof(pushed),
				std::span<std::byte>{raw, sizeof(raw)}),
			"PUSHFQ destination was not readable");
		std::memcpy(&pushed, raw, sizeof(pushed));
		require(pushed == reference.rflags &&
			cpu.rflags == reference.rflags,
			"materialized flags disagree with the reference");
	}
}

void carry_branches_match_the_reference()
{
	using namespace uml::cpu::x86_64;
	/* The four carry-based conditions read CF and ZF straight out of the
	 * deferred record. Every recorded operation and width has to reach the
	 * same decision the materialized flags would.
	 */
	TestMemory memory;
	struct Branch final {
		std::uint64_t address;
		std::uint8_t opcode;
		const char *name;
	};
	constexpr std::array<Branch, 4> branches{{
		{0, 0x72, "JB"}, {16, 0x73, "JAE"}, {32, 0x76, "JBE"}, {48, 0x77, "JA"},
	}};
	constexpr std::array<DeferredFlagOperation, 8> operations{
		DeferredFlagOperation::none,
		DeferredFlagOperation::add,
		DeferredFlagOperation::subtract,
		DeferredFlagOperation::logical,
		DeferredFlagOperation::increment,
		DeferredFlagOperation::decrement,
		DeferredFlagOperation::add_carry,
		DeferredFlagOperation::subtract_borrow,
	};
	constexpr std::array<std::uint8_t, 4> widths{1, 2, 4, 8};
	constexpr std::array<std::uint64_t, 6> values{
		0, 1, 0x80, 0xff, 0x8000000000000000ULL, 0xffffffffffffffffULL,
	};

	for (const auto branch : branches) {
		memory.load(branch.address, {branch.opcode, 0x05});
		const auto built = require_built(
			gadgets::build(memory, branch.address),
			"carry-condition branch did not build");

		for (const auto operation : operations)
			for (const auto width : widths)
				for (const auto left : values)
					for (const auto right : values)
						for (const std::uint16_t override_mask : {0, 1}) {
			CpuState cpu{};
			cpu.rip = branch.address;
			cpu.rflags = 0x2;
			cpu.deferred_flags.left = left;
			cpu.deferred_flags.right = right;
			cpu.deferred_flags.result = left - right;
			cpu.deferred_flags.operation = operation;
			cpu.deferred_flags.width = width;
			cpu.deferred_flags.override_mask = override_mask;
			cpu.deferred_flags.override_value = override_mask;

			CpuState reference = cpu;
			materialize_flags(reference);
			const bool carry = (reference.rflags & 0x1) != 0;
			const bool zero = (reference.rflags & 0x40) != 0;
			const bool taken =
				branch.opcode == 0x72 ? carry :
				branch.opcode == 0x73 ? !carry :
				branch.opcode == 0x76 ? (carry || zero) : !(carry || zero);
			const auto expected =
				branch.address + (taken ? 7 : 2);

			const auto run = gadgets::execute(built.program, cpu, memory, 1);
			require(run.event == uml::cpu::EngineEvent::continued &&
					run.retired_instructions == 1 && cpu.rip == expected,
				"a carry-condition branch disagrees with the reference");
		}
	}
}

void execute_compat32_test_greater_after_syscall()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x85, 0xc0, // test eax, eax
		0x7f, 0x05, // jg 9
	});
	const auto block = require_built(
		gadgets::build(memory, 0, ExecutionMode::compat32, 2),
		"compat32 TEST/JG block did not build");

	const auto run = [&](std::uint64_t value, std::uint64_t expected_rip,
		const char *message) {
		CpuState cpu{};
		cpu.execution_mode = ExecutionMode::compat32;
		cpu.gpr(Register::rax) = value;
		const auto result = gadgets::execute(block.program, cpu, memory, 2);
		require(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 2 && cpu.rip == expected_rip,
			message);
	};

	run(0, 4, "compat32 TEST/JG treated zero as positive");
	run(1, 9, "compat32 TEST/JG rejected a positive result");
	run(0xffff'ffff'ffff'fff5ULL, 4,
		"compat32 TEST/JG treated sign-extended -EAGAIN as positive");
	run(0x1234'5678'ffff'fff5ULL, 4,
		"compat32 TEST/JG used bits above EAX for the sign test");
}

/* Match glibc's complete i386 futex-result validator.  Its accepted common
 * results pass through signed comparisons and a register-indexed BT before
 * the rare timeout/overflow cases take equality branches.
 */
void execute_compat32_futex_result_validator()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x85, 0xc0,                         // test eax, eax
		0x7f, 0x23,                         // jg fatal
		0x83, 0xf8, 0xea,                   // cmp eax, -22
		0x7c, 0x14,                         // jl special
		0x8d, 0x50, 0x16,                   // lea edx, [eax + 22]
		0xb9, 0x01, 0x08, 0x44, 0x00,       // mov ecx, 0x440801
		0x0f, 0xa3, 0xd1,                   // bt ecx, edx
		0x73, 0x11,                         // jae fatal
		0xbf, 0x2a, 0x00, 0x00, 0x00,       // success: mov edi, 42
		0xcd, 0x80,                         // int 0x80
		0x83, 0xf8, 0x92,                   // special: cmp eax, -110
		0x74, 0xf4,                         // je success
		0x83, 0xf8, 0xb5,                   // cmp eax, -75
		0x74, 0xef,                         // je success
		0xbf, 0x63, 0x00, 0x00, 0x00,       // fatal: mov edi, 99
		0xcd, 0x80,                         // int 0x80
	});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);

	const auto run = [&](std::uint32_t value, std::uint64_t marker,
		const char *message) {
		CpuState cpu{};
		cpu.execution_mode = ExecutionMode::compat32;
		cpu.gpr(Register::rax) = value;
		for (unsigned step = 0; step < 8; ++step) {
			const auto block = executor.select_program(
				memory, cpu.rip, ExecutionMode::compat32, 64);
			if (block.status != gadgets::BuildStatus::built ||
				block.program == nullptr)
				fail("compat32 futex validator did not build");
			const auto result = gadgets::execute(
				*block.program, cpu, memory, executor, 64);
			if (result.event == uml::cpu::EngineEvent::system_call) {
				require(cpu.gpr(Register::rdi) == marker, message);
				return;
			}
			if (result.event != uml::cpu::EngineEvent::continued)
				fail("compat32 futex validator exited early");
		}
		fail("compat32 futex validator did not terminate");
	};

	const auto run_all = [&] {
		run(0, 42, "futex validator rejected success");
		run(static_cast<std::uint32_t>(-4), 42,
		"futex validator rejected EINTR");
		run(static_cast<std::uint32_t>(-11), 42,
		"futex validator rejected EAGAIN");
		run(static_cast<std::uint32_t>(-22), 42,
		"futex validator rejected EINVAL");
		run(static_cast<std::uint32_t>(-110), 42,
		"futex validator rejected ETIMEDOUT");
		run(static_cast<std::uint32_t>(-75), 42,
		"futex validator rejected EOVERFLOW");
		run(1, 99, "futex validator accepted a positive result");
		run(static_cast<std::uint32_t>(-516), 99,
		"futex validator accepted an internal restart result");
	};
	run_all();
	run_all();
}

void shifts_overlay_lazy_arithmetic_flags()
{
	using namespace uml::cpu::x86_64;
	TestMemory rotate_memory;
	rotate_memory.load(0, {
		0x01, 0xd8, // add eax, ebx
		0xd1, 0xc8, // ror eax, 1
	});
	const auto rotate = require_built(gadgets::build(rotate_memory, 0, 2),
		"lazy ROR block did not build");
	CpuState cpu{};
	cpu.gpr(Register::rbx) = 1;
	const auto rotate_result = gadgets::execute(
		rotate.program, cpu, rotate_memory, 2);
	constexpr auto rotate_mask = rflags_carry | rflags_overflow;
	require(rotate_result.event == uml::cpu::EngineEvent::continued &&
			rotate_result.retired_instructions == 2 &&
			cpu.gpr(Register::rax) == 0x8000'0000,
		"lazy ROR produced the wrong value");
	require(cpu.deferred_flags.operation == DeferredFlagOperation::add &&
		cpu.deferred_flags.override_mask == rotate_mask &&
		cpu.deferred_flags.override_value == rotate_mask,
		"ROR eagerly materialized or lost its flag overrides");
	materialize_flags(cpu);
	require((cpu.rflags & rotate_mask) == rotate_mask &&
			(cpu.rflags & (rflags_zero | rflags_sign | rflags_parity)) == 0,
		"materialized ROR flags did not preserve the preceding ADD flags");
	require(cpu.deferred_flags.operation == DeferredFlagOperation::none &&
		cpu.deferred_flags.override_mask == 0,
		"materialization did not consume the shift overlay");

	TestMemory branch_memory;
	branch_memory.load(0, {
		0x01, 0xd8,       // add eax, ebx
		0xc1, 0xe8, 0x1f, // shr eax, 31
		0x74, 0x05,       // je +5
	});
	const auto branch = require_built(gadgets::build(branch_memory, 0),
		"lazy shift/branch block did not build");
	cpu = CpuState{};
	cpu.gpr(Register::rax) = 1;
	cpu.gpr(Register::rbx) = 1;
	const auto branch_result = gadgets::execute(
		branch.program, cpu, branch_memory, 3);
	/* SHL, SHR and SAR record their operands instead of overlaying: the
	 * zero branch reads the recorded result and leaves the record pending. */
	require(branch_result.event == uml::cpu::EngineEvent::continued &&
			branch_result.retired_instructions == 3 && cpu.rip == 12,
		"JE did not consume the deferred shift result");
	require(cpu.deferred_flags.operation ==
			DeferredFlagOperation::shift_right &&
		cpu.deferred_flags.result == 0 &&
		cpu.deferred_flags.override_mask == 0,
		"zero-branch fast path materialized the deferred shift");
}

/* The deferred shift record is materialized by the engine (through the full
 * and the carry-only routines) and by the C++ materializer; check all three
 * against the architectural definition for every width, a spread of counts
 * including ones at and beyond the width, and both shift directions. The
 * auxiliary flag is undefined and overflow is only defined for a count of
 * one, so those are not compared elsewhere.
 */
void lazy_shift_flags_match_the_definition()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<std::uint64_t, 9> values{
		0U, 1U, 2U, 0x81U, 0x7fffU, 0x8001U, 0x4000'0001ULL,
		0x8000'0000'0000'0001ULL, 0xc5a3'ffff'0000'a5c3ULL};
	for (unsigned kind = 0; kind < 3U; ++kind) {
		for (const unsigned width : {8U, 16U, 32U, 64U}) {
			const unsigned bits = width;
			const auto mask = bits == 64U ? ~std::uint64_t{} :
				(std::uint64_t{1} << bits) - 1U;
			const auto sign = std::uint64_t{1} << (bits - 1U);
			for (const unsigned count : {1U, 3U, bits - 1U, bits, bits + 3U,
					bits == 64U ? 63U : 31U}) {
				if (count > (bits == 64U ? 63U : 31U))
					continue;
				for (const auto value : values) {
					for (const bool then_inc : {false, true}) {
						std::vector<std::byte> code;
						const auto push = [&code](unsigned byte) {
							code.push_back(static_cast<std::byte>(byte));
						};
						if (width == 16U)
							push(0x66);
						if (width == 64U)
							push(0x48);
						push(width == 8U ? 0xc0 : 0xc1);
						push(kind == 0 ? 0xe0 : kind == 1 ? 0xe8 : 0xf8);
						push(count);
						if (then_inc) {
							push(0xff); // inc ebx
							push(0xc3);
						}
						push(0x0f);
						push(0x05);
						TestMemory memory;
						memory.fill(0, code);
						const auto built = require_built(
							gadgets::build(memory,
								0),
							"shift flag program did not build");
						CpuState cpu{};
						cpu.gpr(Register::rax) = value;
						const auto result = gadgets::execute(
							built.program, cpu, memory, 8);
						require(result.event ==
							uml::cpu::EngineEvent::system_call,
							"shift flag program did not reach SYSCALL");
						materialize_flags(cpu);
						const auto v = value & mask;
						std::uint64_t expected_result;
						bool carry;
						if (kind == 0) {
							expected_result = (v << count) & mask;
							carry = count <= bits &&
								((v >> (bits - count)) & 1U) != 0;
						} else if (kind == 1) {
							expected_result = v >> count;
							carry = count <= bits &&
								((v >> (count - 1U)) & 1U) != 0;
						} else {
							const auto extended = (v & sign) != 0 ?
								v | ~mask : v;
							expected_result = static_cast<std::uint64_t>(
								static_cast<std::int64_t>(extended) >>
									count) & mask;
							carry = ((v >> (count <= bits ? count - 1U :
								bits - 1U)) & 1U) != 0;
						}
						std::uint64_t expected = 0;
						if (carry)
							expected |= rflags_carry;
						if (expected_result == 0)
							expected |= rflags_zero;
						if ((expected_result & sign) != 0)
							expected |= rflags_sign;
						unsigned ones = 0;
						for (unsigned bit = 0; bit < 8U; ++bit)
							ones += (expected_result >> bit) & 1U;
						if (ones % 2U == 0)
							expected |= rflags_parity;
						auto compared = rflags_carry | rflags_zero |
							rflags_sign | rflags_parity;
						if (count == 1U && !then_inc) {
							compared |= rflags_overflow;
							if (kind == 0 && ((((expected_result & sign) != 0)
									? 1U : 0U) ^ (carry ? 1U : 0U)) != 0)
								expected |= rflags_overflow;
							if (kind == 1 && (v & sign) != 0)
								expected |= rflags_overflow;
						}
						/* INC keeps only the shift's carry; its own record
						 * defines the rest. */
						if (then_inc)
							compared = rflags_carry;
						const auto observed = cpu.rflags & compared;
						if (observed != (expected & compared) ||
							(cpu.gpr(Register::rax) & mask) != expected_result) {
							char message[200];
							std::snprintf(message, sizeof(message),
								"lazy %s width %u count %u value %#llx%s: "
								"flags %#llx expected %#llx, result %#llx",
								kind == 0 ? "SHL" : kind == 1 ? "SHR" : "SAR",
								width, count,
								static_cast<unsigned long long>(value),
								then_inc ? " then INC" : "",
								static_cast<unsigned long long>(observed),
								static_cast<unsigned long long>(
									expected & compared),
								static_cast<unsigned long long>(
									cpu.gpr(Register::rax) & mask));
							fail(message);
						}
					}
				}
			}
		}
	}
}

void execute_segment_memory()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x64, 0x48, 0x89, 0x43, 0x08,
		0x64, 0x48, 0x8b, 0x4b, 0x08,
	});
	CpuState cpu{};
	cpu.fs_base = 64;
	cpu.gpr(Register::rbx) = 16;
	cpu.gpr(Register::rax) = 0x1122'3344'5566'7788ULL;
	const auto built = require_built(gadgets::build(memory, 0, 2),
		"FS memory block did not build");
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	require(result.event == uml::cpu::EngineEvent::continued,
		"FS memory block returned the wrong event");
	require(result.retired_instructions == 2,
		"FS memory retirement is wrong");
	require(memory.qword(88) == 0x1122'3344'5566'7788ULL &&
		cpu.gpr(Register::rcx) == 0x1122'3344'5566'7788ULL,
		"FS base was not applied to memory operands");
}

/* Address generation resolves base, index, scale, segment and address size
 * from one stream descriptor with no branch on which of them are present, so
 * every shape the builder can encode needs its own case. The instruction
 * corpus is keyed by operand form rather than addressing shape and cannot
 * cover this.
 */
void execute_addressing_shapes()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t data = 0x800;
	constexpr std::uint64_t marker = 0x0123'4567'89ab'cdefULL;

	struct Shape final {
		const char *name;
		std::initializer_list<std::uint8_t> encoding;
		std::uint64_t rbx;
		std::uint64_t rcx;
		std::uint64_t fs;
		std::uint64_t gs;
	};
	/* Every shape below resolves to the same effective address, so a wrong
	 * result names the shape that mis-resolved.
	 */
	const std::initializer_list<Shape> shapes{
		{"(base)", {0x48, 0x8b, 0x03}, data, 0, 0, 0},
		{"disp(base)", {0x48, 0x8b, 0x43, 0x10}, data - 16, 0, 0, 0},
		{"(base,index,8)", {0x48, 0x8b, 0x04, 0xcb}, 0x400, 0x80, 0, 0},
		{"disp(base,index,8)", {0x48, 0x8b, 0x44, 0xcb, 0x10},
			0x3f0, 0x80, 0, 0},
		{"(,index,8)", {0x48, 0x8b, 0x04, 0xcd, 0x00, 0x00, 0x00, 0x00},
			0, 0x100, 0, 0},
		{"fs:disp", {0x64, 0x48, 0x8b, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00},
			0, 0, data - 16, 0},
		{"gs:disp", {0x65, 0x48, 0x8b, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00},
			0, 0, 0, data - 16},
		{"absolute", {0x48, 0x8b, 0x04, 0x25, 0x00, 0x08, 0x00, 0x00},
			0, 0, 0, 0},
		{"addr32 disp(base)", {0x67, 0x48, 0x8b, 0x43, 0x10},
			data - 16, 0, 0, 0},
		{"addr32 (base,index,8)", {0x67, 0x48, 0x8b, 0x04, 0xcb},
			0x400, 0x80, 0, 0},
	};

	std::uint64_t code = 0x100;
	for (const auto &shape : shapes) {
		TestMemory memory;
		memory.load(data, {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01});
		memory.load(code, shape.encoding);
		CpuState cpu{};
		cpu.rip = code;
		cpu.gpr(Register::rbx) = shape.rbx;
		cpu.gpr(Register::rcx) = shape.rcx;
		cpu.fs_base = shape.fs;
		cpu.gs_base = shape.gs;
		const auto built = gadgets::build(memory, code, 1);
		require(built.status == gadgets::BuildStatus::built, shape.name);
		require(gadgets::execute(built.program, cpu, memory, 1).event ==
			uml::cpu::EngineEvent::continued, shape.name);
		require(cpu.gpr(Register::rax) == marker, shape.name);
		code += 16;
	}

	/* RIP-relative folds the displacement against the instruction end, so it
	 * must be built where it is actually placed.
	 */
	{
		TestMemory memory;
		memory.load(data, {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01});
		const std::uint64_t rip_code = 0x200;
		const auto displacement = static_cast<std::uint32_t>(
			data - (rip_code + 7));
		memory.load(rip_code, {0x48, 0x8b, 0x05,
			static_cast<std::uint8_t>(displacement),
			static_cast<std::uint8_t>(displacement >> 8),
			static_cast<std::uint8_t>(displacement >> 16),
			static_cast<std::uint8_t>(displacement >> 24)});
		CpuState cpu{};
		cpu.rip = rip_code;
		const auto built = gadgets::build(memory, rip_code, 1);
		require(built.status == gadgets::BuildStatus::built,
				"rip-relative did not build");
		require(gadgets::execute(built.program, cpu, memory, 1).event ==
			uml::cpu::EngineEvent::continued,
			"rip-relative did not execute");
		require(cpu.gpr(Register::rax) == marker,
			"rip-relative resolved the wrong address");
	}

	/* A load that spans a page boundary leaves the inline fast path, so the
	 * shape has to stay correct on the slow path too.
	 */
	{
		TestMemory memory;
		memory.load(0xffc, {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01});
		memory.load(0x100, {0x48, 0x8b, 0x43, 0x10});
		CpuState cpu{};
		cpu.rip = 0x100;
		cpu.gpr(Register::rbx) = 0xffc - 16;
		const auto built = gadgets::build(memory, 0x100, 1);
		require(built.status == gadgets::BuildStatus::built,
				"page-crossing load did not build");
		require(gadgets::execute(built.program, cpu, memory, 1).event ==
			uml::cpu::EngineEvent::continued,
			"page-crossing load did not execute");
		require(cpu.gpr(Register::rax) == marker,
			"page-crossing load resolved the wrong address");
	}

	/* The always-zero slot the absent-register encoding names must stay zero:
	 * a stray write to it would silently offset every such operand.
	 */
	CpuState fresh{};
	require(fresh.segment_zero == 0,
		"the address-generation zero slot is not zero");
}

/* PMOVMSKB collapses one sign bit per byte into a bit mask. The corpus carries
 * only one or two cases per width, which a lane-ordering mistake can survive,
 * so walk a single set bit through every position instead.
 */
void execute_byte_sign_masks()
{
	using namespace uml::cpu::x86_64;
	const auto run = [](std::initializer_list<std::uint8_t> encoding,
		std::size_t lanes, std::size_t set_lane) -> std::uint64_t {
		TestMemory memory;
		memory.load(0, encoding);
		CpuState cpu{};
		for (std::size_t lane = 0; lane < lanes; ++lane)
			cpu.vectors[1][lane] = static_cast<std::byte>(
				lane == set_lane ? 0x80 : 0x7f);
		const auto built = gadgets::build(memory, 0, 1);
		if (built.status != gadgets::BuildStatus::built)
			return ~std::uint64_t{0};
		if (gadgets::execute(built.program, cpu, memory, 1).event !=
			uml::cpu::EngineEvent::continued)
			return ~std::uint64_t{0};
		return cpu.gpr(Register::rax);
	};

	for (std::size_t lane = 0; lane < 16; ++lane)
		require(run({0x66, 0x0f, 0xd7, 0xc1}, 16, lane) ==
			(std::uint64_t{1} << lane), "PMOVMSKB xmm lane");
	for (std::size_t lane = 0; lane < 32; ++lane)
		require(run({0xc5, 0xfd, 0xd7, 0xc1}, 32, lane) ==
			(std::uint64_t{1} << lane), "VPMOVMSKB ymm lane");

	/* All sign bits set, and none, bound the walk at both ends. */
	TestMemory memory;
	memory.load(0, {0x66, 0x0f, 0xd7, 0xc1});
	CpuState cpu{};
	for (auto &lane : cpu.vectors[1])
		lane = std::byte{0xff};
	const auto built = gadgets::build(memory, 0, 1);
	require(built.status == gadgets::BuildStatus::built,
			"PMOVMSKB saturated case did not build");
	require(gadgets::execute(built.program, cpu, memory, 1).event ==
		uml::cpu::EngineEvent::continued,
		"PMOVMSKB saturated case did not execute");
	require(cpu.gpr(Register::rax) == 0xffff,
		"PMOVMSKB did not set every mask bit");
}

void execute_call_and_return()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe8, 0x05, 0x00, 0x00, 0x00});
	memory.load(10, {0xc3});
	CpuState cpu{};
	cpu.gpr(Register::rsp) = 400;
	const auto caller = require_built(gadgets::build(memory, 0),
		"CALL did not build");
	const auto call_result = gadgets::execute(caller.program, cpu, memory, 1);
	require(call_result.event == uml::cpu::EngineEvent::continued &&
		cpu.rip == 10 && cpu.gpr(Register::rsp) == 392 &&
		memory.qword(392) == 5,
		"CALL did not push its return address");
	const auto callee = require_built(gadgets::build(memory, cpu.rip),
		"RET did not build");
	const auto return_result = gadgets::execute(
		callee.program, cpu, memory, 1);
	require(return_result.event == uml::cpu::EngineEvent::continued &&
		cpu.rip == 5 && cpu.gpr(Register::rsp) == 400,
		"RET did not restore RIP/RSP");
}

void decode_compat32_defaults()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x40,                         // inc eax, not REX
		0x8b, 0x05, 0x00, 0x01, 0x00, 0x00, // mov eax, [0x100]
	});
	const auto increment = decode(memory, 0, ExecutionMode::compat32);
	require(increment.status == DecodeStatus::decoded &&
		increment.instruction.operation == Operation::increment &&
		increment.instruction.destination.width == OperandWidth::dword &&
		increment.instruction.destination.reg.index == 0 &&
		increment.instruction.length == 1,
		"compat32 opcode 40 was not decoded as INC EAX");
	const auto load = decode(memory, 1, ExecutionMode::compat32);
	const auto &source = load.instruction.source;
	require(load.status == DecodeStatus::decoded &&
		load.instruction.destination.width == OperandWidth::dword,
		"compat32 ordinary operand width is not 32-bit");
	require(source.kind == OperandKind::memory &&
		source.memory.address_32 && source.memory.absolute_address &&
		source.memory.absolute == 0x100 && !source.memory.rip_relative,
		"compat32 ModR/M rm=5 is not an absolute 32-bit address");
}

/* A gather whose destination, mask and index registers are not all distinct
 * is #UD; the comparison is between full register numbers. */
void decode_gather_register_aliases()
{
	using namespace uml::cpu::x86_64;
	struct Case final {
		std::array<std::uint8_t, 6> bytes;
		bool legal;
		const char *message;
	};
	const std::array cases{
		Case{{0xc4, 0xe2, 0x69, 0x90, 0x04, 0x08}, true,
			"distinct gather registers did not decode"},
		Case{{0xc4, 0xa2, 0x69, 0x90, 0x0c, 0x08}, true,
			"gather index XMM9 was compared by its low bits"},
		Case{{0xc4, 0xe2, 0x79, 0x90, 0x04, 0x08}, false,
			"gather destination aliasing the mask decoded"},
		Case{{0xc4, 0xe2, 0x69, 0x90, 0x04, 0x00}, false,
			"gather destination aliasing the index decoded"},
		Case{{0xc4, 0xe2, 0x71, 0x90, 0x04, 0x08}, false,
			"gather mask aliasing the index decoded"},
		Case{{0xc4, 0xa2, 0x31, 0x90, 0x0c, 0x08}, false,
			"gather mask aliasing extended index XMM9 decoded"},
	};
	for (const auto &shape : cases) {
		TestMemory memory;
		memory.load(0, {shape.bytes[0], shape.bytes[1], shape.bytes[2],
			shape.bytes[3], shape.bytes[4], shape.bytes[5]});
		const auto decoded = decode(memory, 0);
		const bool legal = decoded.status == DecodeStatus::decoded &&
			decoded.instruction.operation == Operation::vector_gather;
		const bool illegal =
			decoded.status == DecodeStatus::illegal_instruction;
		require(shape.legal ? legal : illegal, shape.message);
	}
}

/* In 32-bit code C4 and C5 are LES and LDS unless the next byte's top two bits
 * are 11, which a memory ModR/M can never produce.  Inside a VEX prefix that
 * does select, VEX.B-bar and the top bit of vvvv are ignored, so only XMM0-7
 * are reachable. */
void decode_compat32_vex_aliases()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xc4, 0xe1, 0x38, 0x58, 0xc1, // vaddps xmm0, xmm0, xmm1 (vvvv[3] set)
		0xc4, 0xc1, 0x78, 0x28, 0xd1, // vmovaps xmm2, xmm1 (B-bar clear)
		0xcd, 0x80,                   // int 80h
	});
	memory.load(0x40, {0xc4, 0x01, 0x78, 0x28, 0xc1}); // les eax, [ecx]
	memory.load(0x50, {0xc5, 0x41, 0x58, 0xc1});       // lds eax, [ecx+0x58]
	for (const auto address : {0x40U, 0x50U}) {
		const auto alias = decode(memory, address, ExecutionMode::compat32);
		const auto &instruction = alias.instruction;
		require(alias.status == DecodeStatus::decoded &&
			instruction.operation == Operation::load_far_pointer &&
			instruction.vector_group == (address == 0x40U ? 0U : 3U) &&
			instruction.destination.reg.index == 0 &&
			instruction.source.memory.base == 1 &&
			instruction.length == (address == 0x40U ? 2U : 3U),
			"compat32 LES/LDS form decoded as a VEX prefix");
	}
	const auto wide = decode(memory, 5, ExecutionMode::long64);
	require(wide.status == DecodeStatus::decoded &&
		wide.instruction.source.reg.index == 9,
		"64-bit VEX.B-bar no longer selects XMM8-15");
	const auto built = require_built(
		gadgets::build(memory, 0, ExecutionMode::compat32),
		"compat32 VEX block did not build");
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	const auto fill = [&](std::size_t index, float value) {
		for (std::size_t lane = 0; lane < 4; ++lane)
			std::memcpy(&cpu.vectors[index][lane * sizeof(float)],
				&value, sizeof(float));
	};
	const auto lane0 = [&](std::size_t index) {
		float value{};
		std::memcpy(&value, &cpu.vectors[index][0], sizeof(float));
		return value;
	};
	fill(0, 1.0F);
	fill(1, 2.0F);
	fill(8, 100.0F);
	fill(9, 200.0F);
	const auto result = gadgets::execute(built.program, cpu, memory, 3);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		result.retired_instructions == 3,
		"compat32 VEX block did not reach INT 80h");
	require(lane0(0) == 3.0F,
		"compat32 VEX.vvvv[3] selected XMM8");
	require(lane0(2) == 2.0F,
		"compat32 VEX.B-bar selected XMM9");
}

void execute_compat32_stack_and_int80()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x53,             // push ebx
		0x59,             // pop ecx
		0xcd, 0x80,       // int 80h
	});
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.gpr(Register::rsp) = 0x1'0000'0200ULL;
	cpu.gpr(Register::rbx) = 0xaaaa'bbbb'1122'3344ULL;
	const auto built = gadgets::build(memory, 0, ExecutionMode::compat32);
	require(built.status == gadgets::BuildStatus::built &&
		built.program.execution_mode() == ExecutionMode::compat32,
		"compat32 stack/syscall block did not build with mode identity");
	const auto result = gadgets::execute(built.program, cpu, memory, 3);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		result.retired_instructions == 3,
		"compat32 INT 80h did not exit after the stack operations");
	require(cpu.gpr(Register::rsp) == 0x200 &&
		cpu.gpr(Register::rcx) == 0x1122'3344 &&
		memory.dword(0x1fc) == 0x1122'3344,
		"compat32 PUSH/POP did not use a wrapping 32-bit stack");
	require(cpu.system_call_32bit && cpu.rip == 4,
		"compat32 INT 80h did not publish its syscall convention or EIP");
}

/* ENTER's default operand size follows the stack, so an i386 ENTER without a
 * 0x66 prefix builds a 32-bit frame: EBP and each copied frame pointer are
 * dwords, and ESP and EBP are written zero-extended. */
void execute_compat32_enter_frames()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xc8, 0x10, 0x00, 0x00, // enter 0x10, 0
		0xc8, 0x08, 0x00, 0x02, // enter 8, 2
	});
	memory.load(0x17c, {0xbb, 0xbb, 0xaa, 0xaa});
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.gpr(Register::rsp) = 0x1'0000'0200ULL;
	cpu.gpr(Register::rbp) = 0xffff'ffff'1234'5678ULL;
	const auto flat = require_built(
		gadgets::build(memory, 0, ExecutionMode::compat32, 1),
		"compat32 ENTER did not build");
	const auto flat_result = gadgets::execute(flat.program, cpu, memory, 1);
	require(flat_result.event == uml::cpu::EngineEvent::continued &&
			flat_result.retired_instructions == 1 && cpu.rip == 4,
		"compat32 ENTER did not retire");
	require(memory.dword(0x1fc) == 0x1234'5678 &&
		cpu.gpr(Register::rbp) == 0x1fc &&
		cpu.gpr(Register::rsp) == 0x1ec,
		"compat32 ENTER did not build a 32-bit frame");

	cpu.gpr(Register::rsp) = 0x1'0000'0200ULL;
	cpu.gpr(Register::rbp) = 0xffff'ffff'0000'0180ULL;
	const auto nested = require_built(
		gadgets::build(memory, 4, ExecutionMode::compat32, 1),
		"compat32 nested ENTER did not build");
	const auto nested_result =
		gadgets::execute(nested.program, cpu, memory, 1);
	require(nested_result.event ==
			uml::cpu::EngineEvent::continued &&
			nested_result.retired_instructions == 1 && cpu.rip == 8,
		"compat32 nested ENTER did not retire");
	require(memory.dword(0x1fc) == 0x180 &&
		memory.dword(0x1f8) == 0xaaaa'bbbb &&
		memory.dword(0x1f4) == 0x1fc,
		"compat32 nested ENTER did not copy dword frame pointers");
	require(cpu.gpr(Register::rbp) == 0x1fc &&
		cpu.gpr(Register::rsp) == 0x1ec,
		"compat32 nested ENTER did not set EBP and ESP");
}

void execute_compat32_pushfd_popfd()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x9c,                         // pushfd
		0x58,                         // pop eax
		0x35, 0x00, 0x00, 0x20, 0x00, // xor eax, 0x200000 (toggle ID flag)
		0x50,                         // push eax
		0x9d,                         // popfd
		0x9c,                         // pushfd
		0x5b,                         // pop ebx
	});
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.gpr(Register::rsp) = 0x200;
	cpu.rflags = 0x2;
	const auto built = require_built(
		gadgets::build(memory, 0, ExecutionMode::compat32),
		"compat32 pushfd/popfd block did not build");
	const auto result = gadgets::execute(built.program, cpu, memory, 7);
	require(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 7,
		"compat32 pushfd/popfd block did not retire all instructions");
	require((cpu.rflags & 0x200000ULL) != 0,
		"compat32 popfd did not set the ID flag in EFLAGS");
	require((cpu.gpr(Register::rbx) & 0x200000ULL) != 0,
	"compat32 pushfd did not observe the updated ID flag");
}


/* The segment registers Linux gives a task in `mode`, their descriptor
 * caches loaded as at an entry from the kernel. */
void linux_user_segments(uml::cpu::x86_64::CpuState &cpu,
	uml::cpu::x86_64::ExecutionMode mode)
{
	using namespace uml::cpu::x86_64;
	const bool compat = mode == ExecutionMode::compat32;
	cpu.execution_mode = mode;
	cpu.segment_selectors = {compat ? std::uint16_t{0x2b} : std::uint16_t{0},
		compat ? std::uint16_t{0x23} : std::uint16_t{0x33}, 0x2b,
		compat ? std::uint16_t{0x2b} : std::uint16_t{0}, 0, 0};
	(void)enter_segments(cpu);
}

void execute_compat32_legacy_segment_stack()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x1f, // pop ds -- first instruction in Wine's context-return tail
		0xcf, // iret
	});
	memory.load(0x200, {0x2b, 0x00, 0x00, 0x00});
	CpuState cpu{};
	linux_user_segments(cpu, ExecutionMode::compat32);
	cpu.gpr(Register::rsp) = 0x1'0000'0200ULL;
	const auto pop_ds = require_built(
		gadgets::build( memory, 0, ExecutionMode::compat32, 1),
		"compat32 POP DS did not build");
	const auto popped = gadgets::execute(pop_ds.program, cpu, memory, 1);
	require(popped.event == uml::cpu::EngineEvent::continued &&
			popped.retired_instructions == 1 && cpu.rip == 1,
		"compat32 POP DS did not retire before IRET");
	require(cpu.segment_selectors[3] == 0x2b &&
		cpu.gpr(Register::rsp) == 0x204,
		"compat32 POP DS did not load a 32-bit stack slot");
	const auto iret = require_built(
		gadgets::build( memory, cpu.rip, ExecutionMode::compat32, 1),
		"compat32 IRET after POP DS did not build");

	TestMemory round_trip_memory;
	round_trip_memory.load(0, {
		0x1e, // push ds
		0x07, // pop es
	});
	cpu = CpuState{};
	linux_user_segments(cpu, ExecutionMode::compat32);
	cpu.gpr(Register::rsp) = 0x204;
	cpu.segment_selectors[3] = 0x33;
	const auto round_trip = require_built(
		gadgets::build(round_trip_memory, 0, ExecutionMode::compat32,
			2),
		"compat32 segment PUSH/POP pair did not build");
	const auto transferred = gadgets::execute(
		round_trip.program, cpu, round_trip_memory, 2);
	require(transferred.event == uml::cpu::EngineEvent::continued &&
			transferred.retired_instructions == 2 && cpu.rip == 2,
		"compat32 segment PUSH/POP pair did not retire");
	require(cpu.segment_selectors[0] == 0x33 &&
		cpu.gpr(Register::rsp) == 0x204 &&
		round_trip_memory.dword(0x200) == 0x33,
		"compat32 segment PUSH/POP pair lost its selector");

	TestMemory opcode_memory;
	opcode_memory.load(0, {0x06, 0x07, 0x0e, 0x16, 0x17, 0x1e, 0x1f});
	constexpr std::array<std::uint8_t, 7> segments{0, 0, 1, 2, 2, 3, 3};
	constexpr std::array<std::uint8_t, 7> pushes{1, 0, 1, 1, 0, 1, 0};
	for (std::size_t index = 0; index < segments.size(); ++index) {
		const auto decoded = decode(
			opcode_memory, index, ExecutionMode::compat32);
		require(decoded.status == DecodeStatus::decoded &&
				decoded.instruction.operation == Operation::segment_stack &&
				decoded.instruction.vector_group == segments[index] &&
				decoded.instruction.vector_control == pushes[index],
			"legacy i386 segment-stack opcode did not decode");
	}
	require(decode(opcode_memory, 0, ExecutionMode::long64).status ==
		DecodeStatus::illegal_instruction,
		"long64 mode accepted a legacy segment-stack opcode");
}

void execute_long64_interrupt_return_stack_switch()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x48, 0xcf}); // iretq
	constexpr std::uint64_t stack = 0x200;
	constexpr std::uint64_t restored_stack = 0x900;
	constexpr std::uint64_t restored_rip = 0x1234'5678;
	const std::array<std::uint64_t, 5> frame{
		restored_rip,
		0x33,
		0x246,
		restored_stack,
		0x2b,
	};
	if (!memory.write(stack, std::as_bytes(std::span{frame})))
		fail("long64 IRETQ frame setup failed");
	const auto built = require_built(
		gadgets::build( memory, 0, ExecutionMode::long64, 1),
		"long64 IRETQ did not build");
	CpuState cpu{};
	cpu.rip = 0;
	cpu.gpr(Register::rsp) = stack;
	cpu.segment_selectors[1] = 0x33;
	cpu.segment_selectors[2] = 0x2b;
	/* User mode runs with IF set, which IRET at CPL 3 cannot change. */
	cpu.rflags = 0x202;
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	require(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1,
			"long64 IRETQ did not retire");
	require(cpu.rip == restored_rip &&
		cpu.gpr(Register::rsp) == restored_stack,
		"long64 IRETQ left the restored RSP on its old stack");
	require(cpu.segment_selectors[1] == 0x33 &&
		cpu.segment_selectors[2] == 0x2b && cpu.rflags == 0x246,
		"long64 IRETQ did not restore its selectors and flags");
}

/* In 64-bit mode IRET defaults to a 32-bit operand size: REX.W selects IRETQ
 * and 0x66 the 16-bit form, and at every size it pops the five-slot frame,
 * SS:RSP included, without regard to a privilege change. A narrower EIP or
 * ESP (or IP or SP) is zero-extended into RIP and RSP. */
void execute_long64_interrupt_return_operand_sizes()
{
	using namespace uml::cpu::x86_64;
	struct Case final {
		std::array<std::uint8_t, 2> bytes;
		std::size_t slot;
		std::uint64_t restored_rip;
		std::uint64_t restored_stack;
		const char *message;
	};
	const std::array cases{
		Case{{0xcf, 0x90}, 4, 0x1234'5678, 0x8765'4320,
			"64-bit IRETD did not pop its 32-bit frame"},
		Case{{0x48, 0xcf}, 8, 0x1'0000'0040, 0x1'0000'0900,
			"IRETQ did not pop its 64-bit frame"},
		Case{{0x66, 0xcf}, 2, 0x5678, 0x0900,
			"64-bit 16-bit IRET did not pop its 16-bit frame"},
	};
	/* The code and stack sit above 4 GiB, so every narrower result must
	 * clear the upper half the old RIP and RSP carried. */
	constexpr std::uint64_t base = 0x1'0000'0000;
	constexpr std::uint64_t stack = base + 0x200;
	for (const auto &shape : cases) {
		TestMemory memory{base};
		memory.load(base, {shape.bytes[0], shape.bytes[1]});
		const std::array<std::uint64_t, 5> frame{
			shape.restored_rip, 0x33, 0x0246, shape.restored_stack, 0x2b};
		for (std::size_t index = 0; index < frame.size(); ++index) {
			std::array<std::byte, 8> word{};
			std::memcpy(word.data(), &frame[index], word.size());
			if (!memory.write(stack + index * shape.slot,
				std::span{word.data(), shape.slot}))
				fail("64-bit IRET frame setup failed");
		}
		const auto built = require_built(
			gadgets::build( memory, base, ExecutionMode::long64, 1),
			"64-bit IRET did not build");
		CpuState cpu{};
		cpu.rip = base;
		cpu.gpr(Register::rsp) = stack;
		cpu.segment_selectors[1] = 0x33;
		cpu.segment_selectors[2] = 0x23;
		cpu.rflags = 0x202;
		const auto result = gadgets::execute(built.program, cpu, memory, 1);
		require(result.event == uml::cpu::EngineEvent::continued &&
				result.retired_instructions == 1,
				"64-bit IRET did not retire");
		require(cpu.rip == shape.restored_rip &&
			cpu.gpr(Register::rsp) == shape.restored_stack &&
			cpu.segment_selectors[1] == 0x33 &&
			cpu.segment_selectors[2] == 0x2b &&
			cpu.rflags == 0x246, shape.message);
	}
}

/* At CPL 3 IRET loads the flags POPF may load, plus RF: IF, IOPL, VM, VIF and
 * VIP keep their values, the reserved bits stay fixed, and a 16-bit IRET
 * leaves the upper half alone. */
void interrupt_return_masks_user_flags()
{
	using namespace uml::cpu::x86_64;
	struct Case final {
		ExecutionMode mode;
		std::array<std::uint8_t, 2> bytes;
		std::size_t slot;
		std::uint64_t current;
		std::uint64_t popped;
		std::uint64_t expected;
		const char *message;
	};
	const std::array cases{
		Case{ExecutionMode::long64, {0x48, 0xcf}, 8, 0x202,
			~0ULL, 0x25'4fd7,
			"IRETQ loaded flags outside the user mask"},
		Case{ExecutionMode::long64, {0x48, 0xcf}, 8, 0x3202,
			0, 0x3202,
			"IRETQ cleared IF or IOPL"},
		Case{ExecutionMode::long64, {0xcf, 0x90}, 4, 0x202,
			0xffff'ffffULL, 0x25'4fd7,
			"64-bit IRETD loaded flags outside the user mask"},
		Case{ExecutionMode::long64, {0x66, 0xcf}, 2, 0x24'0202,
			0xffffULL, 0x24'4fd7,
			"64-bit 16-bit IRET changed the upper flags"},
		Case{ExecutionMode::compat32, {0xcf, 0x00}, 4, 0x202,
			0xffff'ffffULL, 0x25'4fd7,
			"compat32 IRETD loaded flags outside the user mask"},
		Case{ExecutionMode::compat32, {0x66, 0xcf}, 2, 0x24'0202,
			0xffffULL, 0x24'4fd7,
			"compat32 16-bit IRET changed the upper flags"},
	};
	constexpr std::uint64_t stack = 0x200;
	constexpr std::uint64_t target = 0x40;
	for (const auto &shape : cases) {
		TestMemory memory;
		memory.load(0, {shape.bytes[0], shape.bytes[1]});
		const std::array<std::uint64_t, 5> frame{
			target, 0x23, shape.popped, 0x900, 0x2b};
		for (std::size_t index = 0; index < frame.size(); ++index) {
			std::array<std::byte, 8> word{};
			std::memcpy(word.data(), &frame[index], word.size());
			if (!memory.write(stack + index * shape.slot,
				std::span{word.data(), shape.slot}))
				fail("IRET frame setup failed");
		}
		const auto built = require_built(
			gadgets::build(memory, 0, shape.mode, 1),
			"IRET did not build");
		CpuState cpu{};
		cpu.execution_mode = shape.mode;
		cpu.gpr(Register::rsp) = stack;
		cpu.segment_selectors[1] = 0x23;
		cpu.segment_selectors[2] = 0x2b;
		cpu.rflags = shape.current;
		const auto result = gadgets::execute(built.program, cpu, memory, 1);
		/* A popped image that sets TF arms the next instruction. */
		const auto event = (shape.expected & ~shape.current &
			rflags_trap) != 0 ? uml::cpu::EngineEvent::trap_flag :
			uml::cpu::EngineEvent::continued;
		require(result.event == event &&
			result.retired_instructions == 1 && cpu.rip == target,
			"IRET did not retire");
		require(cpu.rflags == shape.expected, shape.message);
	}
}

void expose_consistent_xsave_cpuid_state()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0xa2});          // cpuid
	memory.load(16, {0x0f, 0x01, 0xd0}); // xgetbv
	const auto cpuid = gadgets::build(memory, 0, ExecutionMode::long64, 1);
	const auto xgetbv = gadgets::build(memory, 16, ExecutionMode::long64, 1);
	require(cpuid.status == gadgets::BuildStatus::built &&
			xgetbv.status == gadgets::BuildStatus::built,
		"extended-state identification instructions did not build");

	const auto run_cpuid = [&](std::uint32_t leaf, std::uint32_t subleaf,
			CpuState &cpu) {
		cpu = CpuState{};
		cpu.gpr(Register::rax) = leaf;
		cpu.gpr(Register::rcx) = subleaf;
		return gadgets::execute(cpuid.program, cpu, memory, 1);
	};
	CpuState cpu{};
	auto result = run_cpuid(0, 0, cpu);
	require(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == 13,
		"CPUID omitted its XSAVE enumeration leaf");
	result = run_cpuid(1, 0, cpu);
	constexpr std::uint64_t through_avx2_leaf_one =
		(1ULL << 0) | (1ULL << 1) | (1ULL << 9) | (1ULL << 12) |
		(1ULL << 13) | (1ULL << 19) | (1ULL << 20) | (1ULL << 22) |
		(1ULL << 23) | (1ULL << 25) | (1ULL << 26) | (1ULL << 27) |
		(1ULL << 28) | (1ULL << 29) | (1ULL << 30);
	require(result.event == uml::cpu::EngineEvent::continued &&
			(cpu.gpr(Register::rcx) & through_avx2_leaf_one) ==
				through_avx2_leaf_one,
		"CPUID leaf 1 omitted a qualified through-AVX2 feature");
	result = run_cpuid(7, 0, cpu);
	constexpr std::uint64_t through_avx2_leaf_seven_ebx =
		(1ULL << 0) | (1ULL << 3) | (1ULL << 5) | (1ULL << 8) |
		(1ULL << 9) | (1ULL << 18) | (1ULL << 19) | (1ULL << 23) |
		(1ULL << 24) | (1ULL << 29);
	require(result.event == uml::cpu::EngineEvent::continued &&
			(cpu.gpr(Register::rbx) & through_avx2_leaf_seven_ebx) ==
				through_avx2_leaf_seven_ebx &&
			(cpu.gpr(Register::rcx) & (1ULL << 22)) != 0,
		"CPUID leaf 7 omitted a qualified AVX2-era extension");
	/* Leaf 7 EAX is the highest subleaf, which software reads before it
	 * asks for subleaf 1; subleaf 1 EAX bit 4 is AVX-VNNI and the rest of
	 * that subleaf, and every subleaf above it, reads as zero. */
	require(cpu.gpr(Register::rax) == 1,
		"CPUID leaf 7 did not report subleaf 1 as its highest");
	result = run_cpuid(7, 1, cpu);
	require(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == (1ULL << 4) &&
			cpu.gpr(Register::rbx) == 0 && cpu.gpr(Register::rcx) == 0 &&
			cpu.gpr(Register::rdx) == 0,
		"CPUID leaf 7 subleaf 1 did not report exactly AVX-VNNI");
	result = run_cpuid(7, 2, cpu);
	require(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == 0 && cpu.gpr(Register::rbx) == 0 &&
			cpu.gpr(Register::rcx) == 0 && cpu.gpr(Register::rdx) == 0,
		"CPUID leaf 7 answered a subleaf above its highest");
	result = run_cpuid(13, 0, cpu);
	require(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == 7 &&
			cpu.gpr(Register::rbx) == 832 &&
			cpu.gpr(Register::rcx) == 832 &&
			cpu.gpr(Register::rdx) == 0,
		"CPUID leaf 0xD did not describe the complete XSAVE image");
	result = run_cpuid(13, 2, cpu);
	require(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == 256 &&
			cpu.gpr(Register::rbx) == 576 &&
			cpu.gpr(Register::rcx) == 0 &&
			cpu.gpr(Register::rdx) == 0,
		"CPUID leaf 0xD subleaf 2 did not locate AVX state");

	cpu = CpuState{};
	cpu.rip = 16;
	cpu.gpr(Register::rcx) = 0;
	result = gadgets::execute(xgetbv.program, cpu, memory, 1);
	require(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == 7 &&
			cpu.gpr(Register::rdx) == 0,
		"XGETBV did not agree with the CPUID XSAVE feature mask");
}

/* CPUID serializes, so it is a code-change checkpoint: the run leaves after
 * it, with the instructions behind it unrun, and the runtime revalidates what
 * it selects for them. */
void cpuid_leaves_the_run()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x0f, 0xa2,       // cpuid
		0x0f, 0x05,       // syscall
	});
	const auto built = require_built(
		gadgets::build(memory, 0, ExecutionMode::long64, 64),
		"CPUID block did not build");
	CpuState cpu{};
	const auto result = gadgets::execute(built.program, cpu, memory, 64);
	require(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1 && cpu.rip == 2,
		"CPUID did not leave the run for the instruction after it");
}

void wrap_compat32_instruction_pointer()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t base = 0xffff'f000ULL;
	constexpr std::uint64_t address = 0xffff'fff0ULL;
	TestMemory memory{base};
	memory.load(address, {0xe9, 0x30, 0x00, 0x00, 0x00});
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.rip = address;
	const auto built = require_built(
		gadgets::build( memory, address, ExecutionMode::compat32, 1),
		"compat32 wrapping JMP did not build");
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	require(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 1 && cpu.rip == 0x25,
		"compat32 relative branch did not wrap EIP at 32 bits");
}

void execute_compat32_tls_segment()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x6b, 0x00, 0x00, 0x00,             // mov eax, 0x6b
		0x8e, 0xe8,                               // mov gs, ax
		0x65, 0xc7, 0x45, 0x04, 0xff, 0xff, 0xff, 0xff,
		0xb8, 0x33, 0x00, 0x00, 0x00,             // mov eax, 0x33
		0x8e, 0xe8,                               // mov gs, ax
	});
	/* An x86-64 kernel's TLS entries are 12 to 14; the second, selector
	 * 0x6b, holds a 4 GiB data segment based at 0xfffff000, and 0x33,
	 * entry 6, is the 64-bit code segment, readable and based at zero. */
	CpuState cpu{};
	cpu.segment_selectors = {0x2b, 0x23, 0x2b, 0x2b, 0, 0};
	cpu.tls_descriptors[1] = 0xffcf'f3ff'f000'ffffULL;
	require(!enter_segments(cpu) &&
		cpu.execution_mode == ExecutionMode::compat32,
		"compat32 entry segments did not load");
	cpu.gpr(Register::rbp) = 0x1080;
	const auto built = require_built(
		gadgets::build( memory, 0, ExecutionMode::compat32, 3),
		"compat32 TLS segment block did not build");
	const auto result = gadgets::execute(built.program, cpu, memory, 3);
	require(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 3,
		"compat32 TLS segment block did not execute");
	require(cpu.segment_selectors[5] == 0x6b &&
		cpu.gs_base == 0xffff'f000,
		"compat32 GS selector did not load its TLS base");
	require(memory.dword(0x84) == 0xffff'ffff,
		"compat32 segmented linear address did not wrap at 32 bits");
	const auto stale = require_built(
		gadgets::build( memory, cpu.rip, ExecutionMode::compat32, 2),
		"compat32 non-TLS selector block did not build");
	const auto cleared = gadgets::execute(stale.program, cpu, memory, 2);
	require(cleared.event == uml::cpu::EngineEvent::continued &&
		cleared.retired_instructions == 2 &&
		cpu.segment_selectors[5] == 0x33 && cpu.gs_base == 0,
		"compat32 selector 0x33 kept a TLS base");
}

void reject_program_mode_mismatch()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x40});
	const auto built = require_built(
		gadgets::build( memory, 0, ExecutionMode::compat32, 1),
		"compat32 identity block did not build");
	CpuState cpu{};
	const auto mismatch = gadgets::execute(built.program, cpu, memory, 1);
	require(mismatch.event == uml::cpu::EngineEvent::fatal &&
		mismatch.retired_instructions == 0,
		"long64 CPU accepted a compat32 native stream");
	cpu.execution_mode = ExecutionMode::compat32;
	const auto match = gadgets::execute(built.program, cpu, memory, 1);
	require(match.event == uml::cpu::EngineEvent::continued &&
		match.retired_instructions == 1 &&
		cpu.gpr(Register::rax) == 1 && cpu.rip == 1,
		"compat32 CPU did not execute its matching native stream");
}

void chunk_repeating_string_for_interrupt()
{
	using namespace uml::cpu::x86_64;
	/* A repeated string operation has to hand the executor a boundary so a
	 * mapping change can take the execution read gate. The element loop
	 * counts elements. The run loop counts the bytes it moves instead: one
	 * run stands for thousands of elements, so counting runs would let a long
	 * copy hold the gate far longer than the element loop ever did.
	 *
	 * Stopping there is where an interrupt finds the instruction, and
	 * hardware then saves RF, so both boundaries set it; the exit that runs
	 * no instruction keeps it, and resuming clears it (audit NC-E6).
	 *
	 * The instruction retires nothing at either boundary, so each reports
	 * that it yielded, and the dispatch charges the work to its instruction
	 * budget; otherwise a loop of long REP strings never returns to the
	 * kernel, which services its timer only there (audit NC-E8).
	 */
	TestMemory descending;
	descending.load(0, {0xf3, 0xaa}); // rep stosb
	CpuState down{};
	down.gpr(Register::rax) = 0x5a;
	down.gpr(Register::rcx) = 4097;
	down.gpr(Register::rdi) = 8192;
	down.rflags |= rflags_direction;
	const auto element_built = require_built(
		gadgets::build(descending, 0, 1),
		"descending REP STOSB did not build");
	const auto element = gadgets::execute(
		element_built.program, down, descending, 1);
	require(element.event == uml::cpu::EngineEvent::continued &&
		element.retired_instructions == 0 && down.rip == 0 &&
		down.gpr(Register::rcx) == 1 &&
		down.gpr(Register::rdi) == 4096 &&
		descending.byte(8192) == 0x5a && descending.byte(4097) == 0x5a,
		"descending REP STOSB did not yield after its element budget");
	require((down.rflags & rflags_resume) != 0,
		"REP STOSB stopped by its element budget did not set RF");
	require(element.repeat_yielded,
		"REP STOSB stopped by its element budget did not yield");

	TestMemory memory{0, 270336};
	memory.load(0, {0xf3, 0xaa}); // rep stosb
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x5a;
	cpu.gpr(Register::rcx) = 262145;
	cpu.gpr(Register::rdi) = 4096;
	const auto built = require_built(gadgets::build(memory, 0, 1),
		"REP STOSB did not build");
	const auto first = gadgets::execute(built.program, cpu, memory, 1);
	require(first.event == uml::cpu::EngineEvent::continued &&
		first.retired_instructions == 0 && cpu.rip == 0,
		"long REP STOSB did not yield before retiring");
	require(cpu.gpr(Register::rcx) == 1 &&
		cpu.gpr(Register::rdi) == 266240 &&
		memory.byte(4096) == 0x5a && memory.byte(266239) == 0x5a,
		"REP STOSB chunk did not preserve its partial state");
	require((cpu.rflags & rflags_resume) != 0,
		"REP STOSB stopped by its run budget did not set RF");
	require(first.repeat_yielded,
		"REP STOSB stopped by its run budget did not yield");
	/* 32-bit address arithmetic reaches the same run loop. Its yield
	 * boundary is the cheapest proof: the element loop would have stopped
	 * after 4096 elements rather than 256 KiB of them.
	 */
	TestMemory narrow{0, 270336};
	narrow.load(0, {0xf3, 0x67, 0xaa}); // addr32 rep stosb
	CpuState small{};
	small.gpr(Register::rax) = 0x5a;
	small.gpr(Register::rcx) = 262145;
	small.gpr(Register::rdi) = 4096;
	const auto narrow_built = require_built(gadgets::build(narrow, 0, 1),
		"32-bit REP STOSB did not build");
	const auto narrow_first = gadgets::execute(
		narrow_built.program, small, narrow, 1);
	require(narrow_first.event == uml::cpu::EngineEvent::continued &&
		small.gpr(Register::rcx) == 1 &&
		small.gpr(Register::rdi) == 266240 &&
		narrow.byte(266239) == 0x5a,
		"32-bit REP STOSB did not move its bytes in runs");

	unsigned int sequence = 1;
	const uml::cpu::InterruptToken interrupt{
		.sequence = &sequence,
		.snapshot = 0,
	};
	const auto interrupted = gadgets::execute(
		built.program, cpu, memory, 1, interrupt);
	require(interrupted.event == uml::cpu::EngineEvent::interrupt &&
		interrupted.retired_instructions == 0 &&
		!interrupted.repeat_yielded &&
		cpu.gpr(Register::rcx) == 1 && cpu.rip == 0 &&
		(cpu.rflags & rflags_resume) != 0,
		"REP STOSB chunk did not expose its interrupt boundary with RF");
	const auto finished = gadgets::execute(built.program, cpu, memory, 1);
	require(finished.event == uml::cpu::EngineEvent::continued &&
		finished.retired_instructions == 1 && cpu.rip == 2 &&
		!finished.repeat_yielded,
		"REP STOSB did not retire after resuming");
	require(cpu.gpr(Register::rcx) == 0 &&
		cpu.gpr(Register::rdi) == 266241 &&
		memory.byte(266240) == 0x5a,
		"REP STOSB did not complete from its partial state");
	require((cpu.rflags & rflags_resume) == 0,
		"resumed REP STOSB kept RF");
}

void repeated_store_into_executable_page_publishes_its_run()
{
	using namespace uml::cpu::x86_64;
	/* A repeated store whose destination page can hold code is the one shape
	 * where a run has to do more than move bytes: it publishes the change and
	 * leaves through the instruction's own exit instead of chaining onward.
	 * Publishing once for a run rather than once per element is the only
	 * reason such a destination is worth running at all, so the result has to
	 * be the element loop's result -- every byte written, one code change
	 * published, and the block ended.
	 */
	TestMemory memory;
	memory.load(0, {0xf3, 0xaa}); // rep stosb
	memory.set_executable_page(0);
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x5a;
	cpu.gpr(Register::rcx) = 256;
	cpu.gpr(Register::rdi) = 0x800;
	gadgets::Executor executor;
	executor.prepare(memory, 1, 1, 1);
	/* Cached, so the destination page is on the shared code-page list. */
	const auto built = executor.select_program(
		memory, 0, ExecutionMode::long64, 1);
	require(built.program != nullptr,
		"executable-destination REP STOSB did not build");
	const auto result = gadgets::execute(
		*built.program, cpu, memory, executor, 64);
	require(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 1 && cpu.rip == 2 &&
		cpu.gpr(Register::rcx) == 0 && cpu.gpr(Register::rdi) == 0x900,
		"executable-destination REP STOSB did not end its block");
	bool written = true;
	for (std::uint64_t address = 0x800; address < 0x900; ++address)
		written = written && memory.byte(address) == 0x5a;
	require(written,
		"executable-destination REP STOSB did not write its run");
	require(memory.byte(0x7ff) == 0 && memory.byte(0x900) == 0,
		"executable-destination REP STOSB wrote past its run");
	require(memory.invalidations() == 1,
		"executable-destination REP STOSB did not publish once");
}

void repeated_move_faults_on_a_run_boundary()
{
	using namespace uml::cpu::x86_64;
	/* A run writes its architectural state back before starting the next
	 * one, so a translation that faults leaves the instruction resumable at
	 * exactly the byte it reached. Nothing else in the engine can check that:
	 * the element loop kept the same invariant one element at a time, and a
	 * run advances thousands of elements between writebacks.
	 */
	TestMemory memory;
	memory.load(0, {0xf3, 0xa4}); // rep movsb
	for (std::uint64_t address = 4096; address < 8192; ++address)
		memory.load(address, {static_cast<std::uint8_t>(address & 0xff)});
	CpuState cpu{};
	cpu.gpr(Register::rcx) = 8192;
	cpu.gpr(Register::rsi) = 4096;
	cpu.gpr(Register::rdi) = 12288;
	const auto built = require_built(gadgets::build(memory, 0, 1),
		"faulting REP MOVSB did not build");
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	/* The first run fills the last mapped page; the second cannot translate
	 * its destination at all. */
	require(result.event == uml::cpu::EngineEvent::memory_fault &&
		cpu.rip == 0 && cpu.gpr(Register::rcx) == 4096 &&
		cpu.gpr(Register::rsi) == 8192 &&
		cpu.gpr(Register::rdi) == 16384,
		"faulting REP MOVSB did not stop on its run boundary");
	bool copied = true;
	for (std::uint64_t offset = 0; offset < 4096; ++offset)
		copied = copied && memory.byte(12288 + offset) ==
			memory.byte(4096 + offset);
	require(copied,
		"faulting REP MOVSB did not keep the bytes it had moved");
}

void bulk_string_runs_match_the_element_definition()
{
	using namespace uml::cpu::x86_64;
	/* The run loop is only a faster spelling of the element loop, so the two
	 * have to agree byte for byte and register for register across every
	 * shape that decides between them: a page boundary falling inside a run
	 * or inside one element, a destination overlapping its source in either
	 * direction, a descending direction, and counts on both sides of the
	 * length below which a run is not worth starting.
	 *
	 * Both are compared against the architectural definition rather than
	 * against each other, so a shape neither one handles is still caught.
	 */
	struct Case final {
		std::uint8_t prefix;
		std::uint8_t opcode;
		unsigned width;
		bool move;
		bool address32;
		const char *name;
	};
	static constexpr Case cases[] = {
		{0x00, 0xa4, 1, true, false, "rep movsb"},
		{0x66, 0xa5, 2, true, false, "rep movsw"},
		{0x00, 0xa5, 4, true, false, "rep movsd"},
		{0x48, 0xa5, 8, true, false, "rep movsq"},
		{0x00, 0xaa, 1, false, false, "rep stosb"},
		{0x66, 0xab, 2, false, false, "rep stosw"},
		{0x00, 0xab, 4, false, false, "rep stosd"},
		{0x48, 0xab, 8, false, false, "rep stosq"},
		/* 32-bit address arithmetic reaches the same bytes here but keeps
		 * its pointers narrow, which is a separate advance. */
		{0x00, 0xa4, 1, true, true, "a32 rep movsb"},
		{0x48, 0xa5, 8, true, true, "a32 rep movsq"},
		{0x00, 0xaa, 1, false, true, "a32 rep stosb"},
		{0x48, 0xab, 8, false, true, "a32 rep stosq"},
	};
	/* Distances from a page boundary that put a run's first byte, its last
	 * byte, or one whole element across it. */
	static constexpr std::int64_t offsets[] = {0, 1, 7, -1, -7, -9, -64};
	static constexpr std::int64_t leads[] = {
		-4096, -70, -33, -16, -8, -1, 0, 1, 8, 16, 33, 70, 4096};
	static constexpr std::uint64_t counts[] = {
		1, 4, 15, 16, 31, 32, 33, 63, 64, 65, 130, 520};

	constexpr std::uint64_t span = 65536;
	constexpr std::uint64_t low = 8192;
	constexpr std::uint64_t high = 57344;
	constexpr std::uint64_t anchor = 32768;
	constexpr std::uint64_t value = 0x0123'4567'89ab'cdefULL;
	std::vector<std::byte> pattern(span);
	for (std::uint64_t address = 0; address < span; ++address)
		pattern[address] = static_cast<std::byte>(
			(address * 7U) ^ (address >> 5U));
	std::vector<std::byte> model(span);
	char message[192];

	for (const auto &item : cases) {
		TestMemory memory{0, span};
		std::uint64_t length = 2;
		if (item.address32 && item.prefix != 0) {
			memory.load(0, {0xf3, 0x67, item.prefix, item.opcode});
			length = 4;
		} else if (item.address32) {
			memory.load(0, {0xf3, 0x67, item.opcode});
			length = 3;
		} else if (item.prefix != 0) {
			memory.load(0, {0xf3, item.prefix, item.opcode});
			length = 3;
		} else {
			memory.load(0, {0xf3, item.opcode});
		}
		const auto built = gadgets::build(memory, 0, 1);
		require(built.status == gadgets::BuildStatus::built, item.name);

		for (const auto descending : {false, true})
		for (const auto offset : offsets)
		for (const auto lead : leads)
		for (const auto count : counts) {
			const auto reach = static_cast<std::int64_t>(
				count * item.width);
			const auto source =
				static_cast<std::int64_t>(anchor) + offset;
			const auto destination = source + lead;
			/* Ends of the two ranges, whichever way they run. */
			const auto source_first = descending ?
				source - reach + static_cast<std::int64_t>(item.width) :
				source;
			const auto destination_first = descending ?
				destination - reach +
					static_cast<std::int64_t>(item.width) :
				destination;
			const auto first = source_first < destination_first ?
				source_first : destination_first;
			const auto last =
				(source_first > destination_first ?
					source_first : destination_first) + reach;
			if (first < static_cast<std::int64_t>(low) ||
				last > static_cast<std::int64_t>(high))
				continue;

			/* Restore only the window the case can touch, plus a guard
			 * band a run that overshoots would land in. */
			const auto begin = static_cast<std::uint64_t>(first) - 64;
			const auto end = static_cast<std::uint64_t>(last) + 64;
			memory.fill(begin, std::span{pattern}.subspan(
				begin, end - begin));
			std::memcpy(model.data() + begin, pattern.data() + begin,
				end - begin);

			CpuState cpu{};
			cpu.gpr(Register::rax) = value;
			cpu.gpr(Register::rcx) = count;
			cpu.gpr(Register::rsi) = static_cast<std::uint64_t>(source);
			cpu.gpr(Register::rdi) =
				static_cast<std::uint64_t>(destination);
			if (descending)
				cpu.rflags |= rflags_direction;

			/* The architectural definition: one element at a time in
			 * address order, each read whole before it is written, so a
			 * later element sees what earlier ones wrote but no element
			 * overwrites its own source.
			 */
			auto si = source;
			auto di = destination;
			const auto step = descending ?
				-static_cast<std::int64_t>(item.width) :
				static_cast<std::int64_t>(item.width);
			for (auto left = count; left != 0; --left) {
				std::array<std::byte, 8> element{};
				for (unsigned index = 0; index < item.width; ++index)
					element[index] = item.move ?
						model[static_cast<std::size_t>(si) + index] :
						static_cast<std::byte>(
							(value >> (8U * index)) & 0xffU);
				for (unsigned index = 0; index < item.width; ++index)
					model[static_cast<std::size_t>(di) + index] =
						element[index];
				si += step;
				di += step;
			}

			unsigned entries = 0;
			bool ran = true;
			while (ran && cpu.rip != length) {
				const auto result = gadgets::execute(
					built.program, cpu, memory, 1);
				ran = result.event ==
					uml::cpu::EngineEvent::continued &&
					++entries <= 8192;
			}

			bool same = ran && cpu.gpr(Register::rcx) == 0 &&
				cpu.gpr(Register::rdi) ==
					static_cast<std::uint64_t>(di) &&
				(!item.move || cpu.gpr(Register::rsi) ==
					static_cast<std::uint64_t>(si));
			for (auto address = begin; same && address < end; ++address)
				same = memory.byte(address) ==
					static_cast<std::uint8_t>(model[address]);
			if (!same) {
				std::snprintf(message, sizeof(message),
					"%s%s diverged from the element definition "
					"(offset %lld lead %lld count %llu)",
					item.name, descending ? " down" : "",
					static_cast<long long>(offset),
					static_cast<long long>(lead),
					static_cast<unsigned long long>(count));
				fail(message);
			}
		}
	}
}
/* CMP, SUB, TEST and AND leave compatible host flags for the next condition:
 * zero and unsigned subtraction ordering at narrow widths, and the supported
 * NZCV conditions at 32/64 bits. The slot after that reads through the deferred
 * record instead, so the two bytes a pair of identical SETcc write must agree
 * for every producer, width, operand pair and condition. That equality is the
 * whole correctness claim of the fused path, and it needs no reference model:
 * the established path is the reference.
 */
void fused_conditions_match_the_deferred_evaluation()
{
	using namespace uml::cpu::x86_64;
	/* CMP, SUB, TEST, AND and ADD register forms, then INC, DEC and NEG. */
	static constexpr std::array<unsigned, 5> producers{
		0x39, 0x29, 0x85, 0x21, 0x01};
	static constexpr std::array<std::array<unsigned, 2>, 3> unary{{
		{0xff, 0xc0}, {0xff, 0xc8}, {0xf7, 0xd8}}};
	static constexpr std::array<std::uint64_t, 17> values{
		0U, 1U, 2U, 0x7fU, 0x80U, 0xffU, 0x7fffU, 0x8000U, 0xffffU,
		0x7fff'ffffULL, 0x8000'0000ULL,
		0xffff'ffffULL, 0x7fff'ffff'ffff'ffffULL, 0x8000'0000'0000'0000ULL,
		~std::uint64_t{}, 0x5555'5555'5555'5555ULL, 0xaaaa'aaaa'aaaa'aaaaULL,
	};
	for (std::size_t index = 0; index < producers.size() + unary.size();
		++index) {
		const bool binary = index < producers.size();
		for (const unsigned width : {8U, 16U, 32U, 64U}) {
			for (unsigned condition = 0; condition < 16U; ++condition) {
				for (const auto left : values) {
					for (const auto right : values) {
						if (!binary && right != values.front())
							break;
						std::vector<std::byte> code;
						const auto push = [&code](unsigned byte) {
							code.push_back(static_cast<std::byte>(byte));
						};
						if (width == 16U)
							push(0x66);
						if (width == 64U)
							push(0x48);
						if (binary) {
							const auto producer = producers[index];
							push(width == 8U ? producer - 1U : producer);
							push(0xd8);
						} else {
							const auto &form = unary[index - producers.size()];
							push(width == 8U ? form[0] - 1U : form[0]);
							push(form[1]);
						}
						/* SYSCALL writes its return address to RCX and
						 * the flags to R11, so the two results live in RDX
						 * and RBX. */
						push(0x0f);
						push(0x90U + condition);
						push(0xc2);
						push(0x0f);
						push(0x90U + condition);
						push(0xc3);
						push(0x0f);
						push(0x05);
						TestMemory memory;
						memory.fill(0, code);
						const auto built = gadgets::build(memory, 0);
						require(built.status ==
							gadgets::BuildStatus::built,
							"fused condition program did not build");
						CpuState cpu{};
						cpu.gpr(Register::rax) = left;
						cpu.gpr(Register::rbx) = right;
						gadgets::Executor executor;
						executor.prepare(memory, 1, 1, 1);
						const auto result = gadgets::execute(
							built.program, cpu, memory, executor, 64);
						require(result.event ==
							uml::cpu::EngineEvent::system_call,
							"fused condition program did not reach SYSCALL");
						const auto fused =
							cpu.gpr(Register::rdx) & 0xffU;
						const auto deferred =
							cpu.gpr(Register::rbx) & 0xffU;
						if (fused != deferred) {
							char message[160];
							std::snprintf(message, sizeof(message),
								"a fused condition disagreed with the "
								"deferred evaluation of the same flags "
								"(producer %zu width %u condition %u left "
								"%#llx right %#llx: fused %u deferred %u)",
								index, width, condition,
								static_cast<unsigned long long>(left),
								static_cast<unsigned long long>(right),
								static_cast<unsigned>(fused),
								static_cast<unsigned>(deferred));
							fail(message);
						}
					}
				}
			}
		}
	}
}

/* Compat32 code runs on the ordinary bank, where a byte or word operand is
 * zero-extended into a 32-bit host operation. Subtraction keeps the zero and
 * unsigned-order conditions there and a logical operation keeps zero alone; an
 * addition keeps nothing, because its carry lands in bit 8 or 16 of the host
 * result, and the narrow unary forms set no host flags at all. The same two
 * SETcc as above hold every producer, width and condition to the deferred
 * evaluation.
 */
void ordinary_bank_conditions_match_the_deferred_evaluation()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<unsigned, 5> producers{
		0x39, 0x29, 0x85, 0x21, 0x01};
	static constexpr std::array<std::array<unsigned, 2>, 3> unary{{
		{0xff, 0xc0}, {0xff, 0xc8}, {0xf7, 0xd8}}};
	static constexpr std::array<std::uint64_t, 12> values{
		0U, 1U, 2U, 0x7fU, 0x80U, 0xffU, 0x100U, 0x7fffU, 0x8000U, 0xffffU,
		0x7fff'ffffULL, 0xffff'ffffULL,
	};
	for (std::size_t index = 0; index < producers.size() + unary.size();
		++index) {
		const bool binary = index < producers.size();
		for (const unsigned width : {8U, 16U, 32U}) {
			for (unsigned condition = 0; condition < 16U; ++condition) {
				std::vector<std::byte> code;
				const auto push = [&code](unsigned byte) {
					code.push_back(static_cast<std::byte>(byte));
				};
				if (width == 16U)
					push(0x66);
				if (binary) {
					push(width == 8U ? producers[index] - 1U :
						producers[index]);
					push(0xd8);
				} else {
					const auto &form = unary[index - producers.size()];
					push(width == 8U ? form[0] - 1U : form[0]);
					push(form[1]);
				}
				push(0x0f);
				push(0x90U + condition);
				push(0xc2);
				push(0x0f);
				push(0x90U + condition);
				push(0xc3);
				TestMemory memory;
				memory.fill(0, code);
				const auto built = gadgets::build(
					memory, 0, ExecutionMode::compat32, 3);
				require(built.status == gadgets::BuildStatus::built &&
					built.program.instruction_count() == 3,
					"ordinary-bank condition program did not build");
				for (const auto left : values) {
					for (const auto right : values) {
						if (!binary && right != values.front())
							break;
						CpuState cpu{};
						cpu.execution_mode = ExecutionMode::compat32;
						cpu.gpr(Register::rax) = left;
						cpu.gpr(Register::rbx) = right;
						const auto result = gadgets::execute(
							built.program, cpu, memory, 3);
						require(result.retired_instructions == 3,
							"ordinary-bank condition program did not "
							"retire its three instructions");
						const auto fused = cpu.gpr(Register::rdx) & 0xffU;
						const auto deferred =
							cpu.gpr(Register::rbx) & 0xffU;
						if (fused != deferred) {
							char message[192];
							std::snprintf(message, sizeof(message),
								"an ordinary-bank condition disagreed "
								"with the deferred evaluation of the same "
								"flags (producer %zu width %u condition "
								"%u left %#llx right %#llx: fused %u "
								"deferred %u)",
								index, width, condition,
								static_cast<unsigned long long>(left),
								static_cast<unsigned long long>(right),
								static_cast<unsigned>(fused),
								static_cast<unsigned>(deferred));
							fail(message);
						}
					}
				}
			}
		}
	}
}

/* INC and DEC preserve the carry flag. When the builder cannot see the
 * producer that defined it -- a store or a block boundary sits between -- the
 * gadget no longer settles it: it parks the producer's record in the carry
 * slot and leaves the carry to whoever reads it through the INC or DEC record.
 * Every reader must see the producer's carry: the carry materializer (ADC),
 * the full one (LAHF), the C++ one (an exit), a second INC or DEC that
 * inherits the pending bit, an override riding on the record (BT), and the
 * memory form. The other five flags come from the INC or DEC itself.
 */
void lazy_carry_reaches_every_reader()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<std::uint64_t, 6> values{
		0U, 1U, 0x7fU, 0x80U, 0xffU, ~std::uint64_t{}};
	enum class Reader { adc, lahf, exit, twice_adc, bt_adc, memory_adc };
	static constexpr std::array<Reader, 6> readers{Reader::adc, Reader::lahf,
		Reader::exit, Reader::twice_adc, Reader::bt_adc, Reader::memory_adc};
	for (const unsigned width : {8U, 16U, 32U, 64U}) {
		const auto mask = width == 64U ? ~std::uint64_t{} :
			(std::uint64_t{1} << width) - 1U;
		for (const bool subtract : {false, true})
		for (const bool decrement : {false, true})
		for (const bool jump : {false, true})
		for (const auto reader : readers)
		for (const auto left : values)
		for (const auto right : values) {
			std::vector<std::byte> code;
			const auto push = [&code](std::initializer_list<unsigned> bytes) {
				for (const auto byte : bytes)
					code.push_back(static_cast<std::byte>(byte));
			};
			const auto prefix = [&]() {
				if (width == 16U)
					push({0x66});
				if (width == 64U)
					push({0x48});
			};
			/* ADD or SUB rax, rbx at the width defines the carry. */
			prefix();
			push({width == 8U ? (subtract ? 0x28U : 0x00U) :
				(subtract ? 0x29U : 0x01U), 0xd8});
			/* A store, or a block boundary, hides it from the builder. */
			if (jump)
				push({0xeb, 0x00});
			else
				push({0x48, 0x89, 0x0f});
			/* INC or DEC of rcx, or of [rdi]. */
			const unsigned unary = width == 8U ? 0xfeU : 0xffU;
			const unsigned modrm = reader == Reader::memory_adc ?
				(decrement ? 0x0fU : 0x07U) : (decrement ? 0xc9U : 0xc1U);
			prefix();
			push({unary, modrm});
			switch (reader) {
			case Reader::twice_adc:
				prefix();
				push({unary, modrm});
				[[fallthrough]];
			case Reader::adc:
			case Reader::memory_adc:
				push({0x48, 0x83, 0xd2, 0x00}); /* adc rdx, 0 */
				break;
			case Reader::bt_adc:
				/* bt rcx, 1: a carry override on the INC or DEC record. */
				push({0x48, 0x0f, 0xba, 0xe1, 0x01, 0x48, 0x83, 0xd2, 0x00});
				break;
			case Reader::lahf:
				push({0x9f});
				break;
			case Reader::exit:
				break;
			}
			push({0x0f, 0x05});
			TestMemory memory;
			memory.fill(0, code);
			CpuState cpu{};
			cpu.gpr(Register::rax) = left;
			cpu.gpr(Register::rbx) = right;
			cpu.gpr(Register::rdi) = 0x2000;
			gadgets::Executor executor;
			executor.prepare(memory, 1, 1, 1);
			run_to_system_call(executor, memory, cpu,
				"lazy carry program");
			const auto l = left & mask;
			const auto r = right & mask;
			const std::uint64_t carry = subtract ? (l < r ? 1U : 0U) :
				(((l + r) & mask) < l ? 1U : 0U);
			std::uint64_t expected = carry;
			std::uint64_t observed = 0;
			switch (reader) {
			case Reader::adc:
			case Reader::twice_adc:
			case Reader::memory_adc:
				observed = cpu.gpr(Register::rdx);
				break;
			case Reader::bt_adc:
				/* Bit 1 of rcx: clear after INC from zero, set after DEC. */
				expected = decrement ? 1U : 0U;
				observed = cpu.gpr(Register::rdx);
				break;
			case Reader::lahf:
				/* SF ZF 0 AF 0 PF 1 CF of the INC or DEC from zero. */
				expected = (decrement ? 0x96U : 0x02U) | carry;
				observed = (cpu.gpr(Register::rax) >> 8U) & 0xffU;
				break;
			case Reader::exit:
				materialize_flags(cpu);
				observed = cpu.rflags & 1U;
				break;
			}
			if (reader == Reader::memory_adc) {
				const auto stored = memory.byte(0x2000);
				require(stored == (decrement ? 0xffU : 0x01U),
					"the memory INC or DEC did not write its result");
			}
			if (observed != expected) {
				char message[200];
				std::snprintf(message, sizeof(message),
					"a lazily preserved carry reached a reader wrong "
					"(width %u %s %s %s reader %d left %#llx right %#llx: "
					"expected %#llx observed %#llx)",
					width, subtract ? "sub" : "add",
					jump ? "jump" : "store", decrement ? "dec" : "inc",
					static_cast<int>(reader),
					static_cast<unsigned long long>(left),
					static_cast<unsigned long long>(right),
					static_cast<unsigned long long>(expected),
					static_cast<unsigned long long>(observed));
				fail(message);
			}
		}
	}
}

/* A record is also hidden from the builder when it no longer exists: every
 * gadget that materializes and republishes the flags leaves the carry settled
 * in RFLAGS and the record empty. STC and CLC define the carry that way, ADCX
 * and ADOX -- the pair OpenSSL's Montgomery multiply interleaves, with a DEC
 * closing every loop between the carry's producer and its consumer -- do it
 * after computing, and so does any consumer that had to materialize. An empty
 * record is not a carry source: parking one in the carry slot makes the slot's
 * operation-and-width table answer a carry of zero, so the settled carry has
 * to survive the INC or DEC untouched instead.
 */
void a_settled_carry_survives_an_increment()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<std::uint64_t, 4> values{
		0U, 1U, 0x8000'0000'0000'0000ULL, ~std::uint64_t{}};
	enum class Producer { set, clear, adcx, adox, materialized };
	static constexpr std::array<Producer, 5> producers{Producer::set,
		Producer::clear, Producer::adcx, Producer::adox,
		Producer::materialized};
	enum class Hide { none, store, jump };
	static constexpr std::array<Hide, 3> hides{Hide::none, Hide::store,
		Hide::jump};
	enum class Reader { adc, lahf, exit, twice_adc };
	static constexpr std::array<Reader, 4> readers{Reader::adc, Reader::lahf,
		Reader::exit, Reader::twice_adc};
	for (const auto producer : producers)
	for (const auto hide : hides)
	for (const auto reader : readers)
	for (const bool decrement : {false, true})
	for (const bool memory_unary : {false, true})
	for (const auto left : values)
	for (const auto right : values) {
		std::vector<std::byte> code;
		const auto push = [&code](std::initializer_list<unsigned> bytes) {
			for (const auto byte : bytes)
				code.push_back(static_cast<std::byte>(byte));
		};
		const auto sum = left + right;
		const std::uint64_t add_carry = sum < left ? 1U : 0U;
		std::uint64_t carry{};
		switch (producer) {
		case Producer::set:
			push({0xf9});
			carry = 1;
			break;
		case Producer::clear:
			push({0xf8});
			carry = 0;
			break;
		case Producer::adcx:
			/* STC; ADCX rax, rbx: a carry in and a carry out. */
			push({0xf9, 0x66, 0x48, 0x0f, 0x38, 0xf6, 0xc3});
			carry = (add_carry != 0 || sum + 1U < sum) ? 1U : 0U;
			break;
		case Producer::adox:
			/* STC; ADOX rax, rbx defines the overflow and preserves the
			 * carry the instruction before it settled. */
			push({0xf9, 0xf3, 0x48, 0x0f, 0x38, 0xf6, 0xc3});
			carry = 1;
			break;
		case Producer::materialized:
			/* ADD rax, rbx; SETP r8b: parity has no host-flag form, so the
			 * consumer materializes and the record goes. */
			push({0x48, 0x01, 0xd8, 0x41, 0x0f, 0x9a, 0xc0});
			carry = add_carry;
			break;
		}
		switch (hide) {
		case Hide::none: break;
		case Hide::store: push({0x48, 0x89, 0x0f}); break;
		case Hide::jump: push({0xeb, 0x00}); break;
		}
		/* INC or DEC of rcx, or of [rdi]; both operands start at zero. */
		const unsigned modrm = memory_unary ?
			(decrement ? 0x0fU : 0x07U) : (decrement ? 0xc9U : 0xc1U);
		push({0x48, 0xff, modrm});
		switch (reader) {
		case Reader::twice_adc:
			push({0x48, 0xff, modrm});
			[[fallthrough]];
		case Reader::adc:
			push({0x48, 0x83, 0xd2, 0x00}); /* adc rdx, 0 */
			break;
		case Reader::lahf:
			push({0x9f});
			break;
		case Reader::exit:
			break;
		}
		push({0x0f, 0x05});
		TestMemory memory;
		memory.fill(0, code);
		CpuState cpu{};
		cpu.gpr(Register::rax) = left;
		cpu.gpr(Register::rbx) = right;
		cpu.gpr(Register::rdi) = 0x2000;
		gadgets::Executor executor;
		executor.prepare(memory, 1, 1, 1);
		run_to_system_call(executor, memory, cpu,
			"settled carry program");
		std::uint64_t expected = carry;
		std::uint64_t observed = 0;
		switch (reader) {
		case Reader::adc:
		case Reader::twice_adc:
			observed = cpu.gpr(Register::rdx);
			break;
		case Reader::lahf:
			/* SF ZF 0 AF 0 PF 1 CF of the INC or DEC from zero. */
			expected = (decrement ? 0x96U : 0x02U) | carry;
			observed = (cpu.gpr(Register::rax) >> 8U) & 0xffU;
			break;
		case Reader::exit:
			materialize_flags(cpu);
			observed = cpu.rflags & 1U;
			break;
		}
		if (observed != expected) {
			char message[220];
			std::snprintf(message, sizeof(message),
				"an increment discarded a settled carry (producer %d hide %d "
				"reader %d %s %s left %#llx right %#llx: expected %#llx "
				"observed %#llx)",
				static_cast<int>(producer), static_cast<int>(hide),
				static_cast<int>(reader), decrement ? "dec" : "inc",
				memory_unary ? "memory" : "register",
				static_cast<unsigned long long>(left),
				static_cast<unsigned long long>(right),
				static_cast<unsigned long long>(expected),
				static_cast<unsigned long long>(observed));
			fail(message);
		}
	}
}

/* A conditional move writes its destination register on either condition: it
 * writes the source or the destination's own value, and a 32-bit write in
 * long mode clears the upper half of the register either way. All three forms
 * owe that write -- the deferred one, the one fused to a producer's host
 * flags, and the memory one.
 */
void conditional_moves_write_the_destination_either_way()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::uint64_t destination = 0xfedc'ba98'7654'3210ULL;
	static constexpr std::uint64_t source = 0x0123'4567'89ab'cdefULL;
	enum class Form { deferred, fused, memory };
	static constexpr std::array<Form, 3> forms{Form::deferred, Form::fused,
		Form::memory};
	for (const unsigned width : {16U, 32U, 64U})
	for (const auto form : forms)
	/* After CMP rcx, rdx with both zero: equal, and not negative. */
	for (const unsigned condition : {0x44U, 0x45U, 0x48U, 0x49U}) {
		const bool take = condition == 0x44U || condition == 0x49U;
		std::vector<std::byte> code;
		const auto push = [&code](std::initializer_list<unsigned> bytes) {
			for (const auto byte : bytes)
				code.push_back(static_cast<std::byte>(byte));
		};
		push({0x48, 0x39, 0xd1});
		/* A NOP resets the host flags the builder tracks, so the same
		 * conditional move keeps the deferred gadget. */
		if (form != Form::fused)
			push({0x90});
		if (width == 16U)
			push({0x66});
		else if (width == 64U)
			push({0x48});
		push({0x0f, condition, form == Form::memory ? 0x07U : 0xc3U});
		push({0x0f, 0x05});
		TestMemory memory;
		memory.fill(0, code);
		for (unsigned index = 0; index < 8U; ++index)
			memory.load(0x2000 + index,
				{static_cast<std::uint8_t>(source >> (index * 8U))});
		CpuState cpu{};
		cpu.gpr(Register::rax) = destination;
		cpu.gpr(Register::rbx) = source;
		cpu.gpr(Register::rdi) = 0x2000;
		const auto block = require_built(
			gadgets::build(memory, 0, ExecutionMode::long64, 4),
			"conditional move program did not build");
		const auto result = gadgets::execute(block.program, cpu, memory, 4);
		require(result.event == uml::cpu::EngineEvent::system_call,
			"conditional move program did not reach SYSCALL");
		const auto chosen = take ? source : destination;
		const auto expected = width == 64U ? chosen :
			width == 32U ? (chosen & 0xffff'ffffULL) :
			(destination & ~0xffffULL) | (chosen & 0xffffULL);
		if (cpu.gpr(Register::rax) != expected) {
			char message[200];
			std::snprintf(message, sizeof(message),
				"a conditional move wrote the wrong destination (width %u "
				"form %d condition %#x: expected %#llx observed %#llx)",
				width, static_cast<int>(form), condition,
				static_cast<unsigned long long>(expected),
				static_cast<unsigned long long>(cpu.gpr(Register::rax)));
			fail(message);
		}
	}
}

/* The equality above holds whether or not the builder actually fuses, so pin
 * which pairs it fuses. A NOP between the producer and the consumer resets the
 * host flags the builder tracks, so the same consumer keeps the deferred
 * gadget; the entry it names differs exactly where fusion is legal. Parity has
 * no host-flag form, and after a logical operation neither do the four
 * conditions that read the carry.
 */
void fused_conditions_cover_exactly_the_legal_pairs()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<unsigned, 5> opcodes{
		0x39, 0x29, 0x85, 0x21, 0x01};
	/* The producer's stream length depends on which bank the builder chose,
	 * so find the consumer where the two streams part: the plain stream holds
	 * the NOP entry there, the fused one the consumer's entry. */
	const auto consumer_index = [](std::span<const std::uintptr_t> fused,
		std::span<const std::uintptr_t> plain) {
		std::size_t index = 0;
		while (index < fused.size() && index < plain.size() &&
			fused[index] == plain[index])
			++index;
		return index;
	};
	for (std::size_t index = 0; index < opcodes.size(); ++index) {
		const bool logical = index == 2 || index == 3;
		for (const unsigned width : {8U, 16U, 32U, 64U}) {
			for (unsigned condition = 0; condition < 16U; ++condition) {
				const bool carry_condition = condition >= 2U && condition <= 3U;
				const bool ordered_carry =
					condition >= 6U && condition <= 7U;
				const bool parity = condition >= 10U && condition <= 11U;
				/* The register-baked bank leaves host flags at every width;
				 * its narrow entries compare the operands shifted to the top
				 * of a 32-bit register. */
				const bool add = index == 4;
				const bool expected_fusion = !parity &&
					!(logical && (carry_condition || ordered_carry)) &&
					!(add && ordered_carry);
				std::vector<std::byte> fused;
				std::vector<std::byte> plain;
				const auto emit = [&](std::vector<std::byte> &code,
					bool barrier) {
					const auto push = [&code](unsigned byte) {
						code.push_back(static_cast<std::byte>(byte));
					};
					if (width == 16U)
						push(0x66);
					if (width == 64U)
						push(0x48);
					push(width == 8U ? opcodes[index] - 1U : opcodes[index]);
					push(0xd8);
					if (barrier)
						push(0x90);
					push(0x0f);
					push(0x90U + condition);
					push(0xc1);
					push(0x0f);
					push(0x05);
				};
				emit(fused, false);
				emit(plain, true);
				TestMemory fused_memory;
				TestMemory plain_memory;
				fused_memory.fill(0, fused);
				plain_memory.fill(0, plain);
				const auto built = gadgets::build(fused_memory, 0);
				const auto separated = gadgets::build(plain_memory, 0);
				require(built.status == gadgets::BuildStatus::built &&
					separated.status == gadgets::BuildStatus::built,
					"condition pairing program did not build");
				const auto fused_stream = built.program.native_stream();
				const auto plain_stream = separated.program.native_stream();
				require(fused_stream.size() + 1U == plain_stream.size() &&
					fused_stream.size() > 3U,
					"condition pairing changed the stream layout");
				const auto consumer = consumer_index(fused_stream, plain_stream);
				require(consumer + 1U < fused_stream.size(),
					"condition pairing streams do not part at the consumer");
				const bool differs =
					fused_stream[consumer] != plain_stream[consumer + 1U];
				require(differs == expected_fusion,
					"the builder fused a condition it should not have, or "
					"declined one it should have fused");
				/* The same pairing, with a conditional jump as the consumer
				 * rather than a SETcc. */
				std::vector<std::byte> fused_jump;
				std::vector<std::byte> plain_jump;
				const auto emit_jump = [&](std::vector<std::byte> &code,
					bool barrier) {
					const auto push = [&code](unsigned byte) {
						code.push_back(static_cast<std::byte>(byte));
					};
					if (width == 16U)
						push(0x66);
					if (width == 64U)
						push(0x48);
					push(width == 8U ? opcodes[index] - 1U : opcodes[index]);
					push(0xd8);
					if (barrier)
						push(0x90);
					push(0x70U + condition);
					push(0x05);
					push(0xba);
					push(0x01);
					push(0x00);
					push(0x00);
					push(0x00);
					push(0x0f);
					push(0x05);
				};
				emit_jump(fused_jump, false);
				emit_jump(plain_jump, true);
				TestMemory fused_jump_memory;
				TestMemory plain_jump_memory;
				fused_jump_memory.fill(0, fused_jump);
				plain_jump_memory.fill(0, plain_jump);
				const auto jump_built = gadgets::build(fused_jump_memory, 0);
				const auto jump_separated =
					gadgets::build(plain_jump_memory, 0);
				require(jump_built.status == gadgets::BuildStatus::built &&
					jump_separated.status == gadgets::BuildStatus::built,
					"jump pairing program did not build");
				const auto jump_fused = jump_built.program.native_stream();
				const auto jump_plain =
					jump_separated.program.native_stream();
				require(jump_fused.size() + 1U == jump_plain.size() &&
					jump_fused.size() > 3U,
					"jump pairing changed the stream layout");
				const auto jump_consumer = consumer_index(jump_fused, jump_plain);
				require(jump_consumer + 1U < jump_fused.size(),
					"jump pairing streams do not part at the consumer");
				require((jump_fused[jump_consumer] !=
					jump_plain[jump_consumer + 1U]) == expected_fusion,
					"the builder fused a conditional jump it should not have, "
					"or declined one it should have fused");
			}
		}
	}
}

/* The SETcc equality pins the condition mapping, which the conditional jump
 * shares. What it does not pin is the jump's own plumbing: a fused jump
 * selects between two stream words with the host condition instead of a
 * materialized bit, and selecting the wrong one would send control to the
 * wrong successor rather than produce a wrong byte. Run the pair for real,
 * across block boundaries, with the two edges leading to different results.
 */
void fused_conditional_jumps_take_the_same_edge()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<unsigned, 4> producers{0x39, 0x29, 0x85, 0x21};
	static constexpr std::array<std::uint64_t, 12> values{
		0U, 1U, 0x7fU, 0x80U, 0xffU, 0x7fffU, 0x8000U, 0xffffU, 0x7fff'ffff'ffff'ffffULL, 0x8000'0000'0000'0000ULL,
		~std::uint64_t{}, 0x5555'5555'5555'5555ULL};
	for (const auto producer : producers) {
		for (const unsigned width : {8U, 16U, 32U, 64U}) {
			for (unsigned condition = 0; condition < 16U; ++condition) {
				for (const auto left : values) {
					for (const auto right : values) {
						std::array<std::uint64_t, 2> taken{};
						for (unsigned barrier = 0; barrier < 2U; ++barrier) {
							std::vector<std::byte> code;
							const auto push = [&code](unsigned byte) {
								code.push_back(
									static_cast<std::byte>(byte));
							};
							if (width == 16U)
								push(0x66);
							if (width == 64U)
								push(0x48);
							push(width == 8U ? producer - 1U : producer);
							push(0xd8);
							if (barrier != 0U)
								push(0x90);
							/* j<cc> over a five-byte MOV. */
							push(0x70U + condition);
							push(0x05);
							push(0xba);
							push(0x01);
							push(0x00);
							push(0x00);
							push(0x00);
							push(0x0f);
							push(0x05);
							TestMemory memory;
							memory.fill(0, code);
							CpuState cpu{};
							cpu.gpr(Register::rax) = left;
							cpu.gpr(Register::rbx) = right;
							gadgets::Executor executor;
							executor.prepare(memory, 1, 1, 1);
							run_to_system_call(executor, memory, cpu,
								"fused jump program");
							taken[barrier] = cpu.gpr(Register::rdx) & 0xffU;
						}
						require(taken[0] == taken[1],
							"a fused conditional jump took a different edge "
							"than the deferred evaluation of the same flags");
					}
				}
			}
		}
	}
}

/* The host reports the emulated CPU to the guest kernel from cpuid.hpp, and
 * the guest reads the same CPU by executing CPUID. Both answers must be the
 * same answer: whatever /proc/cpuinfo says a program may use, CPUID has to
 * agree, or a feature resolver and a capability script reach opposite
 * conclusions about the same machine. Nothing keeps the table and the gadget
 * together except this test, so it walks every leaf either one claims.
 */
/* An exact zero keeps its sign where the SDM gives one: FSIN and FPTAN
 * return a zero operand unchanged, and an FPREM or FPREM1 remainder of zero
 * takes the dividend's sign. */
void x87_zero_results_keep_their_sign()
{
	using namespace uml::cpu::x86_64;
	constexpr X87Register positive_zero{0, 0x0000U};
	constexpr X87Register negative_zero{0, 0x8000U};
	constexpr X87Register three{0xc000000000000000ULL, 0x4000U};
	constexpr X87Register five{0xa000000000000000ULL, 0x4001U};
	constexpr X87Register negative_five{0xa000000000000000ULL, 0xc001U};
	constexpr X87Register six{0xc000000000000000ULL, 0x4001U};
	constexpr X87Register negative_six{0xc000000000000000ULL, 0xc001U};
	const auto same = [](const X87Register &left, const X87Register &right) {
		return left.significand == right.significand &&
			left.sign_exponent == right.sign_exponent;
	};
	CpuState cpu{};
	X87Register result{};
	X87Register cosine{};
	for (const auto &zero : {positive_zero, negative_zero}) {
		gadgets::uml_x86_64_x87_sine(&cpu, &zero, &result);
		require(same(result, zero), "FSIN changed a zero's sign");
		gadgets::uml_x86_64_x87_sincos(&cpu, &zero, &result, &cosine);
		require(same(result, zero),
			"FSINCOS changed a zero sine's sign");
		X87Register second{};
		gadgets::uml_x86_64_x87_tangent(&cpu, &zero, &result, &second);
		require(same(result, zero), "FPTAN changed a zero's sign");
	}
	struct Remainder final {
		X87Register dividend;
		X87Register divisor;
		X87Register expected;
	};
	for (const auto &[dividend, divisor, expected] : {
		Remainder{six, three, positive_zero},
		Remainder{negative_six, three, negative_zero},
		Remainder{five, five, positive_zero},
		Remainder{negative_five, five, negative_zero},
	}) {
		gadgets::uml_x86_64_x87_fprem(&cpu, &dividend, &divisor, &result);
		require(same(result, expected),
			"FPREM gave a zero remainder the wrong sign");
		gadgets::uml_x86_64_x87_fprem1(&cpu, &dividend, &divisor,
			&result);
		require(same(result, expected),
			"FPREM1 gave a zero remainder the wrong sign");
	}
	require((cpu.x87_status & 0x7fU) == 0,
		"an exact zero result raised an x87 exception");
}

void cpuid_table_matches_the_gadget()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0xa2}); // cpuid
	const auto built = require_built(
		gadgets::build(memory, 0, ExecutionMode::long64, 1),
		"CPUID did not build");

	/* Every leaf the table answers, the leaf above each maximum, and a
	 * subleaf of each enumerating leaf that must read as zero. */
	static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 16>
		probed{{
			{0U, 0U}, {1U, 0U}, {7U, 0U}, {7U, 1U}, {7U, 2U},
			{13U, 0U}, {13U, 1U}, {13U, 2U}, {13U, 3U},
			{2U, 0U}, {14U, 0U},
			{0x8000'0000U, 0U}, {0x8000'0001U, 0U},
			{0x8000'0002U, 0U}, {0x8000'0003U, 0U},
			{0x8000'0004U, 0U},
		}};
	for (const auto &[leaf, subleaf] : probed) {
		CpuState cpu{};
		cpu.gpr(Register::rax) = leaf;
		cpu.gpr(Register::rcx) = subleaf;
		const auto result =
			gadgets::execute(built.program, cpu, memory, 1);
		const auto table = cpuid(leaf, subleaf);
		char message[128];
		std::snprintf(message, sizeof message,
			"CPUID leaf 0x%08x subleaf %u disagrees with cpuid.hpp",
			leaf, subleaf);
		require(result.event == uml::cpu::EngineEvent::continued &&
				static_cast<std::uint32_t>(
					cpu.gpr(Register::rax)) == table.eax &&
				static_cast<std::uint32_t>(
					cpu.gpr(Register::rbx)) == table.ebx &&
				static_cast<std::uint32_t>(
					cpu.gpr(Register::rcx)) == table.ecx &&
				static_cast<std::uint32_t>(
					cpu.gpr(Register::rdx)) == table.edx,
			message);
	}

	/* The brand string is what a guest prints as the model name, so it is
	 * read the way a guest reads it: little-endian bytes, EAX through EDX,
	 * across the three brand leaves. */
	char brand[49] = {};
	std::size_t at = 0;
	for (std::uint32_t leaf = 0x8000'0002U; leaf <= 0x8000'0004U; ++leaf) {
		const auto part = cpuid(leaf, 0U);
		for (const auto word : {part.eax, part.ebx, part.ecx, part.edx})
			for (unsigned byte = 0; byte < 4U; ++byte)
				brand[at++] = static_cast<char>(
					(word >> (8U * byte)) & 0xffU);
	}
	require(std::string_view(brand) == uml::cpu::guest_cpu_model_name,
		"the CPUID brand string is not this virtual CPU's own name");
}

void preserve_precise_memory_fault()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x48, 0x8b, 0x03});
	CpuState cpu{};
	cpu.gpr(Register::rbx) = 16383;
	const auto built = require_built(gadgets::build(memory, 0, 1),
		"faulting load did not build");
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	require(result.event == uml::cpu::EngineEvent::memory_fault,
		"faulting load did not exit as a memory fault");
	require(result.fault.address == 16383 &&
		result.fault.access == uml::memory::MemoryAccess::read,
		"faulting load lost precise metadata");
	require(result.retired_instructions == 0 && cpu.rip == 0,
		"faulting load retired or advanced RIP");
}

void retire_valid_prefix_before_decode_fault()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x0b,
	});
	CpuState cpu{};
	const auto prefix = require_built(gadgets::build(memory, 0),
		"valid prefix before UD2 did not build");
	const auto prefix_result = gadgets::execute(
		prefix.program, cpu, memory, 64);
	require(prefix_result.event == uml::cpu::EngineEvent::continued &&
		prefix_result.retired_instructions == 1 && cpu.rip == 5 &&
		cpu.gpr(Register::rax) == 42,
		"valid prefix before UD2 did not retire first");
	const auto illegal = gadgets::build(memory, cpu.rip);
	require(illegal.status == gadgets::BuildStatus::illegal_instruction,
		"UD2 boundary was not reported after the valid prefix");
}

void migrated_gadgets_are_native_only()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x90,
		0x48, 0x0f, 0xc8,
		0xf8,
		0x9f,
		0x48, 0xd1, 0xe0,
		0x48, 0x0f, 0xa4, 0xc8, 0x01,
		0x48, 0x0f, 0xaf, 0xc1,
		0x48, 0xf7, 0xe1,
		0x48, 0xf7, 0xf1,
		0x48, 0xf7, 0xf9,
		0x48, 0x0f, 0xa3, 0xc8,
		0x48, 0x0f, 0xab, 0xc8,
		0x48, 0x0f, 0xb3, 0xc8,
		0x48, 0x0f, 0xbb, 0xc8,
		0x48, 0x0f, 0xbc, 0xc1,
		0x48, 0x0f, 0xbd, 0xc1,
		0xf3, 0x48, 0x0f, 0xbc, 0xc1,
		0xf3, 0x48, 0x0f, 0xbd, 0xc1,
		0xf3, 0x48, 0x0f, 0xb8, 0xc1,
		0x48, 0x0f, 0xb1, 0xc8,
		0x48, 0x0f, 0xc7, 0x0b,
		0x48, 0x0f, 0xc1, 0xc8,
		0x48, 0x0f, 0x38, 0xf0, 0x03,
		0xf2, 0x48, 0x0f, 0x38, 0xf1, 0xc1,
		0x66, 0x48, 0x0f, 0x38, 0xf6, 0xc1,
		0xf3, 0x48, 0x0f, 0x38, 0xf6, 0xc1,
		0x0f, 0xae, 0x03,
		0x0f, 0xae, 0x23,
		0xeb, 0x00,
	});
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x0102'0304'0506'0708ULL;
	const auto built = gadgets::build(memory, 0);
	require(built.status == gadgets::BuildStatus::built,
		"native-only block did not build");
	require(built.program.instruction_count() == 29,
		"native-only block has the wrong instruction count");
	require(!built.program.native_stream().empty(),
		"native-only block has no threaded assembly stream");
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	require(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 2 &&
		result.instruction_budget_exhausted,
		"limited native stream did not exhaust its budget");
	require(cpu.rip == 4 &&
		cpu.gpr(Register::rax) == 0x0807'0605'0403'0201ULL,
		"limited native stream produced the wrong state");
}

void native_divide_traps_precisely()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(100, {0x48, 0xf7, 0xf1});
	CpuState cpu{};
	cpu.rip = 100;
	cpu.gpr(Register::rax) = 42;
	cpu.gpr(Register::rdx) = 0;
	cpu.gpr(Register::rcx) = 0;
	const auto built = require_built(gadgets::build(memory, 100, 1),
		"native DIV trap block did not build");
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	require(result.event == uml::cpu::EngineEvent::floating_point_trap,
		"divide by zero did not return the x86 divide trap");
	require(result.retired_instructions == 0 && cpu.rip == 100,
		"divide trap retired or lost its precise RIP");
	require(cpu.gpr(Register::rax) == 42 &&
		cpu.gpr(Register::rdx) == 0,
		"divide trap modified the dividend registers");
}

void native_into_reports_overflow_precisely()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(100, {0xce});
	const auto built = require_built(
		gadgets::build( memory, 100, ExecutionMode::compat32, 1),
		"native INTO block did not build");
	CpuState taken{};
	taken.execution_mode = ExecutionMode::compat32;
	taken.rip = 100;
	taken.rflags |= rflags_overflow;
	const auto taken_result = gadgets::execute(
		built.program, taken, memory, 1);
	require(taken_result.event == uml::cpu::EngineEvent::overflow,
		"taken INTO did not return the overflow trap");
	require(taken_result.retired_instructions == 1 && taken.rip == 101,
		"taken INTO did not preserve trap retirement semantics");
	CpuState clear{};
	clear.execution_mode = ExecutionMode::compat32;
	clear.rip = 100;
	const auto clear_result = gadgets::execute(
		built.program, clear, memory, 1);
	require(clear_result.event == uml::cpu::EngineEvent::continued &&
		clear_result.instruction_budget_exhausted &&
		clear_result.retired_instructions == 1 && clear.rip == 101,
		"clear-overflow INTO did not retire normally");
}

/* ICEBP (INT1) raises #DB as a trap in either mode: it retires, and RIP
 * names the next instruction (audit NC-E10). */
void icebp_raises_a_debug_trap_after_itself()
{
	using namespace uml::cpu::x86_64;
	for (const auto mode : {ExecutionMode::long64, ExecutionMode::compat32}) {
		TestMemory memory;
		memory.load(100, {0xf1});
		const auto built = require_built(
			gadgets::build(memory, 100, mode, 1),
			"ICEBP did not build");
		CpuState cpu{};
		cpu.execution_mode = mode;
		cpu.rip = 100;
		const auto result = gadgets::execute(built.program, cpu, memory, 1);
		require(result.event == uml::cpu::EngineEvent::debug_trap &&
			result.retired_instructions == 1 && cpu.rip == 101,
			"ICEBP did not raise #DB after itself");
	}
}

/* BOUND compares the signed index in a register with the pair of bounds at
 * its memory operand. Within them, or at either one, it retires; outside
 * them it raises #BR without retiring. Only the operand size's bits of the
 * register count. A register operand makes the byte an EVEX prefix, and 64-bit
 * mode has no BOUND (audit NC-E10). */
void bound_checks_a_signed_range()
{
	using namespace uml::cpu::x86_64;
	struct Case {
		std::uint64_t address;
		std::uint64_t index;
		bool inside;
	};
	/* bound ebx, [0x200]; bound ebx, [0xffc]; bound bx, [0x300]. */
	const Case cases[] = {
		{0x100, 0, true},
		{0x100, 0xffff'fff6U, true},
		{0x100, 20, true},
		{0x100, 0xffff'fff5U, false},
		{0x100, 21, false},
		{0x100, 0x8000'0000U, false},
		{0x110, 20, true},
		{0x110, 21, false},
		{0x120, 5, true},
		{0x120, 0xfffb, true},
		{0x120, 0x1234'0003U, true},
		{0x120, 6, false},
		{0x120, 0xfffa, false},
	};
	TestMemory memory;
	memory.load(0x100, {0x62, 0x1d, 0x00, 0x02, 0x00, 0x00});
	memory.load(0x110, {0x62, 0x1d, 0xfc, 0x0f, 0x00, 0x00});
	memory.load(0x120, {0x66, 0x62, 0x1d, 0x00, 0x03, 0x00, 0x00});
	memory.load(0x200, {0xf6, 0xff, 0xff, 0xff, 20, 0, 0, 0});
	memory.load(0xffc, {0xf6, 0xff, 0xff, 0xff, 20, 0, 0, 0});
	memory.load(0x300, {0xfb, 0xff, 5, 0});
	for (const auto &shape : cases) {
		const auto built = require_built(
			gadgets::build(memory, shape.address,
				ExecutionMode::compat32, 1),
			"BOUND did not build");
		CpuState cpu{};
		cpu.execution_mode = ExecutionMode::compat32;
		cpu.rip = shape.address;
		cpu.gpr(Register::rbx) = shape.index;
		const auto result = gadgets::execute(built.program, cpu, memory, 1);
		const auto length = shape.address == 0x120 ? 7U : 6U;
		const bool ok = shape.inside ?
			result.event == uml::cpu::EngineEvent::continued &&
				result.retired_instructions == 1 &&
				cpu.rip == shape.address + length :
			result.event == uml::cpu::EngineEvent::bound_range &&
				result.retired_instructions == 0 &&
				cpu.rip == shape.address;
		require(ok && cpu.gpr(Register::rbx) == shape.index,
			shape.inside ? "BOUND within its bounds did not retire" :
				"BOUND outside its bounds did not raise #BR");
	}

	TestMemory unmapped;
	unmapped.load(0x100, {0x62, 0x1d, 0x00, 0x00, 0x01, 0x00});
	const auto faulting = require_built(
		gadgets::build( unmapped, 0x100, ExecutionMode::compat32, 1),
		"BOUND with unmapped bounds did not build");
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.rip = 0x100;
	const auto fault = gadgets::execute(faulting.program, cpu, unmapped, 1);
	require(fault.event == uml::cpu::EngineEvent::memory_fault &&
		fault.retired_instructions == 0 && cpu.rip == 0x100 &&
		fault.fault.address == 0x10000,
		"BOUND did not fault precisely on unmapped bounds");

	TestMemory other;
	other.load(0x100, {0x62, 0xc3});
	other.load(0x200, {0x62, 0x1d, 0x00, 0x02, 0x00, 0x00});
	require(gadgets::build(other, 0x100, ExecutionMode::compat32, 1)
			.status == gadgets::BuildStatus::illegal_instruction,
		"BOUND with a register operand built");
	require(gadgets::build(other, 0x200, ExecutionMode::long64, 1)
		.status == gadgets::BuildStatus::illegal_instruction,
	"BOUND built in 64-bit mode");
}

/* User mode runs with IOPL 0 and CR4.TSD under PR_SET_TSC: these raise #GP,
 * which Linux delivers as SIGSEGV, rather than #UD. The faulting instruction
 * does not retire. */
void privileged_instructions_raise_general_protection()
{
	using namespace uml::cpu::x86_64;
	const std::vector<std::vector<std::uint8_t>> encodings{
		{0xed}, {0xec}, {0xe6, 0x80}, {0xe5, 0x70}, {0xef},
		{0x6c}, {0xf3, 0x6f}, {0xf4}, {0xfa}, {0xfb},
		{0xcd, 0x21}, {0x0f, 0x30}, {0x0f, 0x32}, {0x0f, 0x33},
		{0x0f, 0x09}, {0x0f, 0x22, 0xd8},
	};
	for (const auto &encoding : encodings) {
		TestMemory memory;
		for (std::size_t index = 0; index < encoding.size(); ++index)
			memory.load(100 + index, {encoding[index]});
		const auto built = require_built(gadgets::build(memory, 100, 1),
			"privileged instruction did not build");
		CpuState cpu{};
		cpu.rip = 100;
		const auto result = gadgets::execute(built.program, cpu, memory, 1);
		require(result.event ==
				uml::cpu::EngineEvent::general_protection,
			"privileged instruction did not raise #GP");
		require(result.retired_instructions == 0 && cpu.rip == 100,
			"#GP retired or lost its precise RIP");
	}

	TestMemory timestamp_memory;
	timestamp_memory.load(100, {0x0f, 0x31});
	const auto timestamp = require_built(
		gadgets::build(timestamp_memory, 100, 1),
		"RDTSC did not build");
	CpuState enabled{};
	enabled.rip = 100;
	const auto enabled_result = gadgets::execute(
		timestamp.program, enabled, timestamp_memory, 1);
	CpuState disabled{};
	disabled.rip = 100;
	disabled.timestamp_faults = true;
	const auto disabled_result = gadgets::execute(
		timestamp.program, disabled, timestamp_memory, 1);
	require(enabled_result.event == uml::cpu::EngineEvent::continued &&
		enabled.rip == 102, "enabled RDTSC did not retire");
	require(disabled_result.event ==
		uml::cpu::EngineEvent::general_protection &&
		disabled_result.retired_instructions == 0 &&
		disabled.rip == 100,
		"RDTSC under PR_TSC_SIGSEGV did not raise #GP");
}

/* An instruction that would need a sixteenth byte raises #GP at its first
 * byte without fetching that byte.  It used to be reported as a fetch fault
 * at the sixteenth byte, which a mapped page resolves as spurious, so the
 * guest re-ran the same bytes forever.  A fetch that fails before the limit
 * stays a page fault at the byte it wanted. */
void overlong_instructions_raise_general_protection()
{
	using namespace uml::cpu::x86_64;
	constexpr std::size_t span = 16384;
	TestMemory memory;
	memory.load(0x100, {0x90});
	for (std::size_t index = 0; index < 15; ++index) {
		memory.load(0x101 + index, {0x66});
		memory.load(0x200 + index, {0x66});
	}
	memory.load(0x110, {0x90});
	memory.load(0x20f, {0x90});
	for (std::size_t index = 0; index < 14; ++index)
		memory.load(span - 14 + index, {0x66});

	const auto legal = require_built(gadgets::build(memory, 0x201, 1),
		"a fifteen-byte instruction did not build");
	CpuState fits{};
	fits.rip = 0x201;
	const auto fitted = gadgets::execute(legal.program, fits, memory, 1);
	require(fitted.event == uml::cpu::EngineEvent::continued &&
		fitted.retired_instructions == 1 && fits.rip == 0x210,
		"a fifteen-byte instruction did not retire");

	for (const auto start : {0x100U, 0x101U}) {
		const auto built = gadgets::build(memory, start, 4);
		require(built.status == gadgets::BuildStatus::built,
			"an overlong instruction did not build to #GP");
		require(built.program.replay_instructions().size() ==
			built.program.instruction_count(),
			"an overlong instruction did not replay from its source");
		CpuState cpu{};
		cpu.rip = start;
		const auto result = gadgets::execute(built.program, cpu, memory, 4);
		const auto retired = start == 0x100U ? 1U : 0U;
		require(result.event ==
				uml::cpu::EngineEvent::general_protection &&
			result.retired_instructions == retired && cpu.rip == 0x101,
			"an overlong instruction did not raise #GP at its first byte");
	}

	const auto short_fetch = gadgets::build(memory, span - 14, 1);
	require(short_fetch.status == gadgets::BuildStatus::fetch_fault &&
		short_fetch.fault_address == span,
		"a fetch fault inside the length limit was not a page fault");
}

/* RDRAND and RDSEED return host CSPRNG output with CF set.  They used to
 * return one xorshift round of the counter XORed with the CpuState address,
 * which a guest could invert and XOR with RDTSC to learn a host pointer. */
void random_instructions_expose_no_host_state()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x0f, 0x31,             // rdtsc
		0x48, 0x89, 0xc7,       // mov rdi, rax
		0x48, 0x89, 0xd6,       // mov rsi, rdx
		0x48, 0x0f, 0xc7, 0xf0, // rdrand rax
		0x48, 0x0f, 0xc7, 0xfb, // rdseed rbx
		0x0f, 0x05,             // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"RDRAND/RDSEED block did not build");
	/* The inverse of x ^= x << 13; x ^= x >> 7; x ^= x << 17. */
	const auto invert = [](std::uint64_t value) {
		value ^= value << 17;
		value ^= value << 34;
		value ^= value >> 7;
		value ^= value >> 14;
		value ^= value >> 28;
		value ^= value >> 56;
		value ^= value << 13;
		value ^= value << 26;
		value ^= value << 52;
		return value;
	};
	std::uint64_t first{};
	bool varied = false;
	for (int round = 0; round < 16; ++round) {
		CpuState cpu{};
		const auto result = gadgets::execute(built.program, cpu, memory, 8);
		require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 6,
			"RDRAND/RDSEED block did not reach SYSCALL");
		require((cpu.rflags & 0x8d5U) == 1U,
			"RDSEED did not report success with only CF set");
		const auto counter = (cpu.gpr(Register::rsi) << 32U) |
			(cpu.gpr(Register::rdi) & 0xffff'ffffULL);
		const auto host = reinterpret_cast<std::uintptr_t>(&cpu);
		for (const auto value :
			{cpu.gpr(Register::rax), cpu.gpr(Register::rbx)}) {
			const auto recovered = invert(value) ^ counter;
			require((recovered >> 12U) != (host >> 12U),
				"RDRAND/RDSEED output revealed the CpuState address");
		}
		if (round == 0)
			first = cpu.gpr(Register::rax);
		else if (cpu.gpr(Register::rax) != first)
			varied = true;
	}
	require(varied, "RDRAND returned one value sixteen times");
}

/* A guest vCPU has no processor numbering, so RDPID, RDTSCP's ECX and the
 * limit of the per-CPU segment LSL reads all name processor 0 of node 0. */
void processor_numbering_is_cpu_zero_node_zero()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t zero_flag = 1ULL << 6U;
	/* RDTSCP overwrites EAX and EDX, so RDPID reads into another register. */
	TestMemory long64;
	long64.load(0, {
		0x49, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, // mov r8, -1
		0xf3, 0x49, 0x0f, 0xc7, 0xf8,             // rdpid r8
		0x48, 0xc7, 0xc1, 0xff, 0xff, 0xff, 0xff, // mov rcx, -1
		0x0f, 0x01, 0xf9,                         // rdtscp
		0xbf, 0x7b, 0x00, 0x00, 0x00,             // mov edi, 0x7b
		0x48, 0xc7, 0xc6, 0xff, 0xff, 0xff, 0xff, // mov rsi, -1
		0x0f, 0x03, 0xf7,                         // lsl esi, di
	});
	TestMemory compat32;
	compat32.load(0, {
		0xbb, 0xff, 0xff, 0xff, 0xff,             // mov ebx, -1
		0xf3, 0x0f, 0xc7, 0xfb,                   // rdpid ebx
		0xb9, 0xff, 0xff, 0xff, 0xff,             // mov ecx, -1
		0x0f, 0x01, 0xf9,                         // rdtscp
		0xbf, 0x7b, 0x00, 0x00, 0x00,             // mov edi, 0x7b
		0xbe, 0xff, 0xff, 0xff, 0xff,             // mov esi, -1
		0x0f, 0x03, 0xf7,                         // lsl esi, di
	});
	struct Case final {
		ExecutionMode mode;
		TestMemory *memory;
		Register processor;
	};
	const Case cases[] = {
		{ExecutionMode::long64, &long64, Register::r8},
		{ExecutionMode::compat32, &compat32, Register::rbx},
	};
	for (const auto &[mode, memory, processor] : cases) {
		const auto built = require_built(
			gadgets::build(*memory, 0, mode, 7),
			"processor numbering block did not build");
		CpuState cpu{};
		cpu.execution_mode = mode;
		const auto result = gadgets::execute(built.program, cpu, *memory, 7);
		require(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 7,
			"processor numbering block did not retire");
		require(cpu.gpr(processor) == 0,
			"RDPID did not report processor 0");
		require(cpu.gpr(Register::rcx) == 0,
			"RDTSCP did not report processor 0 in ECX");
		require(cpu.gpr(Register::rsi) == 0 &&
			(cpu.rflags & zero_flag) != 0,
			"LSL of the per-CPU segment did not read processor 0");
	}
}

/* WRFSBASE and WRGSBASE with a 64-bit operand raise #GP for a non-canonical
 * address and leave the base unchanged; the 32-bit form zero-extends and so
 * cannot fault. */
void segment_base_writes_reject_non_canonical_addresses()
{
	using namespace uml::cpu::x86_64;
	struct Case final {
		std::uint8_t modrm;
		bool wide;
		std::uint64_t value;
		bool faults;
	};
	constexpr std::uint64_t original = 0x1234'5000ULL;
	const std::array cases{
		Case{0xd0, true, 0x0000'7fff'ffff'f000ULL, false},
		Case{0xd8, true, 0xffff'8000'0000'0000ULL, false},
		Case{0xd0, true, 0x0000'8000'0000'0000ULL, true},
		Case{0xd8, true, 0x0000'8000'0000'0000ULL, true},
		Case{0xd0, true, 0x8000'0000'0000'1000ULL, true},
		Case{0xd8, true, 0xfff7'ffff'ffff'0000ULL, true},
		Case{0xd0, false, 0xffff'ffff'ffff'ffffULL, false},
		Case{0xd8, false, 0x8000'0000'8000'0000ULL, false},
	};
	for (const auto &shape : cases) {
		TestMemory memory;
		if (shape.wide)
			memory.load(100, {0xf3, 0x48, 0x0f, 0xae, shape.modrm});
		else
			memory.load(100, {0xf3, 0x0f, 0xae, shape.modrm});
		const auto built = require_built(gadgets::build(memory, 100, 1),
			"WRFSBASE/WRGSBASE did not build");
		const bool fs = shape.modrm == 0xd0;
		CpuState cpu{};
		cpu.rip = 100;
		cpu.fs_base = original;
		cpu.gs_base = original;
		cpu.gpr(Register::rax) = shape.value;
		const auto result = gadgets::execute(built.program, cpu, memory, 1);
		const auto base = fs ? cpu.fs_base : cpu.gs_base;
		const auto other = fs ? cpu.gs_base : cpu.fs_base;
		if (shape.faults) {
			require(result.event ==
					uml::cpu::EngineEvent::general_protection &&
				result.retired_instructions == 0 && cpu.rip == 100,
				"non-canonical segment base write did not raise #GP");
			require(base == original && other == original,
				"faulting segment base write changed a base");
			continue;
		}
		const auto expected = shape.wide ? shape.value :
			(shape.value & 0xffff'ffffULL);
		require(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1 &&
			base == expected && other == original,
			"canonical segment base write did not retire");
	}
}

/* RFLAGS.TF single-steps: the runtime runs each entry with a budget of one.
 * Once blocks are cached and linked, a one-instruction budget must still
 * stop after exactly one instruction, including across a taken branch. */
void one_instruction_budget_steps_linked_programs()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x48, 0xff, 0xc0,       // inc rax
		0xeb, 0x02,             // jmp +2
		0x90, 0x90,             // nop; nop (skipped)
		0x48, 0xff, 0xc0,       // inc rax
		0x0f, 0x05,             // syscall
	});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto run = [&](CpuState &cpu, std::size_t budget) {
		const auto selected = executor.select_program(
			memory, cpu.rip, ExecutionMode::long64,
			std::min<std::size_t>(64, budget));
		if (selected.status != gadgets::BuildStatus::built ||
			selected.program == nullptr)
			return gadgets::ExecuteResult{};
		return gadgets::execute(*selected.program, cpu, memory, executor,
			budget);
	};
	for (std::size_t entry = 0; entry < 3; ++entry) {
		CpuState warm{};
		auto result = run(warm, 64);
		for (std::size_t block = 0; block < 8 &&
			result.event == uml::cpu::EngineEvent::continued; ++block)
			result = run(warm, 64);
		require(result.event == uml::cpu::EngineEvent::system_call &&
				warm.gpr(Register::rax) == 2,
			"warm-up run did not reach the system call");
	}

	CpuState cpu{};
	constexpr std::array<std::uint64_t, 3> expected_rip{3, 7, 10};
	for (std::size_t index = 0; index < expected_rip.size(); ++index) {
		const auto result = run(cpu, 1);
		require(result.instruction_budget_exhausted &&
				result.retired_instructions == 1 &&
				cpu.rip == expected_rip[index],
			"a one-instruction budget did not stop after one instruction");
	}
	const auto last = run(cpu, 1);
	require(last.event == uml::cpu::EngineEvent::system_call &&
		cpu.gpr(Register::rax) == 2 && cpu.rip == 12,
		"the stepped system call did not report its own exit");
}

/* POPF that sets TF arms #DB after the next instruction, so the program
 * leaves at that instruction for the runtime to run it alone. A POPF that
 * leaves TF clear, or finds it already set, runs on. */
void popf_setting_trap_flag_leaves_at_the_next_instruction()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x68, 0x02, 0x01, 0x00, 0x00, // push 0x102 (TF)
		0x9d,                         // popfq
		0x48, 0xff, 0xc0,             // inc rax
		0x68, 0x02, 0x00, 0x00, 0x00, // push 0x2
		0x9d,                         // popfq
		0x48, 0xff, 0xc0,             // inc rax
		0x0f, 0x05,                   // syscall
	});
	const auto built = require_built(
		gadgets::build(memory, 0, ExecutionMode::long64),
		"POPF block did not build");
	CpuState cpu{};
	cpu.gpr(Register::rsp) = 0x200;
	cpu.rflags = 0x2;
	const auto armed = gadgets::execute(built.program, cpu, memory, 64);
	require(armed.event == uml::cpu::EngineEvent::trap_flag &&
			armed.retired_instructions == 2 && cpu.rip == 6 &&
			(cpu.rflags & rflags_trap) != 0 &&
			cpu.gpr(Register::rax) == 0 &&
			cpu.gpr(Register::rsp) == 0x200,
		"POPF setting TF did not leave at the next instruction");

	/* Already set: no transition, so the program runs to the system call. */
	cpu = CpuState{};
	cpu.gpr(Register::rsp) = 0x200;
	cpu.rflags = 0x2 | rflags_trap;
	const auto held = gadgets::execute(built.program, cpu, memory, 64);
	require(held.event == uml::cpu::EngineEvent::system_call &&
			held.retired_instructions == 7 &&
			cpu.gpr(Register::rax) == 2 && (cpu.rflags & rflags_trap) == 0,
		"POPF with TF already set did not run on");
}

/* IRET that sets TF arms #DB after the instruction it returns to, as POPF
 * does, so it leaves with the trap-flag event and RIP at that instruction for
 * the runtime to run it alone. One that leaves TF clear, clears it, or finds
 * it already set leaves as before, at every operand size (audit NC-E7). */
void iret_setting_trap_flag_arms_the_next_instruction()
{
	using namespace uml::cpu::x86_64;
	constexpr auto armed = uml::cpu::EngineEvent::trap_flag;
	constexpr auto continued = uml::cpu::EngineEvent::continued;
	struct Case final {
		ExecutionMode mode;
		std::array<std::uint8_t, 2> bytes;
		std::size_t slot;
		std::uint64_t current;
		std::uint64_t popped;
		uml::cpu::EngineEvent event;
		const char *message;
	};
	const std::array cases{
		Case{ExecutionMode::long64, {0x48, 0xcf}, 8, 0x202, 0x302, armed,
			"IRETQ setting TF did not arm the next instruction"},
		Case{ExecutionMode::long64, {0x48, 0xcf}, 8, 0x202, 0x10302,
			armed, "IRETQ setting TF and RF did not arm the next "
			"instruction"},
		Case{ExecutionMode::long64, {0x48, 0xcf}, 8, 0x202, 0x202,
			continued, "IRETQ leaving TF clear armed a trap"},
		Case{ExecutionMode::long64, {0x48, 0xcf}, 8, 0x302, 0x302,
			continued, "IRETQ finding TF set armed it again"},
		Case{ExecutionMode::long64, {0x48, 0xcf}, 8, 0x302, 0x202,
			continued, "IRETQ clearing TF armed a trap"},
		Case{ExecutionMode::long64, {0xcf, 0x90}, 4, 0x202, 0x302, armed,
			"64-bit IRETD setting TF did not arm the next instruction"},
		Case{ExecutionMode::long64, {0x66, 0xcf}, 2, 0x202, 0x302, armed,
			"64-bit 16-bit IRET setting TF did not arm the next "
			"instruction"},
		Case{ExecutionMode::compat32, {0xcf, 0x00}, 4, 0x202, 0x302, armed,
			"compat32 IRETD setting TF did not arm the next instruction"},
		Case{ExecutionMode::compat32, {0x66, 0xcf}, 2, 0x202, 0x302, armed,
			"compat32 16-bit IRET setting TF did not arm the next "
			"instruction"},
	};
	constexpr std::uint64_t stack = 0x200;
	constexpr std::uint64_t target = 0x40;
	for (const auto &shape : cases) {
		TestMemory memory;
		memory.load(0, {shape.bytes[0], shape.bytes[1]});
		memory.load(target, {0x90}); // nop
		const std::array<std::uint64_t, 5> frame{
			target, 0x23, shape.popped, 0x900, 0x2b};
		for (std::size_t index = 0; index < frame.size(); ++index) {
			std::array<std::byte, 8> word{};
			std::memcpy(word.data(), &frame[index], word.size());
			if (!memory.write(stack + index * shape.slot,
				std::span{word.data(), shape.slot}))
				fail("IRET frame setup failed");
		}
		const auto built = require_built(
			gadgets::build(memory, 0, shape.mode, 1),
			"IRET did not build");
		CpuState cpu{};
		cpu.execution_mode = shape.mode;
		cpu.gpr(Register::rsp) = stack;
		cpu.segment_selectors[1] = 0x23;
		cpu.segment_selectors[2] = 0x2b;
		cpu.rflags = shape.current;
		const auto result = gadgets::execute(built.program, cpu, memory, 64);
		require(result.event == shape.event &&
				result.retired_instructions == 1 &&
				cpu.rip == target &&
				(cpu.rflags & rflags_trap) ==
					(shape.popped & rflags_trap),
				shape.message);
	}
}

/* Besides IRET, whose case is in the corpus, RF reaches the engine in the
 * flags the kernel hands over: a signal return or a tracer may set it. It
 * lasts until the next instruction starts, so neither SYSCALL's R11 nor the
 * flags a system-call exit gives the kernel for pt_regs and the signal frame
 * shows it. An exit that runs no instruction keeps it, as an interrupt taken
 * before the instruction does (audit NC-E4). */
void resume_flag_lasts_until_an_instruction_starts()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t cleared = rflags_interrupt | rflags_reserved;
	constexpr std::uint64_t resumed = cleared | rflags_resume;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05});  // syscall
	memory.load(16, {0xcd, 0x80}); // int 0x80
	const auto call = gadgets::build(memory, 0, ExecutionMode::long64, 1);
	const auto compat_call =
		gadgets::build(memory, 16, ExecutionMode::compat32, 1);
	require(call.status == gadgets::BuildStatus::built &&
			compat_call.status == gadgets::BuildStatus::built,
		"SYSCALL or INT 80h did not build");

	CpuState cpu{};
	cpu.rflags = resumed;
	unsigned int sequence = 1;
	const uml::cpu::InterruptToken interrupt{
		.sequence = &sequence,
		.snapshot = 0,
	};
	const auto interrupted = gadgets::execute(
		call.program, cpu, memory, 1, interrupt);
	require(interrupted.event == uml::cpu::EngineEvent::interrupt &&
			interrupted.retired_instructions == 0 &&
			cpu.rflags == resumed,
		"an exit that ran no instruction cleared RF");
	const auto called = gadgets::execute(call.program, cpu, memory, 1);
	require(called.event == uml::cpu::EngineEvent::system_call &&
			cpu.gpr(Register::r11) == cleared && cpu.rflags == cleared,
		"SYSCALL showed the RF its flags started with");

	CpuState compat{};
	compat.execution_mode = ExecutionMode::compat32;
	compat.rip = 16;
	compat.segment_selectors[1] = 0x23;
	compat.segment_selectors[2] = 0x2b;
	compat.rflags = resumed;
	const auto compat_called =
		gadgets::execute(compat_call.program, compat, memory, 1);
	require(compat_called.event == uml::cpu::EngineEvent::system_call &&
			compat.rflags == cleared,
		"INT 80h showed the RF its flags started with");
}

/* INT n names its vector for the #GP error code, and FWAIT its opcode for
 * the #MF check. */
void decode_names_int_vectors_and_fwait()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xcd, 0x41, 0x9b});
	const auto interrupt = decode(memory, 0);
	const auto wait = decode(memory, 2);
	require(interrupt.status == DecodeStatus::decoded &&
			interrupt.instruction.operation == Operation::privileged &&
			interrupt.instruction.vector_opcode == 0xcd &&
			interrupt.instruction.source.immediate == 0x41,
		"INT 0x41 did not decode with its vector");
	require(wait.status == DecodeStatus::decoded &&
		wait.instruction.operation == Operation::nop &&
		wait.instruction.vector_opcode == 0x9b,
		"FWAIT did not decode as itself");
}

/* A single step of a repeated string instruction runs one iteration and
 * leaves between elements with RIP naming the instruction, until an
 * iteration ends it, which retires it (audit NC-E12). */
void a_single_step_runs_one_repeat_iteration()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xf3, 0xa6, // repe cmpsb
		0x0f, 0x05, // syscall
	});
	memory.load(0x800, {1, 2, 3, 4, 5});
	memory.load(0x900, {1, 2, 9, 4, 5});
	const auto built = require_built(gadgets::build(memory, 0),
		"REPE CMPSB block did not build");
	gadgets::Executor executor;
	executor.prepare(memory);
	CpuState cpu{};
	cpu.gpr(Register::rsi) = 0x800;
	cpu.gpr(Register::rdi) = 0x900;
	cpu.gpr(Register::rcx) = 5;
	for (std::uint64_t left = 4; left >= 3; --left) {
		const auto step = gadgets::execute(built.program, cpu, memory,
			executor, 1, {}, true);
		require(step.event == uml::cpu::EngineEvent::continued &&
				step.repeat_yielded &&
				step.retired_instructions == 0 && cpu.rip == 0 &&
				cpu.gpr(Register::rcx) == left,
			"a stepped REPE CMPSB did not stop after one iteration");
	}
	const auto last = gadgets::execute(built.program, cpu, memory,
		executor, 1, {}, true);
	require(last.event == uml::cpu::EngineEvent::continued &&
			!last.repeat_yielded && last.retired_instructions == 1 &&
			cpu.rip == 2 && cpu.gpr(Register::rcx) == 2 &&
			cpu.gpr(Register::rsi) == 0x803,
		"the iteration that ended a stepped REPE CMPSB did not retire it");
}

/* LDMXCSR keeps what the guest loads, and LDMXCSR and FLDCW that unmask an
 * exception retire and leave, so the runtime checks what follows. */
void loads_that_unmask_exceptions_leave()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x0f, 0xae, 0x13, // ldmxcsr [rbx]
		0x48, 0xff, 0xc0, // inc rax
		0xd9, 0x2e,       // fldcw [rsi]
		0x48, 0xff, 0xc0, // inc rax
		0x0f, 0x05,       // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"LDMXCSR/FLDCW block did not build");
	const auto run = [&](std::initializer_list<std::uint8_t> mxcsr,
		std::initializer_list<std::uint8_t> control, CpuState &cpu) {
		memory.load(0x800, mxcsr);
		memory.load(0x810, control);
		cpu = CpuState{};
		cpu.gpr(Register::rbx) = 0x800;
		cpu.gpr(Register::rsi) = 0x810;
		return gadgets::execute(built.program, cpu, memory, 64);
	};
	CpuState cpu{};
	const auto masked = run({0xa0, 0x1f, 0, 0}, {0x7f, 0x03}, cpu);
	require(masked.event == uml::cpu::EngineEvent::system_call &&
			cpu.mxcsr == 0x1fa0 && cpu.x87_control == 0x37f &&
			cpu.gpr(Register::rax) == 2,
		"LDMXCSR lost the flags it loaded, or a masked load left");
	const auto simd = run({0x80, 0x1d, 0, 0}, {0x7f, 0x03}, cpu);
	require(simd.event == uml::cpu::EngineEvent::continued &&
			simd.retired_instructions == 1 && cpu.rip == 3 &&
			cpu.mxcsr == 0x1d80,
		"LDMXCSR unmasking #Z did not leave after itself");
	const auto x87 = run({0x80, 0x1f, 0, 0}, {0x7b, 0x03}, cpu);
	require(x87.event == uml::cpu::EngineEvent::continued &&
			x87.retired_instructions == 3 && cpu.rip == 8 &&
			cpu.x87_control == 0x37b && cpu.gpr(Register::rax) == 1,
		"FLDCW unmasking #Z did not leave after itself");
}

[[nodiscard]] std::uint32_t xmm_low(const uml::cpu::x86_64::CpuState &cpu,
	std::size_t index) noexcept
{
	std::uint32_t bits{};
	std::memcpy(&bits, cpu.vectors[index].data(), sizeof(bits));
	return bits;
}

void set_xmm_low(uml::cpu::x86_64::CpuState &cpu, std::size_t index,
	std::uint32_t bits) noexcept
{
	std::memcpy(cpu.vectors[index].data(), &bits, sizeof(bits));
}

/* While MXCSR unmasks an exception, a SIMD instruction that raises it
 * faults with #XM before its result: the destination keeps its value and
 * MXCSR gains the flag. A masked exception only sets its flag. */
void checked_simd_exceptions_fault_before_the_result()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xf3, 0x0f, 0x5e, 0xc1, // divss xmm0, xmm1
		0x0f, 0x05,             // syscall
	});
	const auto built = gadgets::build(memory, 0, ExecutionMode::long64, 64,
		{}, gadgets::check_simd);
	require(built.status == gadgets::BuildStatus::built &&
			built.program.check_head() &&
			built.program.instruction_count() == 1,
		"DIVSS did not build as a check head");
	gadgets::Executor executor;
	executor.prepare(memory);
	CpuState cpu{};
	cpu.mxcsr = 0x1d80;
	set_xmm_low(cpu, 0, 0x3f800000);
	const auto trap = gadgets::execute_checked(built.program, cpu, memory,
		executor, 64, {}, false, gadgets::checked_exceptions(cpu));
	require(trap.event == uml::cpu::EngineEvent::floating_point_trap &&
			trap.vector == 19 && trap.error == 0x1d84 &&
			trap.retired_instructions == 0 && cpu.rip == 0 &&
			cpu.mxcsr == 0x1d84 && xmm_low(cpu, 0) == 0x3f800000,
		"an unmasked division by zero did not fault with #XM");
	cpu = CpuState{};
	cpu.mxcsr = 0x0f80;
	set_xmm_low(cpu, 0, 0x3f800000);
	const auto masked = gadgets::execute_checked(built.program, cpu, memory,
		executor, 64, {}, false, gadgets::checked_exceptions(cpu));
	require(masked.event == uml::cpu::EngineEvent::continued &&
			masked.retired_instructions == 1 && cpu.rip == 4 &&
			cpu.mxcsr == 0x0f84 && xmm_low(cpu, 0) == 0x7f800000,
		"a masked division by zero did not only set its flag");
}

/* Run singly and checked, an SSE instruction raises a denormal operand
 * before it computes, where x86 does: #XM while MXCSR unmasks it, keeping
 * the destination, and otherwise its flag beside the others -- but not
 * under DAZ, for a lane with a NaN or for a division by zero, while a DPPS
 * raises it for a denormal product of normal sources too. */
void checked_simd_denormal_operands_follow_x86()
{
	using namespace uml::cpu::x86_64;
	struct Case {
		std::string_view name;
		std::vector<std::uint8_t> bytes;
		std::uint32_t mxcsr;
		std::uint32_t left;
		std::uint32_t right;
		bool trap;
		std::uint32_t after;
		std::uint32_t result;
	};
	constexpr std::uint32_t one = 0x3f800000;
	constexpr std::uint32_t least = 0x00000001;
	constexpr std::uint32_t quiet = 0x7fc00000;
	const std::vector<Case> cases{
		{"ADDSS of a denormal", {0xf3, 0x0f, 0x58, 0xc1}, 0x1e80, one, least,
		 true, 0x1e82, one},
		{"ADDSS of a denormal, its exception masked",
		 {0xf3, 0x0f, 0x58, 0xc1}, 0x1780, one, least, false, 0x17a2, one},
		{"ADDSS of a denormal under DAZ", {0xf3, 0x0f, 0x58, 0xc1}, 0x1ec0,
		 one, least, false, 0x1ec0, one},
		{"ADDSS of a NaN and a denormal", {0xf3, 0x0f, 0x58, 0xc1}, 0x1e80,
		 quiet, least, false, 0x1e80, quiet},
		{"DIVSS of a denormal by zero", {0xf3, 0x0f, 0x5e, 0xc1}, 0x1e80,
		 least, 0, false, 0x1e84, 0x7f800000},
		{"COMISS of a denormal", {0x0f, 0x2f, 0xc1}, 0x1e80, least, 0,
		 true, 0x1e82, least},
		{"DPPS of a denormal product", {0x66, 0x0f, 0x3a, 0x40, 0xc1, 0x11},
		 0x1e80, 0x1c800000, 0x21800000, true, 0x1e82, 0x1c800000},
	};
	for (const auto &test : cases) {
		TestMemory memory;
		std::vector<std::byte> code(test.bytes.size() + 2U);
		std::memcpy(code.data(), test.bytes.data(), test.bytes.size());
		code[test.bytes.size()] = std::byte{0x0f};
		code[test.bytes.size() + 1U] = std::byte{0x05};
		memory.fill(0, code);
		const auto built = gadgets::build(memory, 0, ExecutionMode::long64,
			64, {}, gadgets::check_simd);
		CpuState cpu{};
		cpu.mxcsr = test.mxcsr;
		set_xmm_low(cpu, 0, test.left);
		set_xmm_low(cpu, 1, test.right);
		require(built.status == gadgets::BuildStatus::built &&
				built.program.check_head() &&
				gadgets::checked_exceptions(cpu) == gadgets::check_simd,
			"a SIMD instruction did not build as a check head");
		gadgets::Executor executor;
		executor.prepare(memory);
		const auto result = gadgets::execute_checked(built.program, cpu,
			memory, executor, 64, {}, false, gadgets::checked_exceptions(cpu));
		const bool right = test.trap ?
			result.event == uml::cpu::EngineEvent::floating_point_trap &&
				result.vector == 19 && result.error == test.after &&
				result.retired_instructions == 0 && cpu.rip == 0 :
			result.event == uml::cpu::EngineEvent::continued &&
				result.retired_instructions == 1 &&
				cpu.rip == test.bytes.size();
		if (right && cpu.mxcsr == test.after &&
			xmm_low(cpu, 0) == test.result)
			continue;
		std::fprintf(stderr, "x86-64 gadget bootstrap: %.*s: event %d "
			"mxcsr %#x xmm0 %#x\n", static_cast<int>(test.name.size()),
			test.name.data(), static_cast<int>(result.event), cpu.mxcsr,
			xmm_low(cpu, 0));
		fail("a denormal operand did not raise what x86 raises");
	}
}

/* Run singly and checked, an SSE instruction raises invalid operation where
 * x86 does: COMISD for a quiet NaN but UCOMISD only for a signalling one, a
 * conversion to an integer for a value out of that integer's range, and a
 * scalar comparison, minimum or maximum only for the lanes it compares. */
void checked_simd_invalid_operations_follow_x86()
{
	using namespace uml::cpu::x86_64;
	struct Case {
		std::string_view name;
		std::vector<std::uint8_t> bytes;
		std::array<std::uint64_t, 2> left;
		std::array<std::uint64_t, 2> right;
		bool invalid;
	};
	constexpr std::uint64_t one = 0x3ff0'0000'0000'0000ULL;
	constexpr std::uint64_t quiet = 0x7ff8'0000'0000'0000ULL;
	constexpr std::uint64_t signalling = 0x7ff4'0000'0000'0000ULL;
	constexpr std::uint64_t ten_billion = 0x4202'a05f'2000'0000ULL;
	/* 3e9 as a single, then 1.0 as a single, in lane 0 and 1. */
	constexpr std::uint64_t three_billion_single = 0x3f80'0000'4f32'd05eULL;
	constexpr std::uint64_t quiet_single_high = 0x7fc0'0000'3f80'0000ULL;
	const std::vector<Case> cases{
		{"COMISD of a quiet NaN", {0x66, 0x0f, 0x2f, 0xc1},
		 {quiet, one}, {one, one}, true},
		{"UCOMISD of a quiet NaN", {0x66, 0x0f, 0x2e, 0xc1},
		 {quiet, one}, {one, one}, false},
		{"UCOMISD of a signalling NaN", {0x66, 0x0f, 0x2e, 0xc1},
		 {signalling, one}, {one, one}, true},
		{"CVTTSD2SI EAX of 1e10", {0xf2, 0x0f, 0x2c, 0xc1},
		 {one, one}, {ten_billion, one}, true},
		{"CVTTSD2SI RAX of 1e10", {0xf2, 0x48, 0x0f, 0x2c, 0xc1},
		 {one, one}, {ten_billion, one}, false},
		{"CVTSD2SI EAX of a NaN", {0xf2, 0x0f, 0x2d, 0xc1},
		 {one, one}, {quiet, one}, true},
		{"CVTTSS2SI EAX of 3e9", {0xf3, 0x0f, 0x2c, 0xc1},
		 {one, one}, {three_billion_single, one}, true},
		{"CVTTPD2DQ of 1e10", {0x66, 0x0f, 0xe6, 0xc1},
		 {one, one}, {one, ten_billion}, true},
		{"CVTPD2DQ in range", {0xf2, 0x0f, 0xe6, 0xc1},
		 {one, one}, {one, one}, false},
		{"MINSD of a NaN in lane 1", {0xf2, 0x0f, 0x5d, 0xc1},
		 {one, one}, {one, quiet}, false},
		{"MINSD of a NaN in lane 0", {0xf2, 0x0f, 0x5d, 0xc1},
		 {one, one}, {quiet, one}, true},
		{"CMPLTSD of a NaN in lane 1", {0xf2, 0x0f, 0xc2, 0xc1, 0x01},
		 {one, quiet}, {one, one}, false},
		{"CMPLTSD of a NaN in lane 0", {0xf2, 0x0f, 0xc2, 0xc1, 0x01},
		 {quiet, one}, {one, one}, true},
		{"MAXSS of a NaN in lane 1", {0xf3, 0x0f, 0x5f, 0xc1},
		 {quiet_single_high, one}, {one, one}, false},
	};
	for (const auto &test : cases) {
		TestMemory memory;
		std::vector<std::byte> code(test.bytes.size());
		std::memcpy(code.data(), test.bytes.data(), code.size());
		memory.fill(0, code);
		const auto built = gadgets::build(memory, 0, ExecutionMode::long64,
			64, {}, gadgets::check_simd);
		require(built.status == gadgets::BuildStatus::built &&
				built.program.check_head(),
			"an SSE instruction did not build as a check head");
		gadgets::Executor executor;
		executor.prepare(memory);
		CpuState cpu{};
		cpu.mxcsr = 0x1f00;
		std::memcpy(cpu.vectors[0].data(), test.left.data(), 16);
		std::memcpy(cpu.vectors[1].data(), test.right.data(), 16);
		const auto result = gadgets::execute_checked(built.program, cpu,
			memory, executor, 64, {}, false, gadgets::check_simd);
		const bool invalid =
			result.event == uml::cpu::EngineEvent::floating_point_trap &&
			result.vector == 19 && (result.error & 0x3fU) == 0x1U;
		if (invalid == test.invalid && (invalid ||
				result.event == uml::cpu::EngineEvent::continued))
			continue;
		std::fprintf(stderr, "x86-64 gadget bootstrap: %.*s: event %d "
			"error %#llx\n", static_cast<int>(test.name.size()),
			test.name.data(), static_cast<int>(result.event),
			static_cast<unsigned long long>(result.error));
		fail(test.invalid ?
			"a checked SSE instruction did not raise invalid operation" :
			"a checked SSE instruction raised invalid operation");
	}
}

/* Runs from RIP as the runtime does, checked while `checked_exceptions`
 * says so, until something other than a continued run. */
[[nodiscard]] uml::cpu::x86_64::gadgets::ExecuteResult run_until_event(
	TestMemory &memory, uml::cpu::x86_64::CpuState &cpu)
{
	using namespace uml::cpu::x86_64;
	gadgets::Executor executor;
	executor.prepare(memory);
	gadgets::ExecuteResult result{};
	for (int dispatch = 0; dispatch < 32; ++dispatch) {
		const auto checked = gadgets::checked_exceptions(cpu);
		const auto built = gadgets::build(memory, cpu.rip,
			ExecutionMode::long64, 64, {}, checked);
		if (built.status != gadgets::BuildStatus::built)
			return {};
		result = checked == 0 ?
			gadgets::execute(built.program, cpu, memory, executor, 64) :
			gadgets::execute_checked(built.program, cpu, memory,
				executor, 64, {}, false, checked);
		if (result.event != uml::cpu::EngineEvent::continued)
			break;
	}
	return result;
}

/* A SIMD instruction with xmm0, xmm1 and xmm2 (low quadword first) as its
 * registers, which raises invalid operation or else leaves `result` in the
 * low quadword of xmm0. */
struct InvalidOperationCase {
	std::string_view name;
	std::vector<std::uint8_t> bytes;
	std::array<std::array<std::uint64_t, 2>, 3> registers;
	bool denormals_are_zero;
	bool invalid;
	std::uint64_t result;
};

/* Runs `test` singly and checked while MXCSR unmasks invalid operation
 * alone, and as the runtime runs it while MXCSR also unmasks division by
 * zero and overflow, where the gadget checks its values and leaves for the
 * runtime to run it again checked. Either faults with #XM and keeps xmm0 if
 * x86 raises invalid operation, and otherwise returns x86's result without a
 * flag. */
void invalid_operation_follows_x86(
	const InvalidOperationCase &test)
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	std::vector<std::byte> code(test.bytes.size() + 2U);
	std::memcpy(code.data(), test.bytes.data(), test.bytes.size());
	code[test.bytes.size()] = std::byte{0x0f};
	code[test.bytes.size() + 1U] = std::byte{0x05};
	memory.fill(0, code);
	const std::uint32_t daz = test.denormals_are_zero ? 0x40U : 0U;
	const auto start = [&](std::uint32_t mxcsr) {
		CpuState cpu{};
		cpu.mxcsr = mxcsr | daz;
		for (std::size_t index = 0; index < 3; ++index)
			std::memcpy(cpu.vectors[index].data(),
				test.registers[index].data(), 16);
		return cpu;
	};
	const auto trapped = [&](const gadgets::ExecuteResult &result,
		const CpuState &cpu, std::uint32_t mxcsr) {
		return result.event == uml::cpu::EngineEvent::floating_point_trap &&
			result.vector == 19 && cpu.rip == 0 &&
			result.error == (mxcsr | daz | 0x1U) &&
			std::memcmp(cpu.vectors[0].data(), test.registers[0].data(),
				16) == 0;
	};
	const auto returned = [&](const CpuState &cpu, std::uint32_t mxcsr) {
		std::uint64_t low{};
		std::memcpy(&low, cpu.vectors[0].data(), sizeof(low));
		return cpu.mxcsr == (mxcsr | daz) && low == test.result;
	};
	const auto built = gadgets::build(memory, 0, ExecutionMode::long64, 64,
		{}, gadgets::check_simd);
	auto checked = start(0x1f00);
	gadgets::Executor executor;
	executor.prepare(memory);
	const auto alone = built.status == gadgets::BuildStatus::built ?
		gadgets::execute_checked(built.program, checked, memory, executor,
			64, {}, false, gadgets::check_simd) :
		gadgets::ExecuteResult{};
	auto runtime = start(0x1900);
	const auto linked = run_until_event(memory, runtime);
	const bool right = test.invalid ?
		trapped(alone, checked, 0x1f00) && trapped(linked, runtime, 0x1900) :
		alone.event == uml::cpu::EngineEvent::continued &&
			returned(checked, 0x1f00) &&
			linked.event == uml::cpu::EngineEvent::system_call &&
			returned(runtime, 0x1900);
	if (right)
		return;
	std::fprintf(stderr, "x86-64 gadget bootstrap: %.*s: checked event %d "
		"error %#llx mxcsr %#x, runtime event %d error %#llx mxcsr %#x\n",
		static_cast<int>(test.name.size()), test.name.data(),
		static_cast<int>(alone.event),
		static_cast<unsigned long long>(alone.error), checked.mxcsr,
		static_cast<int>(linked.event),
		static_cast<unsigned long long>(linked.error), runtime.mxcsr);
	fail(test.invalid ?
		"a SIMD instruction did not raise invalid operation" :
		"a SIMD instruction raised invalid operation x86 does not");
}

/* An FMA raises invalid operation where x86 does: for a signalling NaN, for
 * infinity times zero plus anything but a quiet NaN, and for infinities of
 * opposite signs added -- but not for infinity times zero plus a quiet NaN,
 * which it returns, where the host raises it. MXCSR.DAZ makes a denormal
 * factor zero first. */
void checked_fma_invalid_operations_follow_x86()
{
	/* Singles: lane 1 is 1.0 unless named high. */
	constexpr std::uint64_t one = 0x3f80'0000'3f80'0000ULL;
	constexpr std::uint64_t infinity = 0x3f80'0000'7f80'0000ULL;
	constexpr std::uint64_t zero = 0x3f80'0000'0000'0000ULL;
	constexpr std::uint64_t least = 0x3f80'0000'0000'0001ULL;
	constexpr std::uint64_t quiet = 0x3f80'0000'7fc0'0000ULL;
	constexpr std::uint64_t signalling = 0x3f80'0000'7fa0'0000ULL;
	constexpr std::uint64_t infinity_high = 0x7f80'0000'3f80'0000ULL;
	constexpr std::uint64_t zero_high = 0x0000'0000'3f80'0000ULL;
	constexpr std::uint64_t quiet_high = 0x7fc0'0000'3f80'0000ULL;
	constexpr std::uint64_t infinity_double = 0x7ff0'0000'0000'0000ULL;
	constexpr std::uint64_t quiet_double = 0xfff8'0000'0001'2345ULL;
	/* VFMADD231SS xmm0, xmm1, xmm2 is xmm1 * xmm2 + xmm0; VFNMSUB213SD
	 * xmm0, xmm1, xmm2 is -(xmm1 * xmm0) - xmm2; VFMADDSUB231PS xmm0, xmm1,
	 * xmm2 adds in its odd lanes. */
	const std::vector<std::uint8_t> fmadd231ss{0xc4, 0xe2, 0x71, 0xb9, 0xc2};
	const std::vector<std::uint8_t> fnmsub213sd{0xc4, 0xe2, 0xf1, 0xaf, 0xc2};
	const std::vector<std::uint8_t> fmaddsub231ps{0xc4, 0xe2, 0x71, 0xb6,
		0xc2};
	const std::vector<InvalidOperationCase> cases{
		{"VFMADD231SS of infinity times zero plus a quiet NaN", fmadd231ss,
		 {{{quiet, one}, {infinity, one}, {zero, one}}}, false, false,
		 quiet},
		{"VFMADD231SS of zero times infinity plus a quiet NaN", fmadd231ss,
		 {{{quiet, one}, {zero, one}, {infinity, one}}}, false, false,
		 quiet},
		{"VFNMSUB213SD of infinity times zero plus a quiet NaN",
		 fnmsub213sd,
		 {{{0, one}, {infinity_double, one}, {quiet_double, one}}}, false,
		 false, quiet_double},
		{"VFMADD231SS of infinity times a denormal plus a quiet NaN under "
		 "DAZ", fmadd231ss,
		 {{{quiet, one}, {infinity, one}, {least, one}}}, true, false,
		 quiet},
		{"VFMADDSUB231PS of infinity times zero plus a quiet NaN in lane 1",
		 fmaddsub231ps,
		 {{{quiet_high, one}, {infinity_high, one}, {zero_high, one}}},
		 false, false, 0x7fc0'0000'0000'0000ULL},
		{"VFMADD231SS of infinity times zero plus one", fmadd231ss,
		 {{{one, one}, {infinity, one}, {zero, one}}}, false, true, 0},
		{"VFMADD231SS of infinity times zero plus a signalling NaN",
		 fmadd231ss, {{{signalling, one}, {infinity, one}, {zero, one}}},
		 false, true, 0},
		{"VFMADD231SS of infinity times a denormal plus one under DAZ",
		 fmadd231ss, {{{one, one}, {infinity, one}, {least, one}}},
		 true, true, 0},
	};
	for (const auto &test : cases)
		invalid_operation_follows_x86(test);
}

/* A VEX comparison raises invalid operation for a quiet NaN only under a
 * signalling predicate, which bit 4 of the immediate makes of a quiet one in
 * the low four bits and the reverse, and for a signalling NaN under every
 * predicate, false and true included, which compare nothing. */
void checked_comparison_invalid_operations_follow_x86()
{
	/* Singles: lane 1 is 1.0 unless named high. */
	constexpr std::uint64_t one = 0x3f80'0000'3f80'0000ULL;
	constexpr std::uint64_t quiet = 0x3f80'0000'7fc0'0000ULL;
	constexpr std::uint64_t quiet_high = 0x7fc0'0000'3f80'0000ULL;
	constexpr std::uint64_t signalling_high = 0x7fa0'0000'3f80'0000ULL;
	constexpr std::uint64_t one_double = 0x3ff0'0000'0000'0000ULL;
	constexpr std::uint64_t quiet_double = 0x7ff8'0000'0000'0000ULL;
	/* VCMPSS, VCMPPS and VCMPPD xmm0, xmm0, xmm1, predicate. */
	const auto compare = [](std::uint8_t prefix, std::uint8_t predicate) {
		return std::vector<std::uint8_t>{0xc5, prefix, 0xc2, 0xc1,
			predicate};
	};
	constexpr std::uint8_t ss = 0xfa;
	constexpr std::uint8_t ps = 0xf8;
	constexpr std::uint8_t pd = 0xf9;
	const std::vector<InvalidOperationCase> cases{
		{"VCMPSS less than, ordered and quiet (17), of a quiet NaN",
		 compare(ss, 17), {{{quiet, one}, {one, one}, {}}}, false, false,
		 0x3f80'0000'0000'0000ULL},
		{"VCMPPS greater than, ordered and quiet (30), of a quiet NaN in "
		 "lane 1", compare(ps, 30), {{{quiet_high, one}, {one, one}, {}}},
		 false, false, 0},
		{"VCMPSS true, unordered and quiet (15), of a quiet NaN",
		 compare(ss, 15), {{{quiet, one}, {one, one}, {}}}, false, false,
		 0x3f80'0000'ffff'ffffULL},
		{"VCMPSS equal, ordered and signalling (16), of a quiet NaN",
		 compare(ss, 16), {{{quiet, one}, {one, one}, {}}}, false, true, 0},
		{"VCMPPD not equal, ordered and signalling (28), of a quiet NaN in "
		 "lane 1", compare(pd, 28),
		 {{{one_double, quiet_double}, {one_double, one_double}, {}}},
		 false, true, 0},
		{"VCMPSS false, ordered and signalling (27), of a quiet NaN",
		 compare(ss, 27), {{{quiet, one}, {one, one}, {}}}, false, true, 0},
		{"VCMPPS true, unordered and quiet (15), of a signalling NaN in "
		 "lane 1", compare(ps, 15),
		 {{{signalling_high, one}, {one, one}, {}}}, false, true, 0},
	};
	for (const auto &test : cases)
		invalid_operation_follows_x86(test);
}

/* A SIMD instruction with xmm0, xmm1 and xmm2 (low quadword first) as its
 * registers, run singly and checked under `mxcsr`: it must leave `result`
 * in the low quadword of xmm0 and MXCSR as `after`, the flags x86 raises
 * for it, which an Intel Xeon 8581C confirmed -- or, when `mxcsr` unmasks
 * one of those flags, fault with #XM, keep xmm0 and leave MXCSR as `after`,
 * the fault's flags. */
struct FlagCase {
	std::string_view name;
	std::vector<std::uint8_t> bytes;
	std::array<std::array<std::uint64_t, 2>, 3> registers;
	std::uint32_t mxcsr;
	std::uint32_t after;
	std::uint64_t result;
};

void flags_follow_x86(const FlagCase &test)
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	std::vector<std::byte> code(test.bytes.size());
	std::memcpy(code.data(), test.bytes.data(), code.size());
	memory.fill(0, code);
	const auto built = gadgets::build(memory, 0, ExecutionMode::long64, 64,
		{}, gadgets::check_simd);
	CpuState cpu{};
	cpu.mxcsr = test.mxcsr;
	for (std::size_t index = 0; index < 3; ++index)
		std::memcpy(cpu.vectors[index].data(), test.registers[index].data(),
			16);
	gadgets::Executor executor;
	executor.prepare(memory);
	const auto result = built.status == gadgets::BuildStatus::built ?
		gadgets::execute_checked(built.program, cpu, memory, executor, 1,
			{}, false, gadgets::check_simd) :
		gadgets::ExecuteResult{};
	std::uint64_t low{};
	std::memcpy(&low, cpu.vectors[0].data(), sizeof(low));
	const bool faults = (test.after & ~(test.mxcsr >> 7U) & 0x3fU) != 0;
	if (faults ?
			result.event == uml::cpu::EngineEvent::floating_point_trap &&
				result.vector == 19 && result.error == test.after &&
				cpu.mxcsr == test.after && cpu.rip == 0 &&
				low == test.registers[0][0] :
			result.event == uml::cpu::EngineEvent::continued &&
				result.retired_instructions == 1 &&
				cpu.mxcsr == test.after && low == test.result)
		return;
	std::fprintf(stderr, "x86-64 gadget bootstrap: %.*s: event %d mxcsr "
		"%#x (x86 %#x) xmm0 %#llx (x86 %#llx)\n",
		static_cast<int>(test.name.size()), test.name.data(),
		static_cast<int>(result.event), cpu.mxcsr, test.after,
		static_cast<unsigned long long>(low),
		static_cast<unsigned long long>(test.result));
	fail("a checked SIMD instruction raised other flags "
		"than x86");
}

/* HSUBPS and ADDSUB compute only the lanes x86 does, so a checked run sees
 * no flag from the operation x86 skips: ADDSUBPS adds two infinities in
 * lane 3 without subtracting them, ADDSUBPD subtracts the largest double
 * from itself without adding it, and HSUBPS rounding down subtracts a
 * small negative from the largest single without overflowing the other
 * way. */
void checked_horizontal_flags_follow_x86()
{
	constexpr std::uint64_t ones = 0x3f80'0000'3f80'0000ULL;
	const std::vector<FlagCase> cases{
		{"ADDSUBPS of infinities in lane 3", {0xf2, 0x0f, 0xd0, 0xc1},
		 {{{ones, 0x7f80'0000'3f80'0000ULL},
		   {ones, 0x7f80'0000'3f80'0000ULL}, {}}},
		 0x1f80, 0x1f80, 0x4000'0000'0000'0000ULL},
		{"ADDSUBPD of the largest double", {0x66, 0x0f, 0xd0, 0xc1},
		 {{{0x7fef'ffff'ffff'ffffULL, 0x3ff0'0000'0000'0000ULL},
		   {0x7fef'ffff'ffff'ffffULL, 0x3ff0'0000'0000'0000ULL}, {}}},
		 0x1f80, 0x1f80, 0},
		{"HSUBPS rounding down near the largest single",
		 {0xf2, 0x0f, 0x7d, 0xc1},
		 {{{0xf300'0000'7f7f'ffffULL, ones}, {ones, ones}, {}}},
		 0x3f80, 0x3fa0, 0x8000'0000'7f7f'ffffULL},
	};
	for (const auto &test : cases)
		flags_follow_x86(test);
}

/* x86 detects underflow after rounding, where the host detects it before:
 * a product, an FMA or a narrowing whose exact result lies just below the
 * smallest normal and rounds up to it is not tiny, and raises precision
 * alone, in rounding to nearest and rounding up alike, while underflow is
 * unmasked too. A tiny result that is exact raises underflow while it is
 * unmasked, and, flushed by FTZ, underflow and precision. */
void checked_underflow_follows_x86()
{
	constexpr std::uint64_t one = 0x3f80'0000'3f80'0000ULL;
	constexpr std::uint64_t below_one = 0x3f80'0000'3f7f'fffeULL;
	constexpr std::uint64_t above_least_normal = 0x3f80'0000'0080'0001ULL;
	constexpr std::uint64_t half = 0x3f80'0000'3f00'0000ULL;
	constexpr std::uint64_t least_normal = 0x3f80'0000'0080'0000ULL;
	constexpr std::uint64_t one_double = 0x3ff0'0000'0000'0000ULL;
	const std::vector<std::uint8_t> mulss{0xf3, 0x0f, 0x59, 0xc1};
	/* VFMADD231SS xmm0, xmm1, xmm2 is xmm1 * xmm2 + xmm0; VFNMADD213PD
	 * xmm0, xmm1, xmm2 is -(xmm1 * xmm0) + xmm2. */
	const std::vector<FlagCase> cases{
		{"MULSS just below the smallest normal", mulss,
		 {{{below_one, one}, {above_least_normal, one}, {}}}, 0x1f80,
		 0x1fa0, least_normal},
		{"MULSD just below the smallest normal", {0xf2, 0x0f, 0x59, 0xc1},
		 {{{0x3fef'ffff'ffff'fffeULL, one_double},
		   {0x0010'0000'0000'0001ULL, one_double}, {}}},
		 0x1f80, 0x1fa0, 0x0010'0000'0000'0000ULL},
		{"MULSS rounding up just below the smallest normal", mulss,
		 {{{below_one, one}, {above_least_normal, one}, {}}}, 0x5f80,
		 0x5fa0, least_normal},
		{"VFMADD231SS just below the smallest normal",
		 {0xc4, 0xe2, 0x71, 0xb9, 0xc2},
		 {{{0x3f80'0000'0000'0000ULL, one}, {below_one, one},
		   {above_least_normal, one}}},
		 0x1f80, 0x1fa0, least_normal},
		{"VFNMADD213PD just below the smallest normal in lane 1",
		 {0xc4, 0xe2, 0xf1, 0xac, 0xc2},
		 {{{one_double, 0x3fef'ffff'ffff'fffeULL},
		   {one_double, 0x0010'0000'0000'0001ULL},
		   {0xbff0'0000'0000'0000ULL, 0}}},
		 0x1f80, 0x1fa0, 0xc000'0000'0000'0000ULL},
		{"CVTSD2SS just below the smallest normal single",
		 {0xf2, 0x0f, 0x5a, 0xc1},
		 {{{one, one}, {0x380f'ffff'f800'0000ULL, 0}, {}}}, 0x1f80,
		 0x1fa0, least_normal},
		{"VCVTPS2PH just below the smallest normal half",
		 {0xc4, 0xe3, 0x79, 0x1d, 0xc8, 0x00},
		 {{{}, {0x387f'f800ULL, 0}, {}}}, 0x1f80, 0x1fa0, 0x0400},
		{"MULSS just below the smallest normal, underflow unmasked",
		 mulss, {{{below_one, one}, {above_least_normal, one}, {}}},
		 0x1780, 0x17a0, least_normal},
		{"MULSS of an exact tiny result, underflow unmasked", mulss,
		 {{{half, one}, {least_normal, one}, {}}}, 0x1780, 0x1790, 0},
		{"MULSS of an exact tiny result under FTZ", mulss,
		 {{{half, one}, {least_normal, one}, {}}}, 0x9f80, 0x9fb0,
		 0x3f80'0000'0000'0000ULL},
		{"MULSS of an exact tiny result under FTZ, precision unmasked",
		 mulss, {{{half, one}, {least_normal, one}, {}}}, 0x8f80, 0x8fb0,
		 0},
	};
	for (const auto &test : cases)
		flags_follow_x86(test);
}

/* DPPS and DPPD raise flags step by step, as x86 does: the products, then
 * their sums in pairs, then for DPPS the sum of those. Each step detects
 * underflow after rounding, so that a product rounded up to the smallest
 * normal is not tiny, and FTZ flushing an exact tiny sum raises underflow
 * and precision. A step that raises an unmasked exception faults, and the
 * steps after it raise nothing, a denormal product added included -- unless
 * the exception is invalid operation or a denormal operand, raised before
 * the step computes, which raises nothing else of its step. */
void checked_dot_product_flags_follow_x86()
{
	/* DPPS and DPPD xmm0, xmm1, 0x31: the products of lanes 0 and 1,
	 * summed into lane 0; DPPS 0xf1 sums all four. */
	const std::vector<std::uint8_t> dpps{0x66, 0x0f, 0x3a, 0x40, 0xc1, 0x31};
	const std::vector<std::uint8_t> dppd{0x66, 0x0f, 0x3a, 0x41, 0xc1, 0x31};
	const std::vector<std::uint8_t> dpps_all{0x66, 0x0f, 0x3a, 0x40, 0xc1,
		0xf1};
	constexpr std::uint64_t below_one = 0x3f7f'fffeULL;
	constexpr std::uint64_t above_least_normal = 0x0080'0001ULL;
	constexpr std::uint64_t least_normal = 0x0080'0000ULL;
	constexpr std::uint64_t half = 0x3f00'0000ULL;
	/* 0.75 + 2^-24: times above_least_normal, an inexact tiny product;
	 * times itself, an inexact normal one. */
	constexpr std::uint64_t three_quarters = 0x3f40'0001ULL;
	/* Products 1, 2^-24 and an exact denormal: the first pair sum is
	 * inexact, and the second adds a denormal. */
	const std::array<std::uint64_t, 2> pair_sums_left{
		0x3380'0000'3f80'0000ULL, 0x3f00'0000ULL};
	const std::array<std::uint64_t, 2> pair_sums_right{
		0x3f80'0000'3f80'0000ULL, 0x0080'0000ULL};
	const std::vector<FlagCase> cases{
		{"DPPS of a product just below the smallest normal", dpps,
		 {{{below_one, 0}, {above_least_normal, 0}, {}}}, 0x1f80, 0x1fa0,
		 least_normal},
		{"DPPS of a product just below the smallest normal, underflow "
		 "unmasked", dpps,
		 {{{below_one, 0}, {above_least_normal, 0}, {}}}, 0x1780, 0x17a0,
		 least_normal},
		{"DPPS of an exact tiny sum under FTZ", dpps,
		 {{{0xbf80'0000'3fc0'0000ULL, 0},
		   {0x0080'0000'0080'0000ULL, 0}, {}}},
		 0x9f80, 0x9fb0, 0},
		{"DPPD of a product just below the smallest normal", dppd,
		 {{{0x3fef'ffff'ffff'fffeULL, 0}, {0x0010'0000'0000'0001ULL, 0},
		   {}}},
		 0x1f80, 0x1fa0, 0x0010'0000'0000'0000ULL},
		{"DPPS of an exact tiny product, underflow unmasked", dpps,
		 {{{half, 0}, {least_normal, 0}, {}}}, 0x1780, 0x1790, 0},
		{"DPPS of an exact tiny product, denormal operand unmasked", dpps,
		 {{{half, 0}, {least_normal, 0}, {}}}, 0x1e80, 0x1e82, 0},
		{"DPPS of an exact tiny product, underflow and denormal operand "
		 "unmasked", dpps, {{{half, 0}, {least_normal, 0}, {}}}, 0x1680,
		 0x1690, 0},
		{"DPPS of an inexact tiny product, denormal operand unmasked",
		 dpps, {{{three_quarters, 0}, {above_least_normal, 0}, {}}},
		 0x1e80, 0x1eb2, 0},
		{"DPPS of an inexact tiny product, precision unmasked", dpps,
		 {{{three_quarters, 0}, {above_least_normal, 0}, {}}}, 0x0f80,
		 0x0fb0, 0},
		{"DPPS of an inexact tiny product, underflow unmasked", dpps,
		 {{{three_quarters, 0}, {above_least_normal, 0}, {}}}, 0x1780,
		 0x17b0, 0},
		{"DPPS of an inexact tiny product, underflow and denormal operand "
		 "unmasked", dpps,
		 {{{three_quarters, 0}, {above_least_normal, 0}, {}}}, 0x1680,
		 0x16b0, 0},
		{"DPPS of an inexact tiny product under FTZ, denormal operand "
		 "unmasked", dpps,
		 {{{three_quarters, 0}, {above_least_normal, 0}, {}}}, 0x9e80,
		 0x9eb0, 0},
		{"DPPS of an inexact normal product, precision unmasked", dpps,
		 {{{three_quarters, 0}, {three_quarters, 0}, {}}}, 0x0f80, 0x0fa0,
		 0},
		{"DPPS of an inexact pair sum beside a denormal one, denormal "
		 "operand unmasked", dpps_all,
		 {{pair_sums_left, pair_sums_right, {}}}, 0x1e80, 0x1e82, 0},
		{"DPPS of an inexact pair sum beside a denormal one, precision "
		 "unmasked", dpps_all, {{pair_sums_left, pair_sums_right, {}}},
		 0x0f80, 0x0fa2, 0},
		{"DPPS of an exact tiny final sum, underflow unmasked", dpps_all,
		 {{{0x3f80'0000ULL, 0xbfc0'0000ULL},
		   {0x0100'0000ULL, 0x00c0'0000ULL}, {}}},
		 0x1780, 0x1790, 0},
	};
	for (const auto &test : cases)
		flags_follow_x86(test);
}

/* Beside an unmasked underflow or overflow, which faults, x86 raises
 * precision as the result's significand rounds, as if the exponent were
 * unbounded: an inexact tiny product raises it, and a tiny or overflowing
 * result whose significand is exact does not, however its delivery would
 * round -- except a half narrowed from a denormal single, which does. */
void checked_unmasked_precision_follows_x86()
{
	const std::vector<std::uint8_t> mulss{0xf3, 0x0f, 0x59, 0xc1};
	/* VCVTPS2PH xmm0, xmm1, rounding as MXCSR does. */
	const std::vector<std::uint8_t> vcvtps2ph{0xc4, 0xe3, 0x79, 0x1d, 0xc8,
		0x04};
	constexpr std::uint64_t three_quarters = 0x3f40'0001ULL;
	constexpr std::uint64_t above_least_normal = 0x0080'0001ULL;
	constexpr std::uint64_t largest = 0x7f7f'ffffULL;
	const std::vector<FlagCase> cases{
		{"MULSS of an inexact tiny product, underflow unmasked", mulss,
		 {{{three_quarters, 0}, {above_least_normal, 0}, {}}}, 0x1780,
		 0x17b0, 0},
		{"MULSS of an inexact tiny product, underflow and precision "
		 "unmasked", mulss,
		 {{{three_quarters, 0}, {above_least_normal, 0}, {}}}, 0x0780,
		 0x07b0, 0},
		{"MULSS overflowing with an exact significand, overflow unmasked",
		 mulss, {{{largest, 0}, {0x4000'0000ULL, 0}, {}}}, 0x1380, 0x1388,
		 0},
		{"MULSS overflowing with an inexact significand, overflow "
		 "unmasked", mulss, {{{largest, 0}, {0x3fc0'0000ULL, 0}, {}}},
		 0x1380, 0x13a8, 0},
		{"DPPS of a tiny product with an exact significand, underflow "
		 "unmasked", {0x66, 0x0f, 0x3a, 0x40, 0xc1, 0x31},
		 {{{above_least_normal, 0}, {0x3f00'0000ULL, 0}, {}}}, 0x1780,
		 0x1790, 0},
		{"CVTSD2SS of the least denormal double, underflow unmasked",
		 {0xf2, 0x0f, 0x5a, 0xc1}, {{{}, {1, 0}, {}}}, 0x1780, 0x1792, 0},
		{"CVTSD2SS overflowing with an exact significand, overflow "
		 "unmasked", {0xf2, 0x0f, 0x5a, 0xc1},
		 {{{}, {0x4c78'0000'0000'0000ULL, 0}, {}}}, 0x1380, 0x1388, 0},
		{"VFMADD231SD overflowing with an exact significand, overflow "
		 "unmasked", {0xc4, 0xe2, 0xf1, 0xb9, 0xc2},
		 {{{}, {0x6578'0000'0000'0000ULL, 0},
		   {0x5f30'0000'0000'0000ULL, 0}}},
		 0x1380, 0x1388, 0},
		{"VCVTPS2PH of a denormal single, underflow unmasked", vcvtps2ph,
		 {{{}, {0x0000'0002ULL, 0}, {}}}, 0x1780, 0x17b2, 0},
		{"VCVTPS2PH of a tiny single with an exact significand, underflow "
		 "unmasked", vcvtps2ph, {{{}, {0x33c0'0000ULL, 0}, {}}}, 0x1780,
		 0x1790, 0},
		{"VCVTPS2PH overflowing with an exact significand, overflow "
		 "unmasked", vcvtps2ph, {{{}, {0x49c0'0000ULL, 0}, {}}}, 0x1380,
		 0x1388, 0},
	};
	for (const auto &test : cases)
		flags_follow_x86(test);
}

/* A sum or difference that small is exact, yet tiny: FTZ flushing it raises
 * underflow and precision, in a horizontal or alternating one too, and it
 * raises underflow while that is unmasked. Beside an unmasked overflow, it
 * raises precision as its significand rounds. */
void checked_sum_flags_follow_x86()
{
	const std::vector<std::uint8_t> addss{0xf3, 0x0f, 0x58, 0xc1};
	constexpr std::uint64_t one = 0x3f80'0000'0000'0000ULL;
	constexpr std::uint64_t one_double = 0x3ff0'0000'0000'0000ULL;
	const std::vector<FlagCase> cases{
		{"ADDPS of an exact tiny sum under FTZ", {0x0f, 0x58, 0xc1},
		 {{{one | 0x0080'0001ULL, 0}, {one | 0x8080'0000ULL, 0}, {}}},
		 0x9f80, 0x9fb0, 0x4000'0000'0000'0000ULL},
		{"HSUBPS of an exact tiny difference under FTZ",
		 {0xf2, 0x0f, 0x7d, 0xc1},
		 {{{0x0080'0000'0080'0001ULL, 0x3f80'0000'3f80'0000ULL}, {}, {}}},
		 0x9f80, 0x9fb0, 0},
		{"ADDSUBPD of an exact tiny difference under FTZ",
		 {0x66, 0x0f, 0xd0, 0xc1},
		 {{{0x0010'0000'0000'0001ULL, one_double},
		   {0x0010'0000'0000'0000ULL, one_double}, {}}},
		 0x9f80, 0x9fb0, 0},
		{"ADDSS of an exact tiny sum, underflow unmasked", addss,
		 {{{0x0080'0001ULL, 0}, {0x8080'0000ULL, 0}, {}}}, 0x1780, 0x1790,
		 0},
		{"ADDSS overflowing with an exact significand, overflow unmasked",
		 addss, {{{0x7f40'0000ULL, 0}, {0x7f40'0000ULL, 0}, {}}}, 0x1380,
		 0x1388, 0},
		{"ADDSS overflowing with an inexact significand, overflow "
		 "unmasked", addss, {{{0x7f40'0001ULL, 0}, {0x7f40'0000ULL, 0}, {}}},
		 0x1380, 0x13a8, 0},
	};
	for (const auto &test : cases)
		flags_follow_x86(test);
}

/* ROUND raises precision for a lane that is not an integer, unless bit 3 of
 * its immediate suppresses it, and a conversion to an integer for one that
 * converts in range, where an out-of-range one raises invalid operation
 * alone; MXCSR.DAZ makes a denormal zero first. */
void checked_integer_rounding_precision_follows_x86()
{
	constexpr std::uint64_t one_and_a_half_two = 0x4000'0000'3fc0'0000ULL;
	constexpr std::uint64_t two_twos = 0x4000'0000'4000'0000ULL;
	/* 2147483647.5: truncated in range, rounded to nearest out of it. */
	constexpr std::uint64_t past_int32 = 0x41df'ffff'ffe0'0000ULL;
	const std::vector<FlagCase> cases{
		{"ROUNDPS of 1.5 as MXCSR rounds", {0x66, 0x0f, 0x3a, 0x08, 0xc1,
		 0x04}, {{{}, {one_and_a_half_two, 0}, {}}}, 0x1f80, 0x1fa0,
		 two_twos},
		{"ROUNDPS of 1.5 with precision suppressed", {0x66, 0x0f, 0x3a,
		 0x08, 0xc1, 0x0c}, {{{}, {one_and_a_half_two, 0}, {}}}, 0x1f80,
		 0x1f80, two_twos},
		{"ROUNDSD down of the least denormal", {0x66, 0x0f, 0x3a, 0x0b,
		 0xc1, 0x01}, {{{}, {1, 0}, {}}}, 0x1f80, 0x1fa0, 0},
		{"ROUNDSD down of the least denormal under DAZ", {0x66, 0x0f, 0x3a,
		 0x0b, 0xc1, 0x01}, {{{}, {1, 0}, {}}}, 0x1fc0, 0x1fc0, 0},
		{"CVTTSD2SI truncating 2147483647.5", {0xf2, 0x0f, 0x2c, 0xc1},
		 {{{}, {past_int32, 0}, {}}}, 0x1f80, 0x1fa0, 0},
		{"CVTSD2SI rounding 2147483647.5 out of range",
		 {0xf2, 0x0f, 0x2d, 0xc1}, {{{}, {past_int32, 0}, {}}}, 0x1f80,
		 0x1f81, 0},
		{"CVTPS2DQ of 2.5, precision unmasked", {0x66, 0x0f, 0x5b, 0xc1},
		 {{{}, {0x3f80'0000'4020'0000ULL, 0}, {}}}, 0x0f80, 0x0fa0, 0},
		{"CVTTPS2PI of 1.5", {0x0f, 0x2c, 0xc1},
		 {{{}, {one_and_a_half_two, 0}, {}}}, 0x1f80, 0x1fa0, 0},
		{"CVTSS2SI of 2^63 to 64 bits, invalid operation unmasked",
		 {0xf3, 0x48, 0x0f, 0x2d, 0xc1}, {{{}, {0x5f00'0000ULL, 0}, {}}},
		 0x1f00, 0x1f01, 0},
	};
	for (const auto &test : cases)
		flags_follow_x86(test);
}

/* An unmasked x87 exception leaves its operands, sets ES and B, and raises
 * #MF at the next waiting instruction. */
void checked_x87_exceptions_wait_for_the_next_instruction()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xd9, 0xee, // fldz
		0xd9, 0xe8, // fld1
		0xde, 0xf1, // fdivrp st(1), st: one by zero
		0x9b,       // fwait
		0x0f, 0x05, // syscall
	});
	CpuState cpu{};
	cpu.x87_control = 0x37b;
	const auto trap = run_until_event(memory, cpu);
	require(trap.event == uml::cpu::EngineEvent::floating_point_trap &&
			trap.vector == 16 && cpu.rip == 6 &&
			(trap.error & 0x80ffU) == 0x8084U &&
			(trap.error >> 16U) == 0x37bU &&
			cpu.x87_top == 6 && (cpu.x87_status & 0x8080U) == 0x8080U,
		"an unmasked x87 division by zero did not raise #MF at FWAIT");
}

/* A cache built for enabled exceptions splits its blocks at every
 * instruction that needs a check, so a linked run never reaches one: the
 * block before it runs linked, and a link into it leaves at once. */
void checked_builds_stop_before_checked_instructions()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x48, 0xff, 0xc0, // inc rax
		0xd9, 0xe8,       // fld1
		0x48, 0xff, 0xc0, // inc rax
		0x0f, 0x05,       // syscall
	});
	gadgets::Executor executor;
	executor.prepare(memory);
	executor.check_exceptions(gadgets::check_x87);
	const auto head = executor.select_program(memory, 0,
		ExecutionMode::long64);
	require(head.status == gadgets::BuildStatus::built &&
			head.program != nullptr &&
			head.program->instruction_count() == 1 &&
			!head.program->check_head(),
		"a checked build did not end its block before FLD1");
	CpuState cpu{};
	cpu.x87_control = 0x37b;
	const auto before = gadgets::execute_checked(*head.program, cpu, memory,
		executor, 64, {}, false, gadgets::checked_exceptions(cpu));
	require(before.event == uml::cpu::EngineEvent::continued &&
			before.retired_instructions == 1 && cpu.rip == 3 &&
			cpu.gpr(Register::rax) == 1,
		"the block before a check head did not stop at it");
	const auto check = executor.select_program(memory, 3,
		ExecutionMode::long64);
	require(check.status == gadgets::BuildStatus::built &&
			check.program != nullptr && check.program->check_head(),
		"FLD1 did not build as a check head");
	/* Entered as a link would enter it, the stream only leaves. */
	const auto entered = gadgets::execute(*check.program, cpu, memory,
		executor, 64);
	require(entered.event == uml::cpu::EngineEvent::continued &&
			entered.retired_instructions == 0 && cpu.rip == 3 &&
			cpu.x87_top == 0,
		"a check head's own stream ran its instruction");
	const auto ran = gadgets::execute_checked(*check.program, cpu, memory,
		executor, 64, {}, false, gadgets::checked_exceptions(cpu));
	require(ran.event == uml::cpu::EngineEvent::continued &&
			ran.retired_instructions == 1 && cpu.rip == 5 &&
			cpu.x87_top == 7,
		"a check head did not run its instruction checked");
}

/* While the x87 control word unmasks an exception, as Delphi's default
 * $1332 does, x87 code runs in ordinary linked blocks. An instruction that
 * raises an unmasked exception leaves before it commits, also inside a
 * block a link reached, and runs again checked, so the run retires it with
 * the exception pending for the next waiting instruction. */
void unmasked_x87_exceptions_leave_linked_blocks()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xeb, 0x0e}); // jmp 0x10
	memory.load(0x10, {
		0xd9, 0xee, // fldz
		0xd9, 0xe8, // fld1
		0xde, 0xf1, // fdivrp st(1), st: one by zero
		0x9b,       // fwait
		0x0f, 0x05, // syscall
	});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto run = [&](CpuState &cpu) {
		const auto selected = executor.select_program(memory, cpu.rip,
			ExecutionMode::long64);
		if (selected.status != gadgets::BuildStatus::built ||
			selected.program == nullptr)
			return gadgets::ExecuteResult{};
		return gadgets::execute(*selected.program, cpu, memory, executor,
			64);
	};
	const auto raised = [](const gadgets::ExecuteResult &result,
		const CpuState &cpu, std::size_t retired) {
		return result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == retired && cpu.rip == 0x16 &&
			cpu.x87_top == 6 && cpu.exception_raised == 0 &&
			(cpu.x87_status & 0x80ffU) == 0x8084U;
	};
	CpuState cold{};
	cold.x87_control = 0x1332;
	const auto jump = run(cold);
	const auto block = run(cold);
	require(jump.event == uml::cpu::EngineEvent::continued &&
			jump.retired_instructions == 1 && raised(block, cold, 3),
		"an unmasked x87 division by zero did not leave its block");
	CpuState cpu{};
	cpu.x87_control = 0x1332;
	const auto linked = run(cpu);
	require(raised(linked, cpu, 4),
		"an unmasked x87 division by zero did not leave a linked block");
	const auto trap = run_until_event(memory, cpu);
	require(trap.event == uml::cpu::EngineEvent::floating_point_trap &&
			trap.vector == 16 && cpu.rip == 0x16,
		"the pending x87 division by zero did not raise #MF at FWAIT");
}

/* x87 arithmetic that raises only masked exceptions runs through under an
 * unmasked control word, and a control word load leaves only when the
 * gadgets were not checking or it unmasks a flag already set. */
void unmasked_x87_control_words_run_clean_code_through()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xd9, 0xe8, // fld1
		0xd9, 0xe8, // fld1
		0xde, 0xc1, // faddp st(1), st
		0xd9, 0xfa, // fsqrt: inexact, masked
		0xdd, 0x1b, // fstp qword [rbx]
		0x0f, 0x05, // syscall
	});
	const auto arithmetic = require_built(gadgets::build(memory, 0),
		"x87 arithmetic did not build");
	CpuState cpu{};
	cpu.x87_control = 0x1332;
	cpu.gpr(Register::rbx) = 0x800;
	const auto clean = gadgets::execute(arithmetic.program, cpu, memory, 64);
	require(clean.event == uml::cpu::EngineEvent::system_call &&
			clean.retired_instructions == 6 &&
			memory.qword(0x800) == 0x3ff6'a09e'667f'3bcdULL &&
			(cpu.x87_status & 0x80a0U) == 0x0020U,
		"clean x87 arithmetic under FCW $1332 did not run through");

	memory.load(0x100, {
		0xd9, 0x2e,       // fldcw [rsi]
		0x48, 0xff, 0xc0, // inc rax
		0x0f, 0x05,       // syscall
	});
	memory.load(0x810, {0x32, 0x13});
	const auto load = require_built(gadgets::build(memory, 0x100),
		"FLDCW block did not build");
	const auto run = [&](std::uint16_t control, std::uint16_t status) {
		cpu = CpuState{};
		cpu.rip = 0x100;
		cpu.x87_control = control;
		cpu.x87_status = status;
		cpu.gpr(Register::rsi) = 0x810;
		return gadgets::execute(load.program, cpu, memory, 64);
	};
	const auto checking = run(0x37b, 0);
	require(checking.event == uml::cpu::EngineEvent::system_call &&
			cpu.x87_control == 0x1332 && cpu.gpr(Register::rax) == 1,
		"FLDCW $1332 left while the gadgets were already checking");
	const auto unchecked = run(0x37f, 0);
	require(unchecked.event == uml::cpu::EngineEvent::continued &&
			unchecked.retired_instructions == 1 && cpu.rip == 0x102 &&
			(cpu.x87_status & 0x8080U) == 0,
		"FLDCW $1332 did not leave when the gadgets were not checking");
	const auto pending = run(0x37b, 0x0001);
	require(pending.event == uml::cpu::EngineEvent::continued &&
			pending.retired_instructions == 1 && cpu.rip == 0x102 &&
			(cpu.x87_status & 0x8081U) == 0x8081U,
		"FLDCW unmasking a set invalid-operation flag did not leave");
}

/* While MXCSR unmasks invalid operation, division by zero or overflow and
 * masks underflow and precision, as Delphi's default $1900 does, SIMD code
 * runs in ordinary linked blocks. An instruction whose values could have
 * raised an unmasked exception leaves before it stores anything, also inside
 * a block a link reached, and runs again checked, which faults with #XM. */
void unmasked_simd_exceptions_leave_linked_blocks()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xeb, 0x0e}); // jmp 0x10
	memory.load(0x10, {
		0xf2, 0x0f, 0x58, 0xd3, // addsd xmm2, xmm3
		0xf2, 0x0f, 0x5e, 0xc1, // divsd xmm0, xmm1: one by zero
		0x0f, 0x05,             // syscall
	});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto run = [&](CpuState &cpu) {
		const auto selected = executor.select_program(memory, cpu.rip,
			ExecutionMode::long64);
		if (selected.status != gadgets::BuildStatus::built ||
			selected.program == nullptr)
			return gadgets::ExecuteResult{};
		return gadgets::execute(*selected.program, cpu, memory, executor,
			64);
	};
	const auto start = [] {
		CpuState cpu{};
		cpu.mxcsr = 0x1900;
		set_xmm_low(cpu, 0, 0x3f800000);
		set_xmm_low(cpu, 2, 0x3f800000);
		set_xmm_low(cpu, 3, 0x3f800000);
		std::uint64_t one = 0x3ff0'0000'0000'0000ULL;
		std::memcpy(cpu.vectors[0].data(), &one, sizeof(one));
		return cpu;
	};
	const auto faulted = [](const gadgets::ExecuteResult &result,
		const CpuState &cpu, std::size_t retired) {
		std::uint64_t dividend{};
		std::memcpy(&dividend, cpu.vectors[0].data(), sizeof(dividend));
		return result.event == uml::cpu::EngineEvent::floating_point_trap &&
			result.vector == 19 && result.error == 0x1904 &&
			result.retired_instructions == retired && cpu.rip == 0x14 &&
			cpu.mxcsr == 0x1904 && dividend == 0x3ff0'0000'0000'0000ULL;
	};
	auto cold = start();
	const auto jump = run(cold);
	const auto block = run(cold);
	require(jump.event == uml::cpu::EngineEvent::continued &&
			jump.retired_instructions == 1 && faulted(block, cold, 1),
		"an unmasked SIMD division by zero did not fault in its block");
	auto cpu = start();
	require(faulted(run(cpu), cpu, 2),
		"an unmasked SIMD division by zero did not fault in a linked block");
}

/* The gadgets' own SIMD checks raise exactly what running each instruction
 * singly and checked raises, whatever its values: every instruction #XM
 * covers, register and memory forms, scalar, 128- and 256-bit, under MXCSRs
 * that unmask invalid operation, division by zero, overflow or a denormal
 * operand in any mix and round either way. The flags they keep are the
 * checked path's too, also where an instruction leaves for its run after it
 * computed lanes x86 does not, except the denormal operand, which the
 * gadgets raise only for an instruction they run again while MXCSR.DAZ is
 * clear. */
void simd_checks_match_the_checked_path()
{
	using namespace uml::cpu::x86_64;
	struct Case {
		std::string_view name;
		std::vector<std::uint8_t> bytes;
		unsigned element;
	};
	/* xmm0 (ymm0) is the destination and first source, xmm1 and xmm2 the
	 * others, and [rbx] holds what xmm1 does. */
	const std::vector<Case> cases{
		{"addss", {0xf3, 0x0f, 0x58, 0xc1}, 32},
		{"addsd", {0xf2, 0x0f, 0x58, 0xc1}, 64},
		{"subss", {0xf3, 0x0f, 0x5c, 0xc1}, 32},
		{"subsd m", {0xf2, 0x0f, 0x5c, 0x03}, 64},
		{"mulss m", {0xf3, 0x0f, 0x59, 0x03}, 32},
		{"mulsd", {0xf2, 0x0f, 0x59, 0xc1}, 64},
		{"divss", {0xf3, 0x0f, 0x5e, 0xc1}, 32},
		{"divsd", {0xf2, 0x0f, 0x5e, 0xc1}, 64},
		{"sqrtss", {0xf3, 0x0f, 0x51, 0xc1}, 32},
		{"sqrtsd", {0xf2, 0x0f, 0x51, 0xc1}, 64},
		{"minss", {0xf3, 0x0f, 0x5d, 0xc1}, 32},
		{"maxsd m", {0xf2, 0x0f, 0x5f, 0x03}, 64},
		{"addps", {0x0f, 0x58, 0xc1}, 32},
		{"addpd m", {0x66, 0x0f, 0x58, 0x03}, 64},
		{"mulps m", {0x0f, 0x59, 0x03}, 32},
		{"divpd", {0x66, 0x0f, 0x5e, 0xc1}, 64},
		{"sqrtps", {0x0f, 0x51, 0xc1}, 32},
		{"minps", {0x0f, 0x5d, 0xc1}, 32},
		{"maxpd", {0x66, 0x0f, 0x5f, 0xc1}, 64},
		{"cmpeqss", {0xf3, 0x0f, 0xc2, 0xc1, 0x00}, 32},
		{"cmpltsd", {0xf2, 0x0f, 0xc2, 0xc1, 0x01}, 64},
		{"cmpunordsd m", {0xf2, 0x0f, 0xc2, 0x03, 0x03}, 64},
		{"cmpltps", {0x0f, 0xc2, 0xc1, 0x01}, 32},
		{"cmpneqpd", {0x66, 0x0f, 0xc2, 0xc1, 0x04}, 64},
		{"vcmptrue_uqss", {0xc5, 0xfa, 0xc2, 0xc1, 0x0f}, 32},
		{"vcmpeq_osps", {0xc5, 0xf8, 0xc2, 0xc1, 0x10}, 32},
		{"vcmplt_oqpd ymm", {0xc5, 0xfd, 0xc2, 0xc1, 0x11}, 64},
		{"vcmpnge_uqsd", {0xc5, 0xfb, 0xc2, 0xc1, 0x19}, 64},
		{"vcmpfalse_osps ymm", {0xc5, 0xfc, 0xc2, 0xc1, 0x1b}, 32},
		{"vcmpgt_oqss", {0xc5, 0xfa, 0xc2, 0xc1, 0x1e}, 32},
		{"comiss", {0x0f, 0x2f, 0xc1}, 32},
		{"comisd m", {0x66, 0x0f, 0x2f, 0x03}, 64},
		{"ucomiss", {0x0f, 0x2e, 0xc1}, 32},
		{"ucomisd", {0x66, 0x0f, 0x2e, 0xc1}, 64},
		{"cvtss2sd", {0xf3, 0x0f, 0x5a, 0xc1}, 32},
		{"cvtsd2ss", {0xf2, 0x0f, 0x5a, 0xc1}, 64},
		{"cvtsd2ss m", {0xf2, 0x0f, 0x5a, 0x03}, 64},
		{"cvtps2pd", {0x0f, 0x5a, 0xc1}, 32},
		{"cvtpd2ps", {0x66, 0x0f, 0x5a, 0xc1}, 64},
		{"cvtps2dq", {0x66, 0x0f, 0x5b, 0xc1}, 32},
		{"cvttps2dq", {0xf3, 0x0f, 0x5b, 0xc1}, 32},
		{"cvtpd2dq", {0xf2, 0x0f, 0xe6, 0xc1}, 64},
		{"cvttpd2dq m", {0x66, 0x0f, 0xe6, 0x03}, 64},
		{"cvttss2si eax", {0xf3, 0x0f, 0x2c, 0xc1}, 32},
		{"cvtss2si rax", {0xf3, 0x48, 0x0f, 0x2d, 0xc1}, 32},
		{"cvttsd2si eax m", {0xf2, 0x0f, 0x2c, 0x03}, 64},
		{"cvtsd2si rax", {0xf2, 0x48, 0x0f, 0x2d, 0xc1}, 64},
		{"cvttps2pi", {0x0f, 0x2c, 0xc1}, 32},
		{"cvtpd2pi", {0x66, 0x0f, 0x2d, 0xc1}, 64},
		{"haddps", {0xf2, 0x0f, 0x7c, 0xc1}, 32},
		{"haddpd m", {0x66, 0x0f, 0x7c, 0x03}, 64},
		{"hsubps", {0xf2, 0x0f, 0x7d, 0xc1}, 32},
		{"hsubpd", {0x66, 0x0f, 0x7d, 0xc1}, 64},
		{"addsubps", {0xf2, 0x0f, 0xd0, 0xc1}, 32},
		{"addsubpd", {0x66, 0x0f, 0xd0, 0xc1}, 64},
		{"roundss", {0x66, 0x0f, 0x3a, 0x0a, 0xc1, 0x00}, 32},
		{"roundsd m", {0x66, 0x0f, 0x3a, 0x0b, 0x03, 0x04}, 64},
		{"roundps", {0x66, 0x0f, 0x3a, 0x08, 0xc1, 0x03}, 32},
		{"roundpd", {0x66, 0x0f, 0x3a, 0x09, 0xc1, 0x01}, 64},
		{"dpps", {0x66, 0x0f, 0x3a, 0x40, 0xc1, 0xf1}, 32},
		{"dpps none", {0x66, 0x0f, 0x3a, 0x40, 0xc1, 0xf0}, 32},
		{"dppd m", {0x66, 0x0f, 0x3a, 0x41, 0x03, 0x33}, 64},
		{"vfmadd231sd", {0xc4, 0xe2, 0xf1, 0xb9, 0xc2}, 64},
		{"vfmadd213ss m", {0xc4, 0xe2, 0x71, 0xa9, 0x03}, 32},
		{"vfmadd231ps", {0xc4, 0xe2, 0x71, 0xb8, 0xc2}, 32},
		{"vfnmsub132pd", {0xc4, 0xe2, 0xf1, 0x9e, 0xc2}, 64},
		{"vcvtph2ps", {0xc4, 0xe2, 0x79, 0x13, 0xc1}, 32},
		{"vcvtps2ph", {0xc4, 0xe3, 0x79, 0x1d, 0xc1, 0x00}, 32},
		{"vcvtps2ph m", {0xc4, 0xe3, 0x79, 0x1d, 0x03, 0x04}, 32},
		{"vaddps ymm", {0xc5, 0xfc, 0x58, 0xc1}, 32},
		{"vmulpd ymm m", {0xc5, 0xfd, 0x59, 0x03}, 64},
		{"vmaxps ymm", {0xc5, 0xfc, 0x5f, 0xc1}, 32},
		{"vcmpltpd ymm", {0xc5, 0xfd, 0xc2, 0xc1, 0x01}, 64},
		{"vcvtps2pd ymm", {0xc5, 0xfc, 0x5a, 0xc1}, 32},
		{"vcvtpd2ps ymm", {0xc5, 0xfd, 0x5a, 0xc1}, 64},
		{"vcvttps2dq ymm", {0xc5, 0xfe, 0x5b, 0xc1}, 32},
		{"vcvtpd2dq ymm", {0xc5, 0xff, 0xe6, 0xc1}, 64},
		{"vhaddps ymm", {0xc5, 0xff, 0x7c, 0xc1}, 32},
		{"vaddsubpd ymm", {0xc5, 0xfd, 0xd0, 0xc1}, 64},
		{"vroundps ymm", {0xc4, 0xe3, 0x7d, 0x08, 0xc1, 0x00}, 32},
		{"vdpps ymm", {0xc4, 0xe3, 0x7d, 0x40, 0xc1, 0xff}, 32},
		{"vfmadd231pd ymm", {0xc4, 0xe2, 0xf5, 0xb8, 0xc2}, 64},
		{"vcvtph2ps ymm", {0xc4, 0xe2, 0x7d, 0x13, 0xc1}, 32},
		{"vcvtps2ph ymm", {0xc4, 0xe3, 0x7d, 0x1d, 0xc1, 0x00}, 32},
	};
	const std::array<std::uint32_t, 13> singles{
		0x3fc00000, 0x00000000, 0x80000000, 0x7f800000, 0xff800000,
		0x7fc00000, 0x7fa00000, 0x7f7fffff, 0x7f000000, 0x00000001,
		0xc0000000, 0xcf000000, 0x4f000000,
	};
	const std::array<std::uint64_t, 13> doubles{
		0x3ff8000000000000, 0x0000000000000000, 0x8000000000000000,
		0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000,
		0x7ff4000000000000, 0x7fefffffffffffff, 0x7fe0000000000000,
		0x0000000000000001, 0xc000000000000000, 0xc1e0000000000000,
		0x41e0000000000000,
	};
	const std::array<std::uint32_t, 4> controls{0x1900, 0x1d80, 0x7900, 0x1800};
	const auto fill = [&](CpuState &cpu, std::size_t index, unsigned element,
		std::size_t value, bool upper) {
		auto &bytes = cpu.vectors[index];
		const std::size_t size = element / 8U;
		for (std::size_t lane = 0; lane < 32U / size; ++lane) {
			/* Upper: the value only in the top lane of each half. */
			const bool special = !upper ||
				(lane + 1U) % (16U / size) == 0;
			if (element == 32) {
				const auto bits = special ? singles[value] : singles[0];
				std::memcpy(bytes.data() + lane * size, &bits, size);
			} else {
				const auto bits = special ? doubles[value] : doubles[0];
				std::memcpy(bytes.data() + lane * size, &bits, size);
			}
		}
	};
	const auto run = [](TestMemory &memory, CpuState &cpu, bool checked) {
		gadgets::Executor executor;
		executor.prepare(memory);
		gadgets::ExecuteResult result{};
		for (int dispatch = 0; dispatch < 8; ++dispatch) {
			const unsigned mask = checked ? unsigned{gadgets::check_simd} :
				gadgets::checked_exceptions(cpu);
			const auto built = gadgets::build(memory, cpu.rip,
				ExecutionMode::long64, 64, {}, mask);
			if (built.status != gadgets::BuildStatus::built)
				return gadgets::ExecuteResult{};
			result = mask == 0 ?
				gadgets::execute(built.program, cpu, memory, executor, 64) :
				gadgets::execute_checked(built.program, cpu, memory,
					executor, 64, {}, false, mask);
			if (result.event != uml::cpu::EngineEvent::continued)
				break;
		}
		return result;
	};
	std::size_t compared{};
	std::size_t faults{};
	for (const auto &test : cases) {
		std::vector<std::byte> code(test.bytes.size() + 2U);
		std::memcpy(code.data(), test.bytes.data(), test.bytes.size());
		code[test.bytes.size()] = std::byte{0x0f};
		code[test.bytes.size() + 1U] = std::byte{0x05};
		for (const auto control : controls)
		for (std::size_t left = 0; left < singles.size(); ++left)
		for (std::size_t right = 0; right < singles.size(); ++right)
		for (const bool upper : {false, true}) {
			CpuState start{};
			start.mxcsr = control;
			start.gpr(Register::rbx) = 0x800;
			fill(start, 0, test.element, left, upper);
			fill(start, 1, test.element, right, upper);
			fill(start, 2, test.element, right, upper);
			std::array<CpuState, 2> cpu{start, start};
			std::array<gadgets::ExecuteResult, 2> result{};
			std::array<std::array<std::byte, 32>, 2> stored{};
			for (std::size_t path = 0; path < 2; ++path) {
				TestMemory memory;
				memory.fill(0, code);
				memory.fill(0x800, std::span<const std::byte>{
					start.vectors[1].data(), 32});
				result[path] = run(memory, cpu[path], path == 1);
				(void)memory.read(0x800, stored[path]);
			}
			const auto flags = [](std::uint32_t mxcsr) {
				return mxcsr & ~0x02U;
			};
			const bool trap = result[0].event ==
				uml::cpu::EngineEvent::floating_point_trap;
			faults += trap ? 1U : 0U;
			++compared;
			if (result[0].event == uml::cpu::EngineEvent::system_call &&
				result[1].event == uml::cpu::EngineEvent::system_call &&
				cpu[0].rip == cpu[1].rip &&
				cpu[0].vectors == cpu[1].vectors &&
				cpu[0].registers == cpu[1].registers &&
				cpu[0].mmx == cpu[1].mmx &&
				cpu[0].rflags == cpu[1].rflags &&
				flags(cpu[0].mxcsr) == flags(cpu[1].mxcsr) &&
				stored[0] == stored[1])
				continue;
			if (trap && result[1].event == result[0].event &&
				result[0].vector == result[1].vector &&
				result[0].error == result[1].error &&
				cpu[0].rip == cpu[1].rip &&
				cpu[0].vectors == cpu[1].vectors &&
				cpu[0].registers == cpu[1].registers &&
				cpu[0].mxcsr == cpu[1].mxcsr && stored[0] == stored[1])
				continue;
			std::fprintf(stderr,
				"x86-64 gadget bootstrap: %.*s mxcsr %#x left %zu right "
				"%zu upper %d: event %d/%d vector %u/%u error %#llx/%#llx "
				"rip %#llx/%#llx mxcsr %#x/%#x\n",
				static_cast<int>(test.name.size()), test.name.data(),
				control, left, right, upper ? 1 : 0,
				static_cast<int>(result[0].event),
				static_cast<int>(result[1].event),
				static_cast<unsigned>(result[0].vector),
				static_cast<unsigned>(result[1].vector),
				static_cast<unsigned long long>(result[0].error),
				static_cast<unsigned long long>(result[1].error),
				static_cast<unsigned long long>(cpu[0].rip),
				static_cast<unsigned long long>(cpu[1].rip),
				cpu[0].mxcsr, cpu[1].mxcsr);
			fail("the gadgets' SIMD checks disagree with the checked path");
		}
	}
	require(faults > compared / 8U && faults < compared,
		"the SIMD check comparison raised too few or too many faults");
}

/* LDMXCSR leaves only when what a run checks changes: when it unmasks an
 * exception while the gadgets do not check SIMD exceptions, or unmasks
 * underflow or precision, which only a checked run sees. */
void mxcsr_loads_leave_when_checks_change()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x0f, 0xae, 0x13, // ldmxcsr [rbx]
		0x48, 0xff, 0xc0, // inc rax
		0x0f, 0x05,       // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"LDMXCSR block did not build");
	CpuState cpu{};
	const auto run = [&](std::uint32_t before, std::uint32_t loaded) {
		std::array<std::byte, 4> bytes{};
		std::memcpy(bytes.data(), &loaded, sizeof(loaded));
		memory.fill(0x800, bytes);
		cpu = CpuState{};
		cpu.mxcsr = before;
		cpu.gpr(Register::rbx) = 0x800;
		return gadgets::execute(built.program, cpu, memory, 64);
	};
	const auto checking = run(0x1900, 0x1d00);
	require(checking.event == uml::cpu::EngineEvent::system_call &&
			cpu.mxcsr == 0x1d00 && cpu.gpr(Register::rax) == 1,
		"LDMXCSR left while the gadgets already checked SIMD exceptions");
	const auto unchecked = run(0x1f80, 0x1900);
	require(unchecked.event == uml::cpu::EngineEvent::continued &&
			unchecked.retired_instructions == 1 && cpu.rip == 3 &&
			cpu.mxcsr == 0x1900,
		"LDMXCSR $1900 did not leave when the gadgets were not checking");
	const auto precision = run(0x1900, 0x0900);
	require(precision.event == uml::cpu::EngineEvent::continued &&
			precision.retired_instructions == 1 && cpu.rip == 3,
		"LDMXCSR unmasking precision did not leave");
	const auto denormal = run(0x1900, 0x1800);
	require(denormal.event == uml::cpu::EngineEvent::continued &&
			denormal.retired_instructions == 1 && cpu.rip == 3,
		"LDMXCSR unmasking a denormal operand did not leave");
	const auto masked = run(0x0900, 0x1f80);
	require(masked.event == uml::cpu::EngineEvent::system_call &&
			cpu.mxcsr == 0x1f80 && cpu.gpr(Register::rax) == 1,
		"LDMXCSR masking everything left");
}

/* XRSTOR, LDMXCSR and FXRSTOR that set MXCSR.DAZ or FTZ within a run send
 * the conversions after them to the SIMD gadgets' exact path, and an XRSTOR
 * that initializes the SSE state clears both. */
void mxcsr_loads_switch_the_simd_exact_path()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x0f, 0xae, 0x2b,             // xrstor [rbx]
		0xf3, 0x0f, 0x5a, 0xc8,       // cvtss2sd xmm1, xmm0
		0x66, 0x49, 0x0f, 0x7e, 0xc8, // movq r8, xmm1
		0x0f, 0xae, 0x16,             // ldmxcsr [rsi]
		0xf3, 0x0f, 0x5a, 0xd0,       // cvtss2sd xmm2, xmm0
		0x66, 0x49, 0x0f, 0x7e, 0xd1, // movq r9, xmm2
		0x0f, 0xae, 0x0f,             // fxrstor [rdi]
		0xf2, 0x0f, 0x5a, 0xdc,       // cvtsd2ss xmm3, xmm4
		0x66, 0x41, 0x0f, 0x7e, 0xda, // movd r10d, xmm3
		0x0f, 0xae, 0x56, 0x04,       // ldmxcsr [rsi+4]
		0xf3, 0x0f, 0x5a, 0xe8,       // cvtss2sd xmm5, xmm0
		0x66, 0x49, 0x0f, 0x7e, 0xed, // movq r13, xmm5
		0x0f, 0x05,                   // syscall
	});
	memory.load(0x400, {
		0x0f, 0xae, 0x2b,             // xrstor [rbx]
		0x66, 0x41, 0x0f, 0x6e, 0xc4, // movd xmm0, r12d
		0xf3, 0x0f, 0x5a, 0xc8,       // cvtss2sd xmm1, xmm0
		0x66, 0x49, 0x0f, 0x7e, 0xc8, // movq r8, xmm1
		0x0f, 0x05,                   // syscall
	});
	const auto put = [&](std::uint64_t address, auto value) {
		std::array<std::byte, sizeof(value)> bytes{};
		std::memcpy(bytes.data(), &value, sizeof(value));
		memory.fill(address, bytes);
	};
	/* The XSAVE image at 0x1000 holds the SSE state, MXCSR with DAZ and a
	 * denormal single in xmm0; the FXSAVE image at 0x2000 MXCSR with FTZ,
	 * that single in xmm0 and in xmm4 a double that narrows to a denormal.
	 * LDMXCSR loads $1f80 and then $1fc0. */
	put(0x1000, std::uint16_t{0x037f});
	put(0x1000 + 24, std::uint32_t{0x1fc0});
	put(0x1000 + 160, std::uint32_t{1});
	put(0x1000 + 512, std::uint64_t{2});
	put(0x2000, std::uint16_t{0x037f});
	put(0x2000 + 24, std::uint32_t{0x9f80});
	put(0x2000 + 160, std::uint32_t{1});
	put(0x2000 + 160 + 4 * 16, std::uint64_t{0x37d0000000000000ULL});
	put(0x3000, std::uint32_t{0x1f80});
	put(0x3004, std::uint32_t{0x1fc0});
	constexpr std::uint64_t denormal_single_widened = 0x36a0000000000000ULL;
	const auto program = require_built(gadgets::build(memory, 0),
		"MXCSR-loading block did not build");
	CpuState cpu{};
	cpu.gpr(Register::rax) = 2;
	cpu.gpr(Register::rbx) = 0x1000;
	cpu.gpr(Register::rsi) = 0x3000;
	cpu.gpr(Register::rdi) = 0x2000;
	cpu.gpr(Register::r8) = ~0ULL;
	cpu.gpr(Register::r9) = ~0ULL;
	cpu.gpr(Register::r10) = ~0ULL;
	cpu.gpr(Register::r13) = ~0ULL;
	const auto loaded = gadgets::execute(program.program, cpu, memory, 64);
	require(loaded.event == uml::cpu::EngineEvent::system_call &&
			cpu.mxcsr == 0x1fc0,
		"the MXCSR-loading block did not run to its end");
	require(cpu.gpr(Register::r8) == 0,
		"a conversion after XRSTOR set DAZ kept its denormal input");
	require(cpu.gpr(Register::r9) == denormal_single_widened,
		"a conversion after LDMXCSR cleared DAZ flushed its input");
	require(cpu.gpr(Register::r10) == 0,
		"a narrowing after FXRSTOR set FTZ kept its denormal result");
	require(cpu.gpr(Register::r13) == 0,
		"a conversion after LDMXCSR set DAZ kept its denormal input");
	/* An XSTATE_BV of zero initializes the vector registers, and RFBM still
	 * selects the SSE component, so XRSTOR loads the image's MXCSR; the
	 * image now holds one with DAZ clear over the DAZ set before it. */
	put(0x1000 + 512, std::uint64_t{0});
	put(0x1000 + 24, std::uint32_t{0x1f80});
	const auto initial = require_built(gadgets::build(memory, 0x400),
		"XRSTOR initializing block did not build");
	cpu = CpuState{};
	cpu.rip = 0x400;
	cpu.mxcsr = 0x1fc0;
	cpu.gpr(Register::rax) = 2;
	cpu.gpr(Register::rbx) = 0x1000;
	cpu.gpr(Register::r12) = 1;
	const auto cleared = gadgets::execute(initial.program, cpu, memory, 64);
	require(cleared.event == uml::cpu::EngineEvent::system_call &&
			cpu.mxcsr == 0x1f80 &&
			cpu.gpr(Register::r8) == denormal_single_widened,
		"a conversion after XRSTOR loaded an MXCSR with DAZ clear "
		"flushed its input");
}

/* COMISD raises invalid operation for a NaN, and its #XM leaves RFLAGS as
 * the instruction before it made them, even while those are still only
 * recorded. */
void faulting_comisd_keeps_earlier_flags()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x48, 0x39, 0xd8,       // cmp rax, rbx: 1 - 2 sets CF, PF, AF, SF
		0x66, 0x0f, 0x2f, 0xc1, // comisd xmm0, xmm1
		0x0f, 0x05,             // syscall
	});
	CpuState cpu{};
	cpu.mxcsr = 0x1900;
	cpu.gpr(Register::rax) = 1;
	cpu.gpr(Register::rbx) = 2;
	const std::uint64_t nan = 0x7ff8'0000'0000'0000ULL;
	std::memcpy(cpu.vectors[0].data(), &nan, sizeof(nan));
	const auto trap = run_until_event(memory, cpu);
	materialize_flags(cpu);
	require(trap.event == uml::cpu::EngineEvent::floating_point_trap &&
			trap.vector == 19 && cpu.rip == 3 && cpu.mxcsr == 0x1901 &&
			(cpu.rflags & 0x8d5U) == 0x95U,
		"a faulting COMISD lost the flags the instruction before it set");
}

/* While RFLAGS.AC is set, a misaligned access raises #AC before it
 * happens, and POPF that sets AC leaves so the runtime checks what
 * follows. */
void alignment_checks_follow_rflags_ac()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x8b, 0x43, 0x01, // mov eax, [rbx + 1]
		0x8b, 0x43, 0x04, // mov eax, [rbx + 4]
		0x0f, 0x05,       // syscall
	});
	CpuState cpu{};
	cpu.rflags = 0x2 | (1ULL << 18U);
	cpu.gpr(Register::rbx) = 0x800;
	const auto first = run_until_event(memory, cpu);
	require(first.event == uml::cpu::EngineEvent::memory_fault &&
			first.fault.alignment && first.retired_instructions == 0 &&
			cpu.rip == 0,
		"a misaligned load under AC did not raise #AC");
	cpu.gpr(Register::rbx) = 0x7ff;
	const auto second = run_until_event(memory, cpu);
	require(second.event == uml::cpu::EngineEvent::memory_fault &&
			second.fault.alignment && cpu.rip == 3,
		"an aligned load under AC faulted, or the next did not");

	TestMemory flags;
	flags.load(0, {
		0x68, 0x02, 0x00, 0x04, 0x00, // push 0x40002 (AC)
		0x9d,                         // popfq
		0x48, 0xff, 0xc0,             // inc rax
		0x0f, 0x05,                   // syscall
	});
	const auto built = require_built(gadgets::build(flags, 0),
		"POPF block did not build");
	cpu = CpuState{};
	cpu.gpr(Register::rsp) = 0x200;
	const auto set = gadgets::execute(built.program, cpu, flags, 64);
	require(set.event == uml::cpu::EngineEvent::continued &&
			set.retired_instructions == 2 && cpu.rip == 6 &&
			(cpu.rflags & (1ULL << 18U)) != 0 &&
			cpu.gpr(Register::rax) == 0,
		"POPF setting AC did not leave at the next instruction");
}

/* INT 3 and INT 4 are the user-callable software interrupts besides 0x80:
 * both trap after the instruction. */
void software_interrupt_vectors_trap()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(100, {0xcd, 0x03});
	memory.load(200, {0xcd, 0x04});
	const auto int3 = gadgets::build(memory, 100, 1);
	const auto int4 = gadgets::build(memory, 200, 1);
	require(int3.status == gadgets::BuildStatus::built &&
		int4.status == gadgets::BuildStatus::built,
		"INT 3 or INT 4 did not build");
	CpuState breakpoint{};
	breakpoint.rip = 100;
	const auto breakpoint_result = gadgets::execute(
		int3.program, breakpoint, memory, 1);
	CpuState overflow{};
	overflow.rip = 200;
	const auto overflow_result = gadgets::execute(
		int4.program, overflow, memory, 1);
	require(breakpoint_result.event ==
			uml::cpu::EngineEvent::breakpoint && breakpoint.rip == 102,
		"INT 3 did not trap after the instruction");
	require(overflow_result.event == uml::cpu::EngineEvent::overflow &&
		overflow.rip == 202,
		"INT 4 did not raise the overflow trap with OF clear");
}

/* PALIGNR, PSRLDQ and PSLLDQ take a byte count as a full eight-bit immediate.
 * Every count that reaches the operand's size discards the whole lane, so the
 * only architecturally visible answer is zero.  The gadgets index their
 * operands through a byte-wise table lookup whose index arithmetic wraps at
 * 256, so a count of 241 or more used to fold back into the table and
 * resurrect one byte per lane.  Counts below the size must keep working.
 */
void byte_shifts_discard_a_lane_for_an_oversized_count()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xc4, 0xe3, 0x7d, 0x0f, 0xd1, 0xf1, // vpalignr $0xf1,ymm1,ymm0,ymm2
		0xc5, 0xe5, 0x73, 0xd8, 0xf1,       // vpsrldq  $0xf1,ymm0,ymm3
		0xc5, 0xdd, 0x73, 0xf8, 0xf1,       // vpslldq  $0xf1,ymm0,ymm4
		0x66, 0x0f, 0x73, 0xdd, 0xf1,       // psrldq   $0xf1,xmm5
		0x66, 0x0f, 0x73, 0xfe, 0xf1,       // pslldq   $0xf1,xmm6
		0xc4, 0xe3, 0x7d, 0x0f, 0xf9, 0x11, // vpalignr $0x11,ymm1,ymm0,ymm7
		0xc4, 0x63, 0x7d, 0x0f, 0xc1, 0x05, // vpalignr $0x05,ymm1,ymm0,ymm8
		0x0f, 0x05,                         // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"oversized byte-shift block did not build");
	CpuState cpu{};
	for (std::size_t lane = 0; lane < 32; ++lane) {
		cpu.vectors[0][lane] = static_cast<std::byte>(0x10U + lane);
		cpu.vectors[1][lane] = static_cast<std::byte>(0x80U + lane);
	}
	constexpr auto legacy_high = std::byte{0xc7};
	for (std::size_t lane = 16; lane < 32; ++lane) {
		cpu.vectors[5][lane] = legacy_high;
		cpu.vectors[6][lane] = legacy_high;
	}
	const auto result = gadgets::execute(built.program, cpu, memory, 8);
	require(result.event == uml::cpu::EngineEvent::system_call &&
		result.retired_instructions == 8 && cpu.rip == 40,
		"oversized byte-shift block did not reach SYSCALL");

	/* PALIGNR's window is the r/m operand followed by the VEX operand, one
	 * 128-bit lane at a time; a source byte past that pair reads as zero. */
	const auto aligned = [&cpu](std::size_t lane, std::size_t index,
			std::size_t count) {
		const std::size_t at = index + count;
		if (at >= 32)
			return std::byte{};
		return at < 16 ? cpu.vectors[1][lane * 16 + at] :
			cpu.vectors[0][lane * 16 + at - 16];
	};
	for (std::size_t lane = 0; lane < 2; ++lane)
	for (std::size_t index = 0; index < 16; ++index) {
		const auto at = lane * 16 + index;
		require(cpu.vectors[2][at] == std::byte{},
			"VPALIGNR with a count of 241 kept a byte");
		require(cpu.vectors[3][at] == std::byte{},
			"VPSRLDQ with a count of 241 kept a byte");
		require(cpu.vectors[4][at] == std::byte{},
			"VPSLLDQ with a count of 241 kept a byte");
		require(cpu.vectors[7][at] == aligned(lane, index, 0x11),
			"VPALIGNR with a count of 17 is wrong");
		require(cpu.vectors[8][at] == aligned(lane, index, 0x05),
			"VPALIGNR with a count of 5 is wrong");
	}
	for (std::size_t index = 0; index < 16; ++index) {
		require(cpu.vectors[5][index] == std::byte{},
			"PSRLDQ with a count of 241 kept a byte");
		require(cpu.vectors[6][index] == std::byte{},
			"PSLLDQ with a count of 241 kept a byte");
	}
	/* A legacy SSE shift leaves the upper YMM lane alone. */
	for (std::size_t index = 16; index < 32; ++index) {
		require(cpu.vectors[5][index] == legacy_high &&
			cpu.vectors[6][index] == legacy_high,
			"a legacy byte shift disturbed the upper YMM lane");
	}
}

/* Clang's AVX2 path uses VMOVLHPS with the r/m source aliased to the
 * destination while saving its lexer stack.  The gadget must capture that
 * source lane before replacing the destination with the VEX merge operand.
 */
void vex_vmovlhps_preserves_aliased_source()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xc5, 0xe8, 0x16, 0xc9, // vmovlhps xmm1,xmm2,xmm1
		0x0f, 0x05,             // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"aliased VMOVLHPS sequence did not build");
	CpuState cpu{};
	constexpr std::uint64_t original_destination_low = 0x1122'3344'5566'7788;
	constexpr std::uint64_t merge_low = 0x0102'0304'0506'0708;
	cpu.vectors[1].fill(std::byte{0xa5});
	cpu.vectors[2].fill(std::byte{0x5a});
	std::memcpy(cpu.vectors[1].data(), &original_destination_low,
		sizeof(original_destination_low));
	std::memcpy(cpu.vectors[2].data(), &merge_low, sizeof(merge_low));
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	std::array<std::uint64_t, 4> actual{};
	std::memcpy(actual.data(), cpu.vectors[1].data(), sizeof(actual));
	require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 2 && cpu.rip == 6,
			"aliased VMOVLHPS did not reach SYSCALL");
	require(actual[0] == merge_low &&
		actual[1] == original_destination_low,
		"aliased VMOVLHPS read its source after writing the destination");
	require(actual[2] == 0 && actual[3] == 0,
		"VEX VMOVLHPS did not clear the upper YMM lane");
}

/* How many operands a block takes from the vector result registers instead of
 * from the state slot. Only a vector register form's descriptor sets bit 62;
 * an entry address, a displacement, a guest address and a length word all
 * leave it clear. */
[[nodiscard]] std::size_t forwarded_vector_operands(
	std::initializer_list<std::uint8_t> code)
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, code);
	const auto built = gadgets::build(memory, 0);
	if (built.status != gadgets::BuildStatus::built)
		return ~std::size_t{};
	std::size_t forwarded{};
	for (const auto word : built.program.native_stream())
		if ((word & (std::uintptr_t{1} << 62U)) != 0)
			++forwarded;
	return forwarded;
}

/* A vector result is left in host registers that the calling convention lets
 * any call destroy. A flag-neutral baked transfer hands the producer's host
 * flags to the next consumer, and the bank's miss tails save NZCV around the
 * calls they make -- but nothing saves the vector registers, so a transfer
 * that reaches guest memory must not hand a vector result on as well. */
void vector_results_cross_only_a_transfer_that_stays_inline()
{
	/* vpaddb ymm0,ymm1,ymm2; <transfer>; vpaddb ymm3,ymm0,ymm4; syscall */
	static constexpr std::size_t taken = 1;
	struct Case final {
		std::initializer_list<std::uint8_t> code;
		std::size_t expected;
		const char *message;
	};
	const Case cases[] = {
		{{0xc5, 0xf5, 0xfc, 0xc2, 0xc5, 0xfd, 0xfc, 0xdc, 0x0f, 0x05},
			taken,
			"a vector operand was not taken from the gadget before it"},
		{{0xc5, 0xf5, 0xfc, 0xc2, 0x48, 0x89, 0xd8,
			0xc5, 0xfd, 0xfc, 0xdc, 0x0f, 0x05}, taken,
			"a register MOV stopped a vector operand being forwarded"},
		{{0xc5, 0xf5, 0xfc, 0xc2, 0x48, 0x8d, 0x47, 0x08,
			0xc5, 0xfd, 0xfc, 0xdc, 0x0f, 0x05}, taken,
			"LEA stopped a vector operand being forwarded"},
		{{0xc5, 0xf5, 0xfc, 0xc2, 0x48, 0x8b, 0x07,
			0xc5, 0xfd, 0xfc, 0xdc, 0x0f, 0x05}, 0,
			"a vector operand was taken across a load"},
		{{0xc5, 0xf5, 0xfc, 0xc2, 0x48, 0x89, 0x07,
			0xc5, 0xfd, 0xfc, 0xdc, 0x0f, 0x05}, 0,
			"a vector operand was taken across a store"},
		{{0xc5, 0xf5, 0xfc, 0xc2, 0x0f, 0xb6, 0x07,
			0xc5, 0xfd, 0xfc, 0xdc, 0x0f, 0x05}, 0,
			"a vector operand was taken across a widening load"},
		{{0xc5, 0xf5, 0xfc, 0xc2, 0x50,
			0xc5, 0xfd, 0xfc, 0xdc, 0x0f, 0x05}, 0,
			"a vector operand was taken across a PUSH"},
		{{0xc5, 0xf5, 0xfc, 0xc2, 0x58,
			0xc5, 0xfd, 0xfc, 0xdc, 0x0f, 0x05}, 0,
			"a vector operand was taken across a POP"},
	};
	for (const auto &item : cases)
		require(forwarded_vector_operands(item.code) == item.expected,
			item.message);
}

/* The same rule, executed. The address space's own read of a page-crossing
 * operand runs C++, which is free to use every caller-saved host vector
 * register, including the two a vector gadget leaves its result in. */
/* Every helper a gadget calls is an ordinary function, so it may leave
 * anything in the caller-saved vector registers. This memory leaves a value
 * no test stores, on both the slow paths a gadget reaches: the page-crossing
 * copy and the software TLB's miss, which resolves a guest page through
 * `atomic_pointer`.
 */
class CallingOutMemory final : public TestMemory {
public:
	using TestMemory::TestMemory;

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		dirty_call_clobbered_vectors();
		return TestMemory::read(address, destination);
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, uml::memory::MemoryAccess access) override
	{
		dirty_call_clobbered_vectors();
		return TestMemory::atomic_pointer(address, size, access);
	}

private:
	static void dirty_call_clobbered_vectors() noexcept
	{
		asm volatile(
			"movi v0.16b, #0xaa\n\t"
			"movi v1.16b, #0xaa\n\t"
			"movi v2.16b, #0xaa\n\t"
			"movi v3.16b, #0xaa\n\t"
			"movi v4.16b, #0xaa\n\t"
			"movi v5.16b, #0xaa\n\t"
			"movi v6.16b, #0xaa\n\t"
			"movi v7.16b, #0xaa"
			::: "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
	}
};

void a_call_out_between_vector_gadgets_keeps_the_result()
{
	using namespace uml::cpu::x86_64;
	CallingOutMemory memory;
	memory.load(0, {
		0xc5, 0xf5, 0xfc, 0xc2, // vpaddb ymm0,ymm1,ymm2
		0x48, 0x8b, 0x07,       // mov rax,[rdi]: crosses a page
		0xc5, 0xfd, 0xfc, 0xdc, // vpaddb ymm3,ymm0,ymm4
		0x0f, 0x05,             // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"the calling-out vector sequence did not build");
	CpuState cpu{};
	cpu.registers[7] = 0x0ffc; // rdi, four bytes short of the next page
	cpu.vectors[1].fill(std::byte{0x11});
	cpu.vectors[2].fill(std::byte{0x22});
	cpu.vectors[4].fill(std::byte{0x33});
	const auto result = gadgets::execute(built.program, cpu, memory, 4);
	bool exact = true;
	for (const auto byte : cpu.vectors[3])
		exact = exact && byte == std::byte{0x66};
	require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 4,
			"the calling-out vector sequence did not reach SYSCALL");
	require(exact,
		"a vector operand survived a call out between two gadgets");
}

void a_lane_extract_store_keeps_the_lane_across_a_miss()
{
	using namespace uml::cpu::x86_64;
	CallingOutMemory memory;
	memory.load(0, {
		0xc4, 0xe3, 0x7d, 0x39, 0x07, 0x01, // vextracti128 $1,ymm0,[rdi]
		0x0f, 0x05,                         // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"the lane-extract store did not build");
	CpuState cpu{};
	cpu.registers[7] = 0x2000; // rdi, a page no translation covers yet
	std::fill(cpu.vectors[0].begin(), cpu.vectors[0].begin() + 16,
		std::byte{0x11});
	std::fill(cpu.vectors[0].begin() + 16, cpu.vectors[0].end(),
		std::byte{0x77});
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	bool exact = true;
	for (unsigned index = 0; index < 16; ++index)
		exact = exact && memory.byte(0x2000 + index) == 0x77U;
	require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 2,
			"the lane-extract store did not reach SYSCALL");
	require(exact,
		"a lane extract stored something other than its upper lane");
}

void a_lane_extract_store_keeps_the_lane_across_a_page()
{
	using namespace uml::cpu::x86_64;
	CallingOutMemory memory;
	memory.load(0, {
		0xc4, 0xe3, 0x7d, 0x39, 0x07, 0x01, // vextracti128 $1,ymm0,[rdi]
		0x0f, 0x05,                         // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"the crossing lane-extract store did not build");
	CpuState cpu{};
	cpu.registers[7] = 0x1ff8; // rdi, eight bytes short of the next page
	std::fill(cpu.vectors[0].begin(), cpu.vectors[0].begin() + 16,
		std::byte{0x11});
	std::fill(cpu.vectors[0].begin() + 16, cpu.vectors[0].end(),
		std::byte{0x77});
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	bool exact = true;
	for (unsigned index = 0; index < 16; ++index)
		exact = exact && memory.byte(0x1ff8 + index) == 0x77U;
	require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 2,
			"the crossing lane-extract store did not reach SYSCALL");
	require(exact,
		"a lane extract across a page stored something else");
}

void a_masked_store_keeps_the_unselected_bytes_across_a_miss()
{
	using namespace uml::cpu::x86_64;
	CallingOutMemory memory;
	memory.load(0, {
		0x66, 0x0f, 0xf7, 0xc1, // maskmovdqu xmm0,xmm1: [rdi] under a mask
		0x0f, 0x05,             // syscall
	});
	for (unsigned index = 0; index < 16; ++index)
		memory.load(0x2000 + index, {0x5a});
	const auto built = require_built(gadgets::build(memory, 0),
		"the masked store did not build");
	CpuState cpu{};
	cpu.registers[7] = 0x2000; // rdi, a page no translation covers yet
	std::fill(cpu.vectors[0].begin(), cpu.vectors[0].begin() + 16,
		std::byte{0x33});
	for (unsigned index = 0; index < 16; ++index)
		cpu.vectors[1][index] = static_cast<std::byte>(
			(index % 2) == 0 ? 0x80U : 0x00U);
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	bool exact = true;
	for (unsigned index = 0; index < 16; ++index)
		exact = exact && memory.byte(0x2000 + index) ==
			((index % 2) == 0 ? 0x33U : 0x5aU);
	require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 2,
			"the masked store did not reach SYSCALL");
	require(exact,
		"a masked store did not leave the unselected bytes alone");
}

void vector_invalid_results_use_x86_indefinite_nan()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xf2, 0x0f, 0x5e, 0xc1, // divsd xmm0,xmm1: +0 / +0
		0xf3, 0x0f, 0x51, 0xd3, // sqrtss xmm2,xmm3: sqrt(-1)
		0xf3, 0x0f, 0x52, 0xe5, // rsqrtss xmm4,xmm5: 1 / sqrt(-4)
		0x0f, 0x05,             // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"invalid vector floating-point sequence did not build");
	CpuState cpu{};
	constexpr std::uint32_t negative_one = 0xbf80'0000;
	constexpr std::uint32_t negative_four = 0xc080'0000;
	std::memcpy(cpu.vectors[3].data(), &negative_one, sizeof(negative_one));
	std::memcpy(cpu.vectors[5].data(), &negative_four, sizeof(negative_four));
	const auto result = gadgets::execute(built.program, cpu, memory, 4);
	std::uint64_t quotient{};
	std::uint32_t square_root{};
	std::uint32_t reciprocal_square_root{};
	std::memcpy(&quotient, cpu.vectors[0].data(), sizeof(quotient));
	std::memcpy(&square_root, cpu.vectors[2].data(), sizeof(square_root));
	std::memcpy(&reciprocal_square_root, cpu.vectors[4].data(),
		sizeof(reciprocal_square_root));
	require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 4 && cpu.rip == 14,
			"invalid vector floating-point sequence did not reach SYSCALL");
	require(quotient == 0xfff8'0000'0000'0000ULL,
		"DIVSD 0/0 did not return the x86 double indefinite NaN");
	require(square_root == 0xffc0'0000,
		"SQRTSS of a negative value did not return the x86 indefinite NaN");
	require(reciprocal_square_root == 0xffc0'0000,
		"RSQRTSS of a negative value did not return the x86 indefinite NaN");
}

/* A packed-integer register form leaves its result in v4, and in v5 with it at
 * 256 bits, and bit 62 of the next gadget's descriptor takes its left operand
 * from there instead of reading the state slot back. The bit is only sound
 * while the producer really is the gadget immediately before: anything between
 * the two withdraws it, and a block that begins at the consumer -- which is
 * what a fault between the two leaves behind, because the handler returns to
 * the consumer and the engine translates a fresh block there -- must forward
 * nothing, since v4 then holds whatever last ran.
 */
void vector_integer_forwards_only_from_the_gadget_before()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uintptr_t forward_left = std::uintptr_t{1} << 62U;
	const auto seed = [](CpuState &cpu, std::size_t index,
		std::uint32_t base) noexcept {
		for (std::size_t position = 0; position < 8U; ++position) {
			const auto value = static_cast<std::uint32_t>(
				base + position * 0x11U);
			std::memcpy(cpu.vectors[index].data() + position * 4U,
				&value, sizeof(value));
		}
	};
	const auto lane = [](const CpuState &cpu, std::size_t index,
		std::size_t position) noexcept {
		std::uint32_t value{};
		std::memcpy(&value, cpu.vectors[index].data() + position * 4U,
			sizeof(value));
		return value;
	};
	/* Subtraction tells the two operands apart, and the lanes differ so that
	 * a path taking v4 where it owes v5 shows up.
	 */
	const auto run = [&](std::initializer_list<std::uint8_t> code,
		std::uint64_t entry, std::size_t instructions, CpuState &cpu,
		std::uintptr_t &consumer) {
		TestMemory memory;
		memory.load(0, code);
		const auto built = gadgets::build(memory, entry);
		if (built.status != gadgets::BuildStatus::built)
			return false;
		const auto stream = built.program.native_stream();
		const std::size_t descriptor = entry == 0 ? 3U : 1U;
		consumer = stream.size() > descriptor ? stream[descriptor] : 0U;
		cpu.rip = entry;
		seed(cpu, 1, 0x0100'0000);
		seed(cpu, 2, 0x0002'0000);
		seed(cpu, 3, 0x0033'0000);
		seed(cpu, 4, 0x4000'0000);
		seed(cpu, 5, 0x0000'0500);
		const auto result = gadgets::execute(built.program, cpu, memory,
			instructions);
		return result.event == uml::cpu::EngineEvent::system_call;
	};

	CpuState adjacent{};
	std::uintptr_t adjacent_descriptor{};
	require(run({
			0xc5, 0xf5, 0xfe, 0xda, // vpaddd ymm3,ymm1,ymm2
			0xc5, 0xe5, 0xfa, 0xc4, // vpsubd ymm0,ymm3,ymm4
			0x0f, 0x05,             // syscall
		}, 0, 3, adjacent, adjacent_descriptor),
		"forwarded left operand did not reach SYSCALL");
	require((adjacent_descriptor & forward_left) != 0,
			"a consumer of the gadget before it did not forward its left "
			"operand");
	require(lane(adjacent, 0, 0) == 0x0102'0000U - 0x4000'0000U &&
		lane(adjacent, 0, 7) == (0x0102'0000U + 14U * 0x11U) -
			(0x4000'0000U + 7U * 0x11U),
		"a forwarded left operand produced the wrong difference");

	/* VMOVDQA ymm3,ymm5 between the two: the consumer must read the slot the
	 * move has just written, not the registers the producer left.
	 */
	CpuState withdrawn{};
	std::uintptr_t ignored{};
	require(run({
			0xc5, 0xf5, 0xfe, 0xda, // vpaddd ymm3,ymm1,ymm2
			0xc5, 0xfd, 0x6f, 0xdd, // vmovdqa ymm3,ymm5
			0xc5, 0xe5, 0xfa, 0xc4, // vpsubd ymm0,ymm3,ymm4
			0x0f, 0x05,             // syscall
		}, 0, 4, withdrawn, ignored),
		"withdrawn forwarding did not reach SYSCALL");
	require(lane(withdrawn, 0, 0) == 0x0000'0500U - 0x4000'0000U &&
			lane(withdrawn, 0, 7) == (0x0000'0500U + 7U * 0x11U) -
				(0x4000'0000U + 7U * 0x11U),
			"a move between producer and consumer did not withdraw the "
			"forwarded registers");

	/* The same two instructions, entered at the consumer. */
	CpuState resumed{};
	std::uintptr_t first_descriptor{};
	require(run({
			0xc5, 0xf5, 0xfe, 0xda,
			0xc5, 0xe5, 0xfa, 0xc4,
			0x0f, 0x05,
		}, 4, 2, resumed, first_descriptor),
		"a block entered at the consumer did not reach SYSCALL");
	require((first_descriptor & forward_left) == 0,
			"the first gadget of a block forwarded an operand");
	require(lane(resumed, 0, 0) == 0x0033'0000U - 0x4000'0000U,
		"a block entered at the consumer did not read its operand from "
		"the state");
}

/* The gadget a block selects for its first instruction. */
[[nodiscard]] std::uintptr_t first_native_entry(
	std::initializer_list<std::uint8_t> code)
{
	TestMemory memory;
	memory.load(0, code);
	const auto built = uml::cpu::x86_64::gadgets::build(memory, 0);
	if (built.status != uml::cpu::x86_64::gadgets::BuildStatus::built ||
		built.program.native_stream().empty())
		return 0;
	return built.program.native_stream()[0];
}

[[nodiscard]] std::uintptr_t gadget_entry(void (*entry)()) noexcept
{
	return reinterpret_cast<std::uintptr_t>(entry);
}

/* A LOCK-prefixed memory update has to reach the exclusive-loop gadget.  The
 * plain gadget loads and stores separately, which loses a sibling vCPU's
 * update, so pin the selection rather than only the arithmetic result.
 */
void locked_memory_updates_select_exclusive_gadgets()
{
	using namespace uml::cpu::x86_64::gadgets;
	/* Every block ends in SYSCALL so it terminates at a known boundary. */
	require(first_native_entry({0xf0, 0xff, 0x00, 0x0f, 0x05}) ==
		gadget_entry(native::uml_x86_64_gadget_increment_32_m_locked),
		"LOCK INC did not select the exclusive increment gadget");
	require(first_native_entry({0xf0, 0x01, 0x08, 0x0f, 0x05}) ==
		gadget_entry(native::uml_x86_64_gadget_add_32_mr_locked),
		"LOCK ADD register did not select the exclusive gadget");
	require(first_native_entry({0xf0, 0x83, 0x00, 0x01, 0x0f, 0x05}) ==
		gadget_entry(native::uml_x86_64_gadget_add_32_mi_locked),
		"LOCK ADD immediate did not select the exclusive gadget");
	require(first_native_entry({0xf0, 0x0f, 0xab, 0x08, 0x0f, 0x05}) ==
		gadget_entry(native::uml_x86_64_gadget_bts_32_mr_locked),
		"LOCK BTS did not select the exclusive gadget");
	require(first_native_entry({0xf0, 0x0f, 0xb1, 0x08, 0x0f, 0x05}) ==
		gadget_entry(native::uml_x86_64_gadget_cmpxchg_32_mr),
		"LOCK CMPXCHG did not select the memory gadget");
	require(first_native_entry({0xf0, 0x0f, 0xc1, 0x08, 0x0f, 0x05}) ==
		gadget_entry(native::uml_x86_64_gadget_xadd_32_mr),
		"LOCK XADD did not select the memory gadget");
	require(first_native_entry({0x87, 0x08, 0x0f, 0x05}) ==
		gadget_entry(native::uml_x86_64_gadget_exchange_32_mr),
		"XCHG did not select the memory gadget");
	require(first_native_entry({0xff, 0x00, 0x0f, 0x05}) ==
		gadget_entry(native::uml_x86_64_gadget_increment_32_m),
		"unlocked INC lost its plain gadget");
	require(first_native_entry({0x01, 0x08, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_add_32_mr) ||
		first_native_entry({0x01, 0x08, 0x0f, 0x05}) ==
			baked::uml_x86_64_baked_add_32_mr[1U * baked::base_slots],
		"unlocked ADD lost its plain gadget");
}

/* glibc's pthread_rwlock_rdlock fast path adds one reader with LOCK XADD,
 * converts the returned old value to the prospective state, and checks its
 * sign. Keep the exact instruction sequence that pressure-vessel executes so
 * a width, source-register, or deferred-flag regression is observable without
 * a desktop boot.
 */
void glibc_rwlock_xadd_fast_path_is_exact()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t lock = 0x800;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x08, 0x00, 0x00, 0x00, // mov eax,8
		0xf0, 0x0f, 0xc1, 0x03,       // lock xadd [rbx],eax
		0x83, 0xc0, 0x08,             // add eax,8
		0x85, 0xc0,                   // test eax,eax
		0x0f, 0x05,                   // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0),
		"glibc rwlock XADD sequence did not build");
	CpuState cpu{};
	cpu.gpr(Register::rbx) = lock;
	const auto result = gadgets::execute(built.program, cpu, memory, 64);
	materialize_flags(cpu);
	require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 5 && cpu.rip == 16,
			"glibc rwlock XADD sequence did not reach SYSCALL");
	require(memory.dword(lock) == 8 &&
		cpu.gpr(Register::rax) == 8,
		"glibc rwlock XADD returned or stored the wrong state");
	require((cpu.rflags & (rflags_sign | rflags_zero)) == 0,
		"glibc rwlock XADD produced an impossible failure state");
}

/* GObject stores closure state in one packed dword. Its invalidation path
 * clears the eight-bit notifier count with this compiler-generated sequence
 * before a dword CMPXCHG. Pressure-vessel exposed an impossible count in this
 * exact loop, so retain the complete register-width and immediate semantics.
 */
void gobject_closure_bitfield_cmpxchg_is_exact()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t closure = 0x800;
	constexpr std::uint64_t stack = 0x1000;
	constexpr std::uint32_t initial = 0x8808'0002;
	constexpr std::uint32_t expected = 0x8800'0002;
	TestMemory memory;
	memory.load(0, {
		0x8b, 0x03,                         // mov eax,[rbx]
		0x89, 0x44, 0x24, 0x40,             // mov [rsp+0x40],eax
		0x48, 0x8b, 0x54, 0x24, 0x40,       // mov rdx,[rsp+0x40]
		0x48, 0x8b, 0x4c, 0x24, 0x40,       // mov rcx,[rsp+0x40]
		0x48, 0xc1, 0xea, 0x13,             // shr rdx,19
		0x48, 0x81, 0xe1, 0xff, 0xff, 0x07, 0xf8,
		0x83, 0xea, 0x01,                   // sub edx,1
		0x0f, 0xb6, 0xd2,                   // movzx edx,dl
		0x48, 0xc1, 0xe2, 0x13,             // shl rdx,19
		0x48, 0x09, 0xd1,                   // or rcx,rdx
		0x48, 0x89, 0x4c, 0x24, 0x40,       // mov [rsp+0x40],rcx
		0x48, 0x8b, 0x4c, 0x24, 0x40,       // mov rcx,[rsp+0x40]
		0x8b, 0x54, 0x24, 0x40,             // mov edx,[rsp+0x40]
		0x48, 0xc1, 0xe9, 0x13,             // shr rcx,19
		0x0f, 0xb6, 0xc9,                   // movzx ecx,cl
		0xf0, 0x0f, 0xb1, 0x13,             // lock cmpxchg [rbx],edx
		0x75, 0x01,                         // jne failure
		0xcc,                               // success: int3
		0x0f, 0x0b,                         // failure: ud2
	});
	const std::array<std::uint32_t, 1> closure_word{initial};
	require(memory.write(closure,
			std::as_bytes(std::span{closure_word})),
		"GObject closure state did not fit test memory");
	const auto built = require_built(gadgets::build(memory, 0),
		"GObject closure CMPXCHG sequence did not build");
	CpuState cpu{};
	cpu.gpr(Register::rbx) = closure;
	cpu.gpr(Register::rsp) = stack;
	const auto result = gadgets::execute(built.program, cpu, memory, 64);
	const auto deferred = cpu.deferred_flags;
	materialize_flags(cpu);
	require(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 17 && cpu.rip == 67,
			"GObject closure CMPXCHG took its failure branch after success");
	require(memory.dword(closure) == expected,
		"GObject closure CMPXCHG corrupted the packed notifier count");
	require(cpu.gpr(Register::rax) == initial,
		"GObject closure CMPXCHG changed EAX after success");
	require(cpu.gpr(Register::rcx) == 0,
		"GObject closure bitfield extraction produced the wrong count");
	require(cpu.gpr(Register::rdx) == expected,
		"GObject closure bitfield update produced the wrong source");
	require(deferred.operation == DeferredFlagOperation::subtract,
		"GObject closure CMPXCHG recorded the wrong flag operation");
	require(deferred.left == initial,
		"GObject closure CMPXCHG recorded the wrong expected value");
	require(deferred.right == initial,
		"GObject closure CMPXCHG recorded the wrong observed value");
	require(deferred.result == 0,
		"GObject closure CMPXCHG recorded a nonzero comparison");
	require(deferred.width == 4,
		"GObject closure CMPXCHG recorded the wrong flag width");
	require((cpu.rflags & rflags_zero) != 0,
		"GObject closure CMPXCHG lost its success flag");
}

/* XADD is implicitly atomic against memory even without relying on a distinct
 * LOCK-prefixed gadget. Exercise the same aligned dword operation from the
 * host threads that represent concurrent guest vCPUs.
 */
void xadd_survives_concurrent_vcpus()
{
	using namespace uml::cpu::x86_64;
	constexpr unsigned thread_count = 4;
	constexpr unsigned iterations = 10000;
	constexpr std::uint64_t counter = 0x800;
	TestMemory memory;
	memory.load(0, {0xf0, 0x0f, 0xc1, 0x03, 0x0f, 0x05});
	const auto built = require_built(gadgets::build(memory, 0),
		"contended XADD block did not build");
	const bool all_healthy = on_vcpus(thread_count, [&](auto &healthy) {
		CpuState cpu{};
		cpu.gpr(Register::rbx) = counter;
		for (unsigned step = 0; step < iterations; ++step) {
			cpu.rip = 0;
			cpu.gpr(Register::rax) = 8;
			const auto result = gadgets::execute(
				built.program, cpu, memory, 64);
			if (result.event != uml::cpu::EngineEvent::system_call) {
				healthy.store(false, std::memory_order_relaxed);
				return;
			}
		}
	});
	require(all_healthy,
		"a contended XADD thread left the gadget stream early");
	require(memory.dword(counter) ==
		thread_count * iterations * 8,
		"concurrent XADD lost updates");
}

/* Model the compare/exchange retry used by glibc and libstdc++ locks. A failed
 * CMPXCHG supplies the newly observed value through EAX; the next attempt uses
 * it without reading shared memory non-atomically in the test harness.
 *
 * A compare/exchange retry is lock-free: some thread succeeds, but not every
 * thread within any number of attempts. Threads that run at different speeds
 * (a slower core, a preempted vCPU) leave the slowest to lose every race
 * while the others still have work, so the test bounds no thread's retries.
 * It fails a thread that keeps failing while no thread at all succeeds. Each
 * failure means another thread changed the word, and that thread counts its
 * success before it can change the word again, so at most one change per
 * other thread goes uncounted; a count that stays still across many failures
 * means the failure path itself returns the wrong value or flags.
 */
void cmpxchg_survives_concurrent_vcpus()
{
	using namespace uml::cpu::x86_64;
	constexpr unsigned thread_count = 4;
	constexpr unsigned iterations = 5000;
	constexpr unsigned stall_limit = 1000;
	constexpr std::uint64_t counter = 0x800;
	TestMemory memory;
	memory.load(0, {0xf0, 0x0f, 0xb1, 0x0b, 0x0f, 0x05});
	const auto built = require_built(gadgets::build(memory, 0),
		"contended CMPXCHG block did not build");
	std::atomic<std::uint64_t> exchanges{0};
	const bool all_healthy = on_vcpus(thread_count, [&](auto &healthy) {
		CpuState cpu{};
		cpu.gpr(Register::rbx) = counter;
		std::uint32_t expected = 0;
		for (unsigned step = 0; step < iterations; ++step) {
			bool exchanged = false;
			auto seen = exchanges.load(std::memory_order_relaxed);
			unsigned stalled = 0;
			while (healthy.load(std::memory_order_relaxed) &&
			       stalled < stall_limit) {
				const auto desired = expected + 1;
				cpu.rip = 0;
				cpu.gpr(Register::rax) = expected;
				cpu.gpr(Register::rcx) = desired;
				const auto result = gadgets::execute(
					built.program, cpu, memory, 64);
				if (result.event != uml::cpu::EngineEvent::system_call) {
					healthy.store(false, std::memory_order_relaxed);
					return;
				}
				materialize_flags(cpu);
				if ((cpu.rflags & rflags_zero) != 0) {
					exchanges.fetch_add(1, std::memory_order_relaxed);
					expected = desired;
					exchanged = true;
					break;
				}
				expected = static_cast<std::uint32_t>(
					cpu.gpr(Register::rax));
				const auto now = exchanges.load(std::memory_order_relaxed);
				if (now != seen) {
					seen = now;
					stalled = 0;
				} else {
					++stalled;
				}
			}
			if (!exchanged) {
				healthy.store(false, std::memory_order_relaxed);
				return;
			}
		}
	});
	require(all_healthy,
		"a contended CMPXCHG retry failed with no thread succeeding");
	require(memory.dword(counter) == thread_count * iterations,
		"concurrent CMPXCHG lost successful updates");
}

/* vCPU threads execute guest code concurrently, so a guest LOCK update has to
 * stay atomic against the host threads running the other vCPUs.
 */
void locked_increment_survives_concurrent_vcpus()
{
	using namespace uml::cpu::x86_64;
	constexpr unsigned thread_count = 4;
	constexpr unsigned iterations = 20000;
	constexpr std::uint64_t counter = 0x800;
	TestMemory memory;
	memory.load(0, {0xf0, 0xff, 0x00, 0x0f, 0x05});
	const auto built = require_built(gadgets::build(memory, 0),
		"locked increment block did not build");
	const bool all_healthy = on_vcpus(thread_count, [&](auto &healthy) {
		for (unsigned step = 0; step < iterations; ++step) {
			CpuState cpu{};
			cpu.gpr(Register::rax) = counter;
			const auto result = gadgets::execute(
				built.program, cpu, memory, 64);
			if (result.event != uml::cpu::EngineEvent::system_call) {
				healthy.store(false);
				return;
			}
		}
	});
	require(all_healthy,
		"a locked increment thread left the gadget stream early");
	require(memory.dword(counter) == thread_count * iterations,
		"concurrent LOCK INC lost updates");
}

/* Where a locked operand can sit: naturally aligned, unaligned inside one
 * 16-byte granule, across a granule boundary, and across a page boundary. The
 * first two keep the exclusive path, which FEAT_LSE2 allows at any alignment
 * inside a granule, and the rest take the split lock.
 */
constexpr std::array<std::uint64_t, 4> locked_operand_addresses{
	0x800, 0x801, 0x80c, 0xffc};

/* Every locked form computes the same result wherever its operand sits and
 * whichever path that sends it down, and writes nothing outside it.
 */
void locked_forms_agree_at_every_operand_address()
{
	using namespace uml::cpu::x86_64;
	struct Form {
		const char *name;
		std::vector<std::uint8_t> code;
		std::uint64_t rax;
		std::uint64_t rcx;
		std::uint64_t rdx;
	};
	constexpr std::uint64_t low = 0x8877665544332211ULL;
	constexpr std::uint64_t high = 0x0ff0e0d0c0b0a099ULL;
	const std::vector<Form> forms{
		Form{"lock add [m], r64", {0xf0, 0x48, 0x01, 0x0e, 0x0f, 0x05},
			0, 0xf123456789abcdefULL, 0},
		Form{"lock add [m], r32", {0xf0, 0x01, 0x0e, 0x0f, 0x05},
			0, 0xf1234567U, 0},
		Form{"lock add [m], r16", {0x66, 0xf0, 0x01, 0x0e, 0x0f, 0x05},
			0, 0xf123U, 0},
		Form{"lock adc [m], r64", {0xf9, 0xf0, 0x48, 0x11, 0x0e, 0x0f, 0x05},
			0, 0x7fffffffffffffffULL, 0},
		Form{"lock inc qword [m]", {0xf0, 0x48, 0xff, 0x06, 0x0f, 0x05},
			0, 0, 0},
		Form{"lock neg qword [m]", {0xf0, 0x48, 0xf7, 0x1e, 0x0f, 0x05},
			0, 0, 0},
		Form{"xchg [m], r64", {0x48, 0x87, 0x0e, 0x0f, 0x05},
			0, 0x0123456789abcdefULL, 0},
		Form{"lock xadd [m], r64", {0xf0, 0x48, 0x0f, 0xc1, 0x0e, 0x0f, 0x05},
			0, 0x0123456789abcdefULL, 0},
		Form{"lock cmpxchg [m], r64, equal",
			{0xf0, 0x48, 0x0f, 0xb1, 0x0e, 0x0f, 0x05},
			low, 0x0123456789abcdefULL, 0},
		Form{"lock cmpxchg [m], r64, unequal",
			{0xf0, 0x48, 0x0f, 0xb1, 0x0e, 0x0f, 0x05},
			low + 1, 0x0123456789abcdefULL, 0},
		Form{"lock bts [m], r64", {0xf0, 0x48, 0x0f, 0xab, 0x0e, 0x0f, 0x05},
			0, 5, 0},
		Form{"lock cmpxchg8b, equal", {0xf0, 0x0f, 0xc7, 0x0e, 0x0f, 0x05},
			low & 0xffffffffU, 0x01234567U, low >> 32U},
		Form{"lock cmpxchg8b, unequal", {0xf0, 0x0f, 0xc7, 0x0e, 0x0f, 0x05},
			1, 0x01234567U, low >> 32U},
	};
	struct Outcome {
		std::array<std::uint64_t, 2> operand;
		std::uint64_t rax;
		std::uint64_t rcx;
		std::uint64_t rdx;
		std::uint64_t rflags;
		bool operator==(const Outcome &) const = default;
	};
	const auto run = [&](const Form &form, std::uint64_t address,
		Outcome &outcome) {
		TestMemory memory;
		memory.fill(0, std::as_bytes(std::span{form.code}));
		std::array<std::byte, 32> image;
		image.fill(std::byte{0xa5});
		std::memcpy(image.data() + 8, &low, sizeof(low));
		std::memcpy(image.data() + 16, &high, sizeof(high));
		memory.fill(address - 8, image);
		gadgets::Executor executor;
		executor.prepare(memory);
		const auto built = gadgets::build(memory, 0);
		if (built.status != gadgets::BuildStatus::built)
			return false;
		CpuState cpu{};
		cpu.gpr(Register::rax) = form.rax;
		/* The pair forms store RCX:RBX, so the operand goes through RSI. */
		cpu.gpr(Register::rbx) = 0x0fedcba987654321ULL;
		cpu.gpr(Register::rsi) = address;
		cpu.gpr(Register::rcx) = form.rcx;
		cpu.gpr(Register::rdx) = form.rdx;
		const auto result = gadgets::execute(
			built.program, cpu, memory, executor, 64);
		if (result.event != uml::cpu::EngineEvent::system_call)
			return false;
		materialize_flags(cpu);
		outcome = {{memory.qword(address), memory.qword(address + 8)},
			cpu.gpr(Register::rax), cpu.gpr(Register::rcx),
			cpu.gpr(Register::rdx), cpu.rflags};
		/* The bytes on both sides of the sixteen compared keep their fill. */
		for (std::size_t index = 0; index < 8; ++index) {
			if (memory.byte(address - 8 + index) != 0xa5 ||
				memory.byte(address + 16 + index) != 0xa5)
				return false;
		}
		return true;
	};
	for (const auto &form : forms) {
		Outcome reference{};
		require(run(form, locked_operand_addresses.front(), reference),
			form.name);
		for (const auto address : locked_operand_addresses) {
			Outcome outcome{};
			require(run(form, address, outcome) &&
				outcome == reference, form.name);
		}
	}
}

/* CMPXCHG16B compares and stores an aligned operand, and raises #GP(0) for
 * any other before it reads or writes anything, as a Xeon does, locked or
 * not. */
void compare_exchange_pair_requires_alignment()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t low = 0x8877665544332211ULL;
	constexpr std::uint64_t high = 0x0ff0e0d0c0b0a099ULL;
	for (const auto locked : {true, false}) {
		for (const auto address : locked_operand_addresses) {
			for (const auto equal : {true, false}) {
				TestMemory memory;
				std::vector<std::uint8_t> code{0x48, 0x0f, 0xc7, 0x0e,
					0x0f, 0x05};
				if (locked)
					code.insert(code.begin(), 0xf0);
				memory.fill(0, std::as_bytes(std::span{code}));
				std::array<std::byte, 16> image{};
				std::memcpy(image.data(), &low, sizeof(low));
				std::memcpy(image.data() + 8, &high, sizeof(high));
				memory.fill(address, image);
				const auto built = require_built(gadgets::build(memory, 0),
					"CMPXCHG16B block did not build");
				CpuState cpu{};
				cpu.gpr(Register::rax) = low;
				cpu.gpr(Register::rdx) = equal ? high : high + 1;
				cpu.gpr(Register::rbx) = 0x0fedcba987654321ULL;
				cpu.gpr(Register::rcx) = 0x0123456789abcdefULL;
				cpu.gpr(Register::rsi) = address;
				const auto result =
					gadgets::execute(built.program, cpu, memory, 64);
				const bool aligned = (address & 15U) == 0;
				const auto stored = memory.qword(address);
				const auto stored_high = memory.qword(address + 8);
				if (!aligned) {
					require(result.event ==
							uml::cpu::EngineEvent::general_protection &&
							result.retired_instructions == 0 &&
							cpu.rip == 0 && stored == low &&
							stored_high == high &&
							cpu.gpr(Register::rdx) ==
								(equal ? high : high + 1),
						"misaligned CMPXCHG16B did not raise #GP(0)");
					continue;
				}
				materialize_flags(cpu);
				const bool zero = (cpu.rflags & 0x40U) != 0;
				require(result.event ==
						uml::cpu::EngineEvent::system_call &&
						zero == equal &&
						stored == (equal ? 0x0fedcba987654321ULL : low) &&
						stored_high == (equal ? 0x0123456789abcdefULL :
							high) &&
						cpu.gpr(Register::rax) == low &&
						cpu.gpr(Register::rdx) == high,
					"aligned CMPXCHG16B did not compare and exchange");
			}
		}
	}
}

/* An unaligned LOCK INC stays atomic against the other vCPUs on the exclusive
 * path FEAT_LSE2 allows inside a granule and on the split lock across one.
 */
void unaligned_locked_increment_survives_concurrent_vcpus()
{
	using namespace uml::cpu::x86_64;
	constexpr unsigned thread_count = 4;
	constexpr unsigned iterations = 20000;
	for (const auto counter : locked_operand_addresses) {
		TestMemory memory;
		/* lock inc qword [rax]; syscall */
		memory.load(0, {0xf0, 0x48, 0xff, 0x00, 0x0f, 0x05});
		const auto built = require_built(gadgets::build(memory, 0),
			"unaligned locked increment block did not build");
		const bool all_healthy = on_vcpus(thread_count, [&](auto &healthy) {
			gadgets::Executor executor;
			executor.prepare(memory);
			for (unsigned step = 0; step < iterations; ++step) {
				CpuState cpu{};
				cpu.gpr(Register::rax) = counter;
				const auto result = gadgets::execute(
					built.program, cpu, memory, executor, 64);
				if (result.event !=
					uml::cpu::EngineEvent::system_call) {
					healthy.store(false);
					return;
				}
			}
		});
		require(all_healthy,
			"an unaligned locked increment left the stream early");
		require(memory.qword(counter) == thread_count * iterations,
			"concurrent unaligned LOCK INC lost updates");
	}
}

/* MASKMOVDQU writes only the bytes its mask selects, so a LOCK INC on another
 * vCPU that targets an unselected byte of the same sixteen is never undone.
 * The gadget used to read the whole block, merge and write it back. */
void mask_store_keeps_concurrent_unselected_bytes()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t block = 0x800;
	constexpr unsigned increments = 100000;
	TestMemory memory;
	/* 0: maskmovdqu xmm0, xmm1; dec ecx; jnz 0; syscall */
	memory.load(0, {0x66, 0x0f, 0xf7, 0xc1, 0xff, 0xc9, 0x75, 0xf8,
		0x0f, 0x05});
	/* 0x40: lock inc byte [rdi+1]; syscall */
	memory.load(0x40, {0xf0, 0xfe, 0x47, 0x01, 0x0f, 0x05});
	const auto increment = require_built(gadgets::build(memory, 0x40),
		"concurrent LOCK INC block did not build");
	std::atomic<bool> started{false};
	std::atomic<bool> done{false};
	std::atomic<bool> healthy{true};
	std::thread writer{[&] {
		gadgets::Executor executor;
		executor.prepare(memory, 42, 1, 1);
		/* A pass that has begun runs to its SYSCALL, and the first one
		 * runs whatever `done` says: on a loaded host this thread can
		 * be scheduled only after the increments are over. */
		do {
			started.store(true, std::memory_order_release);
			CpuState cpu{};
			cpu.gpr(Register::rdi) = block;
			cpu.gpr(Register::rcx) = 1000;
			for (std::size_t lane = 0; lane < 16; ++lane)
				cpu.vectors[0][lane] = std::byte{0x5a};
			cpu.vectors[1][0] = std::byte{0x80};
			auto event = uml::cpu::EngineEvent::continued;
			while (event == uml::cpu::EngineEvent::continued) {
				const auto selected = executor.select_program(
					memory, cpu.rip, ExecutionMode::long64);
				if (selected.program == nullptr) {
					healthy.store(false);
					return;
				}
				event = gadgets::execute(*selected.program, cpu,
					memory, executor, 1U << 20U).event;
			}
			if (event != uml::cpu::EngineEvent::system_call)
				healthy.store(false);
		} while (!done.load(std::memory_order_relaxed) &&
			healthy.load(std::memory_order_relaxed));
	}};
	/* The increments begin once the writer is in its first pass, so the
	 * two overlap rather than one ending before the other is scheduled. */
	require(uml::testing::waitUntil([&] {
			return started.load(std::memory_order_acquire);
		}, std::chrono::seconds{10}),
		"the MASKMOVDQU writer never started");
	{
		gadgets::Executor executor;
		executor.prepare(memory);
		for (unsigned step = 0; step < increments; ++step) {
			CpuState cpu{};
			cpu.rip = 0x40;
			cpu.gpr(Register::rdi) = block;
			if (gadgets::execute(increment.program, cpu, memory,
				executor, 4).event !=
				uml::cpu::EngineEvent::system_call) {
				healthy.store(false);
				break;
			}
		}
	}
	done.store(true);
	writer.join();
	require(healthy.load(),
		"concurrent MASKMOVDQU or LOCK INC left the stream");
	require(memory.byte(block) == 0x5a,
		"MASKMOVDQU did not write its selected byte");
	require(memory.byte(block + 1) == (increments & 0xffU),
		"MASKMOVDQU wrote back an unselected byte another vCPU "
		"had changed");
}

/* MASKMOVQ and MASKMOVDQU store through DS:(E/R)DI: a segment override adds
 * its base and 0x67 takes EDI, and only the selected bytes change. */
void mask_store_uses_its_segment_and_address_size()
{
	using namespace uml::cpu::x86_64;
	struct Case final {
		std::array<std::uint8_t, 5> bytes;
		std::size_t length;
		bool mmx;
		std::uint64_t rdi;
		std::uint64_t fs_base;
		std::uint64_t target;
		const char *message;
	};
	const std::array cases{
		Case{{0x66, 0x0f, 0xf7, 0xc1, 0x00}, 4, false,
			0x300, 0, 0x300, "MASKMOVDQU did not store at RDI"},
		Case{{0x64, 0x66, 0x0f, 0xf7, 0xc1}, 5, false,
			0x100, 0x1000, 0x1100,
			"MASKMOVDQU ignored its FS override"},
		Case{{0x67, 0x66, 0x0f, 0xf7, 0xc1}, 5, false,
			0xffff'ffff'0000'0200ULL, 0, 0x200,
			"MASKMOVDQU ignored its address-size prefix"},
		Case{{0x64, 0x0f, 0xf7, 0xc1, 0x00}, 4, true,
			0x180, 0x2000, 0x2180,
			"MASKMOVQ ignored its FS override"},
	};
	for (const auto &shape : cases) {
		TestMemory memory;
		for (std::size_t index = 0; index < shape.length; ++index)
			memory.load(index, {shape.bytes[index]});
		memory.load(shape.length, {0x0f, 0x05});
		for (std::size_t index = 0; index < 16; ++index) {
			memory.load(shape.target + index, {0xee});
			if (shape.rdi < 0x4000 && shape.rdi != shape.target)
				memory.load(shape.rdi + index, {0xdd});
		}
		const auto built = require_built(gadgets::build(memory, 0),
			"MASKMOV block did not build");
		CpuState cpu{};
		cpu.gpr(Register::rdi) = shape.rdi;
		cpu.fs_base = shape.fs_base;
		std::uint64_t data{};
		std::uint64_t mask{};
		for (std::size_t lane = 0; lane < 16; ++lane) {
			const auto selected = lane == 0 || lane == 5 || lane == 11;
			cpu.vectors[0][lane] = static_cast<std::byte>(0x10U + lane);
			cpu.vectors[1][lane] = selected ? std::byte{0x80} : std::byte{0x7f};
			if (lane < 8) {
				data |= std::uint64_t{0x10U + lane} << (lane * 8U);
				mask |= std::uint64_t{selected ? 0x80U : 0x7fU} <<
					(lane * 8U);
			}
		}
		cpu.mmx[0] = data;
		cpu.mmx[1] = mask;
		const auto result = gadgets::execute(built.program, cpu, memory, 4);
		require(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 2, shape.message);
		const std::size_t bytes = shape.mmx ? 8 : 16;
		for (std::size_t index = 0; index < 16; ++index) {
			const bool selected = index < bytes &&
				(index == 0 || index == 5 || index == 11);
			const auto expected = selected ?
				static_cast<std::uint8_t>(0x10U + index) : 0xeeU;
			require(memory.byte(shape.target + index) == expected,
				shape.message);
			if (shape.rdi < 0x4000 && shape.rdi != shape.target)
				require(memory.byte(shape.rdi + index) == 0xdd,
					shape.message);
		}
	}
}

/* A locked operand that faults under the split lock gives the lock back: the
 * next one, on another thread, still gets through.
 */
void faulting_split_operand_releases_the_lock()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xf0, 0xff, 0x00, 0x0f, 0x05});
	const auto built = require_built(gadgets::build(memory, 0),
		"faulting locked increment block did not build");
	CpuState cpu{};
	cpu.gpr(Register::rax) = 16384 - 2;
	const auto faulted = gadgets::execute(built.program, cpu, memory, 64);
	require(faulted.event == uml::cpu::EngineEvent::memory_fault,
		"a locked operand past the end of memory did not fault");
	std::atomic<bool> finished{false};
	std::atomic<bool> completed{false};
	std::thread successor{[&] {
		CpuState next{};
		next.gpr(Register::rax) = 0x80e;
		const auto result = gadgets::execute(
			built.program, next, memory, 64);
		completed.store(
			result.event == uml::cpu::EngineEvent::system_call);
		finished.store(true);
	}};
	require(uml::testing::waitUntil([&] { return finished.load(); },
			std::chrono::seconds{10}),
		"a faulting locked operand kept the split lock");
	successor.join();
	require(completed.load() && memory.dword(0x80e) == 1,
		"the locked operand after a fault did not complete");
}

/* x86 shows the other CPUs one CPU's stores in the order it made them. The
 * host does not: a store to a line this core already owns overtakes an earlier
 * store to a line it has to fetch, and an observer on another core sees the
 * second without the first. One vCPU stores to a line the observer owns and
 * then to a line of its own; the observer orders its own two loads, so seeing
 * the later store and missing the earlier one can only be the vCPU's doing.
 */
void guest_stores_reach_other_cpus_in_program_order()
{
	using namespace uml::cpu::x86_64;
	constexpr unsigned rounds = 40000;
	/* Both stores land in one guest page, on lines far apart, so the second
	 * finds the translation the first left and nothing but gadgets runs
	 * between them. */
	constexpr std::size_t lines = 1024;
	constexpr std::size_t line_bytes = 4096;
	constexpr std::uint64_t earlier_base = 0x10000;
	constexpr std::uint64_t later_base = earlier_base + 2048;
	TestMemory memory{0, earlier_base + lines * line_bytes};
	/* mov dword [rbx], 1; mov dword [rdx], 1; syscall */
	memory.load(0, {0xc7, 0x03, 0x01, 0x00, 0x00, 0x00,
		0xc7, 0x02, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	const auto built = require_built(gadgets::build(memory, 0),
		"ordered store block did not build");
	const auto word = [&](std::uint64_t address) {
		return reinterpret_cast<volatile std::uint32_t *>(
			memory.atomic_pointer(address, 8,
				uml::memory::MemoryAccess::write));
	};
	std::atomic<unsigned> round{0};
	std::atomic<unsigned> finished{0};
	std::atomic<bool> healthy{true};
	std::thread vcpu{[&] {
		CpuState cpu{};
		for (unsigned step = 1; step <= rounds; ++step) {
			while (round.load(std::memory_order_acquire) != step) {
			}
			const auto line = (step % lines) * line_bytes;
			/* Own the later store's line before the block runs. */
			word(later_base + line)[1] = step;
			cpu.rip = 0;
			cpu.gpr(Register::rbx) = earlier_base + line;
			cpu.gpr(Register::rdx) = later_base + line;
			const auto result = gadgets::execute(
				built.program, cpu, memory, 64);
			if (result.event != uml::cpu::EngineEvent::system_call)
				healthy.store(false, std::memory_order_relaxed);
			finished.store(step, std::memory_order_release);
		}
	}};
	unsigned overtaken = 0;
	for (unsigned step = 1; step <= rounds; ++step) {
		const auto line = (step % lines) * line_bytes;
		auto *earlier = word(earlier_base + line);
		auto *later = word(later_base + line);
		earlier[0] = 0;
		later[0] = 0;
		/* Own the earlier store's line, so the vCPU has to fetch it. */
		earlier[1] = step;
		round.store(step, std::memory_order_release);
		for (;;) {
			const auto saw_later = later[0];
			__asm__ volatile("dmb ish");
			const auto saw_earlier = earlier[0];
			if (saw_later != 0) {
				if (saw_earlier == 0)
					++overtaken;
				break;
			}
			if (finished.load(std::memory_order_acquire) == step)
				break;
		}
		while (finished.load(std::memory_order_acquire) != step) {
		}
	}
	vcpu.join();
	require(healthy.load(std::memory_order_relaxed),
		"the storing vCPU left the gadget stream early");
	require(overtaken == 0,
		"another CPU saw a later guest store before an earlier one");
}

/* Total store order is kept by a barrier on the way out of every gadget that
 * touches guest memory, so the first way out of each of these has to carry
 * one. The scan stops at the dispatch that ends the gadget's fast path.
 */
void guest_memory_gadgets_end_in_an_ordering_barrier()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint32_t barrier = 0xd503'3bbfU;
	constexpr std::uint32_t dispatch = 0xd61f'0100U;
	constexpr std::size_t longest_fast_path = 512;
	struct Case {
		const char *message;
		std::initializer_list<std::uint8_t> code;
	};
	const Case cases[] = {
		{"a load leaves without an ordering barrier",
			{0x48, 0x8b, 0x03, 0x0f, 0x05}},
		{"a store leaves without an ordering barrier",
			{0x48, 0x89, 0x03, 0x0f, 0x05}},
		{"a read-modify-write leaves without an ordering barrier",
			{0x48, 0x01, 0x03, 0x0f, 0x05}},
		{"a locked update leaves without an ordering barrier",
			{0xf0, 0x48, 0x01, 0x03, 0x0f, 0x05}},
		{"an exchange with memory leaves without an ordering barrier",
			{0x48, 0x87, 0x03, 0x0f, 0x05}},
		{"a push leaves without an ordering barrier",
			{0x50, 0x0f, 0x05}},
		{"a pop leaves without an ordering barrier",
			{0x58, 0x0f, 0x05}},
		{"a vector load leaves without an ordering barrier",
			{0x0f, 0x10, 0x03, 0x0f, 0x05}},
		{"a vector store leaves without an ordering barrier",
			{0x0f, 0x11, 0x03, 0x0f, 0x05}},
	};
	for (const auto &item : cases) {
		TestMemory memory;
		memory.load(0, item.code);
		const auto built = gadgets::build(memory, 0);
		require(built.status == gadgets::BuildStatus::built &&
			!built.program.native_stream().empty(), item.message);
		const auto *code = reinterpret_cast<const std::uint32_t *>(
			built.program.native_stream().front());
		bool fenced = false;
		for (std::size_t index = 0; index < longest_fast_path; ++index) {
			if (code[index] == barrier)
				fenced = true;
			if (code[index] == dispatch)
				break;
		}
		require(fenced, item.message);
	}
}

/* ---- the instruction position --------------------------------------------
 *
 * An instruction that can fault or leave carries its position in its block
 * (the baked access word, or the descriptor or operand word of the other
 * forms), and the cold exits rebuild the RIP from it and the block's start.
 * The position is ten bits, so a block ends before the first instruction 1024
 * bytes or more from its start and the next block begins there. These put an
 * access at chosen positions of a block that starts away from zero and have it
 * leave through a fault, a budget-limited replay or a store into its own
 * block, and check the immediates the access word holds against the register
 * forms that carry theirs in a word of their own.
 */

void load_code(TestMemory &memory, std::uint64_t address,
	const std::vector<std::uint8_t> &code)
{
	memory.fill(address, std::as_bytes(std::span{code}));
}

/* NOPs adding up to `bytes`, seven-byte ones first; counts them. */
void append_padding(std::vector<std::uint8_t> &code, std::size_t bytes,
	std::size_t &instructions)
{
	for (; bytes >= 7U; bytes -= 7U, ++instructions)
		code.insert(code.end(), {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00});
	for (; bytes != 0; --bytes, ++instructions)
		code.push_back(0x90);
}

enum class AddressField : std::uint8_t { none, rip_relative, absolute };

struct FaultingAccess final {
	const char *name;
	std::vector<std::uint8_t> encoding;
	/* Which field of the encoding names the faulting address. */
	AddressField field;
	uml::memory::MemoryAccess access;
	std::size_t bytes;
	/* What RBX or RSP holds beyond the faulting address. */
	std::int64_t rbx_bias;
	bool stack;
	std::int64_t rsp_bias;
	/* A read-modify-write may fault on its load, before it stores, and
	 * the banks differ in which they report. */
	bool read_modify_write = false;
};

/* The shapes of baked access that fault: loads and stores through a base, a
 * base and an index and no base, memory,immediate forms, the widening load,
 * both stack forms, and the immediate the access word cannot hold. */
[[nodiscard]] std::vector<FaultingAccess> faulting_accesses()
{
	using uml::memory::MemoryAccess;
	constexpr auto none = AddressField::none;
	return {
		{"mov rax,[rbx]", {0x48, 0x8b, 0x03}, none, MemoryAccess::read, 8, 0,
			false, 0},
		{"mov rax,[rbx+rcx*8+0x12345]",
			{0x48, 0x8b, 0x84, 0xcb, 0x45, 0x23, 0x01, 0x00}, none,
			MemoryAccess::read, 8, -0x12345, false, 0},
		{"mov [rbx],rax", {0x48, 0x89, 0x03}, none, MemoryAccess::write, 8, 0,
			false, 0},
		{"mov [rbx+rcx*8],rax", {0x48, 0x89, 0x04, 0xcb}, none,
			MemoryAccess::write, 8, 0, false, 0},
		{"mov dword [rbx],1", {0xc7, 0x03, 0x01, 0x00, 0x00, 0x00}, none,
			MemoryAccess::write, 4, 0, false, 0},
		{"mov dword [rbx+rcx*8],-1",
			{0xc7, 0x04, 0xcb, 0xff, 0xff, 0xff, 0xff}, none,
			MemoryAccess::write, 4, 0, false, 0},
		{"add rax,[rbx]", {0x48, 0x03, 0x03}, none, MemoryAccess::read, 8, 0,
			false, 0},
		{"add [rbx],rax", {0x48, 0x01, 0x03}, none, MemoryAccess::write, 8, 0,
			false, 0, true},
		{"cmp dword [rbx],1", {0x83, 0x3b, 0x01}, none, MemoryAccess::read, 4,
			0, false, 0},
		{"add dword [rbx],0x4000", {0x81, 0x03, 0x00, 0x40, 0x00, 0x00}, none,
			MemoryAccess::write, 4, 0, false, 0, true},
		{"movzx eax,byte [rbx]", {0x0f, 0xb6, 0x03}, none,
			MemoryAccess::read, 1, 0, false, 0},
		{"mov rax,[rip+rel]", {0x48, 0x8b, 0x05, 0, 0, 0, 0},
			AddressField::rip_relative, MemoryAccess::read, 8, 0, false, 0},
		{"mov rax,[abs]", {0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0},
			AddressField::absolute, MemoryAccess::read, 8, 0, false, 0},
		{"push rax", {0x50}, none, MemoryAccess::write, 8, 0, true, 8},
		{"pop rax", {0x58}, none, MemoryAccess::read, 8, 0, true, 0},
	};
}

/* An address no memory covers, and the last byte of the memory, where an
 * access of more than a byte crosses into a page that is not there. */
constexpr std::uint64_t unmapped_address = 0x20000;
constexpr std::uint64_t page_end_address = 16383;

void baked_faults_rebuild_the_rip_from_the_block_start()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t code = 0x1230;
	const std::array<std::size_t, 13> positions{0, 1, 6, 7, 63, 64, 100, 500,
		1016, 1023, 1024, 1025, 1100};
	for (const auto &access : faulting_accesses()) {
		for (const auto position : positions) {
			for (const auto target : {unmapped_address, page_end_address}) {
				if (target == page_end_address && access.bytes == 1U)
					continue;
				std::vector<std::uint8_t> bytes;
				std::size_t padding = 0;
				append_padding(bytes, position, padding);
				const auto at = bytes.size();
				auto encoding = access.encoding;
				if (access.field == AddressField::rip_relative) {
					const auto rel = static_cast<std::uint32_t>(
						target - (code + at + encoding.size()));
					for (unsigned byte = 0; byte < 4U; ++byte)
						encoding[3U + byte] =
							static_cast<std::uint8_t>(rel >> (8U * byte));
				} else if (access.field == AddressField::absolute) {
					for (unsigned byte = 0; byte < 4U; ++byte)
						encoding[4U + byte] =
							static_cast<std::uint8_t>(target >> (8U * byte));
				}
				bytes.insert(bytes.end(), encoding.begin(), encoding.end());
				bytes.insert(bytes.end(), {0x0f, 0x05});
				TestMemory memory;
				load_code(memory, code, bytes);
				const auto built = require_built(
					gadgets::build(memory, code, 400), access.name);
				/* The whole block, then a replay of the instructions up to
				 * and including the access. */
				for (const auto budget : {std::size_t{1000}, padding + 1U}) {
					CpuState cpu{};
					cpu.rip = code;
					cpu.gpr(Register::rbx) = target + access.rbx_bias;
					cpu.gpr(Register::rsp) = access.stack ?
						target + access.rsp_bias : 0x800;
					/* A block ends 1024 bytes in, and the access beyond
					 * that is the first instruction of the next. */
					auto result = gadgets::execute(
						built.program, cpu, memory, budget);
					std::size_t retired = result.retired_instructions;
					while (result.event == uml::cpu::EngineEvent::continued &&
						retired < budget) {
						const auto next = require_built(
							gadgets::build(memory, cpu.rip, 400), access.name);
						result = gadgets::execute(next.program, cpu, memory,
							budget - retired);
						retired += result.retired_instructions;
					}
					const auto where = std::string{access.name} + " at " +
						std::to_string(position) + ", budget " +
						std::to_string(budget) + ", address " +
						std::to_string(target);
					require(result.event ==
								uml::cpu::EngineEvent::memory_fault,
							(where + " did not fault").c_str());
					require(retired == padding &&
							cpu.rip == code + at,
						(where + " reported the wrong RIP or retirement")
							.c_str());
					require(result.fault.access == access.access ||
							access.read_modify_write,
						(where + " reported the wrong access").c_str());
				}
			}
		}
	}
}

/* An access takes the baked bank while it lies within 1024 bytes of its
 * block's start, and the block ends before it beyond. */
void baked_accesses_stop_at_the_position_field()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t code = 0x1230;
	const auto baked_load = reinterpret_cast<std::uintptr_t>(
		gadgets::baked::uml_x86_64_baked_move_64_rm[
			0U * gadgets::baked::base_slots + 3U]);
	for (const auto position : {std::size_t{1023}, std::size_t{1024}}) {
		std::vector<std::uint8_t> bytes;
		std::size_t padding = 0;
		append_padding(bytes, position, padding);
		bytes.insert(bytes.end(), {0x48, 0x8b, 0x03, 0x0f, 0x05});
		TestMemory memory;
		load_code(memory, code, bytes);
		const auto built = require_built(gadgets::build(memory, code, 400),
			"padded load block did not build");
		const auto stream = built.program.native_stream();
		const bool baked_form =
			std::ranges::find(stream, baked_load) != stream.end();
		require(baked_form == (position < 1024U),
			"the baked bank's position limit moved");
	}
}

/* A store into the block that is running leaves through the overlap exit with
 * the RIP after the store, rebuilt from the same position. */
void baked_overlap_exits_rebuild_the_rip_from_the_block_start()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t code = 0x1100;
	struct Store final {
		const char *name;
		std::vector<std::uint8_t> encoding;
		bool stack;
	};
	const std::vector<Store> stores{
		{"mov byte [rbx],0x90", {0xc6, 0x03, 0x90}, false},
		{"mov byte [rbx+rcx*2],0x90", {0xc6, 0x04, 0x4b, 0x90}, false},
		{"mov [rbx],al", {0x88, 0x03}, false},
		{"mov [rbx+rcx*2],al", {0x88, 0x04, 0x4b}, false},
		{"add [rbx],al", {0x00, 0x03}, false},
		{"add byte [rbx],0x7f", {0x80, 0x03, 0x7f}, false},
		{"push rax", {0x50}, true},
	};
	for (const auto &store : stores) {
		for (const auto position : {std::size_t{0}, std::size_t{5},
			std::size_t{200}, std::size_t{1019}, std::size_t{1024},
			std::size_t{1030}}) {
			std::vector<std::uint8_t> bytes;
			std::size_t padding = 0;
			append_padding(bytes, position, padding);
			const auto at = bytes.size();
			bytes.insert(bytes.end(), store.encoding.begin(),
				store.encoding.end());
			bytes.insert(bytes.end(), {0x0f, 0x05});
			TestMemory memory;
			load_code(memory, code, bytes);
			memory.set_executable_page(code);
			const auto built = require_built(
				gadgets::build(memory, code, 400), store.name);
			CpuState cpu{};
			cpu.rip = code;
			cpu.gpr(Register::rax) = 0x90;
			const auto end = code + at + store.encoding.size();
			cpu.gpr(Register::rbx) = end;
			cpu.gpr(Register::rsp) = store.stack ? end + 8U : 0x800;
			gadgets::Executor executor;
			executor.prepare(memory, 1, 1, 1);
			auto result = gadgets::execute(
				built.program, cpu, memory, executor, 1000);
			std::size_t retired = result.retired_instructions;
			/* The store beyond 1024 bytes starts the block after the first. */
			while (result.event == uml::cpu::EngineEvent::continued &&
				cpu.rip <= code + at) {
				const auto next = require_built(
					gadgets::build(memory, cpu.rip, 400), store.name);
				result = gadgets::execute(
					next.program, cpu, memory, executor, 1000);
				retired += result.retired_instructions;
			}
			const auto where = std::string{store.name} + " at " +
				std::to_string(position);
			require(result.event == uml::cpu::EngineEvent::continued &&
					retired == padding + 1U &&
					cpu.rip == end,
					(where + " left the block with the wrong RIP").c_str());
			require(memory.invalidations() == 1,
				(where + " did not publish its change").c_str());
		}
	}
}

/* ---- the ordinary forms' position -----------------------------------------
 *
 * Every instruction that can fault or leave, except the baked forms above,
 * carries its position in the stream word its gadget reads anyway: the
 * descriptor of a memory operand, the operand word of a push, pop, string,
 * division or return, and the word a call that is not five bytes long keeps
 * for it. These run each such instruction at positions of a block and at the
 * top of the 32-bit address space, and have it fault where it does not retire.
 * The RIP it reports must be its own address, wrapped at 4 GiB in
 * compatibility mode.
 */

struct PositionCase final {
	const char *name;
	std::vector<std::uint8_t> encoding;
	uml::cpu::EngineEvent event;
	/* Which registers the case sets beyond the defaults, which leave every
	 * pointer a case could use unmapped but the stack. */
	enum class Setup : std::uint8_t {
		memory, stack, divide, bound_range
	} setup = Setup::memory;
};

[[nodiscard]] std::vector<PositionCase> position_cases(
	uml::cpu::x86_64::ExecutionMode mode)
{
	using Event = uml::cpu::EngineEvent;
	using Setup = PositionCase::Setup;
	const bool compat = mode == uml::cpu::x86_64::ExecutionMode::compat32;
	/* A 64-bit operation in long mode, the same with 32 bits in the other. */
	const auto wide = [&](std::vector<std::uint8_t> encoding) {
		if (!compat)
			encoding.insert(encoding.begin(), 0x48);
		return encoding;
	};
	auto cases = std::vector<PositionCase>{
		/* The baked bank's forms, whose word carries the position in other
		 * bits, share the 32-bit wrap. */
		{"mov rax,[rbx]", wide({0x8b, 0x03}), Event::memory_fault},
		{"mov [rbx],rax", wide({0x89, 0x03}), Event::memory_fault},
		{"mov dword [rbx],1", {0xc7, 0x03, 0x01, 0x00, 0x00, 0x00},
			Event::memory_fault},
		{"movzx eax,byte [rbx]", {0x0f, 0xb6, 0x03}, Event::memory_fault},
		{"push rax", {0x50}, Event::memory_fault, Setup::stack},
		{"pop rax", {0x58}, Event::memory_fault, Setup::stack},
		{"movaps xmm0,[rbx]", {0x0f, 0x28, 0x03}, Event::memory_fault},
		{"movups [rbx],xmm0", {0x0f, 0x11, 0x03}, Event::memory_fault},
		{"vmovups ymm0,[rbx]", {0xc5, 0xfc, 0x10, 0x03}, Event::memory_fault},
		{"addps xmm0,[rbx]", {0x0f, 0x58, 0x03}, Event::memory_fault},
		{"pshufd xmm0,[rbx],0x1b", {0x66, 0x0f, 0x70, 0x03, 0x1b},
			Event::memory_fault},
		{"fld qword [rbx]", {0xdd, 0x03}, Event::memory_fault},
		{"fstp qword [rbx]", {0xdd, 0x1b}, Event::memory_fault},
		{"fld tbyte [rbx]", {0xdb, 0x2b}, Event::memory_fault},
		{"xchg [rbx],rax", wide({0x87, 0x03}), Event::memory_fault},
		{"lock add [rbx],rax", [&] {
			auto encoding = wide({0x01, 0x03});
			encoding.insert(encoding.begin(), 0xf0);
			return encoding;
		}(), Event::memory_fault},
		{"lock cmpxchg [rbx],rcx", [&] {
			auto encoding = wide({0x0f, 0xb1, 0x0b});
			encoding.insert(encoding.begin(), 0xf0);
			return encoding;
		}(), Event::memory_fault},
		{"bt [rbx],rax", wide({0x0f, 0xa3, 0x03}), Event::memory_fault},
		{"shl qword [rbx],cl", wide({0xd3, 0x23}), Event::memory_fault},
		{"imul rax,[rbx]", wide({0x0f, 0xaf, 0x03}), Event::memory_fault},
		{"cmovz rax,[rbx]", wide({0x0f, 0x44, 0x03}), Event::memory_fault},
		{"setz [rbx]", {0x0f, 0x94, 0x03}, Event::memory_fault},
		{"bsf rax,[rbx]", wide({0x0f, 0xbc, 0x03}), Event::memory_fault},
		{"push [rbx]", {0xff, 0x33}, Event::memory_fault},
		{"pop [rbx]", {0x8f, 0x03}, Event::memory_fault},
		{"push 1", {0x6a, 0x01}, Event::memory_fault, Setup::stack},
		{"push 0x12345678", {0x68, 0x78, 0x56, 0x34, 0x12},
			Event::memory_fault, Setup::stack},
		{"pushf", {0x9c}, Event::memory_fault, Setup::stack},
		{"popf", {0x9d}, Event::memory_fault, Setup::stack},
		{"leave", {0xc9}, Event::memory_fault},
		{"enter 16,0", {0xc8, 0x10, 0x00, 0x00}, Event::memory_fault,
			Setup::stack},
		{"ret", {0xc3}, Event::memory_fault, Setup::stack},
		{"ret 8", {0xc2, 0x08, 0x00}, Event::memory_fault, Setup::stack},
		{"call rel", {0xe8, 0x00, 0x00, 0x00, 0x00}, Event::memory_fault,
			Setup::stack},
		{"cs call rel", {0x2e, 0xe8, 0x00, 0x00, 0x00, 0x00},
			Event::memory_fault, Setup::stack},
		{"call rbx", {0xff, 0xd3}, Event::memory_fault, Setup::stack},
		{"call [rbx]", {0xff, 0x13}, Event::memory_fault},
		{"movsb", {0xa4}, Event::memory_fault},
		{"rep movsb", {0xf3, 0xa4}, Event::memory_fault},
		{"stos", wide({0xab}), Event::memory_fault},
		{"rep stosb", {0xf3, 0xaa}, Event::memory_fault},
		{"lodsb", {0xac}, Event::memory_fault},
		{"cmpsb", {0xa6}, Event::memory_fault},
		{"scasb", {0xae}, Event::memory_fault},
		{"xlat", {0xd7}, Event::memory_fault},
		{"div rbx", wide({0xf7, 0xf3}), Event::floating_point_trap,
			Setup::divide},
		{"div [rbx]", wide({0xf7, 0x33}), Event::memory_fault},
		{"hlt", {0xf4}, Event::general_protection},
		{"wrmsr", {0x0f, 0x30}, Event::general_protection},
	};
	if (compat) {
		cases.insert(cases.end(), {
			{"pusha", {0x60}, Event::memory_fault, Setup::stack},
			{"popa", {0x61}, Event::memory_fault, Setup::stack},
			{"push es", {0x06}, Event::memory_fault, Setup::stack},
			{"push cs", {0x0e}, Event::memory_fault, Setup::stack},
			{"bound eax,[ebx]", {0x62, 0x03}, Event::memory_fault},
			{"bound eax,[ebx] out of range", {0x62, 0x03}, Event::bound_range,
				Setup::bound_range},
			/* The guard leaves at the instruction, which `execute` steps
			 * through the segment model; a null CS faults it with #GP. */
			{"mov eax,cs:[ebx]", {0x2e, 0x8b, 0x03}, Event::general_protection},
		});
	}
	return cases;
}

/* Runs `test` at `position` bytes into a block that starts at `code`, with
 * the memory's base at `base`, from the whole block and from a replay of
 * the instructions up to it. */
void position_case_reports_its_address(
	const PositionCase &test, uml::cpu::x86_64::ExecutionMode mode,
	TestMemory &memory, std::uint64_t base, std::uint64_t code,
	std::size_t position)
{
	using namespace uml::cpu::x86_64;
	const bool compat = mode == ExecutionMode::compat32;
	const std::uint64_t mask = compat ? 0xffff'ffffULL : ~std::uint64_t{};
	const std::uint64_t unmapped = base + unmapped_address;
	std::vector<std::uint8_t> bytes;
	std::size_t padding = 0;
	append_padding(bytes, position, padding);
	const auto at = bytes.size();
	bytes.insert(bytes.end(), test.encoding.begin(), test.encoding.end());
	bytes.insert(bytes.end(), {0x0f, 0x05});
	load_code(memory, code, bytes);
	const auto built = require_built(
		gadgets::build(memory, code, mode, 400), test.name);
	for (const auto budget : {std::size_t{1000}, padding + 1U}) {
		CpuState cpu{};
		cpu.execution_mode = mode;
		cpu.rip = code & mask;
		cpu.gpr(Register::rsp) = base + 0x800;
		cpu.gpr(Register::rbx) = unmapped;
		cpu.gpr(Register::rbp) = unmapped;
		cpu.gpr(Register::rsi) = unmapped;
		cpu.gpr(Register::rdi) = unmapped;
		cpu.gpr(Register::rcx) = 4;
		switch (test.setup) {
		case PositionCase::Setup::memory:
			break;
		case PositionCase::Setup::stack:
			cpu.gpr(Register::rsp) = unmapped;
			break;
		case PositionCase::Setup::divide:
			cpu.gpr(Register::rax) = 42;
			cpu.gpr(Register::rdx) = 0;
			cpu.gpr(Register::rbx) = 0;
			break;
		case PositionCase::Setup::bound_range:
			cpu.gpr(Register::rax) = 5;
			cpu.gpr(Register::rbx) = base + 0x900;
			break;
		}
		const auto result = gadgets::execute(
			built.program, cpu, memory, budget);
		const auto where = std::string{test.name} + " at " +
			std::to_string(position) + (compat ? " (32-bit" : " (64-bit") +
			", base " + std::to_string(base) + ", budget " +
			std::to_string(budget) + ")";
		require(result.event == test.event,
				(where + " left with event " +
					std::to_string(static_cast<int>(result.event))).c_str());
		require(result.retired_instructions == padding &&
			cpu.rip == ((code + at) & mask),
			(where + " reported RIP " + std::to_string(cpu.rip) +
				" after retiring " +
				std::to_string(result.retired_instructions)).c_str());
	}
}

/* ---- the stack forms' recovery --------------------------------------------
 *
 * A baked PUSH or POP of a register has no stream word. When it faults, or
 * stores into the block that is running, the gadget leaves with the stream
 * pointer and `execute` finds the instruction by building the block's
 * instructions again, as it does for an exception the guest unmasked. These
 * run every register's PUSH and POP, with and without a REX prefix, to a fault
 * and to an overlap, at positions of a block and at the top of the address
 * space, from the block's own stream and from the fragment a budget-limited
 * replay runs, and check what the recovery must not disturb: a store across a
 * page edge, a block of many such instructions, a block a link enters and an
 * exception the guest unmasked behind them.
 */

/* PUSH or POP of register `index` (0 to 15), as encoded in 64-bit mode. */
[[nodiscard]] std::vector<std::uint8_t> stack_encoding(bool push,
	unsigned index)
{
	std::vector<std::uint8_t> encoding;
	if (index >= 8U)
		encoding.push_back(0x41);
	encoding.push_back(static_cast<std::uint8_t>(
		(push ? 0x50U : 0x58U) + (index & 7U)));
	return encoding;
}

[[nodiscard]] std::string stack_name(bool push, unsigned index)
{
	return std::string{push ? "push r" : "pop r"} + std::to_string(index);
}

/* Every register's PUSH and POP faults on an unmapped stack with its own
 * address, whichever stream runs it: the block's own, where the instruction
 * has no word, and a fragment, where it keeps its position. */
void baked_stack_faults_report_their_instruction()
{
	using namespace uml::cpu::x86_64;
	constexpr std::array<std::size_t, 8> positions{0, 1, 6, 7, 63, 100, 500,
		1000};
	for (const auto mode : {ExecutionMode::long64, ExecutionMode::compat32}) {
		const unsigned registers = mode == ExecutionMode::long64 ? 16U : 8U;
		for (const bool push : {true, false}) {
			for (unsigned index = 0; index < registers; ++index) {
				const auto name = stack_name(push, index);
				const PositionCase test{name.c_str(),
					stack_encoding(push, index), uml::cpu::EngineEvent::memory_fault,
					PositionCase::Setup::stack};
				for (const auto position : positions) {
					TestMemory memory;
					position_case_reports_its_address(
							test, mode, memory, 0, 0x1230, position);
				}
				if (mode == ExecutionMode::long64) {
					for (const auto position : {std::size_t{0},
						std::size_t{9}}) {
						constexpr std::uint64_t base = 0x7f00'0000'0000ULL;
						TestMemory memory{base};
						position_case_reports_its_address(test, mode,
								memory, base, base + 0x1230, position);
					}
				}
			}
		}
	}
}

/* The same instructions at the top of a 32-bit space, where a compatibility
 * mode block wraps to zero; those take the ordinary bank and must keep
 * reporting the wrapped address. */
void compat32_stack_faults_wrap_at_4gib()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t base = 0xffff'f000ULL;
	constexpr std::uint64_t code = 0xffff'fff8ULL;
	for (const bool push : {true, false}) {
		for (unsigned index = 0; index < 8U; ++index) {
			const auto name = stack_name(push, index);
			const PositionCase test{name.c_str(),
				stack_encoding(push, index), uml::cpu::EngineEvent::memory_fault,
				PositionCase::Setup::stack};
			for (const std::size_t position : {1U, 2U, 3U, 5U, 7U, 8U, 9U,
				10U, 16U, 30U}) {
				TestMemory memory{base};
				memory.set_wrap32(true);
				position_case_reports_its_address(test,
						ExecutionMode::compat32, memory, base, code,
						position);
			}
		}
	}
}

/* A store of the stack's eight bytes that lands in the running block retires
 * and leaves for the instruction after it, from every register. */
void baked_stack_overlaps_report_the_next_instruction()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t code = 0x1100;
	for (unsigned index = 0; index < 16U; ++index) {
		const auto encoding = stack_encoding(true, index);
		for (const auto position : {std::size_t{0}, std::size_t{5},
			std::size_t{200}, std::size_t{1019}}) {
			for (const auto budget : {std::size_t{1000}, std::size_t{0}}) {
				std::vector<std::uint8_t> bytes;
				std::size_t padding = 0;
				append_padding(bytes, position, padding);
				const auto at = bytes.size();
				bytes.insert(bytes.end(), encoding.begin(), encoding.end());
				bytes.insert(bytes.end(), {0x0f, 0x05});
				TestMemory memory;
				load_code(memory, code, bytes);
				memory.set_executable_page(code);
				const auto built = require_built(
					gadgets::build(memory, code, 400),
					stack_name(true, index).c_str());
				CpuState cpu{};
				cpu.rip = code;
				const auto end = code + at + encoding.size();
				for (unsigned other = 0; other < 16U; ++other)
					cpu.gpr(static_cast<Register>(other)) = 0x90 + other;
				/* The push stores at the instruction after its own. */
				cpu.gpr(Register::rsp) = end + 8U;
				const auto stored = cpu.gpr(static_cast<Register>(index));
				gadgets::Executor executor;
				executor.prepare(memory, 1, 1, 1);
				/* Budget 0 stands for a replay of the instructions up to
				 * and including the push. */
				const std::size_t limit = budget == 0 ? padding + 1U : budget;
				const auto result = gadgets::execute(
					built.program, cpu, memory, executor, limit);
				const auto where = stack_name(true, index) + " at " +
					std::to_string(position) + ", budget " +
					std::to_string(limit);
				require(result.event == uml::cpu::EngineEvent::continued &&
						result.retired_instructions == padding + 1U &&
						cpu.rip == end,
						(where + " left the block with the wrong RIP")
							.c_str());
				require(memory.qword(end) == stored &&
					cpu.gpr(Register::rsp) == end &&
					memory.invalidations() == 1,
					(where + " lost its store").c_str());
			}
		}
	}
}

/* A stack access across a page edge goes through the crossing tails, which
 * start a PUSH's publication flag clear because no word does: the store
 * lands, and a crossing access off the end of memory faults at the
 * instruction. */
void baked_stack_crosses_page_edges()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t code = 0x3000;
	for (unsigned index = 0; index < 16U; ++index) {
		for (const bool push : {true, false}) {
			for (const std::size_t position : {std::size_t{0}, std::size_t{9}}) {
				const auto encoding = stack_encoding(push, index);
				std::vector<std::uint8_t> bytes;
				std::size_t padding = 0;
				append_padding(bytes, position, padding);
				const auto at = bytes.size();
				bytes.insert(bytes.end(), encoding.begin(), encoding.end());
				bytes.push_back(0xcc);
				const auto where = stack_name(push, index) + " at " +
					std::to_string(position);
				/* A page edge inside the memory, then the memory's end. */
				for (const std::uint64_t address : {std::uint64_t{4092},
					std::uint64_t{8188}, page_end_address}) {
					TestMemory memory;
					load_code(memory, code, bytes);
					const auto built = require_built(
						gadgets::build(memory, code, 400), where.c_str());
					CpuState cpu{};
					cpu.rip = code;
					constexpr std::uint64_t value = 0x1122334455667788ULL;
					cpu.gpr(static_cast<Register>(index)) = value;
					cpu.gpr(Register::rsp) = push ? address + 8U : address;
					const std::uint64_t pushed = cpu.gpr(
						static_cast<Register>(index));
					if (!push)
						for (unsigned byte = 0; byte < 8U &&
							address + byte < 16384U; ++byte)
							memory.load(address + byte,
								{static_cast<std::uint8_t>(
									value >> (8U * byte))});
					const bool inside = address + 8U <= 16384U;
					const auto result = gadgets::execute(
						built.program, cpu, memory, 1000);
					if (!inside) {
						require(result.event ==
								uml::cpu::EngineEvent::memory_fault &&
							result.retired_instructions == padding &&
							cpu.rip == code + at,
							(where + " off the end of memory reported the "
								"wrong RIP").c_str());
						continue;
					}
					/* INT3 ends the block without touching RCX or R11. */
					require(result.event ==
								uml::cpu::EngineEvent::breakpoint &&
							result.retired_instructions >= padding + 1U,
						(where + " across a page edge did not retire")
							.c_str());
					if (push) {
						require(memory.qword(address) == pushed &&
								cpu.gpr(Register::rsp) == address,
							(where + " across a page edge lost its store")
								.c_str());
					} else {
						require(cpu.gpr(static_cast<Register>(index)) ==
									value &&
								(index == 4U ||
								cpu.gpr(Register::rsp) == address + 8U),
							(where + " across a page edge lost its load at " +
								std::to_string(address) + ": register " +
								std::to_string(cpu.gpr(
									static_cast<Register>(index))) +
								", rsp " +
								std::to_string(cpu.gpr(Register::rsp)))
								.c_str());
					}
				}
			}
		}
	}
}

/* A block of many PUSH, POP and ordinary instructions faults at the right
 * one wherever the stack gives out, so the recovery counts the words of the
 * headerless entries before it. */
void baked_stack_faults_inside_a_long_block()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t code = 0x1230;
	struct Step final {
		std::vector<std::uint8_t> encoding;
		int delta;
	};
	std::vector<Step> steps;
	for (unsigned n = 0; n < 40U; ++n) {
		switch (n % 5U) {
		case 0: steps.push_back({stack_encoding(true, n % 16U), 1}); break;
		case 1: steps.push_back({{0x48, 0xff, 0xc1}, 0}); break;
		case 2: steps.push_back({stack_encoding(true, (n * 7U) % 16U), 1});
			break;
		case 3: {
			/* Not RSP: a POP of it moves the stack to what it loaded. */
			const unsigned index = (n * 3U) % 15U;
			steps.push_back({stack_encoding(false,
				index >= 4U ? index + 1U : index), -1});
			break;
		}
		default: steps.push_back({{0xb8, 0x07, 0x00, 0x00, 0x00}, 0}); break;
		}
	}
	std::vector<std::uint8_t> bytes;
	std::vector<std::size_t> offsets;
	for (const auto &step : steps) {
		offsets.push_back(bytes.size());
		bytes.insert(bytes.end(), step.encoding.begin(), step.encoding.end());
	}
	bytes.insert(bytes.end(), {0x0f, 0x05});
	for (unsigned depth = 0; depth < 24U; ++depth) {
		TestMemory memory;
		load_code(memory, code, bytes);
		const auto built = require_built(
			gadgets::build(memory, code, 400), "long stack block");
		CpuState cpu{};
		cpu.rip = code;
		const std::uint64_t start = 8U * depth;
		cpu.gpr(Register::rsp) = start;
		/* The first PUSH to run below address zero faults. */
		std::int64_t rsp = static_cast<std::int64_t>(start);
		std::size_t fault = steps.size();
		for (std::size_t index = 0; index < steps.size(); ++index) {
			if (steps[index].delta > 0 && rsp - 8 < 0) {
				fault = index;
				break;
			}
			rsp -= 8 * steps[index].delta;
		}
		const auto result = gadgets::execute(
			built.program, cpu, memory, 1000);
		const auto where = "stack block from depth " + std::to_string(depth);
		if (fault == steps.size()) {
			require(result.event == uml::cpu::EngineEvent::system_call,
					(where + " did not reach its end").c_str());
			continue;
		}
		require(result.event == uml::cpu::EngineEvent::memory_fault &&
				result.retired_instructions == fault &&
				cpu.rip == code + offsets[fault],
				(where + " faulted at the wrong instruction").c_str());
	}
}

/* A PUSH in a block that a link enters, and one in a program the executor
 * did not cache, are found from the context's program begin and from the
 * program the run was entered with. */
void a_linked_block_recovers_its_stack_instruction()
{
	using namespace uml::cpu::x86_64;
	for (const bool overlap : {false, true}) {
		for (const unsigned padding : {0U, 3U}) {
			for (const bool push : {true, false}) {
				if (overlap && !push)
					continue;
				TestMemory memory;
				memory.load(0, {0xe9, 0x0b, 0x00, 0x00, 0x00});
				std::vector<std::uint8_t> bytes(padding, 0x90);
				bytes.push_back(push ? 0x53 : 0x5b);
				bytes.insert(bytes.end(), {0x0f, 0x05});
				load_code(memory, 16, bytes);
				if (overlap)
					memory.set_executable_page(0);
				Sequences sequences;
				memory.set_code_changes(&sequences.code_changes);
				gadgets::Executor executor;
				executor.prepare(memory, 42, sequences.mapping_changes,
					sequences.code_changes, &sequences, read_sequences);
				const auto target = executor.select_program(
					memory, 16, ExecutionMode::long64);
				const auto jump = executor.select_program(
					memory, 0, ExecutionMode::long64);
				require(target.program != nullptr &&
						jump.program != nullptr,
					"linked stack block did not populate its link");
				CpuState cpu{};
				const std::uint64_t end = 16U + padding + 1U;
				/* The push lands on the instruction after it, or nowhere. */
				cpu.gpr(Register::rsp) = overlap ? end + 8U : 0x20008;
				cpu.gpr(Register::rbx) = 0x55;
				const auto result = gadgets::execute(
					*jump.program, cpu, memory, executor, 64);
				const auto where = std::string{push ? "push" : "pop"} +
					(overlap ? " (overlap)" : " (fault)") + " behind a link, "
					"padding " + std::to_string(padding);
				if (overlap) {
					require(result.event == uml::cpu::EngineEvent::continued &&
							result.retired_instructions == 2U + padding &&
							cpu.rip == end &&
							memory.qword(end) == 0x55,
						(where + " left with the wrong RIP").c_str());
				} else {
					require(result.event ==
							uml::cpu::EngineEvent::memory_fault &&
						result.retired_instructions == 1U + padding &&
						cpu.rip == 16U + padding,
						(where + " faulted at the wrong instruction")
							.c_str());
				}
			}
		}
	}
}

/* An exception the guest unmasked, found behind PUSH and POP, is raised at
 * its own instruction: the recovery for it builds the block's stream as the
 * block's own, whose entries have no word. */
void an_unmasked_exception_is_found_behind_stack_instructions()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x53,                   // push rbx
		0x41, 0x54,             // push r12
		0x59,                   // pop rcx
		0x41, 0x5d,             // pop r13
		0xf3, 0x0f, 0x5e, 0xc1, // divss xmm0, xmm1
		0x0f, 0x05,             // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0, 64),
		"stack block with a DIVSS did not build");
	gadgets::Executor executor;
	executor.prepare(memory);
	CpuState cpu{};
	cpu.mxcsr = 0x1d80;
	cpu.gpr(Register::rsp) = 0x800;
	set_xmm_low(cpu, 0, 0x3f800000);
	const auto trap = gadgets::execute(
		built.program, cpu, memory, executor, 64);
	require(trap.event == uml::cpu::EngineEvent::floating_point_trap &&
			trap.vector == 19 && trap.retired_instructions == 4 &&
			cpu.rip == 6 && cpu.gpr(Register::rsp) == 0x800,
		"an unmasked exception behind PUSH and POP was not raised at its "
		"own instruction");
}

/* A block's own stream holds the entries alone, and only there. */
void baked_stack_entries_carry_no_stream_word()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x53,       // push rbx
		0x41, 0x57, // push r15
		0x5d,       // pop rbp
		0x41, 0x58, // pop r8
		0x0f, 0x05, // syscall
	});
	const auto built = require_built(gadgets::build(memory, 0, 64),
		"stack block did not build");
	const auto stream = built.program.native_stream();
	const auto entry = [](bool push, unsigned index) {
		return reinterpret_cast<std::uintptr_t>(
			(push ? gadgets::baked::uml_x86_64_baked_push_64_r :
				gadgets::baked::uml_x86_64_baked_pop_64_r)[index]);
	};
	require(stream.size() >= 4 && stream[0] == entry(true, 3) &&
			stream[1] == entry(true, 15) && stream[2] == entry(false, 5) &&
			stream[3] == entry(false, 8),
		"PUSH and POP in a block's stream do not follow one another");
}

void ordinary_forms_report_their_position()
{
	using namespace uml::cpu::x86_64;
	/* A block 1000 bytes in is the furthest an instruction of the longest
	 * case, and the SYSCALL behind it, stay within the position field. */
	constexpr std::array<std::size_t, 8> positions{0, 1, 6, 7, 63, 100, 500,
		1000};
	for (const auto mode : {ExecutionMode::long64, ExecutionMode::compat32}) {
		for (const auto &test : position_cases(mode)) {
			for (const auto position : positions) {
				TestMemory memory;
				position_case_reports_its_address(
						test, mode, memory, 0, 0x1230, position);
			}
			/* Above 4 GiB, where long mode keeps the high bits of every
			 * address it rebuilds. */
			if (mode == ExecutionMode::long64) {
				for (const auto position : {std::size_t{0}, std::size_t{9}}) {
					constexpr std::uint64_t base = 0x7f00'0000'0000ULL;
					TestMemory memory{base};
					position_case_reports_its_address(test, mode, memory,
							base, base + 0x1230, position);
				}
			}
		}
	}
}

/* A compatibility-mode block that runs across the top of the address space
 * continues at address zero. Every RIP the exits rebuild from the block's
 * start and a position wraps with it, so an instruction at the top is reported
 * at its own 32-bit address and one past it at its small one. */
void ordinary_forms_report_their_position_across_4gib()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t base = 0xffff'f000ULL;
	constexpr std::uint64_t code = 0xffff'fff8ULL;
	for (const auto &test : position_cases(ExecutionMode::compat32)) {
		for (const std::size_t position : {1U, 2U, 3U, 5U, 7U, 8U, 9U, 10U,
			16U, 30U}) {
			TestMemory memory{base};
			memory.set_wrap32(true);
			position_case_reports_its_address(test,
					ExecutionMode::compat32, memory, base, code, position);
		}
	}
}

/* A store that lands in the block that is running retires, and the run
 * leaves for the instruction after it, whose bytes may have changed: the RIP
 * the ordinary stores and the call rebuild is the address after the
 * instruction, wrapped at 4 GiB in compatibility mode. */
void ordinary_overlap_exits_report_the_next_address()
{
	using namespace uml::cpu::x86_64;
	struct Store final {
		const char *name;
		std::vector<std::uint8_t> encoding;
		bool wide;
	};
	const std::vector<Store> stores{
		{"movups [rbx],xmm0", {0x0f, 0x11, 0x03}, false},
		{"vmovups [rbx],ymm0", {0xc5, 0xfc, 0x11, 0x03}, false},
		{"fst qword [rbx]", {0xdd, 0x13}, false},
		{"xchg [rbx],rax", {0x87, 0x03}, true},
		{"lock add [rbx],rax", {0xf0, 0x01, 0x03}, true},
		{"bts [rbx],rax", {0x0f, 0xab, 0x03}, true},
		{"setz [rbx]", {0x0f, 0x94, 0x03}, false},
		{"mov [rbx],rax", {0x89, 0x03}, true},
		{"mov byte [rbx],0x90", {0xc6, 0x03, 0x90}, false},
	};
	for (const auto mode : {ExecutionMode::long64, ExecutionMode::compat32}) {
		const bool compat = mode == ExecutionMode::compat32;
		const std::uint64_t mask = compat ? 0xffff'ffffULL : ~std::uint64_t{};
		for (auto store : stores) {
			if (store.wide && !compat)
				store.encoding.insert(
					store.encoding.begin() + (store.encoding[0] == 0xf0),
					0x48);
			/* The code is at the top of a 32-bit space, and the store ends
			 * the block at or before the top, or just past it. */
			for (const std::size_t position : {0U, 5U, 200U, 1018U}) {
				for (const bool wrap : {false, true}) {
					if (wrap && !compat)
						continue;
					constexpr std::uint64_t top = 0xffff'f000ULL;
					const std::uint64_t base = wrap ? top : 0;
					const std::uint64_t code = wrap ? 0xffff'fff0ULL : 0x1100;
					TestMemory memory{base};
					memory.set_wrap32(wrap);
					std::vector<std::uint8_t> bytes;
					std::size_t padding = 0;
					append_padding(bytes, position, padding);
					const auto at = bytes.size();
					bytes.insert(bytes.end(), store.encoding.begin(),
						store.encoding.end());
					bytes.insert(bytes.end(), {0x0f, 0x05});
					load_code(memory, code, bytes);
					memory.set_executable_page(code);
					const auto built = require_built(
						gadgets::build(memory, code, mode, 400), store.name);
					const auto end = code + at + store.encoding.size();
					CpuState cpu{};
					cpu.execution_mode = mode;
					cpu.rip = code & mask;
					/* The store covers the SYSCALL behind it, and BTS' bit
					 * offset is zero. */
					cpu.gpr(Register::rbx) = end & mask;
					gadgets::Executor executor;
					executor.prepare(memory, 1, 1, 1);
					const auto result = gadgets::execute(
						built.program, cpu, memory, executor, 1000);
					const auto where = std::string{store.name} + " at " +
						std::to_string(position) + (compat ? " (32-bit" :
							" (64-bit") + (wrap ? ", across 4 GiB)" : ")");
					require(result.event ==
								uml::cpu::EngineEvent::continued &&
							result.retired_instructions == padding + 1U &&
							cpu.rip == (end & mask),
							(where + " left with the wrong RIP").c_str());
					require(compat || memory.invalidations() == 1,
						(where + " published " +
							std::to_string(memory.invalidations()) +
							" times").c_str());
				}
			}
		}
	}
}

/* A CALL pushes the address after it. The five-byte form carries no
 * position and its fault recovers the CALL as five bytes before that address,
 * in either width and across 4 GiB; a prefixed one carries the position. Both
 * push the right address, and a CALL that faults pushing is not retired. */
void call_pushes_and_reports_the_next_and_own_address()
{
	using namespace uml::cpu::x86_64;
	for (const auto mode : {ExecutionMode::long64, ExecutionMode::compat32}) {
		const bool compat = mode == ExecutionMode::compat32;
		const std::uint64_t mask = compat ? 0xffff'ffffULL : ~std::uint64_t{};
		for (const bool prefixed : {false, true}) {
			for (const bool wrap : {false, true}) {
				if (wrap && !compat)
					continue;
				for (const std::size_t position : {0U, 3U, 14U, 500U}) {
					const std::uint64_t base = wrap ? 0xffff'f000ULL : 0;
					const std::uint64_t code = wrap ? 0xffff'fff0ULL : 0x1100;
					TestMemory memory{base};
					memory.set_wrap32(wrap);
					std::vector<std::uint8_t> bytes;
					std::size_t padding = 0;
					append_padding(bytes, position, padding);
					const auto at = bytes.size();
					if (prefixed)
						bytes.push_back(0x2e);
					/* To the SYSCALL behind it, so the CALL lands there. */
					bytes.insert(bytes.end(), {0xe8, 0x00, 0x00, 0x00, 0x00});
					bytes.insert(bytes.end(), {0x0f, 0x05});
					load_code(memory, code, bytes);
					const auto built = require_built(
						gadgets::build(memory, code, mode, 400), "call block");
					const auto next = code + at + (prefixed ? 6U : 5U);
					const auto where = std::string{"call"} +
						(prefixed ? " with a prefix" : "") + " at " +
						std::to_string(position) + (compat ? " (32-bit" :
							" (64-bit") + (wrap ? ", across 4 GiB)" : ")");
					{
						/* The push succeeds. */
						CpuState cpu{};
						cpu.execution_mode = mode;
						cpu.rip = code & mask;
						cpu.gpr(Register::rsp) = base + 0x800;
						gadgets::Executor executor;
						executor.prepare(memory, 1, 1, 1);
						const auto result = gadgets::execute(
							built.program, cpu, memory, executor, 1000);
						const auto pushed = compat ?
							memory.dword(base + 0x800 - 4U) :
							memory.qword(base + 0x800 - 8U);
						/* The CALL ends the block, at its target. */
						require(result.event ==
									uml::cpu::EngineEvent::continued &&
								cpu.rip == (next & mask) &&
								pushed == (next & mask),
								(where + " pushed the wrong address").c_str());
					}
					{
						/* The push faults. */
						CpuState cpu{};
						cpu.execution_mode = mode;
						cpu.rip = code & mask;
						cpu.gpr(Register::rsp) = base + unmapped_address;
						const auto result = gadgets::execute(
							built.program, cpu, memory, 1000);
						require(result.event ==
									uml::cpu::EngineEvent::memory_fault &&
								result.retired_instructions == padding &&
								cpu.rip == ((code + at) & mask),
								(where + " reported the wrong RIP when it "
									"faulted pushing").c_str());
					}
				}
			}
		}
	}
}

/* LEA of a RIP-relative operand, once its address word is gone. */
void rip_relative_lea_needs_no_rip_word()
{
	using namespace uml::cpu::x86_64;
	for (const auto position : {std::size_t{0}, std::size_t{37}}) {
		TestMemory memory;
		std::vector<std::uint8_t> bytes;
		std::size_t padding = 0;
		append_padding(bytes, position, padding);
		bytes.insert(bytes.end(), {0x48, 0x8d, 0x05, 0x10, 0x00, 0x00, 0x00});
		bytes.insert(bytes.end(), {0x0f, 0x05});
		load_code(memory, 0x1000, bytes);
		const auto built = require_built(
			gadgets::build(memory, 0x1000, 400), "RIP-relative LEA");
		CpuState cpu{};
		cpu.rip = 0x1000;
		const auto result = gadgets::execute(built.program, cpu, memory, 1000);
		require(result.event == uml::cpu::EngineEvent::system_call &&
				cpu.gpr(Register::rax) == 0x1000 + position + 7U + 0x10U,
				"a RIP-relative LEA computed the wrong address");
	}
}

/* Every opcode of the one-byte, 0F, 0F 38 and 0F 3A maps, and the VEX forms of
 * the last three, with the memory operand [rbx] in each of its eight reg
 * fields: whatever gadget it takes, or the runtime's step, an instruction that
 * leaves with a fault, or a trap, without retiring names itself in RIP. This
 * reaches the forms the cases above do not list, and any gadget that spends
 * the stream word its exit reads the position from. It runs in both modes. */
void every_faulting_form_reports_its_position()
{
	using namespace uml::cpu::x86_64;
	/* The bytes before the opcode, and how many reg fields to try: the groups
	 * that share an opcode by it are in the one-byte and 0F maps, and the BMI
	 * group of 0F 38 is the only one beyond; the others differ in registers
	 * alone. */
	struct Prefix final {
		std::vector<std::uint8_t> bytes;
		unsigned reg_fields;
	};
	std::vector<Prefix> prefixes;
	const std::vector<std::vector<std::uint8_t>> legacy_prefixes{
		{}, {0x66}, {0xf3}, {0xf2}};
	for (const auto &legacy : legacy_prefixes) {
		prefixes.push_back({legacy, 8});
		for (const auto &[escape, reg_fields] : {
			std::pair{std::vector<std::uint8_t>{0x0f}, 8U},
			std::pair{std::vector<std::uint8_t>{0x0f, 0x38}, 4U},
			std::pair{std::vector<std::uint8_t>{0x0f, 0x3a}, 4U}}) {
			auto bytes = legacy;
			bytes.insert(bytes.end(), escape.begin(), escape.end());
			prefixes.push_back({std::move(bytes), reg_fields});
		}
	}
	/* The three-byte VEX prefix of each map, with both vector lengths and each
	 * implied prefix. */
	for (std::uint8_t map = 1; map <= 3; ++map) {
		for (std::uint8_t length = 0; length <= 1; ++length) {
			for (std::uint8_t implied = 0; implied <= 3; ++implied)
				prefixes.push_back({{0xc4,
					static_cast<std::uint8_t>(0xe0U | map),
					static_cast<std::uint8_t>(0x78U | (length << 2U) |
						implied)}, map == 1 ? 8U : 4U});
		}
	}

	std::size_t faulted = 0;
	for (const auto mode : {ExecutionMode::long64, ExecutionMode::compat32}) {
		const bool compat = mode == ExecutionMode::compat32;
		const std::uint64_t mask = compat ? 0xffff'ffffULL : ~std::uint64_t{};
		/* A position whose ten bits alternate. */
		constexpr std::size_t position = 683;
		TestMemory memory;
		gadgets::Executor executor;
		executor.prepare(memory, 1, 1, 1);
		for (const auto &[prefix, reg_fields] : prefixes) {
			for (unsigned opcode = 0; opcode < 256U; ++opcode) {
				for (unsigned reg = 0; reg < reg_fields; ++reg) {
					std::vector<std::uint8_t> bytes;
					std::size_t padding = 0;
					/* Fifteen-byte NOPs, the longest instruction, keep the
					 * block short. */
					for (std::size_t filled = 0; filled + 15U <= position;
						filled += 15U, ++padding)
						bytes.insert(bytes.end(), {0x66, 0x66, 0x66, 0x66,
							0x66, 0x66, 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00,
							0x00, 0x00, 0x00});
					append_padding(bytes, position - bytes.size(), padding);
					const auto at = bytes.size();
					bytes.insert(bytes.end(), prefix.begin(), prefix.end());
					bytes.push_back(static_cast<std::uint8_t>(opcode));
					bytes.push_back(static_cast<std::uint8_t>(
						0x03U | (reg << 3U)));
					/* Room for the immediate of any form. */
					bytes.insert(bytes.end(), 6, 0x90);
					bytes.insert(bytes.end(), {0x0f, 0x05});
					load_code(memory, 0x1230, bytes);
					const auto built = gadgets::build(
						memory, 0x1230, mode, 400);
					if (built.status != gadgets::BuildStatus::built)
						continue;
					/* An operand wholly outside the memory, a replay of the
					 * instructions up to it, and one that starts in the last
					 * byte of the memory and ends beyond it, which the
					 * gadgets settle on their page-crossing paths. */
					const std::array<std::pair<std::size_t, std::uint64_t>, 3>
						runs{{{1000, unmapped_address},
						{padding + 1U, unmapped_address},
						{1000, 16383}}};
					for (const auto &[budget, operand] : runs) {
						CpuState cpu{};
						cpu.execution_mode = mode;
						cpu.rip = 0x1230;
						cpu.gpr(Register::rsp) = 0x800;
						cpu.gpr(Register::rbx) = operand;
						cpu.gpr(Register::rbp) = operand;
						cpu.gpr(Register::rsi) = operand;
						cpu.gpr(Register::rdi) = operand;
						cpu.gpr(Register::rcx) = 4;
						const auto result = gadgets::execute(
							built.program, cpu, memory, executor, budget);
						const auto event = result.event;
						using Event = uml::cpu::EngineEvent;
						if (result.retired_instructions != padding ||
							(event != Event::memory_fault &&
							 event != Event::general_protection &&
							 event != Event::floating_point_trap &&
							 event != Event::bound_range))
							continue;
						++faulted;
						require(cpu.rip == ((0x1230 + at) & mask),
								("opcode " + std::to_string(opcode) + " /" +
									std::to_string(reg) + " after " +
									std::to_string(prefix.size()) +
									" prefix bytes" +
									(compat ? " (32-bit)" : " (64-bit)") +
									" left with event " +
									std::to_string(static_cast<int>(event)) +
									" and RIP " + std::to_string(cpu.rip))
									.c_str());
					}
				}
			}
		}
	}
	/* The sweep reaches the forms; it must not pass by reaching none. */
	require(faulted > 40000U,
		("only " + std::to_string(faulted) +
			" encodings faulted in the sweep").c_str());
}

enum class ImmediateOp : std::uint8_t {
	add = 0, bit_or = 1, bit_and = 4, subtract = 5, bit_xor = 6, compare = 7,
	test = 8, move = 9,
};

/* `op` of `bits` against the operand `modrm`, `tail` name (a register or a
 * memory operand without its /r digit) and an immediate. */
[[nodiscard]] std::vector<std::uint8_t> encode_immediate(ImmediateOp op,
	unsigned bits, std::int64_t immediate, std::uint8_t modrm,
	const std::vector<std::uint8_t> &tail)
{
	std::vector<std::uint8_t> code;
	if (bits == 16)
		code.push_back(0x66);
	if (bits == 64)
		code.push_back(0x48);
	std::uint8_t digit = static_cast<std::uint8_t>(op);
	std::size_t size = bits == 8 ? 1U : bits == 16 ? 2U : 4U;
	if (op == ImmediateOp::test) {
		code.push_back(bits == 8 ? 0xf6 : 0xf7);
		digit = 0;
	} else if (op == ImmediateOp::move) {
		code.push_back(bits == 8 ? 0xc6 : 0xc7);
		digit = 0;
	} else if (bits == 8) {
		code.push_back(0x80);
	} else if (immediate >= -128 && immediate < 128) {
		code.push_back(0x83);
		size = 1;
	} else {
		code.push_back(0x81);
	}
	code.push_back(static_cast<std::uint8_t>(modrm | (digit << 3U)));
	code.insert(code.end(), tail.begin(), tail.end());
	for (std::size_t byte = 0; byte < size; ++byte)
		code.push_back(static_cast<std::uint8_t>(
			static_cast<std::uint64_t>(immediate) >> (8U * byte)));
	code.insert(code.end(), {0x0f, 0x05});
	return code;
}

/* ALU and MOV against memory with every width and immediates on either side of
 * what the access word holds, against the same operation on a register, which
 * carries its immediate in a word of its own. */
void memory_immediates_match_the_register_form()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t data = 0x800;
	const std::array<std::int64_t, 17> immediates{0, 1, -1, 2, 0x7f, -0x80,
		0xff, 8191, 8192, -8192, -8193, 0x7fff, -0x8000, 0xffff, 0x7fffffff,
		-0x80000000LL, 0xffffffffLL};
	const std::array<std::uint64_t, 6> initial{0, 1, 0x80, 0xffff,
		0x7fffffffffffffffULL, 0x8000000000000000ULL};
	const std::array<ImmediateOp, 8> operations{ImmediateOp::add,
		ImmediateOp::bit_or, ImmediateOp::bit_and, ImmediateOp::subtract,
		ImmediateOp::bit_xor, ImmediateOp::compare, ImmediateOp::test,
		ImmediateOp::move};
	std::size_t checked = 0;
	for (const unsigned bits : {8U, 16U, 32U, 64U}) {
		const std::uint64_t mask = bits == 64 ? ~std::uint64_t{} :
			(std::uint64_t{1} << bits) - 1U;
		/* What the instruction can encode at this width. */
		const std::int64_t low = bits == 8 ? -128 : bits == 16 ? -0x8000 :
			-0x80000000LL;
		const std::int64_t high = bits == 8 ? 255 : bits == 16 ? 0xffff :
			bits == 32 ? 0xffffffffLL : 0x7fffffff;
		for (const auto op : operations) {
			for (const auto immediate : immediates) {
				if (immediate < low || immediate > high)
					continue;
				for (const auto value : initial) {
					for (const bool indexed : {false, true}) {
						const auto memory_code = encode_immediate(op, bits,
							immediate, indexed ? 0x44 : 0x43,
							indexed ? std::vector<std::uint8_t>{0x4b, 0x08} :
								std::vector<std::uint8_t>{0x08});
						const auto register_code = encode_immediate(op, bits,
							immediate, 0xc2, {});

						TestMemory with_memory;
						load_code(with_memory, 0x100, memory_code);
						std::array<std::uint8_t, 8> stored_value{};
						for (unsigned byte = 0; byte < 8U; ++byte)
							stored_value[byte] = static_cast<std::uint8_t>(
								value >> (8U * byte));
						with_memory.fill(data,
							std::as_bytes(std::span{stored_value}));
						CpuState memory_cpu{};
						memory_cpu.rip = 0x100;
						memory_cpu.gpr(Register::rbx) = indexed ?
							data - 10U : data - 8U;
						memory_cpu.gpr(Register::rcx) = 1;
						const auto memory_built = require_built(
							gadgets::build(with_memory, 0x100),
							"memory immediate did not build");
						const auto memory_result = gadgets::execute(
							memory_built.program, memory_cpu, with_memory, 64);

						TestMemory with_register;
						load_code(with_register, 0x100, register_code);
						CpuState register_cpu{};
						register_cpu.rip = 0x100;
						register_cpu.gpr(Register::rdx) = value;
						const auto register_built = require_built(
							gadgets::build(with_register, 0x100),
							"register immediate did not build");
						const auto register_result = gadgets::execute(
							register_built.program, register_cpu,
							with_register, 64);

						const auto where = "ALU op " +
							std::to_string(static_cast<unsigned>(op)) + " at " +
							std::to_string(bits) + " bits, immediate " +
							std::to_string(immediate) + ", operand " +
							std::to_string(value) +
							(indexed ? ", indexed" : ", based");
						require(memory_result.event ==
									uml::cpu::EngineEvent::system_call &&
								register_result.event ==
									uml::cpu::EngineEvent::system_call,
								(where + " did not reach SYSCALL").c_str());
						const auto stored = with_memory.qword(data);
						const bool writes = op != ImmediateOp::compare &&
							op != ImmediateOp::test;
						const std::uint64_t expected = writes ?
							(register_cpu.gpr(Register::rdx) & mask) :
							(value & mask);
						require((stored & mask) == expected,
								(where + " stored the wrong value").c_str());
						require((stored & ~mask) == (value & ~mask),
							(where + " wrote beyond its width").c_str());
						materialize_flags(memory_cpu);
						materialize_flags(register_cpu);
						if (op != ImmediateOp::move)
							require((memory_cpu.rflags & 0x8d5U) ==
									(register_cpu.rflags & 0x8d5U),
								(where + " set different flags").c_str());
						++checked;
					}
				}
			}
		}
	}
	require(checked > 2000, "the immediate sweep ran too few cases");
}

/* An immediate the access word holds selects the baked bank, and one it
 * cannot hold the ordinary bank. */
void wide_memory_immediates_take_the_ordinary_bank()
{
	using namespace uml::cpu::x86_64;
	const auto baked_add = reinterpret_cast<std::uintptr_t>(
		gadgets::baked::uml_x86_64_baked_add_32_mi[3U]);
	const auto entry = [](std::int32_t immediate) {
		const auto word = static_cast<std::uint32_t>(immediate);
		return first_native_entry({0x81, 0x43, 0x08,
			static_cast<std::uint8_t>(word),
			static_cast<std::uint8_t>(word >> 8U),
			static_cast<std::uint8_t>(word >> 16U),
			static_cast<std::uint8_t>(word >> 24U), 0x0f, 0x05});
	};
	require(entry(8191) == baked_add,
			"an immediate of 8191 left the baked bank");
	require(entry(-8192) == baked_add,
		"an immediate of -8192 left the baked bank");
	require(entry(8192) != baked_add,
		"an immediate of 8192 stayed in a bank that cannot hold it");
	require(entry(-8193) != baked_add,
		"an immediate of -8193 stayed in a bank that cannot hold it");
}

/* The address entry hands its access word to the access entry behind it, and
 * an LEA that shares the entry takes only the displacement. */
void indexed_addresses_resolve_every_displacement()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t data = 0x800;
	constexpr std::uint64_t marker = 0x0123'4567'89ab'cdefULL;
	const std::array<std::int32_t, 6> displacements{5, 0x12345678,
		-0x12345678, 0x7fffffff, std::numeric_limits<std::int32_t>::min(),
		-1};
	for (unsigned scale = 0; scale < 4U; ++scale) {
		for (const auto displacement : displacements) {
			for (const bool based : {true, false}) {
				const std::uint64_t index = 3;
				const std::uint64_t scaled = index << scale;
				/* A base-less address is the displacement and the scaled
				 * index alone, so the memory sits where they say. */
				const std::uint64_t base = based ?
					data - static_cast<std::uint64_t>(
						static_cast<std::int64_t>(displacement)) - scaled : 0;
				const std::uint64_t address = base + scaled +
					static_cast<std::uint64_t>(
						static_cast<std::int64_t>(displacement));
				const auto word = static_cast<std::uint32_t>(displacement);
				const std::uint8_t modrm = based ? 0x84 : 0x04;
				const std::uint8_t sib = static_cast<std::uint8_t>(
					(scale << 6U) | (1U << 3U) | (based ? 3U : 5U));
				const auto operand = [&](std::uint8_t opcode, bool wide) {
					std::vector<std::uint8_t> code;
					if (wide)
						code.push_back(0x48);
					code.insert(code.end(), {opcode, modrm, sib,
						static_cast<std::uint8_t>(word),
						static_cast<std::uint8_t>(word >> 8U),
						static_cast<std::uint8_t>(word >> 16U),
						static_cast<std::uint8_t>(word >> 24U), 0x0f, 0x05});
					return code;
				};
				for (const bool wide : {true, false}) {
					TestMemory memory;
					load_code(memory, 0x100, operand(0x8d, wide));
					CpuState cpu{};
					cpu.rip = 0x100;
					cpu.gpr(Register::rbx) = base;
					cpu.gpr(Register::rcx) = index;
					const auto built = require_built(
						gadgets::build(memory, 0x100), "LEA did not build");
					const auto result = gadgets::execute(
						built.program, cpu, memory, 64);
					const auto expected = wide ? address :
						(address & 0xffffffffU);
					require(result.event ==
								uml::cpu::EngineEvent::system_call &&
							cpu.gpr(Register::rax) == expected,
						"an indexed LEA computed the wrong address");
				}
				if (address != data)
					continue;
				TestMemory memory;
				load_code(memory, 0x100, operand(0x8b, true));
				const std::array<std::uint64_t, 1> marker_word{marker};
				memory.fill(data, std::as_bytes(std::span{marker_word}));
				CpuState cpu{};
				cpu.rip = 0x100;
				cpu.gpr(Register::rbx) = base;
				cpu.gpr(Register::rcx) = index;
				const auto built = require_built(
					gadgets::build(memory, 0x100), "indexed load did not build");
				const auto result = gadgets::execute(
					built.program, cpu, memory, 64);
				require(result.event == uml::cpu::EngineEvent::system_call &&
						cpu.gpr(Register::rax) == marker,
					"an indexed load read the wrong address");
			}
		}
	}
}

} // namespace

/* Runs a test with executors that share an address space's translations, or
 * keep them to themselves, whatever the environment says. The variable is
 * read as each executor is made. */
template <typename Test>
void with_sharing(bool shared, Test test)
{
	constexpr const char *name = "DARWIN_UML_GADGET_SHARED_BODIES";
	const char *held = std::getenv(name);
	const std::string previous = held != nullptr ? held : "";
	const bool had = held != nullptr;
	setenv(name, shared ? "1" : "0", 1);
	test();
	if (had)
		setenv(name, previous.c_str(), 1);
	else
		unsetenv(name);
}

/* Runs a test with executors that keep their translations to themselves, then
 * with executors that share them. */
template <typename Test>
void in_both_modes(Test test)
{
	with_sharing(false, test);
	with_sharing(true, test);
}

int main(int argc, char **argv)
{
	/* The suite once more with every executor keeping its translations to
	 * itself, which the default no longer does. */
	if (argc > 1 && std::strcmp(argv[1], "private") == 0)
		setenv("DARWIN_UML_GADGET_SHARED_BODIES", "0", 1);
	/* The address spaces the kernel destroyed alone, which a sanitizer tree
	 * can afford to run; the rest of the suite is long. */
	if (argc > 1 && std::strcmp(argv[1], "dead-generations") == 0) {
		a_destroyed_generation_is_let_go_of_when_the_count_moves();
		the_prepared_generation_can_be_let_go_of();
		a_number_given_out_again_runs_its_new_code();
		a_destroyed_generation_keeps_its_window_place();
		in_both_modes(
			letting_go_of_an_address_space_survives_a_failed_allocation);
		in_both_modes(
			retiring_the_oldest_address_space_survives_a_failed_allocation);
		in_both_modes(
			a_shed_address_space_is_asked_for_again_after_a_failed_allocation);
		the_budget_bounds_what_an_executor_keeps();
		return 0;
	}
	decode_scaled_address();
	a_legacy_prefix_after_rex_voids_it();
	cache_reuses_and_revalidates_programs();
	cache_validates_inline_source_spans();
	cache_counts_validation_faults_and_races();
	cache_rejects_unrelated_code_pages_by_stamp();
	a_moving_code_sequence_keeps_unchanged_programs();
	slow_executable_writes_publish_every_code_page();
	a_slow_store_across_a_page_boundary_publishes_both_pages();
	native_executable_stores_publish_every_code_page();
	a_settled_link_owes_its_page_a_fresh_publication();
	an_unpublished_code_page_strands_another_translation();
	a_masked_store_publishes_the_page_its_elements_reach();
	a_moving_code_sequence_rebuilds_changed_programs();
	a_recorded_code_stamp_is_no_newer_than_its_source();
	a_write_racing_a_first_translation_does_not_survive();
	a_write_racing_a_second_page_does_not_survive();
	a_write_stamped_after_a_first_translation_does_not_survive();
	an_undisturbed_first_translation_is_still_cached();
	a_decode_at_a_page_edge_faults_where_it_stopped();
	a_block_longer_than_the_fetch_window_decodes();
	cache_retires_the_oldest_generation();
	a_recycled_address_space_id_serves_no_old_translation();
	a_destroyed_generation_is_let_go_of_when_the_count_moves();
	the_prepared_generation_can_be_let_go_of();
	a_number_given_out_again_runs_its_new_code();
	a_destroyed_generation_keeps_its_window_place();
	in_both_modes(
		letting_go_of_an_address_space_survives_a_failed_allocation);
	in_both_modes(
		retiring_the_oldest_address_space_survives_a_failed_allocation);
	in_both_modes(
		a_shed_address_space_is_asked_for_again_after_a_failed_allocation);
	the_budget_bounds_what_an_executor_keeps();
	a_validation_does_not_outlive_the_epochs_it_counts();
	cache_links_common_successors();
	cache_chains_direct_branches();
	cache_chains_indirect_transfers();
	an_unrelated_code_change_leaves_a_link_armed();
	with_sharing(false, a_link_sweep_releases_what_only_replaced_blocks_named);
	linked_call_crosses_a_disjoint_executable_stack_write();
	linked_call_stops_when_the_stack_write_hits_its_target();
	short_blocks_retain_only_their_translated_storage();
	a_replay_keeps_what_it_decoded();
	budget_limited_replay_matches_the_whole_block();
	budget_limited_replay_ignores_rewritten_guest_code();
	budget_limited_replay_faults_where_the_block_does();
	executable_stores_publish_a_code_change_each();
	store_publishes_only_where_some_executor_caches_code();
	overlapping_executable_store_stops_immediately();
	executable_store_links_only_to_unchanged_code();
	distant_code_write_cools_its_linked_block();
	code_write_resets_unvalidated_successors();
	slow_executable_writes_publish_every_store();
	execute_scalar_control_flow();
	materialized_flags_match_the_reference();
	carry_branches_match_the_reference();
	execute_absolute_memory_test_branch();
	execute_lazy_zero_branches();
	zero_conditional_moves_preserve_pending_flags();
	execute_compat32_test_greater_after_syscall();
	execute_compat32_futex_result_validator();
	shifts_overlay_lazy_arithmetic_flags();
	lazy_shift_flags_match_the_definition();
	execute_segment_memory();
	execute_addressing_shapes();
	execute_byte_sign_masks();
	execute_call_and_return();
	decode_compat32_defaults();
	decode_compat32_vex_aliases();
	decode_gather_register_aliases();
	execute_compat32_stack_and_int80();
	execute_compat32_pushfd_popfd();
	execute_compat32_enter_frames();
	execute_compat32_legacy_segment_stack();
	execute_long64_interrupt_return_stack_switch();
	execute_long64_interrupt_return_operand_sizes();
	interrupt_return_masks_user_flags();
	expose_consistent_xsave_cpuid_state();
	cpuid_leaves_the_run();
	wrap_compat32_instruction_pointer();
	execute_compat32_tls_segment();
	reject_program_mode_mismatch();
	chunk_repeating_string_for_interrupt();
	bulk_string_runs_match_the_element_definition();
	repeated_store_into_executable_page_publishes_its_run();
	repeated_move_faults_on_a_run_boundary();
	fused_conditions_match_the_deferred_evaluation();
	ordinary_bank_conditions_match_the_deferred_evaluation();
	fused_conditions_cover_exactly_the_legal_pairs();
	lazy_carry_reaches_every_reader();
	a_settled_carry_survives_an_increment();
	conditional_moves_write_the_destination_either_way();
	fused_conditional_jumps_take_the_same_edge();
	cpuid_table_matches_the_gadget();
	x87_zero_results_keep_their_sign();
	preserve_precise_memory_fault();
	retire_valid_prefix_before_decode_fault();
	migrated_gadgets_are_native_only();
	native_divide_traps_precisely();
	native_into_reports_overflow_precisely();
	icebp_raises_a_debug_trap_after_itself();
	bound_checks_a_signed_range();
	privileged_instructions_raise_general_protection();
	segment_base_writes_reject_non_canonical_addresses();
	overlong_instructions_raise_general_protection();
	random_instructions_expose_no_host_state();
	processor_numbering_is_cpu_zero_node_zero();
	software_interrupt_vectors_trap();
	one_instruction_budget_steps_linked_programs();
	popf_setting_trap_flag_leaves_at_the_next_instruction();
	iret_setting_trap_flag_arms_the_next_instruction();
	resume_flag_lasts_until_an_instruction_starts();
	decode_names_int_vectors_and_fwait();
	a_single_step_runs_one_repeat_iteration();
	loads_that_unmask_exceptions_leave();
	checked_simd_exceptions_fault_before_the_result();
	checked_simd_denormal_operands_follow_x86();
	checked_simd_invalid_operations_follow_x86();
	checked_fma_invalid_operations_follow_x86();
	checked_comparison_invalid_operations_follow_x86();
	checked_horizontal_flags_follow_x86();
	checked_underflow_follows_x86();
	checked_dot_product_flags_follow_x86();
	checked_unmasked_precision_follows_x86();
	checked_sum_flags_follow_x86();
	checked_integer_rounding_precision_follows_x86();
	checked_x87_exceptions_wait_for_the_next_instruction();
	checked_builds_stop_before_checked_instructions();
	unmasked_x87_exceptions_leave_linked_blocks();
	unmasked_x87_control_words_run_clean_code_through();
	unmasked_simd_exceptions_leave_linked_blocks();
	simd_checks_match_the_checked_path();
	mxcsr_loads_leave_when_checks_change();
	mxcsr_loads_switch_the_simd_exact_path();
	faulting_comisd_keeps_earlier_flags();
	alignment_checks_follow_rflags_ac();
	byte_shifts_discard_a_lane_for_an_oversized_count();
	vex_vmovlhps_preserves_aliased_source();
	vector_results_cross_only_a_transfer_that_stays_inline();
	a_call_out_between_vector_gadgets_keeps_the_result();
	vector_integer_forwards_only_from_the_gadget_before();
	a_lane_extract_store_keeps_the_lane_across_a_miss();
	a_lane_extract_store_keeps_the_lane_across_a_page();
	a_masked_store_keeps_the_unselected_bytes_across_a_miss();
	vector_invalid_results_use_x86_indefinite_nan();
	locked_memory_updates_select_exclusive_gadgets();
	glibc_rwlock_xadd_fast_path_is_exact();
	gobject_closure_bitfield_cmpxchg_is_exact();
	xadd_survives_concurrent_vcpus();
	cmpxchg_survives_concurrent_vcpus();
	locked_increment_survives_concurrent_vcpus();
	locked_forms_agree_at_every_operand_address();
	compare_exchange_pair_requires_alignment();
	unaligned_locked_increment_survives_concurrent_vcpus();
	mask_store_uses_its_segment_and_address_size();
	mask_store_keeps_concurrent_unselected_bytes();
	faulting_split_operand_releases_the_lock();
	guest_stores_reach_other_cpus_in_program_order();
	guest_memory_gadgets_end_in_an_ordering_barrier();
	baked_faults_rebuild_the_rip_from_the_block_start();
	baked_accesses_stop_at_the_position_field();
	baked_overlap_exits_rebuild_the_rip_from_the_block_start();
	baked_stack_entries_carry_no_stream_word();
	baked_stack_faults_report_their_instruction();
	compat32_stack_faults_wrap_at_4gib();
	baked_stack_overlaps_report_the_next_instruction();
	baked_stack_crosses_page_edges();
	baked_stack_faults_inside_a_long_block();
	a_linked_block_recovers_its_stack_instruction();
	an_unmasked_exception_is_found_behind_stack_instructions();
	ordinary_forms_report_their_position();
	ordinary_forms_report_their_position_across_4gib();
	ordinary_overlap_exits_report_the_next_address();
	call_pushes_and_reports_the_next_and_own_address();
	rip_relative_lea_needs_no_rip_word();
	every_faulting_form_reports_its_position();
	memory_immediates_match_the_register_form();
	wide_memory_immediates_take_the_ordinary_bank();
	indexed_addresses_resolve_every_displacement();
	return 0;
}
