// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Only an exception may cost a guest its exclusive monitor. Each case runs a
 * load-exclusive, one event and a store-exclusive to the same location once,
 * and the store must succeed. The events are the ones the engines cause
 * without an exception:
 *
 * - a branch to a block the engine translates in between;
 * - a branch the engine has linked;
 * - an indirect branch whose site misses, to a block already translated, and
 *   one through a site the engine has trained;
 * - straight-line code that crosses block ends and a code page;
 * - a load whose page misses the TLB, while the host page is resident;
 * - an instruction the gadget engine's library runs (A32 and T32);
 * - another thread's stores to another line of the page, all the while;
 * - the guest's own store to another line, its pair store (a spill's STP or
 *   STRD) to another page, and in A64 a structure store (ST2) to that page,
 *   one that crosses into the next page, and one through a tagged pointer;
 * - in A64 the whole pair through a base that carries a tag in its top byte,
 *   which the engine ignores (top-byte ignore), so the monitor guards the
 *   location and not the pointer.
 *
 * Every case runs in A64, A32 and T32, for a single register (a doubleword in
 * A64, a word in AArch32) and a pair of words, on both engines. A store may
 * still fail as hardware's may, when the host thread takes an interrupt, a
 * preemption or a page fault between the pair (the first run of an engine
 * path faults its code in), so a case passes when one of its attempts
 * succeeds. Each attempt starts a new address space and engine, so the event
 * recurs in every one: a store that fails every time is an engine that loses
 * the monitor.
 */
#include "jit_test_support.hpp"
#include "require.hpp"
#include "runtime_test_route.hpp"
#include "scratch_backing.hpp"
#include "static_mmu_test_support.hpp"
#include "wait_until.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"

#include "uml_darwin_static_mmu.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <vector>

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::emulator::tests::GenerationMapping;
using uml::testing::require;

constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
/* Below 4 GiB, so the AArch32 cases can use them too. */
constexpr std::uint32_t code_base = 0x4f00'0000U;
constexpr std::uint32_t data_base = 0x5100'0000U;
constexpr std::size_t code_pages = 2;
constexpr std::size_t data_pages = 4;
/* Where a program starts, and where a branch between the pair lands. */
constexpr std::size_t entry = 0x100;
constexpr std::size_t target = 0x600;
/* The straight-line program starts this far before the second code page
 * and repeats the pair, five instructions, this often, so it crosses the page
 * and enough block limits that one of them, 59 or 64 instructions apart,
 * falls between a load and its store whatever the prologue's length. */
constexpr std::size_t straight_entry = page - 0x120;
constexpr unsigned straight_pairs = 64;
/* The word a load between the pair reads: a page whose TLB set is not the
 * pair's. */
constexpr std::uint32_t other_page_offset = 2 * page + 0x40;
/* The word another thread stores to: the pair's page, another line. */
constexpr std::uint32_t other_line_offset = 0x200;
constexpr unsigned attempts = 16;
constexpr std::uint64_t initial_first = 10;
constexpr std::uint64_t initial_second = 20;

enum class Isa { a64, a32, t32 };
enum class Width { single, pair };
enum class Between {
	nothing,
	translated_branch,
	linked_branch,
	indirect_miss,
	indirect_trained,
	straight_line,
	tlb_miss,
	library,
	other_thread,
	own_store,
	own_pair_store,
	own_structure_store,
	own_structure_store_crossing,
	own_tagged_structure_store,
	tagged_base,
};

[[nodiscard]] const char *name(Isa isa) noexcept
{
	switch (isa) {
	case Isa::a64:
		return "A64";
	case Isa::a32:
		return "A32";
	case Isa::t32:
		return "T32";
	}
	return "?";
}

[[nodiscard]] const char *name(Width width) noexcept
{
	return width == Width::single ? "single" : "pair";
}

