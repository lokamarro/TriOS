// SPDX-License-Identifier: GPL-2.0-or-later
/* The SM = 0 vector-length family, the deliberate departure from the
 * architecture that lets a virtual CPU with FEAT_SME and without FEAT_SVE
 * count and scale a vector length of 16 bytes outside streaming mode and
 * spill and fill Z and P registers
 * (cpu/arm64/detail/sm0_vector_length.hpp). This checks the shared
 * definition both engines translate from:
 *
 * - the family is exactly the six vector-length forms and the four spill
 *   forms of the encoding table that the words name, over the whole 0x04,
 *   0x85 and 0xe5 bytes of the SVE space, and no other;
 * - each vector-length word's base A64 word is an ADD, SUB, MOVZ, MOVN or
 *   NOP of the same effect on the general registers, as the model the tests
 *   state from the pseudocode computes it;
 * - each spill word's fields, and the JIT slow path's execution of it for a
 *   CPU without predicate state; and
 * - the family applies exactly where the virtual CPU has FEAT_SME.
 *
 * The engines' own tests (arm64-gadget-sme, arm64-jit-sme-control and
 * arm64-jit-sme-codegen) and the corpus (arm64-sm0-vl) run the words. */
#include "arm64_sm0_vector_length_support.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/sm0_vector_length.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/detail/sm0_spill.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/host_capabilities.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace {

using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;
using ::uml::testing::require;
namespace arm64 = ::uml::cpu::arm64;
namespace detail = ::uml::cpu::arm64::detail;
namespace model = ::uml::emulator::tests::sm0_vector_length;

/* The form of the encoding table that a witness word is. */
[[nodiscard]] consteval std::uint16_t form_of(std::uint32_t word)
{
	const auto *row = detail::find_sme_encoding(word);
	if (row == nullptr || (row->form & detail::sme_form_sme_section) != 0U)
		throw "not a word of an SVE-index form";
	return row->form;
}

/* The forms of the encoding table the family is: CNT[BHWD], DEC[BHWD],
 * INC[BHWD], RDVL, ADDVL and ADDPL. */
