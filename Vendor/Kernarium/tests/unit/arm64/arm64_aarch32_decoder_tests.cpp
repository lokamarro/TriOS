// SPDX-License-Identifier: GPL-2.0-or-later
/* The AArch32 decoder (darwin/cpu/arm64/aarch32): the generated encoding
 * table, the IT rules, the profile's gates and the operands the Decode
 * pseudocode computes. The stored sample words and the exhaustive 16-bit
 * Thumb space are checked against the answers the generator computed
 * independently (tests/tooling/arm64/a32_expected.py) and against what LLVM
 * MC decodes. The data directory is the first argument. */
#include "aarch32_decoder/operands_digest.hpp"
#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/decode_support.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using ::uml::testing::require;
using namespace ::uml::cpu::arm64::aarch32;

using Row = std::vector<std::string>;

std::vector<Row> read_tsv(const std::string &path)
{
	std::ifstream input(path);
	require(input.is_open(), "the decoder test data is missing");
	std::vector<Row> rows;
	std::string line;
	std::getline(input, line);
	while (std::getline(input, line)) {
		Row row;
		std::stringstream stream(line);
		std::string cell;
		while (std::getline(stream, cell, '\t'))
			row.push_back(cell);
		rows.push_back(std::move(row));
	}
	return rows;
}

Form form_named(const std::string &name)
{
	static const std::map<std::string, Form> forms = [] {
		std::map<std::string, Form> result;
		for (std::size_t index = 0; index < kFormCount; ++index)
			result.emplace(kForms[index].name, static_cast<Form>(index));
		return result;
	}();
	const auto found = forms.find(name);
	require(found != forms.end(), "the test data names a form the table lacks");
	return found->second;
}

const char *status_name(Status status)
{
	switch (status) {
	case Status::ok:
		return "ok";
	case Status::nop:
		return "nop";
	case Status::undefined:
		return "undefined";
	}
	return "?";
}

const char *reason_name(Reason reason)
{
	switch (reason) {
	case Reason::none:
		return "none";
	case Reason::unallocated:
		return "unallocated";
	case Reason::unpredictable:
		return "unpredictable";
	case Reason::profile:
		return "profile";
	case Reason::feature:
		return "feature";
	}
	return "?";
}

const char *sysreg_name(SysReg sysreg)
{
	static const char *const names[] = {
		"none", "tpidruro", "tpidrurw_read", "tpidrurw_write", "cntfrq",
		"cntvct", "cp15_dsb", "cp15_dmb", "cp15_isb",
	};
	return names[static_cast<std::size_t>(sysreg)];
}

Isa isa_named(const std::string &name)
{
	return name == "A32" ? Isa::a32 : name == "T16" ? Isa::t16 : Isa::t32;
}

Instruction decode(Isa isa, std::uint32_t word, const Profile &profile, ItState it = {})
{
	return isa == Isa::a32 ? decode_a32(word, profile) : decode_thumb(word, it, profile);
}

const Profile complete;

[[noreturn]] void fail_word(const char *what, Isa isa, std::uint32_t word, const Instruction &result)
{
	std::fprintf(stderr, "%s: isa %d word %08x decoded %s %s %s\n", what, static_cast<int>(isa),
		word, result.info().name, status_name(result.status), reason_name(result.reason));
	std::abort();
}

/* The table ----------------------------------------------------------------- */

void test_table()
{
	require(kForms[static_cast<std::size_t>(Form::Undefined)].name == std::string("Undefined"));
	require(kForms[static_cast<std::size_t>(Form::ReservedHint)].name == std::string("ReservedHint"));
	for (std::size_t index = 0; index < kFormCount; ++index) {
		const FormInfo &info = kForms[index];
		require(info.name != nullptr && info.name[0] != '\0');
		require((info.program == kNoProgram) == (info.availability != Availability::implemented
			|| index < 2));
		if (info.feature != kNoFeature)
			require(info.level == Level::complete);
	}
	for (std::size_t index = 0; index < kRowCount; ++index) {
		require(kRows[index].form < kFormCount);
		require(kRows[index].constraint_first + kRows[index].constraint_count <= kConstraintCount);
	}
	for (const std::uint16_t root : kTreeRoots)
		require(root < kNodeCount);
	/* IT's operands sit where decode.cpp reads them. */
	const Instruction it = decode_thumb(0xBF080000, ItState{}, complete);
	require(it.form == Form::IT_T1 && it.status == Status::ok);
}

