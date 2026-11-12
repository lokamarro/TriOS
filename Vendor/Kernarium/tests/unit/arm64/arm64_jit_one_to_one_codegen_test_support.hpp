// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "a64_body_inspection.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <string_view>
#include <utility>

namespace uml::emulator::tests::jit_one_to_one {

namespace jit_test = uml::cpu::arm64::jit::testing;

inline constexpr std::uint32_t rd_mask = 31U;
inline constexpr std::uint32_t rn_rd_mask = (31U << 5U) | rd_mask;
inline constexpr std::uint32_t rm_rn_rd_mask =
	(31U << 16U) | rn_rd_mask;
inline constexpr std::uint32_t ra_rm_rn_rd_mask =
	(31U << 10U) | rm_rn_rd_mask;

struct Metadata final {
	bool accesses_fp_state{};
	bool native_fp{};
};

inline constexpr Metadata no_fp_metadata{};
inline constexpr Metadata vector_state_metadata{true, false};
inline constexpr Metadata native_fp_metadata{true, true};

enum class OperandRole : std::uint8_t {
	gpr,
	stack_pointer,
	zero_register,
	vector,
	constrained_vector,
};

struct Operand final {
	OperandRole role;
	unsigned shift;
	unsigned width;
	unsigned guest;
};

struct OperandSet final {
	std::array<Operand, 4> values{};
	std::size_t size{};
};

[[nodiscard]] constexpr Operand
gpr(unsigned shift, unsigned guest) noexcept
{
	return {OperandRole::gpr, shift, 5U, guest & 31U};
}

[[nodiscard]] constexpr Operand sp(unsigned shift) noexcept
{
	return {OperandRole::stack_pointer, shift, 5U, 31U};
}

[[nodiscard]] constexpr Operand zr(unsigned shift) noexcept
{
	return {OperandRole::zero_register, shift, 5U, 31U};
}

[[nodiscard]] constexpr Operand
vector(unsigned shift, unsigned guest) noexcept
{
	return {OperandRole::vector, shift, 5U, guest & 31U};
}

[[nodiscard]] constexpr Operand
constrained_vector(unsigned shift, unsigned width, unsigned guest) noexcept
{
	return {OperandRole::constrained_vector, shift, width, guest & 31U};
}

template <typename... Operands>
[[nodiscard]] constexpr OperandSet operands(Operands... values) noexcept
{
	static_assert(sizeof...(Operands) <= 4U);
	const std::array input{values...};
	OperandSet result{};
	result.size = input.size();
	for (std::size_t index = 0; index < input.size(); ++index)
		result.values[index] = input[index];
	return result;
}

struct Case final {
	std::string_view name;
	std::uint32_t instruction;
	std::uint32_t register_mask;
	bool direct_after_state_import{true};
	Metadata metadata{};
	OperandSet operand_set{};
	/*
	 * Most cases preserve the guest opcode and change only register fields.
	 * Translation-time simplifications may instead provide the expected
	 * semantic word explicitly.
	 */
	std::uint32_t semantic_instruction{};

