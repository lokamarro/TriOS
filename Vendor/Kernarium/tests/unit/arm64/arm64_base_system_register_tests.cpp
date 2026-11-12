// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_host_counter_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include "cpu/arm64/floating_controls.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace test = arm64_system_test;

namespace {

/* The FPCR fields MSR FPCR can set on this host. */
[[nodiscard]] std::uint32_t fpcr_mask() noexcept
{
	return arm64::fpcr_writable_mask_for(::uml::cpu::host_capabilities());
}
constexpr std::uint32_t fpsr_mask = arm64::fpsr_writable_mask;
constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

struct CounterBounds final {
	std::uint64_t before{};
	std::uint64_t after{};
};

CounterBounds counter_bounds;

void sample_counter_before(void *context, std::string_view,
	unsigned long)
{
	auto &bounds = *static_cast<CounterBounds *>(context);
	bounds.before = engine_tests::read_host_virtual_counter();
}

void sample_counter_after(void *context, std::string_view,
	unsigned long)
{
	auto &bounds = *static_cast<CounterBounds *>(context);
	bounds.after = engine_tests::read_host_virtual_counter();
}

void validate_host_counter(
	const arm64::CpuState &, const arm64::CpuState &expected_template,
	const arm64::CpuState &actual, std::string_view case_name,
	std::string_view engine, std::uint32_t instruction)
{
	const auto value = actual.registers[6];
	test::require(counter_bounds.before <= value);
	test::require(value <= counter_bounds.after);
	auto expected = expected_template;
	expected.registers[6] = value;
	engine_tests::require_architecture_equal(
		expected, actual, case_name, engine, instruction);
}

void check_read(test::Harness &harness, std::string_view name,
	std::uint32_t instruction_base, std::uint64_t value,
	engine_tests::NativeRequirements native)
{
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		const auto initial = test::state();
		auto expected = initial;
		if (transfer != 31)
			expected.registers[transfer] = value;
		harness.check(name, instruction_base | transfer, initial, expected,
			native);
	}
}

template <typename Apply>
void check_write(test::Harness &harness, std::string_view name,
	std::uint32_t instruction_base,
	engine_tests::NativeRequirements native, Apply apply)
{
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		const auto initial = test::state();
		auto expected = initial;
		const auto source = transfer == 31 ?
			std::uint64_t{0} : initial.registers[transfer];
		apply(expected, source);
		harness.check(name, instruction_base | transfer, initial, expected,
			native);
	}
}

void thread_pointer(test::Harness &harness)
{
	check_read(harness, "base-read-thread-pointer", 0xd53b'd040U,
		test::state().tls, engine_tests::NativeRequirements::baseline());
	check_write(harness, "base-write-thread-pointer", 0xd51b'd040U,
		engine_tests::NativeRequirements::baseline(),
		[](arm64::CpuState &expected, std::uint64_t source) {
			expected.tls = source;
		});
	check_read(harness, "base-read-only-thread-pointer", 0xd53b'd060U, 0,
		engine_tests::NativeRequirements::baseline());
}

void floating_control_status(test::Harness &harness)
{
	const auto floating =
		engine_tests::NativeRequirements::features(
			::uml::cpu::ArmFeature::floating_point);
	check_read(harness, "base-read-floating-control", 0xd53b'4400U,
		test::state().fpcr & fpcr_mask(), floating);
	check_write(harness, "base-write-floating-control", 0xd51b'4400U,
		floating,
		[](arm64::CpuState &expected, std::uint64_t source) {
			expected.fpcr = static_cast<std::uint32_t>(source) &
				fpcr_mask();
		});
	check_read(harness, "base-read-floating-status", 0xd53b'4420U,
		test::state().fpsr & fpsr_mask, floating);
	check_write(harness, "base-write-floating-status", 0xd51b'4420U,
		floating,
		[](arm64::CpuState &expected, std::uint64_t source) {
			expected.fpsr = static_cast<std::uint32_t>(source) &
				fpsr_mask;
		});
}