void test_it_state()
{
	/* ITT NE: firstcond 0001, mask 1100. */
	ItState state = ItState::from_it(1, 0b1100);
	require(state.active() && !state.last() && state.cond() == 1 && state.remaining() == 2);
	state = state.advance();
	require(state.active() && state.last() && state.cond() == 1 && state.remaining() == 1);
	state = state.advance();
	require(!state.active() && state.bits() == 0 && state.remaining() == 0);
	/* ITETT EQ: then, else, then, then. */
	state = ItState::from_it(0, 0b1001);
	require(state.cond() == 0 && state.remaining() == 4);
	state = state.advance();
	require(state.cond() == 1 && state.remaining() == 3);
	state = state.advance();
	require(state.cond() == 0 && state.remaining() == 2);
	state = state.advance();
	require(state.cond() == 0 && state.last());
	require(state.advance().bits() == 0);
	/* IT AL with an else-less block of one. */
	state = ItState::from_it(14, 0b1000);
	require(state.cond() == 14 && state.last() && state.remaining() == 1);
}

void test_support_helpers()
{
	using namespace ::uml::cpu::arm64::aarch32::detail;
	require(sint(0xFF, 8) == -1 && sint(0x7F, 8) == 127 && sint(0x80000000, 32) == -2147483648LL);
	require(lowest_set_bit(0, 8) == 8 && lowest_set_bit(0x10, 8) == 4);
	require(decode_imm_shift(0, 5).amount == 5 && decode_imm_shift(1, 0).amount == 32);
	require(decode_imm_shift(2, 0).type == SRType::ASR && decode_imm_shift(3, 0).type == SRType::RRX);
	require(decode_imm_shift(3, 4).type == SRType::ROR && decode_imm_shift(3, 4).amount == 4);
	/* ARM modified immediates: 0xFF ror 8, with the carry-out of a rotation. */
	require(a32_expand_imm(0x4FF) == 0xFF000000 && a32_expand_imm_c(0x4FF).carry == 1);
	require(a32_expand_imm_c(0x0FF).carry_in && a32_expand_imm(0x0FF) == 0xFF);
	require(t32_expand_imm(0x1AB) == 0x00AB00AB && t32_expand_imm(0x2AB) == 0xAB00AB00);
	require(t32_expand_imm(0x3AB) == 0xABABABAB && t32_expand_imm_c(0x3AB).carry_in);
	require(t32_expand_imm(0x4FF) == 0x7F800000 && !t32_expand_imm_c(0x4FF).carry_in);
	require(t32_expand_imm(0x8FF) == 0x007F8000 && t32_expand_imm(0xC7F) == 0x0000FF00);
	/* VFPExpandImm: 0x70 is 1.0 and 0xF0 is -1.0 in single and double. */
	require(vfp_expand_imm(0x70, 32) == 0x3F800000 && vfp_expand_imm(0xF0, 32) == 0xBF800000);
	require(vfp_expand_imm(0x70, 64) == 0x3FF0000000000000ULL);
	require(vfp_expand_imm(0x70, 16) == 0x3C00);
	/* AdvSIMDExpandImm: byte replication and the per-byte mask. */
	require(advsimd_expand_imm(0, 0, 0xAB) == 0x000000AB000000ABULL);
	require(advsimd_expand_imm(0, 0xE, 0xAB) == 0xABABABABABABABABULL);
	require(advsimd_expand_imm(1, 0xE, 0xA5) == 0xFF00FF0000FF00FFULL);
	require(advsimd_expand_imm(0, 0xF, 0x70) == 0x3F8000003F800000ULL);
	require(fp_decode_rm(0) == FPRounding::TIEAWAY && fp_decode_rm(3) == FPRounding::NEGINF);
}

/* Hand-checked decodes -------------------------------------------------------- */

struct Expectation {
	Isa isa;
	std::uint32_t word;
	const char *form;
	Status status;
	Reason reason;
};

void expect(const Expectation &want, const Profile &profile = complete, ItState it = {})
{
	const Instruction result = decode(want.isa, want.word, profile, it);
	if (std::string(result.info().name) != want.form || result.status != want.status
		|| result.reason != want.reason)
		fail_word(want.form, want.isa, want.word, result);
}

