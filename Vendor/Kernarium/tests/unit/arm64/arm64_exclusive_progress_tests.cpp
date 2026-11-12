// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * An architecturally valid load/store-exclusive loop makes progress on both
 * engines whatever the TLB holds. The gadget engine runs the host's own
 * exclusives, so a translation miss between the load and the store loses the
 * host monitor, and a loop that takes the same miss on every pass would never
 * finish. These cases put the pages a loop touches into one TLB entry (the
 * TLB is direct-mapped), so a plain store in the loop evicts the target
 * page's write entry on every pass:
 *
 * - the shape of ART's MessageStack.pushMessage, whose store sits before the
 *   exclusive load, with one and with two colliding stores;
 * - a store between the load and the store-exclusive, which no preparation by
 *   the load can survive, so the gadget engine's store-exclusive must complete
 *   by itself and the JIT's monitor must stand;
 * - every width, ordering and the pair forms;
 * - the faults a load or a store-exclusive takes, the cases in which the
 *   software completion must refuse, and a copy-on-write break between the
 *   load and the store, which replaces the page under the loop.
 *
 * A loop that does not finish inside the harness's instruction budget fails
 * as a wrong terminal event; a finished one is checked for state and memory.
 */
#include "arm64_engine_test_support.hpp"
#include "jit_test_support.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "static_mmu_test_support.hpp"

#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"
#include "memory/tlb.hpp"
#include "platform/apple/host_features.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <vector>

namespace engine = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;