[[nodiscard]] bool family_form(std::uint16_t form) noexcept
{
	return form == form_of(0x0420'e000U) /* CNTB */ ||
		form == form_of(0x0430'e400U) /* DECB */ ||
		form == form_of(0x0430'e000U) /* INCB */ ||
		form == form_of(0x04bf'5000U) /* RDVL */ ||
		form == form_of(0x0420'5000U) /* ADDVL */ ||
		form == form_of(0x0460'5000U) /* ADDPL */;
}

[[nodiscard]] bool table_family(std::uint32_t word) noexcept
{
	const auto *row = detail::find_sme_encoding(word);
	return row != nullptr && (row->form & detail::sme_form_sme_section) == 0U &&
		family_form(row->form);
}

/* The forms of the encoding table the spill words are: LDR and STR of a
 * vector register and of a predicate register, with an immediate offset. */
[[nodiscard]] bool spill_form(std::uint16_t form) noexcept
{
	return form == form_of(0x8580'4000U) /* LDR Z */ ||
		form == form_of(0xe580'4000U) /* STR Z */ ||
		form == form_of(0x8580'0000U) /* LDR P */ ||
		form == form_of(0xe580'0000U) /* STR P */;
}

[[nodiscard]] bool table_spill(std::uint32_t word) noexcept
{
	const auto *row = detail::find_sme_encoding(word);
	return row != nullptr && (row->form & detail::sme_form_sme_section) == 0U &&
		spill_form(row->form);
}

void family_is_the_ten_forms()
{
	std::size_t rows = 0;
	for (std::size_t index = 0; index < detail::sme_encodings.size(); ++index) {
		const auto &row = detail::sme_encodings[index];
		const bool family = (row.form & detail::sme_form_sme_section) == 0U &&
			family_form(row.form);
		require(detail::sm0_vector_length_word(
				detail::sme_encoding_witnesses[index]) == family);
		rows += family;
	}
	/* CNT, DEC and INC in four sizes each, RDVL, ADDVL and ADDPL. */
	require(rows == 15U);
	/* Every word of the SVE space's 0x04 byte, which holds all of them:
	 * the family is the table's rows and no word more, reserved patterns
	 * and multipliers included. */
	for (std::uint32_t low = 0; low < 0x0100'0000U; ++low) {
		const std::uint32_t word = 0x0400'0000U | low;
		if (detail::sm0_vector_length_word(word) != table_family(word)) {
			std::fprintf(stderr, "%08x: family %d, table %d\n", word,
				detail::sm0_vector_length_word(word), table_family(word));
			require(false);
		}
	}
	/* The spill forms are four more rows of the table, and the words of the
	 * 0x85 and 0xe5 bytes that name them are the whole of each. */
	std::size_t spill_rows = 0;
	for (std::size_t index = 0; index < detail::sme_encodings.size(); ++index) {
		const auto &row = detail::sme_encodings[index];
		const bool spill = (row.form & detail::sme_form_sme_section) == 0U &&
			spill_form(row.form);
		require(detail::sm0_spill_word(detail::sme_encoding_witnesses[index]) ==
			spill);
		require(detail::sm0_family_word(detail::sme_encoding_witnesses[index]) ==
			(spill || table_family(detail::sme_encoding_witnesses[index])));
		spill_rows += spill;
	}
	require(spill_rows == 4U);
	for (const std::uint32_t high : {0x8500'0000U, 0xe500'0000U})
		for (std::uint32_t low = 0; low < 0x0100'0000U; ++low) {
			const std::uint32_t word = high | low;
			if (detail::sm0_spill_word(word) != table_spill(word) ||
			    detail::sm0_vector_length_word(word)) {
				std::fprintf(stderr, "%08x: spill %d, table %d\n", word,
					detail::sm0_spill_word(word), table_spill(word));
				require(false);
			}
		}
	/* The word-level definition fits the equivalent function. */
	for (const auto word : model::words())
		require(detail::sm0_vector_length_word(word) &&
			!detail::sm0_spill_word(word) &&
			detail::sm0_vector_length_equivalent(word).has_value());
	for (const std::uint32_t word :
	     {0x04bf'5800U /* RDSVL */, 0x0420'5800U /* ADDSVL */,
	      0x0460'5800U /* ADDSPL */, 0x0420'f3e0U /* SQINCB */,
	      0x0430'f3e0U /* SQINCB X */, 0x04a0'f7e0U /* UQINCW */,
	      0x04f0'c3e0U /* INCD vector */, 0x2520'8000U /* CNTP */,
	      0x2528'8800U /* SQINCP */, 0x0420'e3e0U & ~0x0030'0000U,
	      0xd503'201fU, 0x0000'0000U})
		require(!detail::sm0_vector_length_equivalent(word).has_value());
}

/* A tiny interpreter for the base words the family translates to. */
[[nodiscard]] bool run_base_word(std::uint32_t word,
	model::Registers &registers) noexcept
{
	const unsigned rd = word & 31U;
	const unsigned rn = (word >> 5) & 31U;
	if (word == detail::sm0_vector_length_nop)
		return true;
	if ((word & 0xffe0'0000U) == 0xd280'0000U ||
	    (word & 0xffe0'0000U) == 0x9280'0000U) { // MOVZ, MOVN (hw 0)
		const std::uint64_t immediate = (word >> 5) & 0xffffU;
		if (rd != 31U)
			registers.x[rd] = (word & 0x4000'0000U) != 0U ? immediate :
				~immediate;
		return true;
	}
	if ((word & 0xff80'0000U) == 0x9100'0000U ||
	    (word & 0xff80'0000U) == 0xd100'0000U) { // ADD, SUB (immediate)
		require((word & 0x0040'0000U) == 0U); // no shift
		const std::uint64_t immediate = (word >> 10) & 0xfffU;
		const std::uint64_t base = rn == 31U ? registers.sp :
			registers.x[rn];
		const std::uint64_t result = (word & 0x4000'0000U) != 0U ?
			base - immediate : base + immediate;
		if (rd == 31U)
			registers.sp = result;
		else
			registers.x[rd] = result;
		return true;
	}
	return false;
}

[[nodiscard]] model::Registers sample_registers(unsigned seed) noexcept
{
	model::Registers registers;
	for (unsigned index = 0; index < registers.x.size(); ++index)
		registers.x[index] = 0x5a00'0000'0000'0000ULL |
			(std::uint64_t{index} << 32) | (index * 0x1111U + seed);
	registers.sp = 0x7700'1230ULL + seed;
	return registers;
}

[[nodiscard]] bool same(const model::Registers &a,
	const model::Registers &b) noexcept
{
	return a.x == b.x && a.sp == b.sp;
}

/* Each family word's base word has the effect the model states. */
void equivalents_have_the_effect_of_the_family()
{
	std::size_t checked = 0;
	for (const auto word : model::words()) {
		const auto equivalent = detail::sm0_vector_length_equivalent(word);
		require(equivalent.has_value());
		auto expected = sample_registers(1);
		auto actual = expected;
		require(model::model(word, expected));
		if (!run_base_word(*equivalent, actual)) {
			std::fprintf(stderr, "%08x: %08x is not a base word\n", word,
				*equivalent);
			require(false);
		}
		if (!same(expected, actual)) {
			std::fprintf(stderr, "%08x: %08x differs from the model\n", word,
				*equivalent);
			require(false);
		}
		++checked;
	}
	require(checked > 4000U);
}

/* The spill word's fields, for every form, base, transfer register and
 * immediate. */
void spill_words_decode_to_their_fields()
{
	std::size_t checked = 0;
	for (const bool predicate : {false, true})
		for (const bool store : {false, true})
			for (int immediate = -256; immediate < 256; ++immediate)
				for (const unsigned base : {0U, 9U, 29U, 30U, 31U})
					for (const unsigned transfer :
					     {0U, 1U, 7U, 14U, 15U, predicate ? 15U : 31U}) {
						const model::Spill expected{store, predicate,
							transfer, base, immediate};
						const auto access =
							detail::sm0_spill_access(expected.word());
						require(access.has_value());
						require(access->store == store &&
							access->predicate == predicate &&
							access->transfer == transfer &&
							access->base == base &&
							access->offset == expected.offset() &&
							access->bytes() == expected.bytes());
						require(detail::sm0_family_word(expected.word()));
						require(!detail::sm0_vector_length_equivalent(
							expected.word()));
						++checked;
					}
	require(checked > 10000U);
	/* The reserved bit of the predicate forms, and a word of another form. */
	for (const std::uint32_t word :
	     {0x8580'0010U, 0xe580'0010U, 0xe580'001fU, 0xa580'43efU /* LD1RQB */,
	      0x8540'43efU, 0xe500'43efU, 0x8580'2000U, 0x8580'6000U,
	      0xe590'c000U})
		require(!detail::sm0_spill_access(word).has_value() &&
			!detail::sm0_family_word(word));
}

/* A guest memory of a flat buffer from `base`, mapped to its length. */
class Flat final : public detail::Sm0Memory {
public:
	Flat(std::uint64_t base, std::size_t size) noexcept
		: base_{base}, bytes_(size)
	{
	}
	std::byte *translate(std::uint64_t address,
		::uml::memory::MemoryAccess) noexcept override
	{
		return address >= base_ && address < base_ + bytes_.size() ?
			bytes_.data() + (address - base_) : nullptr;
	}
	void stored(std::byte *, std::size_t) noexcept override { ++stores; }

	std::uint64_t base_;
	std::vector<std::byte> bytes_;
	unsigned stores{};
};

/* The JIT slow path for the spill words, with no predicate state: Z is V, a
 * predicate store writes zeros, a predicate load reads and drops, and the
 * predicate registers and the live mark stay as they were. */
void slow_path_executes_the_spill_words()
{
	constexpr std::uint64_t base = 0x40'0000;
	constexpr std::size_t window = 1U << 16;
	for (const auto &spill : model::spills()) {
		Flat memory{base, window};
		for (std::size_t index = 0; index < window; ++index)
			memory.bytes_[index] = static_cast<std::byte>(index * 13U + 5U);
		arm64::CpuState cpu;
		for (unsigned reg = 0; reg < 32U; ++reg)
			for (unsigned byte = 0; byte < 16U; ++byte)
				cpu.vectors[reg].bytes[byte] =
					static_cast<std::byte>(reg * 16U + byte + 1U);
		const std::uint64_t target = base + 0x8000;
		const std::uint64_t pointer = target - static_cast<std::uint64_t>(
			spill.offset());
		if (spill.base == 31U)
			cpu.sp = pointer & ~15ULL;
		else
			cpu.registers[spill.base] = pointer;
		/* An SP base is 16-byte aligned, so the target moves with it. */
		const std::uint64_t address = (spill.base == 31U ? cpu.sp : pointer) +
			static_cast<std::uint64_t>(spill.offset());
		model::SpillImage expected;
		for (unsigned reg = 0; reg < 32U; ++reg)
			for (unsigned byte = 0; byte < 16U; ++byte)
				expected.v[reg][byte] = static_cast<std::uint8_t>(
					cpu.vectors[reg].bytes[byte]);
		expected.memory.resize(window);
		for (std::size_t index = 0; index < window; ++index)
			expected.memory[index] =
				static_cast<std::uint8_t>(memory.bytes_[index]);
		const auto result =
			detail::execute_sm0_spill(cpu, spill.word(), memory);
		require(result.event == detail::Sm0Event::retired);
		model::apply_spill(spill, address - base, expected);
		for (unsigned reg = 0; reg < 32U; ++reg)
			for (unsigned byte = 0; byte < 16U; ++byte)
				require(static_cast<std::uint8_t>(
						cpu.vectors[reg].bytes[byte]) ==
					expected.v[reg][byte]);
		for (std::size_t index = 0; index < window; ++index)
			require(static_cast<std::uint8_t>(memory.bytes_[index]) ==
				expected.memory[index]);
		require(memory.stores == (spill.store ? 1U : 0U));
	}
}

/* ... and its faults: a page that does not translate, and an SP base that is
 * not 16-byte aligned; any other word is outside the contract. */
void slow_path_faults_on_the_spill_words()
{
	constexpr std::uint64_t base = 0x40'0000;
	constexpr std::size_t page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t end = base + 2U * page;
	Flat memory{base, 2U * page};
	arm64::CpuState cpu;
	/* A vector access that starts in the last mapped page and ends in the
	 * next, which does not translate. */
	cpu.registers[0] = end - 8U;
	for (const auto &spill :
	     {model::Spill{false, false, 3, 0, 0}, model::Spill{true, false, 3, 0, 0}}) {
		const auto result =
			detail::execute_sm0_spill(cpu, spill.word(), memory);
		require(result.event == detail::Sm0Event::memory_fault);
		require(!result.alignment);
		require(result.fault_address == end);
		require(result.access == (spill.store ? ::uml::memory::MemoryAccess::write
			: ::uml::memory::MemoryAccess::read));
	}
	/* A predicate access in the last byte. */
	cpu.registers[0] = end - 1U;
	for (const bool store : {false, true}) {
		const auto predicate = detail::execute_sm0_spill(cpu,
			model::Spill{store, true, 3, 0, 0}.word(), memory);
		require(predicate.event == detail::Sm0Event::memory_fault &&
			predicate.fault_address == end);
	}
	/* A misaligned SP faults as an alignment fault, whatever it points to. */
	cpu.sp = base + 8;
	for (const bool predicate_form : {false, true}) {
		const auto aligned = detail::execute_sm0_spill(cpu,
			model::Spill{false, predicate_form, 3, 31, 0}.word(), memory);
		require(aligned.event == detail::Sm0Event::memory_fault &&
			aligned.alignment && aligned.fault_address == cpu.sp);
	}
	/* Neither the other SVE words nor the vector-length words. */
	for (const std::uint32_t word :
	     {0xa540'a000U /* LD1B */, 0x0420'e3e0U, 0x2520'8000U, 0xd503'201fU,
	      0x8580'0010U})
		require(detail::execute_sm0_spill(cpu, word, memory).event ==
			detail::Sm0Event::fatal);
}

[[nodiscard]] HostCapabilities host_with(
	std::initializer_list<ArmFeature> features, unsigned svl)
{
	HostCapabilities host;
	for (const auto feature : features)
		host.supported.set(static_cast<std::size_t>(feature));
	host.sme_svl_bytes = static_cast<std::uint16_t>(svl);
	return host;
}

/* Only a virtual CPU with FEAT_SME applies the family: without SME the
 * words are as undefined as any SVE word. */
void family_applies_with_sme()
{
	const auto sme = {ArmFeature::sme, ArmFeature::sme_f64f64,
		ArmFeature::sme_i16i64};
	auto sme_only = host_with(sme, 64);
	require(detail::implements_sm0_vector_length(sme_only));
	for (const unsigned svl : {16U, 32U, 128U, 256U})
		require(detail::implements_sm0_vector_length(host_with(sme, svl)));
	auto sme2 = sme_only;
	sme2.supported.set(static_cast<std::size_t>(ArmFeature::sme2));
	require(detail::implements_sm0_vector_length(sme2));

	/* An SVE token changes nothing: no virtual CPU has SVE. */
	auto with_sve = sme_only;
	for (const auto token : {ArmFeature::sve, ArmFeature::sve2})
		with_sve.supported.set(static_cast<std::size_t>(token));
	require(detail::implements_sm0_vector_length(with_sve));

	/* No SME: a missing token, or a streaming vector length it lacks. */
	require(!detail::implements_sm0_vector_length(HostCapabilities{}));
	require(!detail::implements_sm0_vector_length(host_with(sme, 0)));
	require(!detail::implements_sm0_vector_length(host_with(sme, 48)));
	require(!detail::implements_sm0_vector_length(host_with(
		{ArmFeature::sme}, 64)));
	require(!detail::implements_sm0_vector_length(
		host_with({ArmFeature::sve, ArmFeature::sve2}, 0)));
}

} // namespace

int main()
{
	family_is_the_ten_forms();
	equivalents_have_the_effect_of_the_family();
	spill_words_decode_to_their_fields();
	slow_path_executes_the_spill_words();
	slow_path_faults_on_the_spill_words();
	family_applies_with_sme();
	return 0;
}