void test_forms()
{
	constexpr Status ok = Status::ok;
	constexpr Status nop = Status::nop;
	constexpr Status bad = Status::undefined;
	const Expectation cases[] = {
		{Isa::a32, 0xe0910002, "ADDS_r_A1", ok, Reason::none},
		{Isa::a32, 0xe12fff1e, "BX_A1", ok, Reason::none},
		/* BXJ behaves as BX; CPS is a NOP at EL0. */
		{Isa::a32, 0xe12fff20, "BXJ_A1", ok, Reason::none},
		{Isa::a32, 0xf10c0080, "CPSID_A1_AS", nop, Reason::none},
		{Isa::t16, 0xb6720000, "CPSID_T1_AS", nop, Reason::none},
		/* UDF and the hints are instructions; reserved hints are NOPs. */
		{Isa::a32, 0xe7f000f0, "UDF_A1", ok, Reason::none},
		{Isa::a32, 0xe320f000, "NOP_A1", ok, Reason::none},
		{Isa::a32, 0xe320f0ff, "DBG_A1", ok, Reason::none},
		{Isa::a32, 0xe320f0a0, "ReservedHint", nop, Reason::none},
		/* SWP is not in Armv8 AArch32 at all; SETEND and the privileged
		 * forms are UNDEFINED at EL0. */
		{Isa::a32, 0xe1000090, "Undefined", bad, Reason::unallocated},
		{Isa::a32, 0xf1010200, "SETEND_A1", bad, Reason::profile},
		{Isa::t16, 0xb6580000, "SETEND_T1", bad, Reason::profile},
		{Isa::a32, 0xe14f0000, "MRS_A1_AS", bad, Reason::profile},
		{Isa::a32, 0xe10f0000, "MRS_A1_AS", ok, Reason::none},
		{Isa::a32, 0xe128f000, "MSR_r_A1_AS", ok, Reason::none},
		{Isa::a32, 0xe16ff000, "MSR_r_A1_AS", bad, Reason::profile},
		{Isa::a32, 0xe8d00006, "LDM_u_A1_AS", bad, Reason::profile},
		{Isa::a32, 0xf96d0513, "SRSDB_A1_AS", bad, Reason::profile},
		{Isa::a32, 0xf8bd0a00, "RFEIA_A1_AS", bad, Reason::profile},
		{Isa::a32, 0xe160006e, "ERET_A1", bad, Reason::profile},
		{Isa::a32, 0xe1400070, "HVC_A1", bad, Reason::profile},
		/* An exception return through a data-processing instruction. */
		{Isa::a32, 0xe25ef004, "SUBS_i_A1", bad, Reason::unpredictable},
		{Isa::a32, 0xe24ef004, "SUB_i_A1", ok, Reason::none},
		/* VMRS and VMSR reach FPSCR only. */
		{Isa::a32, 0xeef10a10, "VMRS_A1_AS", ok, Reason::none},
		{Isa::a32, 0xeef1fa10, "VMRS_A1_AS", ok, Reason::none},
		{Isa::a32, 0xeef00a10, "VMRS_A1_AS", bad, Reason::profile},
		{Isa::a32, 0xeee10a10, "VMSR_A1_AS", ok, Reason::none},
		{Isa::a32, 0xeee80a10, "VMSR_A1_AS", bad, Reason::profile},
		/* Coprocessor accesses outside the profile's registers. */
		{Isa::a32, 0xee110e10, "MRC_A1", bad, Reason::profile},
		{Isa::a32, 0xee0d0f70, "MCR_A1", bad, Reason::profile},
		/* The Armv8 and optional extensions. */
		{Isa::a32, 0xe1910c9f, "LDA_A1", ok, Reason::none},
		{Isa::a32, 0xe1010042, "CRC32B_A1", ok, Reason::none},
		{Isa::a32, 0xf3b00302, "AESE_A1", ok, Reason::none},
		{Isa::a32, 0xfe800a81, "VMAXNM_A2_S", ok, Reason::none},
		{Isa::a32, 0xeeb30b41, "VCVTB_A1_HD", ok, Reason::none},
		{Isa::a32, 0xeeb30a60, "VCVTB_A1_HS", ok, Reason::none},
		{Isa::t32, 0xf3bf8f5b, "DMB_T1", ok, Reason::none},
		{Isa::t32, 0xe8dff000, "TBB_T1", ok, Reason::none},
		{Isa::t32, 0xfb91f0f2, "SDIV_T1", ok, Reason::none},
		{Isa::t32, 0xf85dfb04, "LDR_i_T4_post", ok, Reason::none},
		{Isa::t16, 0xbd100000, "POP_T1", ok, Reason::none},
		{Isa::t32, 0xee1d0f70, "MRC_T1", ok, Reason::none},
		{Isa::t32, 0xf7ffffff, "BL_i_T1", ok, Reason::none},
	};
	for (const Expectation &want : cases)
		expect(want);
}