[[nodiscard]] const char *name(Between between) noexcept
{
	switch (between) {
	case Between::nothing:
		return "nothing between";
	case Between::translated_branch:
		return "a branch to a block translated in between";
	case Between::linked_branch:
		return "a linked branch";
	case Between::indirect_miss:
		return "an indirect branch missing its site";
	case Between::indirect_trained:
		return "an indirect branch through its trained site";
	case Between::straight_line:
		return "straight-line code across blocks and a page";
	case Between::tlb_miss:
		return "a load that misses the TLB";
	case Between::library:
		return "an instruction the library runs";
	case Between::other_thread:
		return "another thread storing to another line";
	case Between::own_store:
		return "a store of its own to another line";
	case Between::own_pair_store:
		return "a pair store of its own to another page";
	case Between::own_structure_store:
		return "a structure store of its own to another page";
	case Between::own_structure_store_crossing:
		return "a structure store of its own across a page";
	case Between::own_tagged_structure_store:
		return "a structure store of its own through a tagged pointer";
	case Between::tagged_base:
		return "nothing between, through a tagged base";
	}
	return "?";
}

/* A program image. A64 and A32 instructions are words; a T32 instruction is
 * one halfword, or two given as one word, first halfword high. */
class Code final {
public:
	explicit Code(Isa isa) : isa_{isa}, image_(code_pages * page) {}

	[[nodiscard]] std::size_t here() const noexcept { return at_; }
	void at(std::size_t offset) noexcept { at_ = offset; }
	[[nodiscard]] std::span<const std::byte> image() const noexcept
	{
		return image_;
	}

	void word(std::uint32_t value)
	{
		if (isa_ == Isa::t32) {
			half(static_cast<std::uint16_t>(value >> 16));
			half(static_cast<std::uint16_t>(value));
			return;
		}
		require(at_ + 4 <= image_.size());
		std::memcpy(image_.data() + at_, &value, 4);
		at_ += 4;
	}

	void half(std::uint16_t value)
	{
		require(isa_ == Isa::t32 && at_ + 2 <= image_.size());
		std::memcpy(image_.data() + at_, &value, 2);
		at_ += 2;
	}

