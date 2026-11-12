// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace arm64_floating_trap_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

inline constexpr std::uint32_t fpsr_invalid = 1U << 0U;
inline constexpr std::uint32_t fpsr_divide_by_zero = 1U << 1U;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2U;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3U;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4U;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7U;
inline constexpr std::uint32_t fpsr_exception_mask = 0x9fU;
inline constexpr std::uint32_t syndrome_base =
	(0x2cU << 26U) | (1U << 25U) | (1U << 23U);

[[nodiscard]] constexpr std::uint32_t enable_for(
	std::uint32_t flags) noexcept
{
	return (flags & fpsr_exception_mask) << 8U;
}

[[nodiscard]] constexpr std::uint32_t syndrome_for(
	std::uint32_t flags) noexcept
{
	return syndrome_base | (flags & fpsr_exception_mask);
}

[[nodiscard]] inline arm64::CpuState state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (std::size_t index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			0x1020'3040'5060'7080ULL ^
			(seed * 0x0101'0101ULL + index * 0x1111'1111ULL);
	for (std::size_t reg = 0; reg < cpu.vectors.size(); ++reg)
		for (std::size_t byte = 0; byte < 16; ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				(seed + reg * 19U + byte * 7U) & 0xffU);
	cpu.sp = 0x7fff'0000'0000ULL + seed * 16U;
	cpu.nzcv = static_cast<std::uint32_t>(seed << 28U) & 0xf000'0000U;
	cpu.fpsr = 1U << 27U;
	cpu.tls = 0xcafe'0000'0000'0000ULL | seed;
	return cpu;
}

inline void write_lane(arm64::CpuState &cpu, unsigned reg,
	unsigned lane, unsigned lane_bytes, std::uint64_t bits)
{
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		cpu.vectors[reg].bytes[lane * lane_bytes + byte] =
			static_cast<std::byte>(bits >> (byte * 8U));
}

inline void clear_vector(arm64::CpuState &cpu, unsigned reg)
{
	cpu.vectors[reg].bytes.fill(std::byte{});
}

inline void check_illegal_with_enables(engine::EngineHarness &harness,
	std::string_view name, std::uint32_t instruction,
	const arm64::CpuState &initial,
	engine::NativeRequirements native = engine::NativeRequirements::baseline())
{
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::illegal_instruction,
		.native = native,
		.invalid_encoding = true,
	});
}

[[nodiscard]] inline engine::NativeRequirements scalar_native() noexcept
{
	return engine::NativeRequirements::features(
		::uml::cpu::ArmFeature::floating_point);
}

[[nodiscard]] inline engine::NativeRequirements vector_native() noexcept
{
	return engine::NativeRequirements::features(
		::uml::cpu::ArmFeature::advanced_simd);
}

static_assert(enable_for(fpsr_exception_mask) == 0x9f00U);
static_assert(syndrome_for(fpsr_invalid) == 0xb280'0001U);

} // namespace arm64_floating_trap_test

namespace test = arm64_floating_trap_test;