void test_features()
{
	/* The profile has the integer instructions, the Armv8 acquire/release
	 * group, VFP and NEON, and the other Armv8 additions whatever the
	 * optional features are. */
	const Expectation integer{Isa::a32, 0xe0910002, "ADDS_r_A1", Status::ok, Reason::none};
	const Expectation neon{Isa::a32, 0xf2220844, "VADD_i_A1_Q", Status::ok, Reason::none};
	const Expectation lda{Isa::a32, 0xe1910c9f, "LDA_A1", Status::ok, Reason::none};
	const Expectation vsel{Isa::a32, 0xfe800a81, "VMAXNM_A2_S", Status::ok, Reason::none};
	for (const Expectation &want : {integer, neon, lda, vsel}) {
		expect(want);
		expect(want, Profile{FeatureSet{}});
	}
	/* A feature the profile lacks makes its instructions UNDEFINED, and no
	 * other. */
	Profile no_crc{FeatureSet::all().without(Feature::crc32)};
	expect({Isa::a32, 0xe1010042, "CRC32B_A1", Status::undefined, Reason::feature}, no_crc);
	expect({Isa::t16, 0x18880000, "ADD_r_T1", Status::ok, Reason::none}, no_crc);
	Profile no_aes{FeatureSet::all().without(Feature::aes)};
	expect({Isa::a32, 0xf3b00302, "AESE_A1", Status::undefined, Reason::feature}, no_aes);
	/* VMULL.P64 checks FEAT_PMULL in its Decode pseudocode. */
	Profile no_pmull{FeatureSet::all().without(Feature::pmull)};
	expect({Isa::a32, 0xf2a10e02, "VMULL_i_A1", Status::ok, Reason::none});
	expect({Isa::a32, 0xf2a10e02, "VMULL_i_A1", Status::undefined, Reason::feature}, no_pmull);
	/* FEAT_FP16 gates the half-precision forms of VABS. */
	Profile no_fp16{FeatureSet::all().without(Feature::fp16)};
	const Instruction half = decode_a32(0xeeb009c1, complete);
	require(half.status == Status::ok);
	const Instruction half_no = decode_a32(0xeeb009c1, no_fp16);
	require(half_no.status == Status::undefined && half_no.reason == Reason::feature);
}

void test_coprocessor_registers()
{
	struct Case {
		Isa isa;
		std::uint32_t word;
		SysReg want;
	};
	const Case cases[] = {
		{Isa::a32, 0xee1d0f70, SysReg::tpidruro},
		{Isa::a32, 0xee1d0f50, SysReg::tpidrurw_read},
		{Isa::a32, 0xee0d0f50, SysReg::tpidrurw_write},
		{Isa::a32, 0xee1e0f10, SysReg::cntfrq},
		{Isa::a32, 0xee070fba, SysReg::cp15_dmb},
		{Isa::a32, 0xee070f9a, SysReg::cp15_dsb},
		{Isa::a32, 0xee070f95, SysReg::cp15_isb},
		{Isa::a32, 0xec510f1e, SysReg::cntvct},
		{Isa::t32, 0xee1d0f70, SysReg::tpidruro},
		{Isa::t32, 0xee0d0f50, SysReg::tpidrurw_write},
		{Isa::t32, 0xec510f1e, SysReg::cntvct},
	};
	for (const Case &c : cases) {
		const Instruction result = decode(c.isa, c.word, complete);
		if (result.status != Status::ok || result.sysreg != c.want)
			fail_word("coprocessor register", c.isa, c.word, result);
	}
}