	constexpr Case(std::string_view case_name, std::uint32_t guest_instruction,
		       std::uint32_t mask, bool direct = true,
		       Metadata expected_metadata = {},
		       OperandSet expected_operands = {},
		       std::uint32_t semantic = 0) noexcept
		: name{case_name}, instruction{guest_instruction},
		  register_mask{mask}, direct_after_state_import{direct},
		  metadata{expected_metadata}, operand_set{expected_operands},
		  semantic_instruction{semantic == 0 ? guest_instruction : semantic}
	{
	}
};

[[noreturn]] inline void fail(
	std::string_view name, const char *message,
	const std::source_location location = std::source_location::current())
{
	std::fprintf(stderr, "%.*s: %s at %s:%u\n",
		static_cast<int>(name.size()), name.data(), message,
		location.file_name(), location.line());
	std::abort();
}

inline void require(
	bool condition, std::string_view name, const char *message,
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail(name, message, location);
}

[[nodiscard]] constexpr bool is_state_import(
	std::uint32_t instruction) noexcept
{
	/*
	 * The compiler imports resident integer and vector values from CpuState
	 * through x15. Ignore the destination and scaled offset. These are the
	 * only first-use imports needed by the ordinary compute cases below.
	 */
	const auto shape = instruction & 0xffc0'03e0U;
	return shape == 0xf940'01e0U || shape == 0xb940'01e0U ||
	       shape == 0x3dc0'01e0U;
}

[[nodiscard]] constexpr bool is_runtime_control_transfer(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0x7c00'0000U) == 0x1400'0000U)
		return true; // B or BL
	if ((instruction & 0xff00'0010U) == 0x5400'0000U)
		return true; // B.cond
	if ((instruction & 0x7e00'0000U) == 0x3400'0000U ||
	    (instruction & 0x7e00'0000U) == 0x3600'0000U)
		return true; // compare/test-and-branch
	return (instruction & 0xfe00'0000U) == 0xd600'0000U;
}

[[nodiscard]] constexpr bool is_conditional_runtime_branch(
	std::uint32_t instruction) noexcept
{
	return uml::testing::a64::conditional_branch(instruction);
}

enum class NativeOpcodeClass : std::uint8_t {
	reserved_or_extension,
	data_processing_immediate,
	data_processing_register,
	floating_or_vector,
	memory,
	branch_exception_or_system,
};

[[nodiscard]] constexpr NativeOpcodeClass native_opcode_class(
	std::uint32_t instruction) noexcept
{
	/*
	 * A64's primary decode is selected by op0, bits [28:25]. Register
	 * remapping is allowed to alter only operand fields below this primary
	 * class. Keep this classification independent of the JIT decoder so the
	 * test catches a mask or rewrite that turns an ordinary operation into a
	 * host memory access or control/system operation.
	 */
	switch ((instruction >> 25U) & 15U) {
	case 8U:
	case 9U:
		return NativeOpcodeClass::data_processing_immediate;
	case 5U:
	case 13U:
		return NativeOpcodeClass::data_processing_register;
	case 7U:
	case 15U:
		return NativeOpcodeClass::floating_or_vector;
	case 4U:
	case 6U:
	case 12U:
	case 14U:
		return NativeOpcodeClass::memory;
	case 10U:
	case 11U:
		return NativeOpcodeClass::branch_exception_or_system;
	default:
		return NativeOpcodeClass::reserved_or_extension;
	}
}

[[nodiscard]] constexpr bool is_ordinary_compute_class(
	std::uint32_t instruction) noexcept
{
	const auto opcode_class = native_opcode_class(instruction);
	return opcode_class == NativeOpcodeClass::data_processing_immediate ||
	       opcode_class == NativeOpcodeClass::data_processing_register ||
	       opcode_class == NativeOpcodeClass::floating_or_vector;
}

static_assert(is_ordinary_compute_class(0x9100'0000U)); // ADD immediate
static_assert(is_ordinary_compute_class(0x8b00'0000U)); // ADD register
static_assert(is_ordinary_compute_class(0x4e20'8400U)); // vector compute
static_assert(!is_ordinary_compute_class(0xf940'0000U)); // native memory
static_assert(!is_ordinary_compute_class(0x1400'0000U)); // native branch
static_assert(!is_ordinary_compute_class(0xd400'0001U)); // exception
static_assert(!is_ordinary_compute_class(0xd53b'4200U)); // system register

[[nodiscard]] constexpr std::int64_t sign_extend(
	std::uint32_t value, unsigned width) noexcept
{
	const auto sign = std::uint64_t{1} << (width - 1U);
	return static_cast<std::int64_t>(
		(static_cast<std::uint64_t>(value) ^ sign) - sign);
}

[[nodiscard]] constexpr bool direct_branch_displacement(
	std::uint32_t instruction, std::int64_t &displacement) noexcept
{
	if ((instruction & 0x7c00'0000U) == 0x1400'0000U) {
		displacement = sign_extend(instruction & 0x03ff'ffffU, 26U);
		return true;
	}
	if ((instruction & 0xff00'0010U) == 0x5400'0000U ||
	    (instruction & 0x7e00'0000U) == 0x3400'0000U) {
		displacement =
			sign_extend((instruction >> 5U) & 0x7ffffU, 19U);
		return true;
	}
	if ((instruction & 0x7e00'0000U) == 0x3600'0000U) {
		displacement =
			sign_extend((instruction >> 5U) & 0x3fffU, 14U);
		return true;
	}
	return false;
}

[[nodiscard]] constexpr bool is_indirect_control_transfer(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0xfe00'0000U) == 0xd600'0000U;
}

[[nodiscard]] constexpr bool is_return_to_host(
	std::uint32_t instruction) noexcept
{
	/*
	 * x30 is host-owned by the generated prologue. Guest X30 is always
	 * remapped and therefore cannot influence this target.
	 */
	return instruction == 0xd65f'03c0U;
}

[[nodiscard]] constexpr bool is_helper_call_through_x16(
	std::uint32_t instruction) noexcept
{
	return instruction == 0xd63f'0200U;
}

[[nodiscard]] constexpr bool is_literal_load_to(
	std::uint32_t instruction, unsigned destination) noexcept
{
	return (instruction & 0xff00'001fU) ==
	       (0x5800'0000U | (destination & 31U));
}

[[nodiscard]] constexpr std::int64_t literal_target(
	std::uint32_t instruction, std::size_t source) noexcept
{
	return static_cast<std::int64_t>(source) +
	       sign_extend((instruction >> 5U) & 0x7ffffU, 19U);
}

[[nodiscard]] constexpr bool control_target_decoder_self_test() noexcept
{
	std::int64_t displacement{};
	if (!direct_branch_displacement(0x1400'0002U, displacement) ||
	    displacement != 2)
		return false;
	if (!direct_branch_displacement(0x54ff'ffe0U, displacement) ||
	    displacement != -1)
		return false;
	if (!direct_branch_displacement(0xb400'0040U, displacement) ||
	    displacement != 2)
		return false;
	if (!direct_branch_displacement(0x3600'0040U, displacement) ||
	    displacement != 2)
		return false;
	return literal_target(0x5800'0040U, 7U) == 9;
}

static_assert(control_target_decoder_self_test());

struct Match final {
	jit_test::CompiledCode compiled;
	std::size_t semantic_index;
	std::size_t successor_index;
	std::uint32_t successor_instruction;
};

struct Sentinel final {
	std::uint32_t instruction;
	std::uint32_t register_mask;
};

[[nodiscard]] constexpr bool matches_semantic(
	std::uint32_t native, std::uint32_t semantic,
	std::uint32_t register_mask) noexcept
{
	return (native & ~register_mask) == (semantic & ~register_mask);
}

[[nodiscard]] constexpr Sentinel sentinel_for(const Case &test) noexcept
{
	/*
	 * A second ordinary instruction gives the target a precise right-hand
	 * boundary. This makes a post-semantic helper/check visible without
	 * mistaking the normal block epilogue for target lowering. Keep several
	 * candidates so the target and sentinel never share an opcode shape.
	 */
	constexpr std::array candidates{
		Sentinel{0xca19'0317U, rm_rn_rd_mask}, // EOR X23, X24, X25
		Sentinel{0x9ad9'0b17U, rm_rn_rd_mask}, // UDIV X23, X24, X25
		Sentinel{0x93d9'1f17U, rm_rn_rd_mask}, // EXTR X23, X24, X25, #7
	};
	for (const auto candidate : candidates) {
		const bool candidate_matches_target = matches_semantic(
			candidate.instruction, test.semantic_instruction,
			test.register_mask);
		const bool target_matches_candidate = matches_semantic(
			test.semantic_instruction, candidate.instruction,
			candidate.register_mask);
		if (!candidate_matches_target && !target_matches_candidate)
			return candidate;
	}
	return {};
}

[[nodiscard]] constexpr bool gpr_role(OperandRole role) noexcept
{
	return role == OperandRole::gpr ||
	       role == OperandRole::stack_pointer ||
	       role == OperandRole::zero_register;
}

[[nodiscard]] constexpr bool vector_role(OperandRole role) noexcept
{
	return role == OperandRole::vector ||
	       role == OperandRole::constrained_vector;
}

[[nodiscard]] constexpr unsigned field(
	std::uint32_t instruction, const Operand &operand) noexcept
{
	const auto mask = (std::uint32_t{1} << operand.width) - 1U;
	return (instruction >> operand.shift) & mask;
}

inline void require_metadata(
	const Case &test, const jit_test::CompiledCode &compiled)
{
	require(compiled.accesses_fp_state == test.metadata.accesses_fp_state,
		test.name, "generated block has wrong FP-state metadata");
	require(compiled.native_fp == test.metadata.native_fp, test.name,
		"generated block has wrong native FP-environment metadata");
}

struct OperandRoleCheck final {
	bool valid;
	const char *message;
};

[[nodiscard]] inline OperandRoleCheck check_operand_roles(
	const Case &test, std::uint32_t native)
{
	for (std::size_t index = 0; index < test.operand_set.size; ++index) {
		const auto &operand = test.operand_set.values[index];
		if (operand.width == 0U || operand.width > 5U)
			return {false, "invalid operand-field width in test"};
		const auto host = field(native, operand);
		switch (operand.role) {
		case OperandRole::gpr:
		case OperandRole::stack_pointer: {
			/*
			 * x14/x15 carry the TLB/CpuState context. x18 is the
			 * Darwin platform register. x19-x21 carry linked-block
			 * state. x22-x28 remain untouched host callee-saved
			 * registers. x29/x30/SP retain their frame, link, and stack
			 * roles.
			 */
			if (!(host < 14U || host == 16U || host == 17U))
				return {
					false,
					"ordinary guest GPR used a host-reserved register"};
			break;
		}
		case OperandRole::zero_register:
			if (host != 31U)
				return {
					false,
					"architectural zero register was not preserved"};
			break;
		case OperandRole::vector:
		case OperandRole::constrained_vector:
			/*
			 * Darwin preserves v8-v15 across calls. The generated
			 * prologue deliberately leaves them untouched.
			 */
			if (!(host < 8U || host >= 16U))
				return {
					false,
					"guest vector used a host ABI-preserved register"};
			break;
		}
	}

	for (std::size_t left = 0; left < test.operand_set.size; ++left) {
		const auto &first = test.operand_set.values[left];
		for (std::size_t right = left + 1U;
		     right < test.operand_set.size; ++right) {
			const auto &second = test.operand_set.values[right];
			if (gpr_role(first.role) != gpr_role(second.role) ||
			    vector_role(first.role) != vector_role(second.role))
				continue;
			if (first.role == OperandRole::zero_register ||
			    second.role == OperandRole::zero_register)
				continue;
			const auto first_host = field(native, first);
			const auto second_host = field(native, second);
			if (first.guest == second.guest) {
				if (first_host != second_host)
					return {
						false,
						"aliased guest operands changed host binding"};
			} else {
				if (first_host == second_host)
					return {
						false,
						"distinct live guest operands share a host register"};
			}
		}
	}
	return {true, nullptr};
}

inline void require_operand_roles(
	const Case &test, std::uint32_t native)
{
	const auto checked = check_operand_roles(test, native);
	require(checked.valid, test.name, checked.message);
}

inline void require_translation_owned_control_targets(
	const Case &test, const jit_test::CompiledCode &compiled)
{
	const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
		compiled.hot_words);
	for (std::size_t index = 0; index < hot.size(); ++index) {
		const auto instruction = hot[index];
		std::int64_t displacement{};
		if (direct_branch_displacement(instruction, displacement)) {
			const auto target =
				static_cast<std::int64_t>(index) + displacement;
			require(target >= 0 &&
					static_cast<std::size_t>(target) < hot.size(),
				test.name,
				"native direct branch escaped translation-owned code");
			continue;
		}
		if (!is_indirect_control_transfer(instruction))
			continue;
		if (is_return_to_host(instruction))
			continue;
		require(is_helper_call_through_x16(instruction), test.name,
			"native indirect branch target is not translation-owned");
		require(index != 0U &&
				is_literal_load_to(hot[index - 1U], 16U),
			test.name,
			"native helper target was not loaded from the immutable pool");
		const auto target =
			literal_target(hot[index - 1U], index - 1U);
		require(target >=
					static_cast<std::int64_t>(
						compiled.hot_words) &&
				target >= 0 &&
				static_cast<std::size_t>(target) + 1U <
					compiled.words.size() &&
				(static_cast<std::size_t>(target) & 1U) == 0U,
			test.name,
			"native helper pointer is not a translation-owned literal");
	}
}

[[nodiscard]] inline Match compile_and_find(
	const Case &test, std::uint64_t pc)
{
	const auto sentinel = sentinel_for(test);
	require(sentinel.instruction != 0U, test.name,
		"could not select a distinct successor instruction");
	const std::array guest{test.instruction, sentinel.instruction};
	auto compiled = jit_test::compile_block(pc, guest);
	require(compiled.guest_instructions == guest.size(), test.name,
		"supported instruction and successor were not compiled");
	require(!compiled.guest_illegal, test.name,
		"supported instruction was classified as illegal");
	require(compiled.hot_words <= compiled.words.size(), test.name,
		"hot-word boundary exceeds generated code");
	require_metadata(test, compiled);

	const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
		compiled.hot_words);
	std::size_t semantic_index = hot.size();
	std::size_t matches = 0;
	for (std::size_t index = 0; index < hot.size(); ++index) {
		if ((hot[index] & ~test.register_mask) !=
		    (test.semantic_instruction & ~test.register_mask))
			continue;
		if (!check_operand_roles(test, hot[index]).valid)
			continue;
		semantic_index = index;
		++matches;
	}
	if (matches != 1U) {
		std::fprintf(stderr,
			"%.*s: guest=%08x mask=%08x matches=%zu hot=%zu\n",
			static_cast<int>(test.name.size()), test.name.data(),
			test.semantic_instruction, test.register_mask, matches,
			hot.size());
		for (std::size_t index = 0; index < hot.size(); ++index)
			std::fprintf(stderr, "  %zu: %08x\n", index, hot[index]);
	}
	require(matches == 1U, test.name,
		"expected exactly one remapped native semantic instruction");
	require(is_ordinary_compute_class(hot[semantic_index]), test.name,
		"remapped semantic word became native memory, control-flow, "
		"exception, system, or another non-compute class");
	if (test.register_mask != 0U)
		require((hot[semantic_index] & test.register_mask) !=
			    (test.semantic_instruction & test.register_mask),
			test.name,
			"semantic instruction did not remap its guest registers");
	require_operand_roles(test, hot[semantic_index]);

	std::size_t successor_index = hot.size();
	std::size_t successor_matches = 0;
	for (std::size_t index = semantic_index + 1U; index < hot.size();
	     ++index) {
		if (!matches_semantic(hot[index], sentinel.instruction,
				      sentinel.register_mask))
			continue;
		successor_index = index;
		++successor_matches;
	}
	require(successor_matches == 1U, test.name,
		"expected exactly one native successor semantic instruction");
	require_translation_owned_control_targets(test, compiled);
	return Match{std::move(compiled), semantic_index, successor_index,
		     sentinel.instruction};
}

inline void require_one_to_one(const Case &test, std::uint64_t pc)
{
	auto match = compile_and_find(test, pc);
	const auto hot = std::span<const std::uint32_t>{match.compiled.words}
				 .first(match.compiled.hot_words);

	for (std::size_t index = 0; index < match.semantic_index; ++index)
		require(!is_runtime_control_transfer(hot[index]), test.name,
			"helper call or runtime decision branch precedes the "
			"semantic instruction");
	for (std::size_t index = match.semantic_index + 1U;
	     index < match.successor_index; ++index)
		require(!is_runtime_control_transfer(hot[index]), test.name,
			"helper call or runtime decision branch follows the "
			"semantic instruction");

	if (!test.direct_after_state_import)
		return;
	std::size_t last_import = hot.size();
	for (std::size_t index = 0; index < match.semantic_index; ++index)
		if (is_state_import(hot[index]))
			last_import = index;
	require(last_import != hot.size(), test.name,
		"test case did not exercise a first-use state import");
	require(match.semantic_index == last_import + 1U, test.name,
		"work remained in the hot path after the final first-use import");
}

template <std::size_t Size>
inline void require_all(
	const std::array<Case, Size> &cases, std::uint64_t base)
{
	for (std::size_t index = 0; index < cases.size(); ++index)
		require_one_to_one(cases[index], base + index * 0x1000U);
}

} // namespace uml::emulator::tests::jit_one_to_one