void floating_status_boundary_round_trips(test::Harness &harness)
{
	const auto floating =
		engine_tests::NativeRequirements::features(
			::uml::cpu::ArmFeature::floating_point);

	for (const auto condition_bit : {
		0x8000'0000U, 0x4000'0000U, 0x2000'0000U, 0x1000'0000U,
	}) {
		auto initial = test::state();
		initial.fpcr = 0;
		initial.fpsr = 0;
		initial.registers[4] = condition_bit;
		auto expected = initial;
		harness.check("base-discard-floating-status-condition",
			0xd51b'4424U, initial, expected, floating);
	}

	for (const auto mutable_flag : {
		0x0800'0000U, 0x0000'0080U, 0x0000'0010U, 0x0000'0008U,
		0x0000'0004U, 0x0000'0002U, 0x0000'0001U,
	}) {
		auto initial = test::state();
		initial.fpcr = 0;
		initial.fpsr = 0;
		initial.registers[4] = mutable_flag;
		auto expected = initial;
		expected.fpsr = mutable_flag;
		harness.check("base-round-trip-floating-status-flag",
			0xd51b'4424U, initial, expected, floating);
	}

	const std::array code{
		engine_tests::ProgramWord{
			.offset = 0,
			.instruction = 0xd51b'443fU, // MSR FPSR, XZR
		},
		engine_tests::ProgramWord{
			.offset = 4,
			.instruction = 0x1e62'2020U, // FCMP D1, D2
		},
		engine_tests::ProgramWord{
			.offset = 8,
			.instruction = terminal_system_call,
		},
	};
	auto initial = test::state();
	initial.fpcr = 0;
	initial.fpsr = 0;
	const std::uint64_t exceptional = 0x7ff0'0000'0000'0001ULL;
	const std::uint64_t ordinary = 0x3ff0'0000'0000'0000ULL;
	std::memcpy(initial.vectors[1].bytes.data(), &exceptional,
		sizeof(exceptional));
	std::memcpy(initial.vectors[2].bytes.data(), &ordinary,
		sizeof(ordinary));
	auto expected = initial;
	expected.fpsr = 1;
	expected.nzcv = 0x3000'0000U;
	expected.pc = engine_tests::engine_test_code_base + 12;
	harness.engines().check_program({
		.name = "base-native-floating-status-update",
		.code = std::span<const engine_tests::ProgramWord>{code},
		.initial = initial,
		.expected = expected,
		.event = engine_tests::ExpectedEvent::system_call,
		.native = floating,
	});
}

void condition_flags(test::Harness &harness)
{
	check_read(harness, "base-read-condition-flags", 0xd53b'4200U,
		test::state().nzcv, engine_tests::NativeRequirements::baseline());
	check_write(harness, "base-write-condition-flags", 0xd51b'4200U,
		engine_tests::NativeRequirements::baseline(),
		[](arm64::CpuState &expected, std::uint64_t source) {
			expected.nzcv = static_cast<std::uint32_t>(source) &
				0xf000'0000U;
		});
}

void data_independent_timing(test::Harness &harness)
{
	/* DIT is PSTATE.DIT presented as a system register.  The architecture
	 * defines the read as Zeros(39):PSTATE.DIT:Zeros(24) and the write as
	 * PSTATE.DIT = X[t]<24>, so the register form has to agree with the MSR
	 * immediate form that writes the same bit.  Go's runtime reads it through
	 * MRS whenever HWCAP_DIT is advertised. */
	const auto timing =
		engine_tests::NativeRequirements::features(
			::uml::cpu::ArmFeature::dit);
	check_read(harness, "base-read-data-independent-timing", 0xd53b'42a0U,
		test::state().pstate_dit ? 0x0100'0000ULL : 0ULL, timing);
	check_write(harness, "base-write-data-independent-timing", 0xd51b'42a0U,
		timing,
		[](arm64::CpuState &expected, std::uint64_t source) {
			expected.pstate_dit = (source & 0x0100'0000ULL) != 0;
		});
}

void fixed_read_only_registers(test::Harness &harness)
{
	check_read(harness, "base-read-cache-zero-identification",
		0xd53b'00e0U, 4U,
		engine_tests::NativeRequirements::baseline());
	check_read(harness, "base-read-cache-type", 0xd53b'0020U,
		0x8444'8004U,
		engine_tests::NativeRequirements::baseline());
	check_read(harness, "base-read-counter-frequency", 0xd53b'e000U,
		engine_tests::read_host_counter_frequency(),
		engine_tests::NativeRequirements::baseline());
}

void virtual_counter(test::Harness &harness)
{
	{
		const auto initial = test::state();
		const auto expected = initial;
		harness.engines().check({
			.name = "base-read-host-virtual-counter",
			.instruction = 0xd53b'e046U, // MRS X6, CNTVCT_EL0
			.initial = initial,
			.expected = expected,
			.native =
				engine_tests::NativeRequirements::baseline(),
			.validator = validate_host_counter,
			.execution_context = &counter_bounds,
			.before_engine = sample_counter_before,
			.after_engine = sample_counter_after,
		});
	}

	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		const auto counter = 0xd53b'e040U | transfer;
		if (transfer == 31) {
			harness.check_unchanged("base-discard-virtual-counter",
				counter);
			continue;
		}

		const std::array code{
			engine_tests::ProgramWord{
				.offset = 0,
				.instruction = counter,
			},
			engine_tests::ProgramWord{
				.offset = 4,
				/* CMP Xtransfer, #0. */
				.instruction = 0xf100'001fU |
					(transfer << 5),
			},
			engine_tests::ProgramWord{
				.offset = 8,
				/* CSET Xtransfer, NE. */
				.instruction = 0x9a9f'07e0U | transfer,
			},
			engine_tests::ProgramWord{
				.offset = 12,
				.instruction = terminal_system_call,
			},
		};
		const auto initial = test::state();
		auto expected = initial;
		expected.registers[transfer] = 1;
		expected.nzcv = 0x2000'0000U;
		expected.pc = engine_tests::engine_test_code_base + 16;
		harness.engines().check_program({
			.name = "base-read-virtual-counter",
			.code = std::span<const engine_tests::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.event = engine_tests::ExpectedEvent::system_call,
			.native =
				engine_tests::NativeRequirements::baseline(),
		});
	}
}

void virtual_counter_is_monotonic(test::Harness &harness)
{
	const std::array code{
		engine_tests::ProgramWord{
			.offset = 0,
			.instruction = 0xd53b'e040U, // MRS X0, CNTVCT_EL0
		},
		engine_tests::ProgramWord{
			.offset = 4,
			.instruction = 0xd53b'e041U, // MRS X1, CNTVCT_EL0
		},
		engine_tests::ProgramWord{
			.offset = 8,
			.instruction = 0xeb00'003fU, // CMP X1, X0
		},
		engine_tests::ProgramWord{
			.offset = 12,
			.instruction = 0x9a9f'37e2U, // CSET X2, CS
		},
		engine_tests::ProgramWord{
			.offset = 16,
			.instruction = 0xaa1f'03e0U, // MOV X0, XZR
		},
		engine_tests::ProgramWord{
			.offset = 20,
			.instruction = 0xaa1f'03e1U, // MOV X1, XZR
		},
		engine_tests::ProgramWord{
			.offset = 24,
			.instruction = 0xf100'001fU, // CMP X0, #0
		},
		engine_tests::ProgramWord{
			.offset = 28,
			.instruction = terminal_system_call,
		},
	};
	const auto initial = test::state();
	auto expected = initial;
	expected.registers[0] = 0;
	expected.registers[1] = 0;
	expected.registers[2] = 1;
	expected.nzcv = 0x6000'0000U;
	expected.pc = engine_tests::engine_test_code_base + 32;
	harness.engines().check_program({
		.name = "base-virtual-counter-monotonic",
		.code = std::span<const engine_tests::ProgramWord>{code},
		.initial = initial,
		.expected = expected,
		.event = engine_tests::ExpectedEvent::system_call,
		.native = engine_tests::NativeRequirements::baseline(),
	});
}

/* FEAT_ECV's CNTVCTSS_EL0 reads the virtual count without waiting for an
 * ISB. The virtual CPU implements FEAT_ECV exactly where the host does, and
 * a host counter read cannot be pinned in the instruction corpus, so both
 * outcomes live here. CNTPCTSS_EL0, like CNTPCT_EL0, is UNDEFINED on every
 * host: Linux gives EL0 no physical count. */
void self_synchronized_counter(test::Harness &harness)
{
	const auto ecv = engine_tests::NativeRequirements::features(
		engine_tests::ArmFeature::ecv);
	if (engine_tests::host_supports(ecv)) {
		const auto initial = test::state();
		harness.engines().check({
			.name = "ecv-read-self-synchronized-virtual-counter",
			.instruction = 0xd53b'e0c6U, // MRS X6, CNTVCTSS_EL0
			.initial = initial,
			.expected = initial,
			.native = ecv,
			.validator = validate_host_counter,
			.execution_context = &counter_bounds,
			.before_engine = sample_counter_before,
			.after_engine = sample_counter_after,
		});
		harness.check_unchanged(
			"ecv-discard-self-synchronized-virtual-counter",
			0xd53b'e0dfU, ecv);
	} else {
		harness.require_illegal(0xd53b'e0c6U);
		harness.require_illegal(0xd53b'e0dfU);
	}
	harness.require_illegal(0xd53b'e026U); // MRS X6, CNTPCT_EL0
	harness.require_illegal(0xd53b'e0a6U); // MRS X6, CNTPCTSS_EL0
	harness.require_illegal(0xd53b'e0bfU); // MRS XZR, CNTPCTSS_EL0
}

void inaccessible_and_reserved(test::Harness &harness)
{
	for (const auto instruction : {
		0xd538'1000U, 0xd518'1000U, // SCTLR_EL1
		0xd53f'ffe3U, 0xd51f'ffe3U,
		0xd53b'4440U, 0xd51b'4440U, // FPMR (FEAT_FP8)
		0xd550'0000U, 0xd570'0000U, // system-register pair
		0xd550'0001U, 0xd570'0001U, // reserved odd Rt
		0xd55f'ffe2U, 0xd57f'ffe2U,
	})
		harness.require_illegal(instruction);

	for (const unsigned op1 : {0U, 7U})
		for (const unsigned crn : {0U, 8U, 9U, 15U})
			for (const unsigned crm : {0U, 15U})
				for (const unsigned op2 : {0U, 7U})
					for (const unsigned transfer : {0U, 31U})
						harness.require_illegal(0xd548'0000U |
							(op1 << 16) | (crn << 12) |
							(crm << 8) | (op2 << 5) |
							transfer);
}

} // namespace

/* FPSR.N, Z, C and V are the AArch32 comparison flags, RES0 on this
 * AArch64-only virtual CPU: MSR FPSR discards them with QC and IXC set, and
 * MRS FPSR reads them as zero, before and after a native operation, as an M1
 * does. */
void floating_status_condition_is_res0(test::Harness &harness)
{
	const auto floating =
		engine_tests::NativeRequirements::features(
			::uml::cpu::ArmFeature::floating_point);
	constexpr std::uint32_t inexact = 0x0000'0010U;
	constexpr std::uint32_t saturation = 0x0800'0000U;
	constexpr std::uint32_t condition = 0xf000'0000U;
	for (const bool divide : {false, true}) {
		const std::array code{
			engine_tests::ProgramWord{
				.offset = 0,
				.instruction = 0xd51b'4424U, // MSR FPSR, X4
			},
			engine_tests::ProgramWord{
				.offset = 4,
				.instruction = divide ? 0x1e21'1802U : // FDIV S2, S0, S1
					0xd503'201fU, // NOP
			},
			engine_tests::ProgramWord{
				.offset = 8,
				.instruction = 0xd53b'4425U, // MRS X5, FPSR
			},
			engine_tests::ProgramWord{
				.offset = 12,
				.instruction = terminal_system_call,
			},
		};
		auto initial = test::state();
		initial.fpcr = 0;
		initial.fpsr = 0;
		initial.registers[4] = condition | saturation |
			(divide ? 0U : inexact);
		const std::uint32_t one = 0x3f80'0000U;
		const std::uint32_t three = 0x4040'0000U;
		initial.vectors[0] = {};
		initial.vectors[1] = {};
		std::memcpy(initial.vectors[0].bytes.data(), &one, sizeof(one));
		std::memcpy(initial.vectors[1].bytes.data(), &three,
			sizeof(three));
		auto expected = initial;
		expected.fpsr = saturation | inexact;
		expected.registers[5] = saturation | inexact;
		if (divide) {
			const std::uint32_t third = 0x3eaa'aaabU;
			expected.vectors[2] = {};
			std::memcpy(expected.vectors[2].bytes.data(), &third,
				sizeof(third));
		}
		expected.pc = engine_tests::engine_test_code_base + 16;
		harness.engines().check_program({
			.name = "base-floating-status-condition-reads-zero",
			.code = std::span<const engine_tests::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.event = engine_tests::ExpectedEvent::system_call,
			.native = floating,
		});
	}
}

int main()
{
	test::Harness harness;
	thread_pointer(harness);
	floating_control_status(harness);
	floating_status_boundary_round_trips(harness);
	condition_flags(harness);
	data_independent_timing(harness);
	fixed_read_only_registers(harness);
	virtual_counter(harness);
	virtual_counter_is_monotonic(harness);
	inaccessible_and_reserved(harness);
	self_synchronized_counter(harness);
	floating_status_condition_is_res0(harness);
	harness.print_summary("arm64-base-system-registers");
	return 0;
}