namespace {

using uml::testing::require;

constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr unsigned page_shift = DARWIN_UML_GUEST_PAGE_SHIFT;
constexpr std::uint64_t code_base = engine::engine_test_code_base;
constexpr std::uint64_t data_base = engine::engine_test_data_base;
constexpr std::size_t top_offset = 0x40;
constexpr std::size_t message_offset = 16;

/* Encodings, checked against the assembler. */
constexpr std::uint32_t svc = 0xd400'0001U;
constexpr std::uint32_t clrex = 0xd503'3f5fU;

[[nodiscard]] constexpr std::uint32_t ldar(unsigned size, unsigned rt,
	unsigned rn)
{
	return 0x08df'fc00U | (size << 30) | (rn << 5) | rt;
}

[[nodiscard]] constexpr std::uint32_t ldxr(unsigned size, bool acquire,
	unsigned rt, unsigned rn)
{
	return 0x085f'7c00U | (size << 30) | (acquire ? 0x8000U : 0U) |
		(rn << 5) | rt;
}

[[nodiscard]] constexpr std::uint32_t stxr(unsigned size, bool release,
	unsigned rs, unsigned rt, unsigned rn)
{
	return 0x0800'7c00U | (size << 30) | (release ? 0x8000U : 0U) |
		(rs << 16) | (rn << 5) | rt;
}

[[nodiscard]] constexpr std::uint32_t ldxp(bool wide, bool acquire,
	unsigned rt, unsigned rt2, unsigned rn)
{
	return (wide ? 0xc87f'0000U : 0x887f'0000U) |
		(acquire ? 0x8000U : 0U) | (rt2 << 10) | (rn << 5) | rt;
}

[[nodiscard]] constexpr std::uint32_t stxp(bool wide, bool release,
	unsigned rs, unsigned rt, unsigned rt2, unsigned rn)
{
	return (wide ? 0xc820'0000U : 0x8820'0000U) |
		(release ? 0x8000U : 0U) | (rs << 16) | (rt2 << 10) |
		(rn << 5) | rt;
}

[[nodiscard]] constexpr std::uint32_t ldp(bool wide, unsigned rt,
	unsigned rt2, unsigned rn)
{
	return (wide ? 0xa940'0000U : 0x2940'0000U) | (rt2 << 10) |
		(rn << 5) | rt;
}

[[nodiscard]] constexpr std::uint32_t str_w(unsigned rt, unsigned rn,
	unsigned byte_offset)
{
	return 0xb900'0000U | ((byte_offset / 4U) << 10) | (rn << 5) | rt;
}

[[nodiscard]] constexpr std::uint32_t cmp(bool wide, unsigned rn, unsigned rm)
{
	return (wide ? 0xeb00'001fU : 0x6b00'001fU) | (rm << 16) | (rn << 5);
}

/* A word displacement in bits 23:5, as B.cond and CBNZ encode it. */
[[nodiscard]] constexpr std::uint32_t displacement(std::size_t from,
	std::size_t target)
{
	const auto words = static_cast<std::int64_t>(target) / 4 -
		static_cast<std::int64_t>(from) / 4;
	return static_cast<std::uint32_t>(words & 0x7ffff) << 5;
}

[[nodiscard]] constexpr std::uint32_t b_ne(std::size_t from, std::size_t target)
{
	return 0x5400'0001U | displacement(from, target);
}

[[nodiscard]] constexpr std::uint32_t cbnz_w(unsigned rt, std::size_t from,
	std::size_t target)
{
	return 0x3500'0000U | displacement(from, target) | rt;
}

static_assert(ldar(2, 0, 1) == 0x88df'fc20U);
static_assert(ldxr(3, false, 3, 1) == 0xc85f'7c23U);
static_assert(stxr(3, true, 4, 5, 1) == 0xc804'fc25U);
static_assert(ldxp(true, true, 3, 8, 1) == 0xc87f'a023U);
static_assert(stxp(false, false, 4, 5, 9, 1) == 0x8824'2425U);
static_assert(ldp(true, 10, 11, 1) == 0xa940'2c2aU);
static_assert(str_w(6, 2, 16) == 0xb900'1046U);
static_assert(cmp(false, 3, 0) == 0x6b00'007fU);
static_assert(b_ne(16, 0) == 0x54ff'ff81U);
static_assert(cbnz_w(4, 24, 0) == 0x35ff'ff44U);

/* The TLB set of a guest page key: the production table's index. */
[[nodiscard]] constexpr std::uint64_t tlb_set(std::uint64_t key)
{
	return key & (uml::memory::tlb_entry_count - 1);
}

/* The address of the `ordinal`-th page (1 and up) after the target's whose
 * set is the target's, or, with `colliding` false, is not. */
[[nodiscard]] std::uint64_t page_after(std::uint64_t target, unsigned ordinal,
	bool colliding)
{
	const auto target_key = target >> page_shift;
	unsigned seen = 0;
	for (std::uint64_t key = target_key + 1;; ++key) {
		if ((tlb_set(key) == tlb_set(target_key)) != colliding)
			continue;
		if (++seen == ordinal)
			return key << page_shift;
	}
}

/* A deterministic fill, different in every page and byte. */
[[nodiscard]] std::vector<std::byte> pattern_page(std::uint64_t seed)
{
	std::vector<std::byte> bytes(page);
	std::uint64_t state = seed * 0x9e37'79b9'7f4a'7c15ULL + 1;
	for (auto &byte : bytes) {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		byte = static_cast<std::byte>(state >> 56);
	}
	return bytes;
}

void put(std::vector<std::byte> &bytes, std::size_t offset,
	std::uint64_t value, std::size_t size)
{
	std::memcpy(bytes.data() + offset, &value, size);
}

[[nodiscard]] std::uint64_t get(const std::vector<std::byte> &bytes,
	std::size_t offset, std::size_t size)
{
	std::uint64_t value = 0;
	std::memcpy(&value, bytes.data() + offset, size);
	return value;
}

[[nodiscard]] std::uint64_t mask(std::size_t size)
{
	return size == 8 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << (8 * size)) - 1;
}

[[nodiscard]] std::vector<engine::ProgramWord> program(
	const std::vector<std::uint32_t> &words)
{
	std::vector<engine::ProgramWord> code;
	for (std::size_t index = 0; index < words.size(); ++index)
		code.push_back({index * 4, words[index]});
	return code;
}

/* Cases that stop with the monitor image set compare without it. */
void ignore_monitor(const arm64::CpuState &, const arm64::CpuState &expected,
	const arm64::CpuState &actual, std::string_view name,
	std::string_view engine_name, std::uint32_t instruction)
{
	auto normalized = expected;
	normalized.exclusive_address = actual.exclusive_address;
	normalized.exclusive_key = actual.exclusive_key;
	normalized.exclusive_value = actual.exclusive_value;
	normalized.exclusive_value2 = actual.exclusive_value2;
	normalized.exclusive_version = actual.exclusive_version;
	normalized.exclusive_size = actual.exclusive_size;
	engine::require_architecture_equal(normalized, actual, name, engine_name,
		instruction);
}

/* The data every loop case uses. */
constexpr std::uint64_t first_value = 0x1122'3344'5566'7788ULL;
constexpr std::uint64_t second_value = 0x99aa'bbcc'ddee'ff01ULL;
constexpr std::uint64_t new_first = 0xa1a2'a3a4'a5a6'a7a8ULL;
constexpr std::uint64_t new_second = 0xb1b2'b3b4'b5b6'b7b8ULL;
constexpr std::uint64_t message_data = 0xc1c2'c3c4'c5c6'c7c8ULL;

/* What a loop test varies. */
struct Loop final {
	/* log2 of the size: 0 to 3 for singles, 2 and 3 for pairs. */
	unsigned size{2};
	bool pair{};
	bool acquire{};
	bool release{};
	/* How many plain stores the loop makes, each to a page of its own. */
	unsigned stores{1};
	/* Whether they sit between the exclusive load and the store, where no
	 * preparation by the load survives. */
	bool inside{};
	/* Whether their pages share the target's TLB set. */
	bool colliding{true};
};

[[nodiscard]] std::string name_of(const Loop &loop)
{
	char text[160];
	std::snprintf(text, sizeof(text),
		"%s size=%u %s/%s stores=%u %s %s", loop.pair ? "pair" : "single",
		loop.size, loop.acquire ? "acquire" : "relaxed",
		loop.release ? "release" : "relaxed", loop.stores,
		loop.inside ? "inside" : "before",
		loop.colliding ? "colliding" : "separate");
	return text;
}

/* A substring of the loops to run, when the command line names one. */
std::string_view loop_filter;

/* A loop that ends with the new value in the target and the status zero.
 *   before: ldar/ldp; stores; ldxr/ldxp; [cmp; b.ne]; stxr/stxp; cbnz
 *   inside: ldxr/ldxp; stores; stxr/stxp; cbnz
 * x1 is the target, x2 and x7 the pages the stores go to. */
void check_loop(engine::EngineHarness &harness, const Loop &loop)
{
	if (!loop_filter.empty() &&
	    name_of(loop).find(loop_filter) == std::string::npos)
		return;
	const std::size_t size = std::size_t{1} << loop.size;
	const bool wide = loop.size == 3;
	const std::uint64_t top_page = data_base;
	std::array<std::uint64_t, 2> message_page{};
	for (unsigned index = 0; index < loop.stores; ++index)
		message_page[index] = page_after(top_page, index + 1U,
			loop.colliding);

	std::vector<std::byte> top_initial = pattern_page(1);
	put(top_initial, top_offset, first_value, size);
	if (loop.pair)
		put(top_initial, top_offset + size, second_value, size);
	auto top_expected = top_initial;
	put(top_expected, top_offset, new_first, size);
	if (loop.pair)
		put(top_expected, top_offset + size, new_second, size);
	std::array<std::vector<std::byte>, 2> message_initial;
	std::array<std::vector<std::byte>, 2> message_expected;
	for (unsigned index = 0; index < loop.stores; ++index) {
		message_initial[index] = pattern_page(2 + index);
		message_expected[index] = message_initial[index];
		put(message_expected[index], message_offset, message_data, 4);
	}

	std::vector<std::uint32_t> words;
	const auto emit_stores = [&] {
		static constexpr std::array<unsigned, 2> registers{2, 7};
		for (unsigned index = 0; index < loop.stores; ++index)
			words.push_back(str_w(6, registers[index], message_offset));
	};
	if (!loop.inside) {
		words.push_back(loop.pair ?
			ldp(wide, 10, 11, 1) : ldar(loop.size, 0, 1));
		emit_stores();
	}
	words.push_back(loop.pair ?
		ldxp(wide, loop.acquire, 3, 8, 1) :
		ldxr(loop.size, loop.acquire, 3, 1));
	if (loop.inside) {
		emit_stores();
	} else if (!loop.pair) {
		words.push_back(cmp(wide, 3, 0));
		words.push_back(b_ne(words.size() * 4, 0));
	}
	words.push_back(loop.pair ?
		stxp(wide, loop.release, 4, 5, 9, 1) :
		stxr(loop.size, loop.release, 4, 5, 1));
	words.push_back(cbnz_w(4, words.size() * 4, 0));
	words.push_back(clrex);
	words.push_back(svc);

	arm64::CpuState initial{};
	initial.registers[1] = top_page + top_offset;
	initial.registers[2] = message_page[0];
	initial.registers[7] = message_page[1];
	initial.registers[5] = new_first;
	initial.registers[9] = new_second;
	initial.registers[6] = message_data;
	initial.registers[4] = 0xdead'beefULL;
	auto expected = initial;
	expected.pc = code_base + words.size() * 4;
	expected.registers[3] = first_value & mask(size);
	expected.registers[4] = 0;
	if (loop.pair) {
		expected.registers[8] = second_value & mask(size);
		if (!loop.inside) {
			expected.registers[10] = first_value & mask(size);
			expected.registers[11] = second_value & mask(size);
		}
	} else if (!loop.inside) {
		expected.registers[0] = first_value & mask(size);
		/* CMP of equal operands sets Z and C. */
		expected.nzcv = 0x6000'0000U;
	}

	std::vector<engine::MemoryRegion> regions;
	regions.push_back({
		.address = top_page,
		.initial = top_initial,
		.expected = top_expected,
	});
	for (unsigned index = 0; index < loop.stores; ++index)
		regions.push_back({
			.address = message_page[index],
			.initial = message_initial[index],
			.expected = message_expected[index],
		});

	const auto name = name_of(loop);
	const auto code = program(words);
	harness.check_program({
		.name = name,
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = engine::NativeRequirements::baseline(),
		.memory = regions,
		.variable_instruction_count = true,
	});
}

/* The loops with their stores before the exclusive load (the shape of ART's
 * MessageStack.pushMessage) or between the pair. */
void every_loop(engine::EngineHarness &harness, bool inside)
{
	const auto sizes = [&](const Loop &base) {
		for (unsigned size = 0; size < 4; ++size) {
			auto loop = base;
			loop.size = size;
			check_loop(harness, loop);
		}
		for (const unsigned size : {2U, 3U}) {
			auto loop = base;
			loop.size = size;
			loop.pair = true;
			check_loop(harness, loop);
		}
	};
	for (const unsigned stores : {1U, 2U}) {
		/* Both orderings on both sides, and each one alone. */
		for (const bool acquire : {false, true}) {
			for (const bool release : {false, true})
				sizes({
					.acquire = acquire,
					.release = release,
					.stores = stores,
					.inside = inside,
				});
		}
	}
}

/* The same loops with pages of other sets, the control: a failure above is
 * the collision's. */
void separate_pages(engine::EngineHarness &harness)
{
	for (const bool inside : {false, true}) {
		for (const unsigned size : {2U, 3U}) {
			check_loop(harness, {
				.size = size,
				.acquire = true,
				.release = true,
				.stores = 2,
				.inside = inside,
				.colliding = false,
			});
			check_loop(harness, {
				.size = size,
				.pair = true,
				.acquire = true,
				.release = true,
				.stores = 2,
				.inside = inside,
				.colliding = false,
			});
		}
	}
}

/* A one-shot sequence: load, then plain stores to colliding pages, then a
 * store-exclusive that takes the software path; `refuse` names what the
 * sequence changed so that the store-exclusive must report failure. */
enum class Refusal {
	no_load,
	other_address,
	changed_value,
	changed_second_value,
};

void check_refusal(engine::EngineHarness &harness, Refusal refusal, bool pair)
{
	const bool wide = true;
	const std::size_t size = 8;
	const std::uint64_t top_page = data_base;
	const auto message_one = page_after(top_page, 1, true);
	const auto message_two = page_after(top_page, 2, true);
	const auto other = page_after(top_page, 3, true);

	auto top_initial = pattern_page(11);
	put(top_initial, top_offset, first_value, size);
	put(top_initial, top_offset + size, second_value, size);
	auto top_expected = top_initial;
	auto other_initial = pattern_page(12);
	std::array<std::vector<std::byte>, 2> message_initial{
		pattern_page(13), pattern_page(14)};
	auto message_expected = message_initial;
	for (auto &bytes : message_expected)
		put(bytes, message_offset, message_data, 4);

	std::vector<std::uint32_t> words;
	if (refusal != Refusal::no_load)
		words.push_back(pair ? ldxp(wide, false, 3, 8, 1) :
			ldxr(3, false, 3, 1));
	/* The pages' plain stores evict the target's write entry. */
	if (refusal == Refusal::changed_value) {
		words.push_back(str_w(7, 1, 0));
		put(top_expected, top_offset, message_data & 0xffff'ffffU, 4);
	} else if (refusal == Refusal::changed_second_value) {
		words.push_back(str_w(7, 1, 8));
		put(top_expected, top_offset + 8, message_data & 0xffff'ffffU, 4);
	}
	words.push_back(str_w(6, 2, message_offset));
	words.push_back(str_w(6, 10, message_offset));
	const unsigned base = refusal == Refusal::other_address ? 11 : 1;
	words.push_back(pair ? stxp(wide, false, 4, 5, 9, base) :
		stxr(3, false, 4, 5, base));
	words.push_back(clrex);
	words.push_back(svc);

	arm64::CpuState initial{};
	initial.registers[1] = top_page + top_offset;
	initial.registers[2] = message_one;
	initial.registers[10] = message_two;
	initial.registers[11] = other + top_offset;
	initial.registers[5] = new_first;
	initial.registers[9] = new_second;
	initial.registers[6] = message_data;
	initial.registers[7] = message_data;
	initial.registers[4] = 0xdead'beefULL;
	auto expected = initial;
	expected.pc = code_base + words.size() * 4;
	expected.registers[4] = 1;
	if (refusal != Refusal::no_load) {
		expected.registers[3] = first_value;
		if (pair)
			expected.registers[8] = second_value;
	}

	std::vector<engine::MemoryRegion> regions{
		{.address = top_page, .initial = top_initial,
			.expected = top_expected},
		{.address = message_one, .initial = message_initial[0],
			.expected = message_expected[0]},
		{.address = message_two, .initial = message_initial[1],
			.expected = message_expected[1]},
		{.address = other, .initial = other_initial},
	};
	static constexpr std::array<const char *, 4> names{
		"refuses without a load", "refuses another address",
		"refuses a changed value", "refuses a changed second value"};
	const std::string name = std::string{pair ? "pair " : "single "} +
		names[static_cast<std::size_t>(refusal)];
	const auto code = program(words);
	harness.check_program({
		.name = name,
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = engine::NativeRequirements::baseline(),
		.memory = regions,
	});
}

void software_completion_refuses(engine::EngineHarness &harness)
{
	for (const bool pair : {false, true}) {
		check_refusal(harness, Refusal::no_load, pair);
		check_refusal(harness, Refusal::other_address, pair);
		check_refusal(harness, Refusal::changed_value, pair);
	}
	check_refusal(harness, Refusal::changed_second_value, true);
}

/* A load or a store-exclusive that cannot complete leaves the block with the
 * fault of its own access and its own instruction as the PC: a load reads, a
 * store-exclusive writes, and a load of a page the guest may read does not
 * report the write it could not do. */
void faults(engine::EngineHarness &harness)
{
	const std::uint64_t page_address = data_base;
	const auto initial_page = pattern_page(21);
	for (const bool pair : {false, true}) {
		const char *kind = pair ? "pair" : "single";
		arm64::CpuState initial{};
		initial.registers[1] = page_address + top_offset;
		initial.registers[5] = new_first;
		initial.registers[9] = new_second;
		initial.registers[4] = 0xdead'beefULL;

		/* Unmapped: the load reads, the store-exclusive writes. */
		{
			const std::vector<std::uint32_t> words{
				pair ? ldxp(true, false, 3, 8, 1) : ldxr(3, false, 3, 1),
				svc};
			const auto code = program(words);
			auto expected = initial;
			expected.pc = code_base;
			const auto name = std::string{kind} + " load of an unmapped page";
			harness.check_program({
				.name = name,
				.code = code,
				.initial = initial,
				.expected = expected,
				.event = engine::ExpectedEvent::memory_fault,
				.native = engine::NativeRequirements::baseline(),
				.fault = {page_address + top_offset,
					arm64::MemoryAccess::read},
				.compare_fault = true,
				.validator = ignore_monitor,
			});
		}
		{
			const std::vector<std::uint32_t> words{
				pair ? stxp(true, false, 4, 5, 9, 1) :
					stxr(3, false, 4, 5, 1),
				svc};
			const auto code = program(words);
			auto expected = initial;
			expected.pc = code_base;
			const auto name = std::string{kind} +
				" store-exclusive to an unmapped page";
			harness.check_program({
				.name = name,
				.code = code,
				.initial = initial,
				.expected = expected,
				.event = engine::ExpectedEvent::memory_fault,
				.native = engine::NativeRequirements::baseline(),
				.fault = {page_address + top_offset,
					arm64::MemoryAccess::write},
				.compare_fault = true,
				.validator = ignore_monitor,
			});
		}
		/* Read-only: the load succeeds and the store-exclusive faults. */
		{
			auto data = initial_page;
			put(data, top_offset, first_value, 8);
			put(data, top_offset + 8, second_value, 8);
			const std::vector<std::uint32_t> words{
				pair ? ldxp(true, false, 3, 8, 1) : ldxr(3, false, 3, 1),
				pair ? stxp(true, false, 4, 5, 9, 1) :
					stxr(3, false, 4, 5, 1),
				svc};
			const auto code = program(words);
			auto expected = initial;
			expected.pc = code_base + 4;
			expected.registers[3] = first_value;
			if (pair)
				expected.registers[8] = second_value;
			const std::array regions{
				engine::MemoryRegion{
					.address = page_address,
					.initial = data,
					.writable = false,
				},
			};
			const auto name = std::string{kind} +
				" load succeeds and store-exclusive faults on a read-only page";
			harness.check_program({
				.name = name,
				.code = code,
				.initial = initial,
				.expected = expected,
				.event = engine::ExpectedEvent::memory_fault,
				.native = engine::NativeRequirements::baseline(),
				.fault = {page_address + top_offset,
					arm64::MemoryAccess::write},
				.compare_fault = true,
				.memory = regions,
				.validator = ignore_monitor,
			});
		}
	}
}

/* ------------------------------------------------------------------------ */
/* A copy-on-write break between the load and the store                      */

class AddressSpace final {
public:
	explicit AddressSpace(unsigned long generation) : generation_{generation}
	{
		require(darwin_uml_static_vm_activate(generation_) == 0);
	}

	~AddressSpace()
	{
		require(darwin_uml_static_vm_destroy(generation_) == 0);
	}

	AddressSpace(const AddressSpace &) = delete;
	AddressSpace &operator=(const AddressSpace &) = delete;

private:
	unsigned long generation_;
};

/* One page of guest memory over a backing of its own. */
class Page final {
public:
	Page(unsigned long generation, std::uint64_t address,
		std::span<const std::byte> contents, int protection,
		bool guest_code = false)
		: backing_{"uml-exclusive.XXXXXX", page}
	{
		backing_.put(0, contents);
		mapping_.emplace(generation, address, backing_, 1, protection,
			guest_code);
	}

	Page(const Page &) = delete;
	Page &operator=(const Page &) = delete;

	[[nodiscard]] std::vector<std::byte> contents() const
	{
		std::vector<std::byte> bytes(page);
		backing_.get(0, bytes);
		return bytes;
	}

private:
	uml::testing::ScratchBacking backing_;
	std::optional<uml::emulator::tests::GenerationMapping> mapping_;
};

/* ART's loop on a page the guest may only read, so the store-exclusive
 * faults, and the kernel's answer, a new writable page with the contents,
 * under the loop. The engine resumes at the store-exclusive with the CpuState
 * as the runtime restores it (the monitor image cleared) and the loop has to
 * finish. */
void copy_on_write_break(uml::cpu::EngineKind kind, std::string_view label)
{
	if (!uml::cpu::cpu_engine_available(kind))
		return;
	static unsigned long next_generation = 0x20000;
	const auto generation = next_generation++;
	AddressSpace space{generation};
	auto host_engine = uml::cpu::make_cpu_engine(kind,
		uml::cpu::EngineConfiguration{
			.host_capabilities =
				uml::platform::apple::probed_host_capabilities()});
	require(host_engine != nullptr);

	const std::uint64_t top_page = data_base;
	const auto message_page = page_after(top_page, 1, true);
	auto top_contents = pattern_page(31);
	put(top_contents, top_offset, first_value, 4);

	std::vector<std::uint32_t> words;
	words.push_back(ldar(2, 0, 1));
	words.push_back(str_w(6, 2, message_offset));
	const auto load_offset = words.size() * 4;
	words.push_back(ldxr(2, true, 3, 1));
	words.push_back(cmp(false, 3, 0));
	words.push_back(b_ne(words.size() * 4, 0));
	const auto store_offset = words.size() * 4;
	words.push_back(stxr(2, true, 4, 5, 1));
	words.push_back(cbnz_w(4, words.size() * 4, 0));
	words.push_back(clrex);
	words.push_back(svc);
	std::vector<std::byte> code_bytes(page);
	std::memcpy(code_bytes.data(), words.data(), words.size() * 4);

	Page code{generation, code_base, code_bytes, PROT_READ | PROT_EXEC, true};
	Page message{generation, message_page, pattern_page(32),
		PROT_READ | PROT_WRITE};
	auto old_top = std::make_unique<Page>(generation, top_page, top_contents,
		PROT_READ);

	arm64::CpuState cpu{};
	cpu.pc = code_base;
	cpu.registers[1] = top_page + top_offset;
	cpu.registers[2] = message_page;
	cpu.registers[5] = new_first;
	cpu.registers[6] = message_data;

	const auto run = [&](std::size_t budget) {
		uml::memory::StaticGuestMemory memory;
		memory.prepare(generation,
			darwin_uml_static_vm_sequences(generation).mapping_changes);
		uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory,
			.translated_memory = &memory,
			.generation = generation,
			.instruction_budget = budget,
		};
		return host_engine->run(request);
	};

	/* The engines fault at the store-exclusive, or, where the JIT checks the
	 * whole load/store pair once, at the load; both resume correctly. */
	const auto first = run(64);
	if (first.reason != uml::cpu::ExitReason::memory_fault ||
	    first.fault.address != top_page + top_offset ||
	    first.fault.access != uml::memory::MemoryAccess::write ||
	    cpu.pc < code_base + load_offset ||
	    cpu.pc > code_base + store_offset) {
		std::fprintf(stderr,
			"%.*s: the store-exclusive on a read-only page did not fault "
			"as a write (reason %d, pc %#llx)\n",
			static_cast<int>(label.size()), label.data(),
			static_cast<int>(first.reason),
			static_cast<unsigned long long>(cpu.pc));
		std::abort();
	}
	require(get(old_top->contents(), top_offset, 4) ==
		(first_value & 0xffff'ffffU));

	/* The break: the new page replaces the old one at the same address. */
	old_top.reset();
	Page new_top{generation, top_page, top_contents, PROT_READ | PROT_WRITE};
	cpu.exclusive_address = ~std::uint64_t{0};
	cpu.exclusive_key = 0;
	cpu.exclusive_value = 0;
	cpu.exclusive_value2 = 0;
	cpu.exclusive_version = 0;
	cpu.exclusive_size = 0;

	const auto second = run(64);
	if (second.reason != uml::cpu::ExitReason::system_call) {
		std::fprintf(stderr,
			"%.*s: the loop did not finish after the page was replaced "
			"(reason %d, pc %#llx)\n",
			static_cast<int>(label.size()), label.data(),
			static_cast<int>(second.reason),
			static_cast<unsigned long long>(cpu.pc));
		std::abort();
	}
	require(cpu.registers[4] == 0);
	require(get(new_top.contents(), top_offset, 4) == (new_first & 0xffff'ffffU));
	require(get(message.contents(), message_offset, 4) ==
		(message_data & 0xffff'ffffU));
}

} // namespace

int main(int argc, char **argv)
{
	/* With a group name, only that group runs: a failing group aborts the
	 * binary, so naming one shows which groups an engine passes. A second
	 * argument limits the loops of a group to the names that contain it. */
	const std::string_view only = argc > 1 ? argv[1] : "";
	if (argc > 2)
		loop_filter = argv[2];
	const auto selected = [&](std::string_view group) {
		return only.empty() || only == group;
	};
	if (selected("before") || selected("inside") || selected("separate") ||
	    selected("refuse") || selected("faults")) {
		engine::EngineHarness harness;
		if (selected("before"))
			every_loop(harness, false);
		if (selected("inside"))
			every_loop(harness, true);
		if (selected("separate"))
			separate_pages(harness);
		if (selected("refuse"))
			software_completion_refuses(harness);
		if (selected("faults"))
			faults(harness);
		harness.print_summary("arm64-exclusive-progress");
	}
	/* The harness holds an engine of each kind, and the JIT admits one
	 * executor at a time: the engines of the break cases come after it. */
	if (selected("cow")) {
		copy_on_write_break(uml::cpu::EngineKind::gadgets, "gadgets");
		copy_on_write_break(uml::cpu::EngineKind::jit, "jit");
	}
	return 0;
}
