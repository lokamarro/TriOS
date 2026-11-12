// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The AArch32 semantic library's own contract (cpu/arm64/aarch32/semantics.hpp):
 * what the corpus's model lane cannot pin because its oracle records the
 * state at an event and not the library's promises about it. Fault ordering,
 * the exclusive monitor, the IT state of the single-instruction entry, the
 * events, the library-exit counters and the coverage predicate.
 */
#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using ::uml::memory::MemoryAccess;
using ::uml::testing::require;
using namespace ::uml::cpu::arm64;
using namespace ::uml::cpu::arm64::aarch32;

constexpr std::uint32_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint32_t code_base = 0x1000'0000;
/* Two data pages side by side, then the page after them. */
constexpr std::uint32_t data_base = 0x2000'0000;

/* Guest memory of separate host pages, so a library that assumed two pages
 * adjacent in the host would fail. */
class Machine : public A32Memory {
public:
	std::byte *translate(std::uint64_t address, MemoryAccess access) noexcept override
	{
		const auto found = pages_.find(static_cast<std::uint32_t>(address / page));
		if (found == pages_.end() || address >> 32 != 0)
			return nullptr;
		Page &entry = found->second;
		if ((access == MemoryAccess::write && !entry.write) ||
			(access == MemoryAccess::execute && !entry.execute))
			return nullptr;
		++translations;
		return entry.bytes.get() + address % page;
	}

	void stored(std::byte *host, std::size_t size) noexcept override
	{
		stores.push_back({host, size});
	}

	void map(std::uint32_t address, bool write = true, bool execute = false)
	{
		Page entry;
		void *memory = std::aligned_alloc(page, page);
		require(memory != nullptr, "out of memory");
		std::memset(memory, 0, page);
		entry.bytes.reset(static_cast<std::byte *>(memory));
		entry.write = write;
		entry.execute = execute;
		pages_[address / page] = std::move(entry);
	}

	void unmap(std::uint32_t address) { pages_.erase(address / page); }

	template<typename T>
	void put(std::uint32_t address, T value)
	{
		std::memcpy(host(address), &value, sizeof value);
	}

	template<typename T>
	[[nodiscard]] T get(std::uint32_t address)
	{
		T value;
		std::memcpy(&value, host(address), sizeof value);
		return value;
	}

	std::vector<std::pair<std::byte *, std::size_t>> stores;
	std::size_t translations = 0;

private:
	struct Free final {
		void operator()(std::byte *pointer) const noexcept { std::free(pointer); }
	};
	struct Page final {
		std::unique_ptr<std::byte, Free> bytes;
		bool write{}, execute{};
	};

	std::byte *host(std::uint32_t address)
	{
		return pages_.at(address / page).bytes.get() + address % page;
	}

	std::map<std::uint32_t, Page> pages_;
};

template<typename M>
struct BasicFixture final {
	M machine;
	CpuState cpu{};
	Profile profile{FeatureSet{}};

	BasicFixture()
	{
		machine.map(code_base, true, true);
		machine.map(data_base);
		machine.map(data_base + page);
		cpu.a32 = 1;
		cpu.a32_cp15ben = 1;
		set_pc(code_base, false);
	}

	void set_pc(std::uint32_t address, bool thumb)
	{
		cpu.pc = thumb ? t32_key(address) : a32_key(address);
	}

	[[nodiscard]] std::uint32_t pc() const { return key_pc(cpu.pc); }
	[[nodiscard]] std::uint32_t r(unsigned n) const
	{
		return static_cast<std::uint32_t>(cpu.registers[n]);
	}
	[[nodiscard]] std::uint64_t d(unsigned n) const
	{
		std::uint64_t value;
		std::memcpy(&value, cpu.vectors[n / 2].bytes.data() + 8U * (n & 1U), 8);
		return value;
	}
	void set_d(unsigned n, std::uint64_t value)
	{
		std::memcpy(cpu.vectors[n / 2].bytes.data() + 8U * (n & 1U), &value, 8);
	}
	[[nodiscard]] std::uint32_t s(unsigned n) const
	{
		std::uint32_t value;
		std::memcpy(&value, cpu.vectors[n / 4].bytes.data() + 4U * (n & 3U), 4);
		return value;
	}

	void set_s(unsigned n, std::uint32_t value)
	{
		std::memcpy(cpu.vectors[n / 4].bytes.data() + 4U * (n & 3U), &value, 4);
	}

	/* Places A32 words at the code page and steps the first. */
	StepResult step_a32_word(std::uint32_t word)
	{
		machine.put(code_base, word);
		set_pc(code_base, false);
		return step_a32(cpu, machine, profile);
	}

	/* Places T32 halfwords at the code page and steps the first instruction. */
	StepResult step_t32(std::initializer_list<std::uint16_t> halves)
	{
		std::uint32_t at = code_base;
		for (const auto half : halves) {
			machine.put(at, half);
			at += 2;
		}
		set_pc(code_base, true);
		return step_a32(cpu, machine, profile);
	}
};

using Fixture = BasicFixture<Machine>;

constexpr std::uint32_t ldm_r0_r1_r3 = 0xe890000e; /* ldm r0, {r1, r2, r3} */
constexpr std::uint32_t stmia_wb_r0_r1_r3 = 0xe8a0000e; /* stm r0!, {r1, r2, r3} */
constexpr std::uint32_t ldr_r1_r0 = 0xe5901000;
constexpr std::uint32_t str_r1_r0 = 0xe5801000;
constexpr std::uint32_t ldrd_r2_r0 = 0xe1c020d0; /* ldrd r2, r3, [r0] */
constexpr std::uint32_t strd_r2_r0 = 0xe1c020f0;
constexpr std::uint32_t ldrex_r1_r0 = 0xe1901f9f;
constexpr std::uint32_t strex_r2_r3_r0 = 0xe1802f93; /* strex r2, r3, [r0] */
constexpr std::uint32_t ldrexd_r2_r0 = 0xe1b02f9f; /* ldrexd r2, r3, [r0] */
constexpr std::uint32_t strexd_r1_r4_r0 = 0xe1a01f94; /* strexd r1, r4, r5, [r0] */
constexpr std::uint32_t ldrexb_r2_r1 = 0xe1d12f9f;
constexpr std::uint32_t strexb_r3_r2_r1 = 0xe1c13f92;
constexpr std::uint32_t clrex = 0xf57ff01f;
constexpr std::uint32_t lda_r1_r0 = 0xe1901c9f;
constexpr std::uint32_t stl_r1_r0 = 0xe180fc91;
constexpr std::uint32_t ldaex_r1_r0 = 0xe1901e9f;
constexpr std::uint32_t stlex_r2_r1_r0 = 0xe1802e91;

void test_coverage()
{
	std::size_t covered = 0;
	for (std::size_t index = 0; index < kFormCount; ++index) {
		const auto form = static_cast<Form>(index);
		const FormInfo &info = kForms[index];
		const bool pseudo = form == Form::Undefined || form == Form::ReservedHint;
		/* Every implemented form, and nothing else. */
		const bool expected = pseudo || info.availability == Availability::implemented;
		require(a32_semantics_covers(form) == expected,
			"the coverage predicate follows the form's availability");
		covered += a32_semantics_covers(form) ? 1 : 0;
	}
	require(covered > 2100, "the library covers every implemented form");
}