/* Operands the Decode pseudocode computes, checked against the assembly. */
void test_operands()
{
	Instruction i = decode_a32(0xe0910002, complete); /* adds r0, r1, r2 */
	require(i.operands.d == 0 && i.operands.n == 1 && i.operands.m == 2 && i.operands.setflags);
	require(i.operands.shift_t == SRType::LSL && i.operands.shift_n == 0);
	i = decode_a32(0x00810002, complete); /* addeq r0, r1, r2 */
	require(i.cond == 0 && !i.operands.setflags);
	/* ADD r0, r1, r2, lsl #3 */
	i = decode_a32(0xe0810182, complete);
	require(i.operands.shift_t == SRType::LSL && i.operands.shift_n == 3 && i.operands.m == 2);
	/* AND takes the raw imm12 (the XML expands it at execution). */
	i = decode_a32(0xe20104ff, complete);
	require(i.operands.a32 && i.operands.imm == 0x4ff && !i.operands.setflags);
	/* mov r0, #0xff000000 (ARM modified immediate, rotated; carry-out set) */
	i = decode_a32(0xe3a004ff, complete);
	require(static_cast<std::uint32_t>(i.operands.imm32) == 0xFF000000 && !i.operands.carry_flags
		&& i.operands.carry == 1);
	/* movs r0, #0xff: the carry-out is the carry flag. */
	i = decode_a32(0xe3b000ff, complete);
	require(static_cast<std::uint32_t>(i.operands.imm32) == 0xFF && i.operands.carry_flags
		&& i.operands.setflags);
	/* ldr r0, [r1, #-8]! : pre-indexed, subtracting, writeback */
	i = decode_a32(0xe5310008, complete);
	require(i.operands.t == 0 && i.operands.n == 1 && !i.operands.add && i.operands.index
		&& i.operands.wback && static_cast<std::uint32_t>(i.operands.imm32) == 8);
	/* ldm r0, {r1, r2} */
	i = decode_a32(0xe8900006, complete);
	require(i.operands.n == 0 && i.operands.registers == 0x0006 && !i.operands.wback);
	/* ldrex r0, [r1] */
	i = decode_a32(0xe1910f9f, complete);
	require(i.operands.t == 0 && i.operands.n == 1);
	/* mrs r0, apsr */
	i = decode_a32(0xe10f0000, complete);
	require(i.operands.d == 0 && !i.operands.read_spsr);
	/* movw r0, #0x1234 */
	i = decode_a32(0xe3010234, complete);
	require(i.status == Status::ok && i.operands.d == 0 && static_cast<std::uint32_t>(i.operands.imm32) == 0x1234);
	/* A Thumb flag-setting 16-bit instruction sets flags only outside IT. */
	i = decode_thumb(0x18880000, ItState{}, complete); /* adds r0, r1, r2 */
	require(i.status == Status::ok && i.operands.setflags && i.operands.d == 0
		&& i.operands.n == 1 && i.operands.m == 2 && i.length == 2);
	i = decode_thumb(0x18880000, ItState::from_it(0, 0b1000), complete); /* addeq */
	require(i.status == Status::ok && !i.operands.setflags && i.in_it && i.last_in_it && i.cond == 0);
	/* vadd.i32 q0, q1, q2 */
	i = decode_a32(0xf2220844, complete);
	require(i.operands.d == 0 && i.operands.n == 2 && i.operands.m == 4 && i.operands.regs == 2
		&& i.operands.esize == 32 && i.operands.elements == 2);
}

void test_thumb_it()
{
	/* it eq; addeq r0, r1, r2 */
	Instruction it = decode_thumb(0xBF080000, ItState{}, complete);
	require(it.status == Status::ok && it.length == 2 && it.it_length == 1 && !it.in_it);
	require(it.next_it == ItState::from_it(0, 8));
	Instruction add = decode_thumb(0x18880000, it.next_it, complete);
	require(add.cond == 0 && add.in_it && add.last_in_it && !add.operands.setflags);
	require(add.next_it.bits() == 0);
	/* itt ne; movne; movne */
	it = decode_thumb(0xBF1C0000, ItState{}, complete);
	require(it.it_length == 2 && it.next_it == ItState::from_it(1, 0b1100));
	Instruction first = decode_thumb(0x20010000, it.next_it, complete);
	require(first.cond == 1 && first.in_it && !first.last_in_it && first.next_it.last());
	Instruction second = decode_thumb(0x21020000, first.next_it, complete);
	require(second.cond == 1 && second.last_in_it && second.next_it.bits() == 0);
	/* IT inside an IT block, and IT with a bad condition, are UNPREDICTABLE. */
	const Instruction nested = decode_thumb(0xBF080000, ItState::from_it(0, 8), complete);
	require(nested.status == Status::undefined && nested.reason == Reason::unpredictable);
	const Instruction bad_cond = decode_thumb(0xBFF80000, ItState{}, complete);
	require(bad_cond.status == Status::undefined && bad_cond.reason == Reason::unpredictable);
	/* A branch inside a block must be its last instruction. */
	const Instruction branch_mid = decode_thumb(0x47700000, ItState::from_it(0, 0b1100), complete);
	require(branch_mid.status == Status::undefined);
	const Instruction branch_last = decode_thumb(0x47700000, ItState::from_it(0, 0b1000), complete);
	require(branch_last.status == Status::ok);
	/* B<c> is not allowed inside a block. */
	require(decode_thumb(0xd0fe0000, ItState::from_it(0, 8), complete).status == Status::undefined);
	require(decode_thumb(0xd0fe0000, ItState{}, complete).cond == 0);
	/* BKPT executes whatever the block's condition. */
	const Instruction bkpt = decode_thumb(0xbe010000, ItState::from_it(1, 8), complete);
	require(bkpt.status == Status::ok && bkpt.cond == 14);
	/* Thumb lengths. */
	require(thumb_length(0xE7FE) == 2 && thumb_length(0xE800) == 4 && thumb_length(0xF000) == 4);
}