namespace {

inline constexpr std::uint32_t fdiv_s0_s1_s2 = 0x1e22'1820U;
inline constexpr std::uint32_t fdiv_s3_s4_s5 = 0x1e25'1883U;
inline constexpr std::uint32_t fmul_s0_s1_s2 = 0x1e22'0820U;

struct NativeFpEnvironment final {
	std::uint64_t fpcr{};
	std::uint64_t fpsr{};
};

[[nodiscard]] NativeFpEnvironment native_fp_environment()
{
	NativeFpEnvironment result;
	asm volatile("mrs %0, fpcr\n\tmrs %1, fpsr" :
		"=r"(result.fpcr), "=r"(result.fpsr) : : "memory");
	return result;
}

void set_native_fp_environment(const NativeFpEnvironment &environment)
{
	asm volatile("msr fpsr, %0\n\tmsr fpcr, %1\n\tisb" : :
		"r"(environment.fpsr), "r"(environment.fpcr) : "memory");
}

[[nodiscard]] constexpr bool same_environment(
	const NativeFpEnvironment &left,
	const NativeFpEnvironment &right) noexcept
{
	return left.fpcr == right.fpcr && left.fpsr == right.fpsr;
}

struct HostEnvironmentTrace final {
	std::array<std::string_view, 2> engines{};
	std::array<NativeFpEnvironment, 2> installed{};
	std::array<NativeFpEnvironment, 2> observed{};
	NativeFpEnvironment saved{};
	std::size_t calls{};
	bool active{};
};

void install_host_environment(void *opaque, std::string_view engine,
	unsigned long)
{
	auto &trace = *static_cast<HostEnvironmentTrace *>(opaque);
	if (trace.active || trace.calls >= trace.engines.size())
		std::abort();
	trace.saved = native_fp_environment();
	set_native_fp_environment({
		.fpcr = (2ULL << 22U) | (1ULL << 24U) | (1ULL << 25U),
		.fpsr = (1ULL << 27U) | test::fpsr_invalid |
			test::fpsr_underflow | test::fpsr_inexact,
	});
	trace.engines[trace.calls] = engine;
	trace.installed[trace.calls] = native_fp_environment();
	trace.active = true;
}

void observe_host_environment(void *opaque, std::string_view engine,
	unsigned long)
{
	auto &trace = *static_cast<HostEnvironmentTrace *>(opaque);
	if (!trace.active || trace.calls >= trace.engines.size() ||
	    trace.engines[trace.calls] != engine)
		std::abort();
	trace.observed[trace.calls] = native_fp_environment();
	set_native_fp_environment(trace.saved);
	++trace.calls;
	trace.active = false;
}

void require_host_environment_restored(const HostEnvironmentTrace &trace)
{
	constexpr std::array expected_engines{
		std::string_view{"gadgets"},
		std::string_view{"jit"},
	};
	if (trace.active || (trace.calls != 1U && trace.calls != 2U))
		std::abort();
	for (std::size_t index = 0; index < trace.calls; ++index) {
		if (trace.engines[index] == expected_engines[index] &&
		    same_environment(trace.installed[index],
			    trace.observed[index]))
			continue;
		std::fprintf(stderr,
			"host FP environment not restored after %.*s trap: "
			"FPCR=%#llx/%#llx FPSR=%#llx/%#llx\n",
			static_cast<int>(trace.engines[index].size()),
			trace.engines[index].data(),
			static_cast<unsigned long long>(
				trace.installed[index].fpcr),
			static_cast<unsigned long long>(
				trace.observed[index].fpcr),
			static_cast<unsigned long long>(
				trace.installed[index].fpsr),
			static_cast<unsigned long long>(
				trace.observed[index].fpsr));
		std::abort();
	}
}

void preexisting_flag_does_not_retrap(
	test::engine::EngineHarness &harness)
{
	auto initial = test::state(200U);
	initial.fpcr = test::enable_for(test::fpsr_divide_by_zero);
	initial.fpsr |= test::fpsr_divide_by_zero;
	test::write_lane(initial, 1U, 0U, 4U, 0x4080'0000U);
	test::write_lane(initial, 2U, 0U, 4U, 0x4000'0000U);
	auto expected = initial;
	test::clear_vector(expected, 0U);
	test::write_lane(expected, 0U, 0U, 4U, 0x4000'0000U);
	harness.check({
		.name = "preexisting enabled flag does not retrap",
		.instruction = fdiv_s0_s1_s2,
		.initial = initial,
		.expected = expected,
		.native = test::scalar_native(),
	});
}

void unrelated_enable_commits_result(test::engine::EngineHarness &harness)
{
	auto initial = test::state(201U);
	initial.fpcr = test::enable_for(test::fpsr_invalid);
	test::write_lane(initial, 1U, 0U, 4U, 0x3f80'0000U);
	test::write_lane(initial, 2U, 0U, 4U, 0U);
	auto expected = initial;
	test::clear_vector(expected, 0U);
	test::write_lane(expected, 0U, 0U, 4U, 0x7f80'0000U);
	expected.fpsr |= test::fpsr_divide_by_zero;
	harness.check({
		.name = "unrelated enable commits result and cumulative flag",
		.instruction = fdiv_s0_s1_s2,
		.initial = initial,
		.expected = expected,
		.native = test::scalar_native(),
	});
}

void trap_commits_no_flag(test::engine::EngineHarness &harness)
{
	auto initial = test::state(208U);
	initial.fpcr = test::enable_for(test::fpsr_overflow);
	initial.fpsr |= test::fpsr_input_denormal;
	test::write_lane(initial, 1U, 0U, 4U, 0x7f7f'ffffU);
	test::write_lane(initial, 2U, 0U, 4U, 0x4000'0000U);
	/* The overflow also raises Inexact, which is not enabled; the trap
	 * leaves FPSR as it was all the same. */
	const auto expected = initial;
	harness.check({
		.name = "trap commits no newly raised unenabled flag",
		.instruction = fmul_s0_s1_s2,
		.initial = initial,
		.expected = expected,
		.event = test::engine::ExpectedEvent::floating_point_trap,
		.native = test::scalar_native(),
		.syndrome = test::syndrome_for(test::fpsr_overflow),
	});
}

void unenabled_flags_survive_a_later_trap(
	test::engine::EngineHarness &harness)
{
	const std::array code{
		test::engine::ProgramWord{
			.offset = 0U, .instruction = fdiv_s0_s1_s2},
		test::engine::ProgramWord{
			.offset = 4U, .instruction = fdiv_s3_s4_s5},
		test::engine::ProgramWord{
			.offset = 8U, .instruction = 0xd400'0001U},
	};
	auto initial = test::state(202U);
	initial.fpcr = test::enable_for(test::fpsr_invalid);
	test::write_lane(initial, 1U, 0U, 4U, 0x3f80'0000U);
	test::write_lane(initial, 2U, 0U, 4U, 0U);
	test::write_lane(initial, 4U, 0U, 4U, 0U);
	test::write_lane(initial, 5U, 0U, 4U, 0U);
	auto expected = initial;
	test::clear_vector(expected, 0U);
	test::write_lane(expected, 0U, 0U, 4U, 0x7f80'0000U);
	expected.fpsr |= test::fpsr_divide_by_zero;
	expected.pc = test::engine::engine_test_code_base + 4U;
	harness.check_program({
		.name = "unenabled cumulative flag survives later precise trap",
		.code = code,
		.initial = initial,
		.expected = expected,
		.event = test::engine::ExpectedEvent::floating_point_trap,
		.native = test::scalar_native(),
		.syndrome = test::syndrome_for(test::fpsr_invalid),
	});
}

void fpcr_write_takes_effect_at_next_instruction(
	test::engine::EngineHarness &harness)
{
	const std::array code{
		test::engine::ProgramWord{
			.offset = 0U, .instruction = 0xd51b'4409U},
		test::engine::ProgramWord{
			.offset = 4U, .instruction = fdiv_s0_s1_s2},
		test::engine::ProgramWord{
			.offset = 8U, .instruction = 0xd400'0001U},
	};
	auto initial = test::state(203U);
	initial.fpcr = 0U;
	initial.registers[9] =
		test::enable_for(test::fpsr_divide_by_zero);
	test::write_lane(initial, 1U, 0U, 4U, 0x3f80'0000U);
	test::write_lane(initial, 2U, 0U, 4U, 0U);
	auto expected = initial;
	expected.fpcr = test::enable_for(test::fpsr_divide_by_zero);
	expected.pc = test::engine::engine_test_code_base + 4U;
	harness.check_program({
		.name = "FPCR enable write is visible at next instruction",
		.code = code,
		.initial = initial,
		.expected = expected,
		.event = test::engine::ExpectedEvent::floating_point_trap,
		.native = test::scalar_native(),
		.syndrome =
			test::syndrome_for(test::fpsr_divide_by_zero),
	});
}

void nonfloating_and_reserved_instructions(
	test::engine::EngineHarness &harness)
{
	auto ordinary = test::state(204U);
	ordinary.fpcr = test::enable_for(test::fpsr_exception_mask);
	auto expected = ordinary;
	++expected.registers[10];
	harness.check({
		.name = "nonfloating instruction ignores floating enables",
		.instruction = 0x9100'054aU,
		.initial = ordinary,
		.expected = expected,
		.native = test::engine::NativeRequirements::baseline(),
	});

	auto reserved = test::state(205U);
	reserved.fpcr = test::enable_for(test::fpsr_exception_mask);
	reserved.fpsr |= test::fpsr_input_denormal;
	test::check_illegal_with_enables(harness,
		"reserved data-processing encoding stays illegal",
		0x2e00'fc00U, reserved);
}

void memory_instructions_remain_nontransactional(
	test::engine::EngineHarness &harness)
{
	const std::array code{
		test::engine::ProgramWord{
			.offset = 0U, .instruction = 0x3dc0'0020U},
		test::engine::ProgramWord{
			.offset = 4U, .instruction = 0x3d80'0040U},
		test::engine::ProgramWord{
			.offset = 8U, .instruction = 0xd400'0001U},
	};
	std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> initial_memory{};
	std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> expected_memory{};
	for (unsigned byte = 0; byte < 16U; ++byte) {
		initial_memory[byte] =
			static_cast<std::byte>(0x30U + byte * 7U);
		initial_memory[16U + byte] =
			static_cast<std::byte>(0xc0U - byte * 3U);
	}
	expected_memory = initial_memory;
	std::copy_n(initial_memory.begin(), 16U,
		expected_memory.begin() + 16U);
	const std::array regions{
		test::engine::MemoryRegion{
			.address = test::engine::engine_test_data_base,
			.initial = initial_memory,
			.expected = expected_memory,
		},
	};
	auto initial = test::state(206U);
	initial.fpcr = test::enable_for(test::fpsr_exception_mask);
	initial.registers[1] = test::engine::engine_test_data_base;
	initial.registers[2] = test::engine::engine_test_data_base + 16U;
	auto expected = initial;
	expected.vectors[0].bytes = {};
	std::copy_n(initial_memory.begin(), 16U,
		expected.vectors[0].bytes.begin());
	expected.pc = test::engine::engine_test_code_base + 12U;
	harness.check_program({
		.name = "enabled traps do not transact SIMD memory",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = test::vector_native(),
		.memory = regions,
	});
}

void host_environment_is_restored_after_trap(
	test::engine::EngineHarness &harness)
{
	auto initial = test::state(207U);
	initial.fpcr = test::enable_for(test::fpsr_divide_by_zero);
	initial.fpsr |= test::fpsr_overflow;
	test::write_lane(initial, 1U, 0U, 4U, 0x3f80'0000U);
	test::write_lane(initial, 2U, 0U, 4U, 0U);
	HostEnvironmentTrace trace;
	harness.check({
		.name = "host FPCR and FPSR restored after trap",
		.instruction = fdiv_s0_s1_s2,
		.initial = initial,
		.expected = initial,
		.event = test::engine::ExpectedEvent::floating_point_trap,
		.native = test::scalar_native(),
		.syndrome =
			test::syndrome_for(test::fpsr_divide_by_zero),
		.execution_context = &trace,
		.before_engine = &install_host_environment,
		.after_engine = &observe_host_environment,
	});
	require_host_environment_restored(trace);
}

} // namespace

int main()
{
	test::engine::EngineHarness harness;
	preexisting_flag_does_not_retrap(harness);
	unrelated_enable_commits_result(harness);
	trap_commits_no_flag(harness);
	unenabled_flags_survive_a_later_trap(harness);
	fpcr_write_takes_effect_at_next_instruction(harness);
	nonfloating_and_reserved_instructions(harness);
	memory_instructions_remain_nontransactional(harness);
	host_environment_is_restored_after_trap(harness);
	harness.print_summary("arm64-floating-trap-control");
	return 0;
}