void test_events()
{
	Fixture f;
	/* A form the library does not cover is fatal, and changes nothing. An
	 * HVC is outside the profile, so no decode returns it with a status
	 * that executes. */
	f.cpu.registers[0] = 0x1234;
	Instruction uncovered;
	uncovered.form = Form::HVC_A1;
	uncovered.status = Status::ok;
	f.machine.put(code_base, std::uint32_t{0xe1400070});
	f.set_pc(code_base, false);
	auto result = execute_a32(f.cpu, uncovered, f.machine);
	require(result.event == StepEvent::fatal && f.pc() == code_base && f.r(0) == 0x1234,
		"an uncovered form is fatal and changes nothing");

	/* A word the decoder rejects is undefined. */
	f.cpu.a32_itstate = 0;
	const auto no_crc = decode_a32(0xe1010042, Profile{FeatureSet{}});
	require(!no_crc.legal(), "a CRC32 word needs a feature the profile lacks");
	result = execute_a32(f.cpu, no_crc, f.machine);
	require(result.event == StepEvent::undefined && f.pc() == code_base,
		"a word outside the profile is undefined at the PC");

	/* UDF. */
	result = f.step_a32_word(0xe7f000f0);
	require(result.event == StepEvent::undefined && f.pc() == code_base, "UDF is undefined");

	/* SVC advances the PC; BKPT stays and reports its immediate. */
	result = f.step_a32_word(0xef000005);
	require(result.event == StepEvent::supervisor_call && result.syndrome == 5 &&
			f.pc() == code_base + 4,
		"SVC leaves the PC past the instruction");
	result = f.step_a32_word(0xe1212374);
	require(result.event == StepEvent::breakpoint && result.syndrome == 0x1234 && f.pc() == code_base,
		"BKPT stays at the instruction and reports its immediate");
	/* Thumb BKPT and SVC. */
	result = f.step_t32({0xbe42});
	require(result.event == StepEvent::breakpoint && result.syndrome == 0x42 && f.pc() == code_base,
		"T16 BKPT");
	result = f.step_t32({0xdf07});
	require(result.event == StepEvent::supervisor_call && result.syndrome == 7 && f.pc() == code_base + 2,
		"T16 SVC leaves the PC past the instruction");

	/* A failed condition is a retired NOP. */
	f.cpu.nzcv = 0;
	f.cpu.registers[0] = 5;
	result = f.step_a32_word(0x02800001); /* addeq r0, r0, #1 */
	require(result.event == StepEvent::retired && f.r(0) == 5 && f.pc() == code_base + 4,
		"a failed condition retires with no effect");
	f.cpu.nzcv = 0x4000'0000;
	result = f.step_a32_word(0x02800001);
	require(result.event == StepEvent::retired && f.r(0) == 6, "a passed condition executes");

	/* The A32 target with bit 1 set is a PC alignment event at the next step. */
	f.cpu.registers[1] = code_base + 0x102;
	result = f.step_a32_word(0xe12fff11); /* bx r1 */
	require(result.event == StepEvent::retired && !key_is_canonical(f.cpu.pc),
		"BX to an unaligned A32 target leaves a non canonical key");
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::pc_alignment && result.fault_address == code_base + 0x102,
		"the next fetch is the PC alignment event");

	/* A fetch from a page that cannot be executed. */
	f.set_pc(data_base, false);
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::execute &&
			result.fault_address == data_base && f.pc() == data_base,
		"an unfetchable PC faults for execute at the PC");
}

void test_it_state()
{
	Fixture f;
	/* it eq ; addeq r0, r0, #1 ; run as the architecture does: IT, then the
	 * block's instructions with ITAdvance after each. */
	f.cpu.registers[0] = 10;
	f.cpu.nzcv = 0x4000'0000; /* Z set */
	std::uint32_t at = code_base;
	for (const std::uint16_t half : {std::uint16_t{0xbf08}, std::uint16_t{0x1c40}}) {
		f.machine.put(at, half);
		at += 2;
	}
	f.set_pc(code_base, true);
	auto result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired && f.cpu.a32_itstate == 0x08, "IT starts the block");
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired && f.r(0) == 11 && f.cpu.a32_itstate == 0,
		"the conditional instruction executes and the block ends");
	/* The flag-setting 16-bit add inside an IT block does not set flags. */
	require(f.cpu.nzcv == 0x4000'0000, "an IT block's 16-bit add leaves the flags");

	/* The same entered in the middle: a state the engine resumes with. */
	f.cpu.nzcv = 0;
	f.cpu.registers[0] = 10;
	f.cpu.a32_itstate = 0x08;
	f.set_pc(code_base + 2, true);
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired && f.r(0) == 10 && f.cpu.a32_itstate == 0 &&
			f.pc() == code_base + 4,
		"a mid-IT entry honours the state and advances it");

	/* A fault inside a block reports the instruction's own ITSTATE. */
	f.cpu.registers[0] = data_base + 0x10;
	f.cpu.nzcv = 0x4000'0000;
	f.machine.put(code_base, std::uint16_t{0x6800}); /* ldr r0, [r0] */
	f.cpu.registers[0] = 0xffff'fffc;
	f.cpu.a32_itstate = 0x08;
	f.set_pc(code_base, true);
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::memory_fault && f.cpu.a32_itstate == 0x08 && f.pc() == code_base,
		"a fault leaves the instruction's own ITSTATE");
}

void test_load_multiple_writes_last()
{
	Fixture f;
	f.machine.unmap(data_base + page);
	for (unsigned n = 1; n < 4; ++n)
		f.cpu.registers[n] = 0xdead'0000 + n;
	/* r0 points at the last word of the first page: the block crosses into
	 * the unmapped page after one word. */
	f.cpu.registers[0] = data_base + page - 4;
	f.machine.put(data_base + page - 4, std::uint32_t{0x11});
	auto result = f.step_a32_word(ldm_r0_r1_r3);
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::read &&
			result.fault_address == data_base + page && !result.alignment,
		"an LDM faults at the first word of the page that does not translate");
	require(f.r(1) == 0xdead'0001 && f.r(2) == 0xdead'0002 && f.pc() == code_base,
		"a faulting load changes no register");
}

void test_store_multiple_partial()
{
	Fixture f;
	f.machine.unmap(data_base + page);
	for (unsigned n = 1; n < 4; ++n)
		f.cpu.registers[n] = 0xaaaa'0000 + n;
	f.cpu.registers[0] = data_base + page - 8;
	/* stm r0!, {r1, r2, r3}: two words fit, the third faults. */
	auto result = f.step_a32_word(stmia_wb_r0_r1_r3);
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::write &&
			result.fault_address == data_base + page,
		"an STM faults at the word that does not translate");
	require(f.machine.get<std::uint32_t>(data_base + page - 8) == 0xaaaa'0001 &&
			f.machine.get<std::uint32_t>(data_base + page - 4) == 0xaaaa'0002,
		"the words ahead of the fault are stored");
	require(f.r(0) == data_base + page - 8 && f.pc() == code_base,
		"the base register is unchanged");
}

void test_single_store_is_all_or_nothing()
{
	Fixture f;
	f.machine.unmap(data_base + page);
	/* A word store that straddles into the unmapped page writes no byte. */
	f.cpu.registers[0] = data_base + page - 2;
	f.cpu.registers[1] = 0x8877'6655;
	auto result = f.step_a32_word(str_r1_r0);
	require(result.event == StepEvent::memory_fault && result.fault_address == data_base + page &&
			f.machine.get<std::uint16_t>(data_base + page - 2) == 0,
		"a straddling store writes nothing");
	/* STRD at a word-aligned address that straddles the same way. */
	f.cpu.registers[0] = data_base + page - 4;
	f.cpu.registers[2] = 1;
	f.cpu.registers[3] = 2;
	result = f.step_a32_word(strd_r2_r0);
	require(result.event == StepEvent::memory_fault && result.fault_address == data_base + page &&
			f.machine.get<std::uint32_t>(data_base + page - 4) == 0,
		"a straddling doubleword store writes nothing");
	require(f.machine.stores.empty(), "no store was reported");

	/* The same load straddling two mapped pages reads across the host gap. */
	f.machine.map(data_base + page);
	f.machine.put(data_base + page - 2, std::uint16_t{0x2211});
	f.machine.put(data_base + page, std::uint16_t{0x4433});
	f.cpu.registers[0] = data_base + page - 2;
	result = f.step_a32_word(ldr_r1_r0);
	require(result.event == StepEvent::retired && f.r(1) == 0x4433'2211,
		"an unaligned load reads across a page boundary");
	result = f.step_a32_word(str_r1_r0);
	require(result.event == StepEvent::retired && f.machine.stores.size() == 2 &&
			f.machine.get<std::uint16_t>(data_base + page) == 0x4433,
		"an unaligned store reports one range per page");
}

void test_alignment_faults()
{
	Fixture f;
	f.cpu.registers[0] = data_base + 2;
	auto result = f.step_a32_word(ldrd_r2_r0);
	require(result.event == StepEvent::memory_fault && result.alignment &&
			result.fault_address == data_base + 2 && result.access == MemoryAccess::read,
		"LDRD needs word alignment");
	result = f.step_a32_word(ldm_r0_r1_r3);
	require(result.event == StepEvent::memory_fault && result.alignment, "LDM needs word alignment");
	f.cpu.registers[0] = data_base + 1;
	result = f.step_a32_word(ldrex_r1_r0);
	require(result.event == StepEvent::memory_fault && result.alignment && result.fault_address == data_base + 1,
		"LDREX needs natural alignment");
	result = f.step_a32_word(lda_r1_r0);
	require(result.event == StepEvent::memory_fault && result.alignment, "LDA needs natural alignment");
	result = f.step_a32_word(stl_r1_r0);
	require(result.event == StepEvent::memory_fault && result.alignment &&
			result.access == MemoryAccess::write,
		"STL needs natural alignment");
	f.cpu.registers[0] = data_base + 4;
	result = f.step_a32_word(ldrexd_r2_r0);
	require(result.event == StepEvent::memory_fault && result.alignment, "LDREXD needs doubleword alignment");
	/* A plain word load does not. */
	f.cpu.registers[0] = data_base + 1;
	result = f.step_a32_word(ldr_r1_r0);
	require(result.event == StepEvent::retired, "LDR allows unaligned addresses");
	/* The address wraps at 4 GiB. */
	f.cpu.registers[0] = 0xffff'fffe;
	result = f.step_a32_word(ldr_r1_r0);
	require(result.event == StepEvent::memory_fault && result.fault_address == 0xffff'fffe,
		"an access at the top of the address space faults at its first byte");
}