/* The stored samples ------------------------------------------------------------ */

void test_samples(const std::string &directory)
{
	std::size_t count = 0;
	for (const Row &row : read_tsv(directory + "/samples.tsv")) {
		require(row.size() == 13, "a sample row has thirteen columns");
		const Isa isa = isa_named(row[0]);
		const std::uint32_t word = static_cast<std::uint32_t>(std::stoul(row[1], nullptr, 16));
		const Form form = form_named(row[3]);
		const bool violation = row[5] == "1";
		const bool llvm_accepts = row[6] != "-" && row[6] != "?";
		const Instruction result = decode(isa, word, complete);
		++count;
		if (classify(isa, word) != form || result.form != form)
			fail_word("form", isa, word, result);
		if (row[8] != status_name(result.status) || row[9] != reason_name(result.reason))
			fail_word("status", isa, word, result);
		if (row[10] != sysreg_name(result.sysreg) || std::stoi(row[11]) != result.cond)
			fail_word("sysreg or condition", isa, word, result);
		if (result.status == Status::ok) {
			char digest[17];
			std::snprintf(digest, sizeof digest, "%016llx",
				static_cast<unsigned long long>(operands_digest(result.operands)));
			if (row[12] != digest)
				fail_word("operands", isa, word, result);
			/* Whatever the decoder executes, LLVM decodes (BKPT executes under
			 * any condition; LLVM insists on AL). */
			if (!violation && !llvm_accepts && form != Form::BKPT_A1)
				fail_word("LLVM rejects", isa, word, result);
		}
		if (violation && result.status != Status::undefined)
			fail_word("should-be bits", isa, word, result);
		if (violation)
			require(result.reason == (result.info().availability == Availability::undefined_at_el0
					? Reason::profile
					: Reason::unpredictable),
				"a should-be violation is UNPREDICTABLE unless the profile rejects the form");
		require(result.length == (isa == Isa::t16 ? 2U : 4U));
	}
	require(count > 5000, "the sample table is missing rows");
}

void test_t16_space(const std::string &directory)
{
	std::uint32_t next = 0;
	for (const Row &row : read_tsv(directory + "/t16_reference.tsv")) {
		require(row.size() == 7, "a T16 run has seven columns");
		const std::uint32_t first = static_cast<std::uint32_t>(std::stoul(row[0], nullptr, 16));
		const std::uint32_t last = static_cast<std::uint32_t>(std::stoul(row[1], nullptr, 16));
		require(first == next && last >= first, "the T16 runs are not contiguous");
		next = last + 1;
		const Form form = form_named(row[2]);
		for (std::uint32_t halfword = first; halfword <= last; ++halfword) {
			const std::uint32_t word = halfword << 16;
			const Instruction result = decode_thumb(word, ItState{}, complete);
			if (result.form != form || row[5] != status_name(result.status)
				|| row[6] != reason_name(result.reason))
				fail_word("T16", Isa::t16, word, result);
			if (result.status == Status::ok && row[3] == "-" && form != Form::BKPT_A1)
				fail_word("T16 LLVM rejects", Isa::t16, word, result);
			require(result.length == 2);
		}
	}
	require(next == 0xE800, "the T16 runs do not cover the halfword space");
	/* A halfword at or above 0xE800 starts a 32-bit instruction. */
	require(decode_thumb(0xE8000000, ItState{}, complete).length == 4);
}

} // namespace

int main(int argc, char **argv)
{
	require(argc == 2, "usage: arm64_aarch32_decoder_tests <data directory>");
	test_table();
	test_it_state();
	test_support_helpers();
	test_forms();
	test_features();
	test_coprocessor_registers();
	test_operands();
	test_thumb_it();
	test_samples(argv[1]);
	test_t16_space(argv[1]);
	std::puts("aarch32 decoder: ok");
	return 0;
}
