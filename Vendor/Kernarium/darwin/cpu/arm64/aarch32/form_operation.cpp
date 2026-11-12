// SPDX-License-Identifier: GPL-2.0-or-later
//
// The grouping of the decoder's forms into operations (form_operation.hpp):
// by mnemonic, with the shape of a data-processing form and the SP base read
// from its encoding name.

#include "cpu/arm64/aarch32/form_operation.hpp"

#include <array>
#include <string_view>
#include <unordered_map>

namespace uml::cpu::arm64::aarch32 {

namespace {

struct Named final {
	std::string_view mnemonic;
	Op op;
};

constexpr Named named_ops[] = {
	{"ADC", Op::adc}, {"ADCS", Op::adc}, {"ADD", Op::add}, {"ADDS", Op::add},
	{"ADDW", Op::addw}, {"SUBW", Op::subw}, {"ADR", Op::adr},
	{"AND", Op::and_}, {"ANDS", Op::and_}, {"BIC", Op::bic},
	{"BICS", Op::bic}, {"CMN", Op::cmn}, {"CMP", Op::cmp}, {"EOR", Op::eor},
	{"EORS", Op::eor}, {"MOV", Op::mov}, {"MOVS", Op::mov},
	{"MOVW", Op::movw}, {"MOVT", Op::movt}, {"MVN", Op::mvn},
	{"MVNS", Op::mvn}, {"ORN", Op::orn}, {"ORNS", Op::orn}, {"ORR", Op::orr},
	{"ORRS", Op::orr}, {"RSB", Op::rsb}, {"RSBS", Op::rsb}, {"RSC", Op::rsc},
	{"RSCS", Op::rsc}, {"SBC", Op::sbc}, {"SBCS", Op::sbc}, {"SUB", Op::sub},
	{"SUBS", Op::sub}, {"TEQ", Op::teq}, {"TST", Op::tst},
	{"B", Op::b}, {"BL", Op::bl}, {"BLX", Op::blx}, {"BX", Op::bx},
	{"BXJ", Op::bx}, {"CBZ", Op::cbz}, {"CBNZ", Op::cbnz}, {"TBB", Op::tb},
	{"TBH", Op::tb},
	{"BFC", Op::bfc}, {"BFI", Op::bfi}, {"SBFX", Op::sbfx},
	{"UBFX", Op::ubfx}, {"CLZ", Op::clz}, {"RBIT", Op::rbit},
	{"REV", Op::rev}, {"REV16", Op::rev16}, {"REVSH", Op::revsh},
	{"SXTB", Op::sxtb}, {"SXTH", Op::sxth}, {"UXTB", Op::uxtb},
	{"UXTH", Op::uxth}, {"SXTAB", Op::sxtab}, {"SXTAH", Op::sxtah},
	{"UXTAB", Op::uxtab}, {"UXTAH", Op::uxtah}, {"SXTB16", Op::sxtb16},
	{"UXTB16", Op::uxtb16}, {"SXTAB16", Op::sxtab16},
	{"UXTAB16", Op::uxtab16}, {"PKHBT", Op::pkh}, {"PKHTB", Op::pkh},
	{"SEL", Op::sel},
	{"MUL", Op::mul}, {"MULS", Op::mul}, {"MLA", Op::mla}, {"MLAS", Op::mla},
	{"MLS", Op::mls}, {"UMULL", Op::umull}, {"UMULLS", Op::umull},
	{"UMLAL", Op::umlal}, {"UMLALS", Op::umlal}, {"SMULL", Op::smull},
	{"SMULLS", Op::smull}, {"SMLAL", Op::smlal}, {"SMLALS", Op::smlal},
	{"UMAAL", Op::umaal}, {"SDIV", Op::sdiv}, {"UDIV", Op::udiv},
	{"SMLABB", Op::smla_xy}, {"SMLABT", Op::smla_xy},
	{"SMLATB", Op::smla_xy}, {"SMLATT", Op::smla_xy},
	{"SMULBB", Op::smul_xy}, {"SMULBT", Op::smul_xy},
	{"SMULTB", Op::smul_xy}, {"SMULTT", Op::smul_xy},
	{"SMLALBB", Op::smlal_xy}, {"SMLALBT", Op::smlal_xy},
	{"SMLALTB", Op::smlal_xy}, {"SMLALTT", Op::smlal_xy},
	{"SMLAWB", Op::smlaw}, {"SMLAWT", Op::smlaw}, {"SMULWB", Op::smulw},
	{"SMULWT", Op::smulw}, {"SMLAD", Op::smlad}, {"SMLADX", Op::smlad},
	{"SMLSD", Op::smlsd}, {"SMLSDX", Op::smlsd}, {"SMUAD", Op::smuad},
	{"SMUADX", Op::smuad}, {"SMUSD", Op::smusd}, {"SMUSDX", Op::smusd},
	{"SMLALD", Op::smlald}, {"SMLALDX", Op::smlald}, {"SMLSLD", Op::smlsld},
	{"SMLSLDX", Op::smlsld}, {"SMMLA", Op::smmla}, {"SMMLAR", Op::smmla},
	{"SMMLS", Op::smmls}, {"SMMLSR", Op::smmls}, {"SMMUL", Op::smmul},
	{"SMMULR", Op::smmul}, {"USAD8", Op::usad8}, {"USADA8", Op::usada8},
	{"QADD", Op::qadd}, {"QSUB", Op::qsub}, {"QDADD", Op::qdadd},
	{"QDSUB", Op::qdsub}, {"SSAT", Op::ssat}, {"USAT", Op::usat},
	{"SSAT16", Op::ssat16}, {"USAT16", Op::usat16},
	{"LDR", Op::ldr}, {"LDRB", Op::ldrb}, {"LDRH", Op::ldrh},
	{"LDRSB", Op::ldrsb}, {"LDRSH", Op::ldrsh}, {"LDRD", Op::ldrd},
	{"STR", Op::str}, {"STRB", Op::strb}, {"STRH", Op::strh},
	{"STRD", Op::strd}, {"LDRT", Op::ldrt}, {"LDRBT", Op::ldrbt},
	{"LDRHT", Op::ldrht}, {"LDRSBT", Op::ldrsbt}, {"LDRSHT", Op::ldrsht},
	{"STRT", Op::strt}, {"STRBT", Op::strbt}, {"STRHT", Op::strht},
	{"LDM", Op::ldm}, {"POP", Op::ldm}, {"STM", Op::stm}, {"PUSH", Op::stm},
	{"LDREX", Op::ldrex}, {"LDREXB", Op::ldrexb}, {"LDREXH", Op::ldrexh},
	{"LDREXD", Op::ldrexd}, {"STREX", Op::strex}, {"STREXB", Op::strexb},
	{"STREXH", Op::strexh}, {"STREXD", Op::strexd}, {"LDAEX", Op::ldaex},
	{"LDAEXB", Op::ldaexb}, {"LDAEXH", Op::ldaexh}, {"LDAEXD", Op::ldaexd},
	{"STLEX", Op::stlex}, {"STLEXB", Op::stlexb}, {"STLEXH", Op::stlexh},
	{"STLEXD", Op::stlexd}, {"LDA", Op::lda}, {"LDAB", Op::ldab},
	{"LDAH", Op::ldah}, {"STL", Op::stl}, {"STLB", Op::stlb},
	{"STLH", Op::stlh}, {"CLREX", Op::clrex},
	{"PLD", Op::prefetch}, {"PLDW", Op::prefetch}, {"PLI", Op::prefetch},
	{"SVC", Op::svc}, {"BKPT", Op::bkpt}, {"UDF", Op::udf}, {"MRS", Op::mrs},
	{"MSR", Op::msr}, {"MCR", Op::coprocessor}, {"MRC", Op::coprocessor},
	{"MCRR", Op::coprocessor}, {"MRRC", Op::coprocessor},
	{"CPS", Op::nop}, {"CPSID", Op::nop}, {"CPSIE", Op::nop},
	{"NOP", Op::nop}, {"DBG", Op::nop}, {"CLRBHB", Op::nop},
	{"CSDB", Op::nop}, {"ESB", Op::nop}, {"TSB", Op::nop}, {"SEV", Op::nop},
	{"SB", Op::nop},
	{"SEVL", Op::nop}, {"WFE", Op::nop}, {"WFI", Op::nop},
	{"YIELD", Op::nop}, {"DMB", Op::dmb}, {"DSB", Op::dsb},
	{"SSBB", Op::dsb}, {"PSSBB", Op::dsb}, {"ISB", Op::isb}, {"IT", Op::it},
	{"VLDR", Op::vldr}, {"VSTR", Op::vstr}, {"VLDM", Op::vldm},
	{"VLDMDB", Op::vldm}, {"FLDMIAX", Op::vldm}, {"FLDMDBX", Op::vldm},
	{"VSTM", Op::vstm}, {"VSTMDB", Op::vstm}, {"FSTMIAX", Op::vstm},
	{"FSTMDBX", Op::vstm}, {"VLD1", Op::vld}, {"VLD2", Op::vld},
	{"VLD3", Op::vld}, {"VLD4", Op::vld}, {"VST1", Op::vst},
	{"VST2", Op::vst}, {"VST3", Op::vst}, {"VST4", Op::vst},
};

/* S, Q, SH, U, UH, UQ followed by ADD16, ADD8, ASX, SAX, SUB16 or SUB8. */
[[nodiscard]] bool parallel_mnemonic(std::string_view name, FormOperation &info) noexcept
{
	struct Prefix final {
		std::string_view text;
		ParallelMode mode;
	};
	constexpr Prefix prefixes[] = {
		{"SH", ParallelMode::signed_halving},
		{"UH", ParallelMode::unsigned_halving},
		{"UQ", ParallelMode::unsigned_saturating},
		{"S", ParallelMode::signed_wrap},
		{"Q", ParallelMode::saturating},
		{"U", ParallelMode::unsigned_wrap},
	};
	struct Suffix final {
		std::string_view text;
		ParallelKind kind;
	};
	constexpr Suffix suffixes[] = {
		{"ADD16", ParallelKind::add16}, {"ADD8", ParallelKind::add8},
		{"ASX", ParallelKind::asx}, {"SAX", ParallelKind::sax},
		{"SUB16", ParallelKind::sub16}, {"SUB8", ParallelKind::sub8},
	};
	for (const auto &prefix : prefixes) {
		if (!name.starts_with(prefix.text))
			continue;
		const auto rest = name.substr(prefix.text.size());
		for (const auto &suffix : suffixes) {
			if (rest != suffix.text)
				continue;
			info.op = Op::parallel;
			info.kind = suffix.kind;
			info.mode = prefix.mode;
			return true;
		}
	}
	return false;
}

[[nodiscard]] const std::array<FormOperation, kFormCount> &table()
{
	static const auto built = [] {
		std::array<FormOperation, kFormCount> result{};
		std::unordered_map<std::string_view, Op> by_name;
		for (const auto &named : named_ops)
			by_name.emplace(named.mnemonic, named.op);
		for (std::size_t index = 0; index < kFormCount; ++index) {
			const std::string_view name = kForms[index].mnemonic;
			const std::string_view form = kForms[index].name;
			FormOperation info;
			if (const auto found = by_name.find(name); found != by_name.end())
				info.op = found->second;
			else
				(void)parallel_mnemonic(name, info);
			info.sp_base = form.find("_SP_") != std::string_view::npos;
			if (form.find("_rr_") != std::string_view::npos)
				info.shape = Shape::reg_shifted;
			else if (form.find("_r_") != std::string_view::npos)
				info.shape = Shape::reg;
			else if (form.find("_i_") != std::string_view::npos)
				info.shape = Shape::immediate;
			else if (form.find("_l_") != std::string_view::npos)
				info.shape = Shape::literal;
			result[index] = info;
		}
		return result;
	}();
	return built;
}

} // namespace

const FormOperation &form_operation(Form form) noexcept
{
	return table()[static_cast<std::size_t>(form)];
}

} // namespace uml::cpu::arm64::aarch32