void test_exclusives()
{
	Fixture f;
	const std::uint32_t address = data_base + 0x40;
	f.machine.put(address, std::uint32_t{0x1111'1111});
	f.cpu.registers[0] = address;
	f.cpu.registers[3] = 0x2222'2222;

	auto result = f.step_a32_word(ldrex_r1_r0);
	require(result.event == StepEvent::retired && f.r(1) == 0x1111'1111 && f.cpu.exclusive_size == 4 &&
			f.cpu.exclusive_address == address && f.cpu.exclusive_value == 0x1111'1111 &&
			f.cpu.exclusive_key != 0,
		"LDREX records the monitor");
	result = f.step_a32_word(strex_r2_r3_r0);
	require(result.event == StepEvent::retired && f.r(2) == 0 &&
			f.machine.get<std::uint32_t>(address) == 0x2222'2222 && f.cpu.exclusive_size == 0 &&
			!f.machine.stores.empty(),
		"STREX stores after LDREX and clears the monitor");
	f.cpu.registers[3] = 0x3333'3333;
	f.machine.stores.clear();
	result = f.step_a32_word(strex_r2_r3_r0);
	require(result.event == StepEvent::retired && f.r(2) == 1 &&
			f.machine.get<std::uint32_t>(address) == 0x2222'2222 && f.machine.stores.empty(),
		"STREX without a monitor fails and stores nothing");

	/* The location changing between the pair fails the store. */
	(void)f.step_a32_word(ldrex_r1_r0);
	f.machine.put(address, std::uint32_t{0x4444'4444});
	result = f.step_a32_word(strex_r2_r3_r0);
	require(f.r(2) == 1 && f.machine.get<std::uint32_t>(address) == 0x4444'4444 && f.cpu.exclusive_size == 0,
		"STREX fails when the value changed and clears the monitor");

	/* CLREX. */
	(void)f.step_a32_word(ldrex_r1_r0);
	f.cpu.exclusive_value2 = 7;
	f.cpu.exclusive_version = 9;
	result = f.step_a32_word(clrex);
	require(result.event == StepEvent::retired && f.cpu.exclusive_address == ~std::uint64_t{0} &&
			f.cpu.exclusive_key == 0 && f.cpu.exclusive_value == 0 && f.cpu.exclusive_value2 == 0 &&
			f.cpu.exclusive_version == 0 && f.cpu.exclusive_size == 0,
		"CLREX clears every field of the monitor");
	(void)f.step_a32_word(strex_r2_r3_r0);
	require(f.r(2) == 1, "STREX after CLREX fails");

	/* A store-exclusive to another address fails. */
	(void)f.step_a32_word(ldrex_r1_r0);
	f.cpu.registers[0] = address + 4;
	(void)f.step_a32_word(strex_r2_r3_r0);
	require(f.r(2) == 1 && f.machine.get<std::uint32_t>(address + 4) == 0, "STREX needs the monitored address");

	/* STREX to an unmapped page faults even without a monitor. */
	f.cpu.registers[0] = 0x3000'0000;
	result = f.step_a32_word(strex_r2_r3_r0);
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::write &&
			result.fault_address == 0x3000'0000,
		"STREX translates its address first");

	/* Doubleword and byte forms. */
	f.cpu.registers[0] = address + 8;
	f.machine.put(address + 8, std::uint64_t{0x8877'6655'4433'2211});
	result = f.step_a32_word(ldrexd_r2_r0);
	require(f.r(2) == 0x4433'2211 && f.r(3) == 0x8877'6655 && f.cpu.exclusive_size == 8,
		"LDREXD loads the pair");
	f.cpu.registers[4] = 0xa1a2'a3a4;
	f.cpu.registers[5] = 0xb1b2'b3b4;
	result = f.step_a32_word(strexd_r1_r4_r0);
	require(f.r(1) == 0 && f.machine.get<std::uint64_t>(address + 8) == 0xb1b2'b3b4'a1a2'a3a4,
		"STREXD stores the pair");
	f.cpu.registers[1] = address + 0x11;
	f.machine.put(address + 0x11, std::uint8_t{0x5a});
	(void)f.step_a32_word(ldrexb_r2_r1);
	require(f.r(2) == 0x5a && f.cpu.exclusive_size == 1, "LDREXB");
	f.cpu.registers[2] = 0x1ff;
	(void)f.step_a32_word(strexb_r3_r2_r1);
	require(f.r(3) == 0 && f.machine.get<std::uint8_t>(address + 0x11) == 0xff, "STREXB stores a byte");

	/* LDAEX / STLEX. */
	f.cpu.registers[0] = address;
	f.machine.put(address, std::uint32_t{0x77});
	(void)f.step_a32_word(ldaex_r1_r0);
	f.cpu.registers[1] = 0x78;
	(void)f.step_a32_word(stlex_r2_r1_r0);
	require(f.r(2) == 0 && f.machine.get<std::uint32_t>(address) == 0x78, "STLEX after LDAEX");
	/* LDA and STL do not touch the monitor. */
	(void)f.step_a32_word(ldrex_r1_r0);
	(void)f.step_a32_word(lda_r1_r0);
	require(f.cpu.exclusive_size == 4, "LDA leaves the monitor");
	f.cpu.registers[1] = 0x99;
	(void)f.step_a32_word(stl_r1_r0);
	require(f.machine.get<std::uint32_t>(address) == 0x99, "STL stores");
}

void test_branches_and_stack()
{
	Fixture f;
	/* BL sets LR to the next instruction; BLX (immediate) switches to Thumb. */
	auto result = f.step_a32_word(0xeb000002);
	require(result.event == StepEvent::retired && f.pc() == code_base + 16 &&
			key_is_aarch32(f.cpu.pc) && !key_is_thumb(f.cpu.pc) && f.r(14) == code_base + 4,
		"BL branches and links");
	result = f.step_a32_word(0xfa000002);
	require(result.event == StepEvent::retired && f.pc() == code_base + 16 && key_is_thumb(f.cpu.pc) &&
			f.r(14) == code_base + 4,
		"BLX immediate changes to Thumb");
	/* BX with bit 0 set changes to Thumb and clears the bit. */
	f.cpu.registers[1] = code_base + 0x21;
	result = f.step_a32_word(0xe12fff11);
	require(result.event == StepEvent::retired && key_is_thumb(f.cpu.pc) && f.pc() == code_base + 0x20,
		"BX to an odd address enters Thumb");
	/* The Thumb BLX register form links with the Thumb bit. */
	f.cpu.registers[1] = code_base + 0x40;
	result = f.step_t32({0x4788});
	require(result.event == StepEvent::retired && !key_is_thumb(f.cpu.pc) && f.pc() == code_base + 0x40 &&
			f.r(14) == (code_base + 2 + 1),
		"T16 BLX register links with the Thumb bit");

	/* PUSH and POP with writeback; POP {pc} interworks. */
	f.cpu.registers[13] = data_base + 0x100;
	f.cpu.registers[4] = 0x44;
	f.cpu.registers[5] = 0x55;
	result = f.step_a32_word(0xe92d0030);
	require(result.event == StepEvent::retired && f.r(13) == data_base + 0xf8 &&
			f.machine.get<std::uint32_t>(data_base + 0xf8) == 0x44 &&
			f.machine.get<std::uint32_t>(data_base + 0xfc) == 0x55,
		"PUSH stores the lowest register at the lowest address");
	f.cpu.registers[4] = f.cpu.registers[5] = 0;
	result = f.step_a32_word(0xe8bd0030);
	require(result.event == StepEvent::retired && f.r(13) == data_base + 0x100 && f.r(4) == 0x44 &&
			f.r(5) == 0x55,
		"POP restores the registers and the stack pointer");
	f.machine.put(data_base + 0x100, std::uint32_t{code_base + 0x81});
	f.cpu.registers[0] = data_base + 0x100;
	result = f.step_a32_word(0xe8b08000); /* ldm r0!, {pc} */
	require(result.event == StepEvent::retired && key_is_thumb(f.cpu.pc) && f.pc() == code_base + 0x80 &&
			f.r(0) == data_base + 0x104,
		"LDM into the PC interworks and writes back");
}

void test_it_udf()
{
	Fixture f;
	/* A UDF in an IT block whose condition fails is a retired NOP; one
	 * that passes is undefined at its own ITSTATE. */
	f.machine.put(code_base, std::uint16_t{0xbf08}); /* it eq */
	f.machine.put(code_base + 2, std::uint16_t{0xde01}); /* udf #1 */
	f.cpu.nzcv = 0;
	f.set_pc(code_base, true);
	auto result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired, "IT");
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired && f.cpu.a32_itstate == 0 && f.pc() == code_base + 4,
		"a failed UDF retires and ends the block");
	f.cpu.nzcv = 0x4000'0000;
	f.set_pc(code_base, true);
	f.cpu.a32_itstate = 0;
	(void)step_a32(f.cpu, f.machine, f.profile);
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::undefined && f.cpu.a32_itstate == 0x08 && f.pc() == code_base + 2,
		"a UDF that passes is undefined at its own ITSTATE");
}

void test_vector_memory()
{
	Fixture f;
	f.profile = Profile{};
	const std::uint32_t area = data_base + 0x100;
	f.cpu.registers[0] = area;
	f.machine.put(area + 8, std::uint64_t{0x1122'3344'5566'7788});
	f.machine.put(area + 4, std::uint32_t{0xcafe'f00d});

	/* VLDR / VSTR, D and S. */
	auto result = f.step_a32_word(0xed901b02); /* vldr d1, [r0, #8] */
	require(result.event == StepEvent::retired && f.d(1) == 0x1122'3344'5566'7788, "VLDR D");
	result = f.step_a32_word(0xedd01a01); /* vldr s3, [r0, #4] */
	require(result.event == StepEvent::retired && f.s(3) == 0xcafe'f00d && f.d(1) >> 32 == 0xcafe'f00d,
		"VLDR S writes only its word of the D register");
	f.machine.put(area + 8, std::uint64_t{0});
	result = f.step_a32_word(0xed801b02); /* vstr d1, [r0, #8] */
	require(result.event == StepEvent::retired &&
			f.machine.get<std::uint64_t>(area + 8) == 0xcafe'f00d'5566'7788,
		"VSTR D");
	result = f.step_a32_word(0xedc01a01); /* vstr s3, [r0, #4] */
	require(result.event == StepEvent::retired && f.machine.get<std::uint32_t>(area + 4) == 0xcafe'f00d,
		"VSTR S");

	/* VLDR (literal) reads from the word-aligned PC. */
	f.machine.put(code_base + 16, std::uint64_t{0x0102'0304'0506'0708});
	result = f.step_a32_word(0xed9f1b02); /* vldr d1, [pc, #8] */
	require(result.event == StepEvent::retired && f.d(1) == 0x0102'0304'0506'0708, "VLDR literal");

	/* MemA: a misaligned address is an alignment fault, with nothing changed. */
	f.cpu.registers[0] = area + 2;
	result = f.step_a32_word(0xed901b02);
	require(result.event == StepEvent::memory_fault && result.alignment &&
			result.fault_address == area + 10 && f.d(1) == 0x0102'0304'0506'0708,
		"VLDR needs word alignment");
	result = f.step_a32_word(0xec902b06); /* vldm r0, {d2-d4} */
	require(result.event == StepEvent::memory_fault && result.alignment && result.fault_address == area + 2,
		"VLDM needs word alignment");

	/* VLDM that runs into an unmapped page changes no register. */
	f.machine.unmap(data_base + page);
	f.cpu.registers[0] = data_base + page - 16;
	f.cpu.vectors[1].bytes.fill(std::byte{0x5a});
	result = f.step_a32_word(0xec902b06);
	require(result.event == StepEvent::memory_fault && !result.alignment &&
			result.access == MemoryAccess::read && result.fault_address == data_base + page &&
			f.d(2) == 0x5a5a'5a5a'5a5a'5a5a,
		"a faulting VLDM leaves the registers");

	/* VSTM stores the words ahead of the fault and leaves the base. */
	f.cpu.registers[0] = data_base + page - 12;
	f.set_d(2, 0x2121'2121'2121'2121);
	f.set_d(3, 0x3131'3131'3131'3131);
	f.set_d(4, 0x4141'4141'4141'4141);
	result = f.step_a32_word(0xeca02b06); /* vstmia r0!, {d2-d4} */
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::write &&
			result.fault_address == data_base + page && f.r(0) == data_base + page - 12,
		"a faulting VSTM leaves the base");
	require(f.machine.get<std::uint32_t>(data_base + page - 12) == 0x2121'2121 &&
			f.machine.get<std::uint32_t>(data_base + page - 8) == 0x2121'2121 &&
			f.machine.get<std::uint32_t>(data_base + page - 4) == 0x3131'3131,
		"the words ahead of the fault are stored");
	f.machine.map(data_base + page);

	/* VLDM with writeback; FSTMX writes the doublewords of an odd count. */
	f.cpu.registers[0] = area;
	for (unsigned n = 0; n < 3; ++n)
		f.machine.put(area + 4 * n, std::uint32_t{0x100 + n});
	result = f.step_a32_word(0xecb02a03); /* vldmia r0!, {s4-s6} */
	require(result.event == StepEvent::retired && f.s(4) == 0x100 && f.s(5) == 0x101 &&
			f.s(6) == 0x102 && f.r(0) == area + 12,
		"VLDM single registers with writeback");
	f.cpu.registers[0] = area + 0x40;
	f.set_d(2, 0x7777'7777'7777'7777);
	f.set_d(3, 0x7777'7777'7777'7777);
	result = f.step_a32_word(0xec802b05); /* fstmiax r0, {d2, d3} */
	require(result.event == StepEvent::retired && f.r(0) == area + 0x40 &&
			f.machine.get<std::uint64_t>(area + 0x40) == 0x7777'7777'7777'7777 &&
			f.machine.get<std::uint64_t>(area + 0x48) == 0x7777'7777'7777'7777 &&
			f.machine.get<std::uint64_t>(area + 0x50) == 0,
		"FSTMX stores its doublewords");
}

void test_vector_structures()
{
	Fixture f;
	f.profile = Profile{};
	const std::uint32_t area = data_base + 0x200;
	for (unsigned n = 0; n < 32; ++n)
		f.machine.put(area + n, static_cast<std::uint8_t>(n));
	f.cpu.registers[0] = area;
	f.cpu.registers[1] = 24;

	/* VLD1 multiple: consecutive. */
	auto result = f.step_a32_word(0xf4200a8d); /* vld1.32 {d0, d1}, [r0]! */
	require(result.event == StepEvent::retired && f.d(0) == 0x0706'0504'0302'0100 &&
			f.d(1) == 0x0f0e'0d0c'0b0a'0908 && f.r(0) == area + 16,
		"VLD1 loads consecutive doublewords and writes back");
	f.cpu.registers[0] = area;
	result = f.step_a32_word(0xf4200741); /* vld1.16 {d0}, [r0], r1 */
	require(result.event == StepEvent::retired && f.r(0) == area + 24, "post-indexed by a register");

	/* VLD2, VLD3 and VLD4 de-interleave. */
	f.cpu.registers[0] = area;
	result = f.step_a32_word(0xf420030f); /* vld2.8 {d0-d3}, [r0] */
	/* Four registers are two pairs (D0, D2) and (D1, D3). */
	require(result.event == StepEvent::retired && f.d(0) == 0x0e0c'0a08'0604'0200 &&
			f.d(2) == 0x0f0d'0b09'0705'0301 && f.d(1) == 0x1e1c'1a18'1614'1210 &&
			f.d(3) == 0x1f1d'1b19'1715'1311,
		"VLD2 de-interleaves pairs");
	result = f.step_a32_word(0xf420040f); /* vld3.8 {d0, d1, d2}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x1512'0f0c'0906'0300 &&
			f.d(1) == 0x1613'100d'0a07'0401 && f.d(2) == 0x1714'110e'0b08'0502,
		"VLD3 de-interleaves triples");
	result = f.step_a32_word(0xf420000f); /* vld4.8 {d0-d3}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x1c18'1410'0c08'0400 &&
			f.d(3) == 0x1f1b'1713'0f0b'0703,
		"VLD4 de-interleaves quadruples");

	/* VST4 interleaves it back. */
	f.cpu.registers[0] = area + 0x80;
	result = f.step_a32_word(0xf400000f); /* vst4.8 {d0-d3}, [r0] */
	require(result.event == StepEvent::retired);
	for (unsigned n = 0; n < 32; ++n)
		require(f.machine.get<std::uint8_t>(area + 0x80 + n) == n, "VST4 interleaves");
	result = f.step_a32_word(0xf400084f); /* vst2.16 {d0, d1}, [r0] */
	require(result.event == StepEvent::retired &&
			f.machine.get<std::uint16_t>(area + 0x80 + 2) == 0x0501,
		"VST2");

	/* Single lane: only that element changes. */
	f.cpu.registers[0] = area;
	f.cpu.vectors[0].bytes.fill(std::byte{0xee});
	result = f.step_a32_word(0xf4a0048f); /* vld1.16 {d0[2]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0xeeee'0100'eeee'eeee,
		"VLD1 to one lane");
	f.cpu.registers[0] = area + 0x90;
	result = f.step_a32_word(0xf480088f); /* vst1.32 {d0[1]}, [r0] */
	require(result.event == StepEvent::retired &&
			f.machine.get<std::uint32_t>(area + 0x90) == 0xeeee'0100,
		"VST1 from one lane");
	f.cpu.registers[0] = area;
	f.cpu.vectors[0].bytes.fill(std::byte{0});
	f.cpu.vectors[1].bytes.fill(std::byte{0});
	result = f.step_a32_word(0xf4a00a8f); /* vld3.32 {d0[1], d1[1], d2[1]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x0302'0100'0000'0000 &&
			f.d(1) == 0x0706'0504'0000'0000 && f.d(2) == 0x0b0a'0908'0000'0000,
		"VLD3 to one lane of three registers");

	/* All lanes replicate. */
	result = f.step_a32_word(0xf4a00c0f); /* vld1.8 {d0[]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x0000'0000'0000'0000,
		"VLD1 to all lanes of byte zero");
	f.machine.put(area, std::uint16_t{0xa1b2});
	result = f.step_a32_word(0xf4a00c6f); /* vld1.16 {d0[], d1[]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0xa1b2'a1b2'a1b2'a1b2 &&
			f.d(1) == 0xa1b2'a1b2'a1b2'a1b2,
		"VLD1 to all lanes of two registers");
	f.machine.put(area, std::uint32_t{0x04030201});
	f.machine.put(area + 4, std::uint32_t{0x08070605});
	result = f.step_a32_word(0xf4a00d0f); /* vld2.8 {d0[], d1[]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x0101'0101'0101'0101 &&
			f.d(1) == 0x0202'0202'0202'0202,
		"VLD2 to all lanes");
	result = f.step_a32_word(0xf4a00f4f); /* vld4.16 {d0[], d1[], d2[], d3[]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x0201'0201'0201'0201 &&
			f.d(3) == 0x0807'0807'0807'0807,
		"VLD4 to all lanes");

	/* The alignment qualifier faults when the address lacks it. */
	f.cpu.registers[0] = area + 8;
	result = f.step_a32_word(0xf4200aaf); /* vld1.32 {d0, d1}, [r0:128] */
	require(result.event == StepEvent::memory_fault && result.alignment && result.fault_address == area + 8,
		"an alignment hint is enforced");
	f.cpu.registers[0] = area + 16;
	result = f.step_a32_word(0xf4200aaf);
	require(result.event == StepEvent::retired, "an aligned hint passes");

	/* A structure access that crosses into an unmapped page changes nothing:
	 * a load leaves the registers, a store writes no byte. */
	f.machine.unmap(data_base + page);
	f.cpu.registers[0] = data_base + page - 4;
	f.cpu.vectors[0].bytes.fill(std::byte{0x33});
	result = f.step_a32_word(0xf420070f); /* vld1.8 {d0}, [r0] */
	require(result.event == StepEvent::memory_fault && result.fault_address == data_base + page &&
			f.d(0) == 0x3333'3333'3333'3333,
		"a faulting VLD1 leaves the register");
	result = f.step_a32_word(0xf400070f); /* vst1.8 {d0}, [r0] */
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::write &&
			f.machine.get<std::uint32_t>(data_base + page - 4) == 0,
		"a faulting VST1 stores nothing");
}

/* A reservation model of the kind an engine with versioned reservations
 * keeps: it fails a store-exclusive when anything was stored since the
 * load-exclusive, even if the location holds the same value, which the
 * default compare-and-swap monitor passes. */
class VersionedMachine final : public Machine {
public:
	void stored(std::byte *host, std::size_t size) noexcept override
	{
		++version;
		Machine::stored(host, size);
	}

	std::uint64_t load_exclusive(CpuState &, std::uint32_t address, std::byte *host,
		unsigned size, bool acquire) noexcept override
	{
		++loads;
		last_acquire = acquire;
		reserved_address = address;
		reserved_version = version;
		reserved = true;
		std::uint64_t value = 0;
		std::memcpy(&value, host, size);
		return value;
	}

	bool store_exclusive(CpuState &, std::uint32_t address, std::byte *host, unsigned size,
		std::uint64_t value, bool release) noexcept override
	{
		++store_attempts;
		last_release = release;
		const bool held = reserved && reserved_address == address && reserved_version == version;
		reserved = false;
		if (held)
			std::memcpy(host, &value, size);
		return held;
	}

	void clear_exclusive(CpuState &) noexcept override
	{
		++clears;
		reserved = false;
	}

	std::uint64_t version = 0, reserved_version = 0;
	std::uint32_t reserved_address = 0;
	bool reserved = false, last_acquire = false, last_release = false;
	unsigned loads = 0, store_attempts = 0, clears = 0;
};

void test_custom_monitor()
{
	BasicFixture<VersionedMachine> f;
	const std::uint32_t address = data_base + 0x40;
	f.machine.put(address, std::uint32_t{0x11});
	f.cpu.registers[0] = address;
	f.cpu.registers[3] = 0x22;

	/* The hooks run in place of CpuState's record. */
	auto result = f.step_a32_word(ldrex_r1_r0);
	require(result.event == StepEvent::retired && f.r(1) == 0x11 && f.machine.loads == 1 &&
			!f.machine.last_acquire && f.cpu.exclusive_size == 0,
		"LDREX goes through the monitor hook and leaves CpuState's record");
	result = f.step_a32_word(strex_r2_r3_r0);
	require(result.event == StepEvent::retired && f.r(2) == 0 && f.machine.store_attempts == 1 &&
			!f.machine.last_release && f.machine.get<std::uint32_t>(address) == 0x22 &&
			f.machine.stores.size() == 1,
		"STREX passes when the hook says so, and the library reports the store");

	/* A store since the load fails the store-exclusive, though the value is
	 * the same (the compare-and-swap monitor would pass it). */
	(void)f.step_a32_word(ldrex_r1_r0);
	f.cpu.registers[5] = 0x22;
	f.cpu.registers[6] = address;
	(void)f.step_a32_word(0xe5865000); /* str r5, [r6] */
	f.machine.stores.clear();
	result = f.step_a32_word(strex_r2_r3_r0);
	require(result.event == StepEvent::retired && f.r(2) == 1 && f.machine.stores.empty() &&
			f.machine.store_attempts == 2,
		"STREX fails when the hook says so and nothing is stored");

	/* The ordered forms pass their ordering on; CLREX reaches the hook. */
	(void)f.step_a32_word(ldaex_r1_r0);
	require(f.machine.last_acquire, "LDAEX asks for acquire");
	(void)f.step_a32_word(stlex_r2_r1_r0);
	require(f.machine.last_release, "STLEX asks for release");
	(void)f.step_a32_word(clrex);
	require(f.machine.clears == 1, "CLREX is the clear hook");

	/* LDA and STL never use the monitor. */
	const unsigned loads = f.machine.loads;
	const unsigned attempts = f.machine.store_attempts;
	(void)f.step_a32_word(lda_r1_r0);
	(void)f.step_a32_word(stl_r1_r0);
	require(f.machine.loads == loads && f.machine.store_attempts == attempts,
		"LDA and STL do not touch the monitor");

	/* The library still checks alignment and translation first. */
	f.cpu.registers[0] = address + 1;
	result = f.step_a32_word(strex_r2_r3_r0);
	require(result.event == StepEvent::memory_fault && result.alignment &&
			f.machine.store_attempts == attempts,
		"an unaligned STREX faults before the hook");
}

void test_system()
{
	Fixture f;
	/* MSR APSR_nzcvq, r1 then MRS r0, apsr. */
	f.cpu.registers[1] = 0xf800'0000;
	auto result = f.step_a32_word(0xe128f001);
	require(result.event == StepEvent::retired && f.cpu.nzcv == 0xf000'0000 && f.cpu.a32_q == 1,
		"MSR writes N, Z, C, V and Q");
	f.cpu.a32_ge_mask = 0x00ff'00ff;
	result = f.step_a32_word(0xe10f0000);
	require(f.r(0) == 0xf805'0010, "MRS reads the flags with the User mode field");

	/* The CP15 barrier is a barrier when enabled and undefined otherwise. */
	result = f.step_a32_word(0xee070fba);
	require(result.event == StepEvent::retired, "the CP15 barrier executes when enabled");
	f.cpu.a32_cp15ben = 0;
	result = f.step_a32_word(0xee070fba);
	require(result.event == StepEvent::undefined && f.pc() == code_base,
		"the CP15 barrier is undefined when disabled");
}

void test_counters()
{
	Fixture f;
	reset_a32_library_exits();
	count_a32_library_exits(false);
	f.cpu.registers[0] = data_base;
	(void)f.step_a32_word(ldr_r1_r0);
	require(a32_library_exit_total() == 0, "counting is off by default");
	count_a32_library_exits(true);
	(void)f.step_a32_word(ldr_r1_r0);
	(void)f.step_a32_word(ldr_r1_r0);
	(void)f.step_a32_word(str_r1_r0);
	count_a32_library_exits(false);
	const auto ldr = decode_a32(ldr_r1_r0, Profile{}).form;
	const auto str = decode_a32(str_r1_r0, Profile{}).form;
	require(a32_library_exits(ldr) == 2 && a32_library_exits(str) == 1 && a32_library_exit_total() == 3,
		"every call counts against its form");
	reset_a32_library_exits();
	require(a32_library_exit_total() == 0 && a32_library_exits(ldr) == 0, "reset clears the counts");
}

} // namespace

/* The FPSCR split, the rounding and flush controls that VFP honours and
 * Advanced SIMD does not, the cumulative flags, and a few integer forms. */
void test_floating_point()
{
	Fixture f;
	f.profile = Profile{};
	constexpr std::uint32_t qc = 1U << 27;

	/* VMSR keeps the control, status and flag bits; Len, Stride and the
	 * trap enables read as zero. */
	f.cpu.registers[0] = 0xffff'ffff;
	require(f.step_a32_word(0xeee10a10).event == StepEvent::retired); /* vmsr fpscr, r0 */
	require(f.step_a32_word(0xeef11a10).event == StepEvent::retired && f.r(1) == 0xffc8'009f,
		"FPSCR reads back only the bits that are implemented");
	require(f.step_a32_word(0xeef1fa10).event == StepEvent::retired && (f.cpu.nzcv >> 28) == 0xf,
		"VMRS APSR_nzcv copies the FPSCR flags");

	/* Advanced SIMD adds under the Standard FPSCR value, VFP under the
	 * guest's: round to zero changes only the VFP result. */
	f.cpu.fpcr = 0x00c0'0000;
	f.cpu.fpsr = 0;
	f.set_d(1, 0x3f80'0000'3f80'0000);
	f.set_d(2, 0x3380'0001'3380'0001);
	require(f.step_a32_word(0xf2010d02).event == StepEvent::retired && /* vadd.f32 d0, d1, d2 */
			f.d(0) == 0x3f80'0001'3f80'0001 && (f.cpu.fpsr & 0x10U) != 0U,
		"Advanced SIMD floating point rounds to nearest whatever the FPSCR says");
	f.set_d(0, 0x3f80'0000'0000'0000);
	f.set_d(1, 0x0000'0000'3380'0001);
	require(f.step_a32_word(0xee300a81).event == StepEvent::retired && /* vadd.f32 s0, s1, s2 */
			f.s(0) == 0x3f80'0000 && f.s(1) == 0x3f80'0000,
		"VFP honours the rounding mode");

	/* Default NaN: VFP only when DN is set, Advanced SIMD always. */
	f.cpu.fpcr = 0;
	f.set_d(0, 0x7fc1'2345'0000'0000);
	f.set_d(1, 0x0000'0000'3f80'0000);
	require(f.step_a32_word(0xee300a81).event == StepEvent::retired && f.s(0) == 0x7fc1'2345,
		"VFP propagates the NaN payload with DN clear");
	f.cpu.fpcr = 1U << 25;
	f.set_d(0, 0x7fc1'2345'0000'0000);
	require(f.step_a32_word(0xee300a81).event == StepEvent::retired && f.s(0) == 0x7fc0'0000,
		"VFP returns the default NaN with DN set");
	f.cpu.fpcr = 0;
	f.set_d(1, 0x7fc1'2345'7fc1'2345);
	f.set_d(2, 0x3f80'0000'3f80'0000);
	require(f.step_a32_word(0xf2010d02).event == StepEvent::retired &&
			f.d(0) == 0x7fc0'0000'7fc0'0000,
		"Advanced SIMD returns the default NaN");

	/* Saturation sets QC, which VMRS reads in FPSCR[27]. */
	f.cpu.fpsr = 0;
	f.set_d(1, 0x7f7f'7f7f'7f7f'7f7f);
	f.set_d(2, 0x0101'0101'0101'0101);
	require(f.step_a32_word(0xf2010012).event == StepEvent::retired && /* vqadd.s8 d0, d1, d2 */
			f.d(0) == 0x7f7f'7f7f'7f7f'7f7f && (f.cpu.fpsr & qc) != 0U,
		"VQADD saturates and sets QC");
	require(f.step_a32_word(0xeef11a10).event == StepEvent::retired && (f.r(1) & qc) != 0U,
		"VMRS reads QC");

	/* VCMP puts its result in the FPSCR flags; only VCMPE signals a quiet NaN. */
	f.cpu.fpsr = 0;
	f.set_d(0, 0x4000'0000'3f80'0000); /* s0 = 1.0, s1 = 2.0 */
	require(f.step_a32_word(0xeeb40a60).event == StepEvent::retired && /* vcmp.f32 s0, s1 */
			f.cpu.a32_fpscr_nzcv == 0x8000'0000,
		"VCMP less than");
	f.set_d(0, 0x7fc0'0000'3f80'0000);
	require(f.step_a32_word(0xeeb40a60).event == StepEvent::retired &&
			f.cpu.a32_fpscr_nzcv == 0x3000'0000 && (f.cpu.fpsr & 1U) == 0U,
		"VCMP with a quiet NaN is unordered and quiet");
	require(f.step_a32_word(0xeeb40ae0).event == StepEvent::retired && (f.cpu.fpsr & 1U) != 0U,
		"VCMPE signals a quiet NaN");

	/* VCVT truncates; VCVTR rounds by the FPSCR. */
	f.cpu.fpsr = 0;
	f.set_d(0, 0x3fc0'0000'0000'0000); /* s1 = 1.5 */
	require(f.step_a32_word(0xeebd0ae0).event == StepEvent::retired && f.s(0) == 1 &&
			(f.cpu.fpsr & 0x10U) != 0U,
		"VCVT rounds toward zero and is inexact");
	f.set_d(0, 0x3fc0'0000'0000'0000);
	require(f.step_a32_word(0xeebd0a60).event == StepEvent::retired && f.s(0) == 2,
		"VCVTR rounds to nearest even by default");
	f.cpu.fpcr = 2U << 22;
	f.set_d(0, 0x3fc0'0000'0000'0000);
	require(f.step_a32_word(0xeebd0a60).event == StepEvent::retired && f.s(0) == 1,
		"VCVTR rounds toward minus infinity under RMode 2");

	/* VMLA rounds the product, VFMA does not. */
	f.cpu.fpcr = 0;
	f.set_d(0, 0x3f80'0800'bf80'1000); /* s0 = -(1 + 2^-11), s1 = 1 + 2^-12 */
	f.set_d(1, 0x0000'0000'3f80'0800);
	require(f.step_a32_word(0xee000a81).event == StepEvent::retired && f.s(0) == 0,
		"VMLA rounds the product before the addition");
	f.set_d(0, 0x3f80'0800'bf80'1000);
	require(f.step_a32_word(0xeea00a81).event == StepEvent::retired && f.s(0) == 0x3380'0000,
		"VFMA keeps the exact product");

	/* VRECPS treats infinity times zero as a zero product, without a flag. */
	f.cpu.fpsr = 0;
	f.set_d(1, 0x7f80'0000'7f80'0000);
	f.set_d(2, 0);
	require(f.step_a32_word(0xf2010f12).event == StepEvent::retired &&
			f.d(0) == 0x4000'0000'4000'0000 && (f.cpu.fpsr & 1U) == 0U,
		"VRECPS of infinity and zero is two");
	/* The single-precision denormal is always a zero here: FPUnpack flushes it
	 * under the Standard FPSCR value, whatever the guest's FZ. */
	f.cpu.fpsr = 0;
	f.set_d(1, 0x7f80'0000'ff80'0000);
	f.set_d(2, 0x0000'0001'8000'0001);
	require(f.step_a32_word(0xf2010f12).event == StepEvent::retired &&
			f.d(0) == 0x4000'0000'4000'0000 && (f.cpu.fpsr & 0x80U) != 0U,
		"VRECPS of infinity and a single-precision denormal is two and sets IDC");

	/* Double precision. */
	f.cpu.fpsr = 0;
	f.set_d(1, 0xbff0'0000'0000'0000);
	require(f.step_a32_word(0xeeb00bc1).event == StepEvent::retired && f.d(0) == 0x3ff0'0000'0000'0000,
		"VABS.F64");
	f.set_d(1, 0x4010'0000'0000'0000);
	require(f.step_a32_word(0xeeb10bc1).event == StepEvent::retired && f.d(0) == 0x4000'0000'0000'0000,
		"VSQRT.F64");
	f.set_d(1, 0x3ff0'0000'0000'0000);
	f.set_d(2, 0x4008'0000'0000'0000);
	require(f.step_a32_word(0xee810b02).event == StepEvent::retired &&
			f.d(0) == 0x3fd5'5555'5555'5555 && (f.cpu.fpsr & 0x10U) != 0U,
		"VDIV.F64 is inexact");

	/* Integer forms: a by-register shift with a negative count, a long
	 * multiply, a rounding saturating multiply and a pairwise add. */
	f.set_d(1, 0x8080'8080'8080'8080);
	f.set_d(2, 0xfefe'fefe'fefe'fefe);
	require(f.step_a32_word(0xf2020401).event == StepEvent::retired && /* vshl.s8 d0, d1, d2 */
			f.d(0) == 0xe0e0'e0e0'e0e0'e0e0,
		"VSHL by a negative count shifts right arithmetically");
	f.set_d(2, 0xffff'0003'0002'0001);
	f.set_d(3, 0x0028'001e'0014'000a);
	require(f.step_a32_word(0xf2920c03).event == StepEvent::retired && /* vmull.s16 q0, d2, d3 */
			f.d(0) == 0x0000'0028'0000'000a && f.d(1) == 0xffff'ffd8'0000'005a,
		"VMULL widens the products");
	f.cpu.fpsr = 0;
	f.set_d(1, 0x0000'0000'0000'8000);
	f.set_d(2, 0x0000'0000'0000'8000);
	require(f.step_a32_word(0xf3110b02).event == StepEvent::retired && /* vqrdmulh.s16 d0, d1, d2 */
			(f.d(0) & 0xffff) == 0x7fff && (f.cpu.fpsr & qc) != 0U,
		"VQRDMULH saturates the one overflowing product");
	f.set_d(1, 0x0004'0003'0002'0001);
	f.set_d(2, 0x0028'001e'0014'000a);
	require(f.step_a32_word(0xf2110b12).event == StepEvent::retired && /* vpadd.i16 d0, d1, d2 */
			f.d(0) == 0x0046'001e'0007'0003,
		"VPADD adds adjacent pairs");
}

/* The Armv8 extensions: half-precision arithmetic, the Armv8 floating-point
 * additions, VJCVT, the integer, crypto and CRC32 groups, and BFloat16. */
void test_armv8_extensions()
{
	Fixture f;
	f.profile = Profile{};
	constexpr std::uint32_t z_flag = 0x4000'0000;

	/* Half precision: the result zero-extends into the S register. */
	f.set_s(1, 0x3c00);
	f.set_s(2, 0x3c00);
	f.set_s(0, 0xffff'ffff);
	require(f.step_a32_word(0xee300981).event == StepEvent::retired && /* vadd.f16 s0, s1, s2 */
			f.s(0) == 0x4000,
		"VADD.F16 adds and clears the top half");
	f.set_s(0, 0xaaaa'bbbb);
	f.set_s(1, 0x3333'4444);
	require(f.step_a32_word(0xfeb00ae0).event == StepEvent::retired && f.s(0) == 0x4444'bbbb,
		"VINS puts Hm in the top half of Sd");
	require(f.step_a32_word(0xfeb00a60).event == StepEvent::retired && f.s(0) == 0x3333,
		"VMOVX moves the top half of Sm");
	f.cpu.registers[1] = 0xabcd'1234;
	require(f.step_a32_word(0xee001910).event == StepEvent::retired && f.s(0) == 0x1234,
		"VMOV.F16 from a core register clears the top half");

	/* VRECPS.F16 and VRSQRTS.F16: infinity times a half-precision denormal is a
	 * zero product only where FPSCR.FZ16 flushes the denormal, and that flush
	 * sets no IDC; otherwise the product is infinite (vrecps.f16 d0, d4, d0). */
	constexpr std::uint64_t infinities = 0xfc00'fc00'0400'0000;
	constexpr std::uint64_t denormals = 0x0080'0001'4040'0000;
	f.cpu.fpsr = 0;
	f.set_d(4, infinities);
	f.set_d(0, denormals);
	require(f.step_a32_word(0xf2140f10).event == StepEvent::retired &&
			f.d(0) == 0x7c00'7c00'4000'4000 && (f.cpu.fpsr & 0x80U) == 0U,
		"VRECPS.F16 of infinity and a denormal is infinite with FZ16 clear");
	f.cpu.fpsr = 0;
	f.set_d(4, infinities);
	f.set_d(0, denormals);
	require(f.step_a32_word(0xf2340f10).event == StepEvent::retired &&
			f.d(0) == 0x7c00'7c00'3e00'3e00 && (f.cpu.fpsr & 0x80U) == 0U,
		"VRSQRTS.F16 of infinity and a denormal is infinite with FZ16 clear");
	f.cpu.fpcr = 1U << 19;
	f.cpu.fpsr = 0;
	f.set_d(4, infinities);
	f.set_d(0, denormals);
	require(f.step_a32_word(0xf2140f10).event == StepEvent::retired &&
			f.d(0) == 0x4000'4000'4000'4000 && (f.cpu.fpsr & 0x80U) == 0U,
		"VRECPS.F16 flushes the denormal to a zero product with FZ16 set, without IDC");
	f.cpu.fpsr = 0;
	f.set_d(4, infinities);
	f.set_d(0, denormals);
	require(f.step_a32_word(0xf2340f10).event == StepEvent::retired &&
			f.d(0) == 0x3e00'3e00'3e00'3e00 && (f.cpu.fpsr & 0x80U) == 0U,
		"VRSQRTS.F16 flushes the denormal to a zero product with FZ16 set, without IDC");
	f.cpu.fpcr = 0;

	/* VSEL tests the flags and changes none. */
	f.set_s(1, 0x3f80'0000);
	f.set_s(2, 0x4000'0000);
	f.cpu.nzcv = z_flag;
	require(f.step_a32_word(0xfe000a81).event == StepEvent::retired && f.s(0) == 0x3f80'0000 &&
			f.cpu.nzcv == z_flag,
		"VSELEQ selects Sn when Z is set");
	f.cpu.nzcv = 0;
	require(f.step_a32_word(0xfe000a81).event == StepEvent::retired && f.s(0) == 0x4000'0000,
		"VSELEQ selects Sm when Z is clear");

	/* VRINT and VCVT with a rounding: 2.5 and 1.5. */
	f.cpu.fpsr = 0;
	f.set_s(1, 0x4020'0000);
	require(f.step_a32_word(0xfeb80a60).event == StepEvent::retired && f.s(0) == 0x4040'0000 &&
			f.cpu.fpsr == 0,
		"VRINTA rounds ties away and is exact");
	require(f.step_a32_word(0xfeb90a60).event == StepEvent::retired && f.s(0) == 0x4000'0000,
		"VRINTN rounds ties to even");
	f.set_s(1, 0x3fc0'0000);
	require(f.step_a32_word(0xeeb70a60).event == StepEvent::retired && f.s(0) == 0x4000'0000 &&
			(f.cpu.fpsr & 0x10U) != 0U,
		"VRINTX rounds by the FPSCR and raises Inexact");
	f.set_s(1, 0x4020'0000);
	require(f.step_a32_word(0xfebc0ae0).event == StepEvent::retired && f.s(0) == 3,
		"VCVTA rounds ties away");
	require(f.step_a32_word(0xfebd0ae0).event == StepEvent::retired && f.s(0) == 2,
		"VCVTN rounds ties to even");

	/* VMAXNM returns the number for a quiet NaN. */
	f.set_s(1, 0x7fc0'0000);
	f.set_s(2, 0x3f80'0000);
	require(f.step_a32_word(0xfe800a81).event == StepEvent::retired && f.s(0) == 0x3f80'0000,
		"VMAXNM prefers the number to a quiet NaN");

	/* VJCVT wraps modulo 2^32 and sets Z only for an exact conversion. */
	f.cpu.fpsr = 0;
	f.set_d(1, 0x41f0'0000'0038'0000); /* 2^32 + 3.5 */
	require(f.step_a32_word(0xeeb90bc1).event == StepEvent::retired && f.s(0) == 3 &&
			f.cpu.a32_fpscr_nzcv == 0 && f.cpu.fpsr == 1U,
		"VJCVT wraps a value out of range and signals Invalid Operation");
	f.cpu.fpsr = 0;
	f.set_d(1, 0x3ff8'0000'0000'0000); /* 1.5 */
	require(f.step_a32_word(0xeeb90bc1).event == StepEvent::retired && f.s(0) == 1 &&
			f.cpu.a32_fpscr_nzcv == 0 && f.cpu.fpsr == 0x10U,
		"VJCVT of an inexact value clears Z and signals Inexact");
	f.set_d(1, 0x4008'0000'0000'0000);
	require(f.step_a32_word(0xeeb90bc1).event == StepEvent::retired && f.s(0) == 3 &&
			f.cpu.a32_fpscr_nzcv == z_flag,
		"VJCVT of an exact value sets Z");
	f.set_d(1, 0x8000'0000'0000'0000);
	require(f.step_a32_word(0xeeb90bc1).event == StepEvent::retired && f.s(0) == 0 &&
			f.cpu.a32_fpscr_nzcv == 0,
		"VJCVT of a negative zero clears Z");

	/* CRC32 and CRC32C against independent values. */
	f.cpu.registers[1] = 0;
	f.cpu.registers[2] = 0x61;
	require(f.step_a32_word(0xe1010042).event == StepEvent::retired && f.r(0) == 0x3ab5'51ce,
		"CRC32B");
	f.cpu.registers[1] = 0xffff'ffff;
	f.cpu.registers[2] = 0x0123'4567;
	require(f.step_a32_word(0xe1410042).event == StepEvent::retired && f.r(0) == 0xc7b5'e785,
		"CRC32W");
	f.cpu.registers[1] = 1;
	f.cpu.registers[2] = 0xffff'abcd;
	require(f.step_a32_word(0xe1210042).event == StepEvent::retired && f.r(0) == 0x264b'e222,
		"CRC32H uses only the low half");
	f.cpu.registers[1] = 0xffff'ffff;
	for (const char digit : {'1', '2', '3', '4', '5', '6', '7', '8', '9'}) {
		f.cpu.registers[2] = static_cast<std::uint32_t>(digit);
		require(f.step_a32_word(0xe1010242).event == StepEvent::retired, "CRC32CB");
		f.cpu.registers[1] = f.r(0);
	}
	require((f.r(0) ^ 0xffff'ffff) == 0xe306'9283, "CRC32C of 123456789");

	/* AESE then AESMC on the FIPS-197 appendix B state. */
	f.set_d(0, 0x8d30'5a88'a8f6'4332);
	f.set_d(1, 0x3407'37e0'a298'3131);
	f.set_d(2, 0xa6d2'ae28'1615'7e2b);
	f.set_d(3, 0x3c4f'cf09'8815'f7ab);
	require(f.step_a32_word(0xf3b00302).event == StepEvent::retired && /* aese.8 q0, q1 */
			f.d(0) == 0xae52'b4e0'305d'bfd4 && f.d(1) == 0xe598'271e'f111'41b8,
		"AESE");
	f.set_d(2, f.d(0));
	f.set_d(3, f.d(1));
	require(f.step_a32_word(0xf3b00382).event == StepEvent::retired && /* aesmc.8 q0, q1 */
			f.d(0) == 0x9a19'cbe0'e581'6604 && f.d(1) == 0x4c26'0628'7ad3'f848,
		"AESMC");
	f.set_d(2, 0x1234'5678'8000'0001);
	f.set_d(3, 0xffff'ffff'ffff'ffff);
	require(f.step_a32_word(0xf3b902c2).event == StepEvent::retired && /* sha1h.32 q0, q1 */
			f.d(0) == 0x6000'0000 && f.d(1) == 0,
		"SHA1H rotates the low word and clears the rest");

	/* Complex numbers: (1 + 2i) and (3 + 4i). */
	f.set_d(1, 0x4000'0000'3f80'0000);
	f.set_d(2, 0x4080'0000'4040'0000);
	require(f.step_a32_word(0xfc910802).event == StepEvent::retired && /* vcadd.f32 d0, d1, d2, #90 */
			f.d(0) == 0x40a0'0000'c040'0000,
		"VCADD rotates by 90 degrees");
	f.set_d(0, 0x3e80'0000'3f00'0000); /* 0.5 + 0.25i */
	require(f.step_a32_word(0xfc310802).event == StepEvent::retired && /* vcmla.f32 d0, d1, d2, #0 */
			f.d(0) == 0x4088'0000'4060'0000,
		"VCMLA accumulates the real-part products");

	/* VFMAL widens the half-precision products. */
	f.set_d(4, 0);
	f.set_d(0, 0x4000'3c00'0000'0000); /* s1 = {1.0, 2.0} */
	f.set_d(1, 0x0000'0000'4200'4000); /* s2 = {2.0, 3.0} */
	require(f.step_a32_word(0xfc204891).event == StepEvent::retired && /* vfmal.f16 d4, s1, s2 */
			f.d(4) == 0x40c0'0000'4000'0000,
		"VFMAL");

	/* VQRDMLAH: one saturating lane, one exact lane. */
	f.cpu.fpsr = 0;
	f.set_d(0, 0x0100'0000);
	f.set_d(1, 0x4000'8000);
	f.set_d(2, 0x4000'8000);
	require(f.step_a32_word(0xf3110b12).event == StepEvent::retired && /* vqrdmlah.s16 d0, d1, d2 */
			f.d(0) == 0x2100'7fff && (f.cpu.fpsr & (1U << 27)) != 0U,
		"VQRDMLAH saturates and sets QC");

	/* VSDOT sums four byte products per word. */
	f.set_d(0, 0);
	f.set_d(1, 0x0807'0605'0403'0201);
	f.set_d(2, 0x0202'0202'0101'0101);
	require(f.step_a32_word(0xfc210d02).event == StepEvent::retired && /* vsdot.s8 d0, d1, d2 */
			f.d(0) == 0x0000'0034'0000'000a,
		"VSDOT");

	/* BFloat16: ties round to even in VCVT, and VDOT rounds to odd. */
	f.cpu.fpsr = 0;
	f.set_d(2, 0x3f81'8000'3f80'8000);
	f.set_d(3, 0x0000'0001'7f7f'ffff);
	require(f.step_a32_word(0xf3b60642).event == StepEvent::retired && /* vcvt.bf16.f32 d0, q1 */
			f.d(0) == 0x0000'7f80'3f82'3f80 && f.cpu.fpsr == 0x94,
		"VCVT.BF16 rounds to nearest even, overflows to infinity and flushes denormals");
	f.set_d(0, 0x3080'0000);
	f.set_d(1, 0x3f80);
	f.set_d(2, 0x3f80'3f80'3f80'3f80);
	require(f.step_a32_word(0xfc010d02).event == StepEvent::retired && /* vdot.bf16 d0, d1, d2 */
			f.d(0) == 0x3f80'0001,
		"VDOT.BF16 rounds the sum to odd");
	f.set_s(0, 0xaaaa'0000);
	f.set_s(1, 0x3f81'8000);
	require(f.step_a32_word(0xeeb30960).event == StepEvent::retired && f.s(0) == 0xaaaa'3f82,
		"VCVTB.BF16 replaces the bottom half");

	/* SB retires. */
	require(f.step_a32_word(0xf57ff070).event == StepEvent::retired, "SB retires");
}

int main()
{
	test_coverage();
	test_events();
	test_it_state();
	test_load_multiple_writes_last();
	test_store_multiple_partial();
	test_single_store_is_all_or_nothing();
	test_alignment_faults();
	test_exclusives();
	test_custom_monitor();
	test_branches_and_stack();
	test_it_udf();
	test_vector_memory();
	test_vector_structures();
	test_floating_point();
	test_armv8_extensions();
	test_system();
	test_counters();
	return 0;
}