	/* Store the initial value(s) of the pair's location. */
	void initialise(Width width)
	{
		switch (isa_) {
		case Isa::a64:
			if (width == Width::single) {
				word(0xd280'0144U);	// movz x4, #10
				word(0xf900'0004U);	// str x4, [x0]
			} else {
				word(0x5280'0144U);	// movz w4, #10
				word(0xb900'0004U);	// str w4, [x0]
				word(0x5280'0284U);	// movz w4, #20
				word(0xb900'0404U);	// str w4, [x0, #4]
			}
			return;
		case Isa::a32:
			word(0xe3a0'400aU);		// mov r4, #10
			word(0xe580'4000U);		// str r4, [r0]
			if (width == Width::pair) {
				word(0xe3a0'4014U);	// mov r4, #20
				word(0xe580'4004U);	// str r4, [r0, #4]
			}
			return;
		case Isa::t32:
			half(0x240a);			// movs r4, #10
			half(0x6004);			// str r4, [r0]
			if (width == Width::pair) {
				half(0x2414);		// movs r4, #20
				half(0x6044);		// str r4, [r0, #4]
			}
			return;
		}
	}

	/* The load-exclusive of r2 (and r3) from [r0]. */
	void load(Width width)
	{
		const bool pair = width == Width::pair;
		switch (isa_) {
		case Isa::a64:	// ldxr x2, [x0] / ldxp w2, w3, [x0]
			word(pair ? 0x887f'0c02U : 0xc85f'7c02U);
			return;
		case Isa::a32:	// ldrex r2, [r0] / ldrexd r2, r3, [r0]
			word(pair ? 0xe1b0'2f9fU : 0xe190'2f9fU);
			return;
		case Isa::t32:	// ldrex r2, [r0] / ldrexd r2, r3, [r0]
			word(pair ? 0xe8d0'237fU : 0xe850'2f00U);
			return;
		}
	}

	/* r2 += 1. */
	void increment(Width width)
	{
		switch (isa_) {
		case Isa::a64:	// add x2, x2, #1 / add w2, w2, #1
			word(width == Width::pair ? 0x1100'0442U : 0x9100'0442U);
			return;
		case Isa::a32:
			word(0xe282'2001U);		// add r2, r2, #1
			return;
		case Isa::t32:
			half(0x3201);			// adds r2, #1
			return;
		}
	}

	/* The store-exclusive of r2 (and r3) to [r0], status in r8, then
	 * r9 |= r8. */
	void store(Width width)
	{
		const bool pair = width == Width::pair;
		switch (isa_) {
		case Isa::a64:	// stxr w8, x2, [x0] / stxp w8, w2, w3, [x0]
			word(pair ? 0x8828'0c02U : 0xc808'7c02U);
			word(0xaa08'0129U);		// orr x9, x9, x8
			return;
		case Isa::a32:	// strex r8, r2, [r0] / strexd r8, r2, r3, [r0]
			word(pair ? 0xe1a0'8f92U : 0xe180'8f92U);
			word(0xe189'9008U);		// orr r9, r9, r8
			return;
		case Isa::t32:	// strex r8, r2, [r0] / strexd r8, r2, r3, [r0]
			word(pair ? 0xe8c0'2378U : 0xe840'2800U);
			word(0xea49'0908U);		// orr.w r9, r9, r8
			return;
		}
	}

	/* r5 += 1: an instruction of no consequence. */
	void filler()
	{
		switch (isa_) {
		case Isa::a64:
			word(0x9100'04a5U);		// add x5, x5, #1
			return;
		case Isa::a32:
			word(0xe285'5001U);		// add r5, r5, #1
			return;
		case Isa::t32:
			half(0x3501);			// adds r5, #1
			return;
		}
	}

	void supervisor_call()
	{
		switch (isa_) {
		case Isa::a64:
			word(0xd400'0001U);		// svc #0
			return;
		case Isa::a32:
			word(0xef00'0000U);		// svc #0
			return;
		case Isa::t32:
			half(0xdf00);			// svc #0
			return;
		}
	}

	/* B to `to`. */
	void branch(std::size_t to)
	{
		const auto from = static_cast<std::int64_t>(at_);
		const auto distance = static_cast<std::int64_t>(to) - from;
		switch (isa_) {
		case Isa::a64:
			word(0x1400'0000U |
				(static_cast<std::uint32_t>(distance / 4) &
					0x3ff'ffffU));
			return;
		case Isa::a32:
			word(0xea00'0000U |
				(static_cast<std::uint32_t>((distance - 8) / 4) &
					0xff'ffffU));
			return;
		case Isa::t32:
			half(static_cast<std::uint16_t>(0xe000U |
				(static_cast<std::uint32_t>((distance - 4) / 2) &
					0x7ffU)));
			return;
		}
	}

	/* BR x7 / BX r7. */
	void indirect_branch()
	{
		switch (isa_) {
		case Isa::a64:
			word(0xd61f'00e0U);
			return;
		case Isa::a32:
			word(0xe12f'ff17U);
			return;
		case Isa::t32:
			half(0x4738);
			return;
		}
	}

	/* ldr w4, [x6] / ldr r4, [r6]. */
	void other_load()
	{
		switch (isa_) {
		case Isa::a64:
			word(0xb940'00c4U);
			return;
		case Isa::a32:
			word(0xe596'4000U);
			return;
		case Isa::t32:
			half(0x6834);
			return;
		}
	}

	/* str w5, [x0, #0x200] / str r5, [r0, #0x200]: another line. */
	void own_store()
	{
		switch (isa_) {
		case Isa::a64:
			word(0xb902'0005U);
			return;
		case Isa::a32:
			word(0xe580'5200U);
			return;
		case Isa::t32:
			word(0xf8c0'5200U);
			return;
		}
	}

	/* stp w4, w5, [x6, #8] / strd r4, r5, [r6, #8]: a spill's shape. */
	void own_pair_store()
	{
		switch (isa_) {
		case Isa::a64:
			word(0x2901'14c4U);
			return;
		case Isa::a32:
			word(0xe1c6'40f8U);
			return;
		case Isa::t32:
			word(0xe9c6'4502U);
			return;
		}
	}

	/* st2 {v0.4s, v1.4s}, [x6]: A64 only. */
	void own_structure_store()
	{
		require(isa_ == Isa::a64);
		word(0x4c00'88c0U);
	}

	/* usad8 r4, r5, r6, which the gadget engine runs in its library. */
	void library_instruction()
	{
		require(isa_ != Isa::a64);
		word(isa_ == Isa::a32 ? 0xe784'f615U : 0xfb75'f406U);
	}

private:
	Isa isa_;
	std::vector<std::byte> image_;
	std::size_t at_{};
};

/* The program of a case, and where it starts. */
struct Program final {
	Code code;
	std::size_t start{};
	unsigned pairs{1};
};

[[nodiscard]] Program program(Isa isa, Width width, Between between)
{
	Program result{Code{isa}};
	auto &c = result.code;
	if (between == Between::straight_line ||
	    between == Between::other_thread) {
		result.start = between == Between::straight_line ?
			straight_entry : entry;
		result.pairs = straight_pairs;
		c.at(result.start);
		c.initialise(width);
		for (unsigned pair = 0; pair < straight_pairs; ++pair) {
			c.load(width);
			c.increment(width);
			c.filler();
			c.store(width);
		}
		c.supervisor_call();
		return result;
	}
	result.start = entry;
	c.at(entry);
	c.initialise(width);
	c.load(width);
	switch (between) {
	case Between::translated_branch:
	case Between::linked_branch:
		c.branch(target);
		c.at(target);
		break;
	case Between::indirect_miss:
	case Between::indirect_trained:
		c.indirect_branch();
		c.at(target);
		break;
	case Between::tlb_miss:
		c.other_load();
		break;
	case Between::library:
		c.library_instruction();
		break;
	case Between::own_store:
		c.own_store();
		break;
	case Between::own_pair_store:
		c.own_pair_store();
		break;
	case Between::own_structure_store:
	case Between::own_structure_store_crossing:
	case Between::own_tagged_structure_store:
		c.own_structure_store();
		break;
	default:
		break;
	}
	c.increment(width);
	c.store(width);
	c.supervisor_call();
	return result;
}

class Generation final {
public:
	explicit Generation(unsigned long generation) : generation_{generation}
	{
		require(darwin_uml_static_vm_activate(generation_) == 0);
	}
	~Generation() { require(darwin_uml_static_vm_destroy(generation_) == 0); }

	Generation(const Generation &) = delete;
	Generation &operator=(const Generation &) = delete;

	[[nodiscard]] unsigned long number() const noexcept { return generation_; }

private:
	unsigned long generation_;
};

/* One address space holding a program, and an engine to run it. */
class Machine final {
public:
	Machine(unsigned long generation, uml::cpu::EngineKind kind, Isa isa,
		const Program &built)
		: isa_{isa}, generation_{generation},
		  code_{"uml-exclusive-events-code.XXXXXX", code_pages * page},
		  data_{"uml-exclusive-events-data.XXXXXX", data_pages * page},
		  engine_{uml::cpu::make_cpu_engine(kind)}
	{
		require(engine_ != nullptr);
		code_.put(0, built.code.image());
		code_mapping_ = std::make_unique<GenerationMapping>(
			generation_.number(), code_base, code_, code_pages,
			PROT_READ | PROT_EXEC, true);
		data_mapping_ = std::make_unique<GenerationMapping>(
			generation_.number(), data_base, data_, data_pages,
			PROT_READ | PROT_WRITE, false);
	}

	/* A host pointer to the data word at `offset`. */
	[[nodiscard]] std::byte *data(std::uint32_t offset) const
	{
		void *host = nullptr;
		require(darwin_uml_static_vm_translate(generation_.number(),
			data_base + offset, DARWIN_UML_STATIC_WRITE, &host) ==
			DARWIN_UML_STATIC_TRANSLATED);
		return static_cast<std::byte *>(host);
	}

	/* The CPU at `offset` in the code, with x0/r0 at the data word at
	 * `data_offset`. */
	[[nodiscard]] arm64::CpuState cpu_at(std::size_t offset,
		std::uint32_t data_offset = 0) const
	{
		arm64::CpuState cpu{};
		const auto address = code_base + static_cast<std::uint32_t>(offset);
		cpu.registers[0] = data_base + data_offset;
		cpu.registers[6] = data_base + other_page_offset;
		cpu.registers[7] = code_base + target +
			(isa_ == Isa::t32 ? 1U : 0U);
		cpu.registers[8] = 0x5a;
		cpu.registers[9] = 0;
		if (isa_ == Isa::a64) {
			cpu.pc = address;
		} else {
			cpu.a32 = 1;
			cpu.pc = isa_ == Isa::t32 ?
				arm64::aarch32::t32_key(address) :
				arm64::aarch32::a32_key(address);
		}
		return cpu;
	}

	[[nodiscard]] uml::cpu::ExitReason run(arm64::CpuState &cpu)
	{
		uml::memory::StaticGuestMemory memory;
		memory.prepare(generation_.number(),
			darwin_uml_static_vm_sequences(generation_.number())
				.mapping_changes);
		uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory,
			.translated_memory = &memory,
			.generation = generation_.number(),
			.instruction_budget = 4096,
		};
		const auto result = isa_ == Isa::a64 ? engine_->run(request) :
			engine_->run_aarch32(request);
		return result.reason;
	}

private:
	Isa isa_;
	Generation generation_;
	uml::testing::ScratchBacking code_;
	uml::testing::ScratchBacking data_;
	std::unique_ptr<GenerationMapping> code_mapping_;
	std::unique_ptr<GenerationMapping> data_mapping_;
	std::unique_ptr<uml::cpu::CpuEngine> engine_;
};

struct Case final {
	std::string_view engine;
	Isa isa;
	Width width;
	Between between;
};

[[noreturn]] void failed(const Case &c, const char *what,
	unsigned long long value)
{
	std::fprintf(stderr, "%.*s, %s %s, %s: %s (%#llx)\n",
		static_cast<int>(c.engine.size()), c.engine.data(), name(c.isa),
		name(c.width), name(c.between), what, value);
	std::abort();
}

template<typename T>
[[nodiscard]] T load_word(const std::byte *host) noexcept
{
	T value;
	std::memcpy(&value, host, sizeof(value));
	return value;
}

/* Runs a case's program once in `machine`; true when every store-exclusive
 * in it succeeded. */
[[nodiscard]] bool attempt(const Case &c, Machine &machine,
	const Program &built)
{
	auto cpu = machine.cpu_at(built.start);
	/* With top-byte ignore on, which Android's tasks have, the engines
	 * ignore the tag a pointer carries in its top byte. */
	constexpr std::uint64_t tag = std::uint64_t{0x5a} << 56;
	cpu.tbi_enabled = c.between == Between::tagged_base ||
		c.between == Between::own_tagged_structure_store;
	if (c.between == Between::tagged_base)
		cpu.registers[0] |= tag;
	if (c.between == Between::own_tagged_structure_store)
		cpu.registers[6] |= tag;
	/* A structure store of 32 bytes that ends one page on: the JIT leaves
	 * a store that crosses a page to its semantic helper. */
	if (c.between == Between::own_structure_store_crossing)
		cpu.registers[6] = data_base + 3 * page - 16;
	const auto reason = machine.run(cpu);
	if (reason != uml::cpu::ExitReason::system_call)
		failed(c, "the program did not finish; exit reason",
			static_cast<unsigned long long>(reason));
	const auto status = cpu.registers[8] & 0xffff'ffffU;
	if (status > 1)
		failed(c, "a store-exclusive's status", status);
	const bool stored = (cpu.registers[9] & 0xffff'ffffU) == 0;
	const auto *word = machine.data(0);
	const std::uint64_t first = c.isa == Isa::a64 &&
			c.width == Width::single ?
		load_word<std::uint64_t>(word) :
		load_word<std::uint32_t>(word);
	if (stored && first != initial_first + built.pairs)
		failed(c, "every store-exclusive succeeded, but memory holds",
			first);
	if (!stored && (first < initial_first ||
			first >= initial_first + built.pairs))
		failed(c, "a store-exclusive failed, but memory holds", first);
	if (c.width == Width::pair &&
	    load_word<std::uint32_t>(word + 4) != initial_second)
		failed(c, "the pair's second word",
			load_word<std::uint32_t>(word + 4));
	return stored;
}

/* Prepares what a case's event needs before the attempt: a linked branch or
 * a trained site needs a run of the program, an indirect miss its target
 * translated on its own, a TLB miss the host page resident. */
void prepare(const Case &c, Machine &machine, const Program &built)
{
	switch (c.between) {
	case Between::linked_branch:
	case Between::indirect_trained: {
		auto cpu = machine.cpu_at(built.start);
		if (machine.run(cpu) != uml::cpu::ExitReason::system_call)
			failed(c, "the first run did not finish", 0);
		return;
	}
	case Between::indirect_miss: {
		/* The target alone, storing to a word of its own. */
		auto cpu = machine.cpu_at(target, 0x100);
		if (machine.run(cpu) != uml::cpu::ExitReason::system_call)
			failed(c, "the target did not finish", 0);
		return;
	}
	case Between::tlb_miss: {
		/* Fault the host page in, so that the load between the pair
		 * misses only the engine's TLB. */
		const auto *host = machine.data(other_page_offset);
		[[maybe_unused]] const auto value =
			*reinterpret_cast<const volatile std::uint32_t *>(host);
		return;
	}
	default:
		return;
	}
}

/* The attempts the case took to succeed, or 0 when none did. */
unsigned run_case(const Case &c, uml::cpu::EngineKind kind,
	unsigned long &generation)
{
	const auto built = program(c.isa, c.width, c.between);
	for (unsigned index = 1; index <= attempts; ++index) {
		Machine machine{generation++, kind, c.isa, built};
		prepare(c, machine, built);
		if (c.between != Between::other_thread) {
			if (attempt(c, machine, built))
				return index;
			continue;
		}
		auto *other = reinterpret_cast<std::uint32_t *>(
			machine.data(other_line_offset));
		std::atomic_bool started{false};
		std::atomic_bool stop{false};
		std::thread storing{[&] {
			std::atomic_ref<std::uint32_t> word{*other};
			for (std::uint32_t n = 0;
			     !stop.load(std::memory_order_relaxed); ++n) {
				word.store(n, std::memory_order_relaxed);
				started.store(true, std::memory_order_relaxed);
			}
		}};
		const bool began = uml::testing::waitUntil([&] {
			return started.load(std::memory_order_relaxed);
		}, std::chrono::seconds{10}, std::chrono::seconds{0});
		const bool stored = began && attempt(c, machine, built);
		stop.store(true);
		storing.join();
		if (!began)
			failed(c, "the other thread did not start", 0);
		if (stored)
			return index;
	}
	std::fprintf(stderr, "%.*s, %s %s, %s: no store-exclusive succeeded "
		"in %u attempts\n", static_cast<int>(c.engine.size()),
		c.engine.data(), name(c.isa), name(c.width), name(c.between),
		attempts);
	return 0;
}

} // namespace

/* With an engine name (gadgets or jit), only its cases run. Every case
 * runs; the test fails when any case's store-exclusive never succeeded. */
int main(int argc, char **argv)
{
	const uml::testing::RuntimeTestRoute runtime_route;
	const std::string_view only_engine = argc > 1 ? argv[1] : "";
	unsigned long generation = 0xe7c0'0000UL;
	unsigned lost = 0;
	for (const auto kind : {uml::cpu::EngineKind::gadgets,
		     uml::cpu::EngineKind::jit}) {
		const std::string_view engine =
			kind == uml::cpu::EngineKind::jit ? "jit" : "gadgets";
		if (!only_engine.empty() && only_engine != engine)
			continue;
		if (!uml::cpu::cpu_engine_available(kind)) {
			std::printf("%.*s: unavailable on this host; skipped\n",
				static_cast<int>(engine.size()), engine.data());
			continue;
		}
		unsigned cases = 0;
		unsigned retried = 0;
		for (const auto isa : {Isa::a64, Isa::a32, Isa::t32}) {
			for (const auto width : {Width::single, Width::pair})
				for (const auto between : {Between::nothing,
					     Between::translated_branch,
					     Between::linked_branch,
					     Between::indirect_miss,
					     Between::indirect_trained,
					     Between::straight_line,
					     Between::tlb_miss,
					     Between::library,
					     Between::other_thread,
					     Between::own_store,
					     Between::own_pair_store,
					     Between::own_structure_store,
					     Between::own_structure_store_crossing,
					     Between::own_tagged_structure_store,
					     Between::tagged_base}) {
					/* The library case has no A64 form, nor the
					 * structure store or a tagged base an AArch32
					 * one. */
					const bool a64_only =
						between >= Between::own_structure_store;
					if ((between == Between::library &&
					     isa == Isa::a64) ||
					    (a64_only && isa != Isa::a64))
						continue;
					const Case c{engine, isa, width, between};
					const auto taken =
						run_case(c, kind, generation);
					if (taken == 0)
						++lost;
					else if (taken > 1) {
						std::printf("%.*s, %s %s, %s: "
							"%u attempts\n",
							static_cast<int>(
								engine.size()),
							engine.data(), name(isa),
							name(width),
							name(between), taken);
						++retried;
					}
					++cases;
				}
		}
		std::printf("%.*s: %u cases, %u needed more than one attempt\n",
			static_cast<int>(engine.size()), engine.data(), cases,
			retried);
	}
	if (lost != 0) {
		std::fprintf(stderr, "%u cases lost the exclusive monitor\n",
			lost);
		return 1;
	}
	return 0;
}
