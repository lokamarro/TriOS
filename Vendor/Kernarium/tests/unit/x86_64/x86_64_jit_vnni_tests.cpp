// SPDX-License-Identifier: GPL-2.0-or-later
/* AVX-VNNI through the x86-64 JIT. FEXCore lowers VPDPBUSD and VPDPBUSDS to
 * one USDOT on a host with I8MM, to three SDOTs with DotProd and to a long NEON
 * sequence with neither, and the embedder chooses by the host's capability
 * profile. Every lowering the host can run must give the architectural result,
 * so this runs the four dot products at both vector lengths under each profile
 * the host supports, with edge and pseudo-random operands, and compares with
 * the scalar definition. */
#define UML_X86_64_SSE_LOOP_JIT 1
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "cpu/x86_64/segments.hpp"
#include "platform/apple/host_features.hpp"
#include "x86_64_flat_memory.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

namespace x86_64 = uml::cpu::x86_64;
using uml::platform::ArmFeature;
using uml::test::x86_64_flat_memory::FlatMemory;

constexpr std::uint64_t acc_address = 0x1000;
constexpr std::uint64_t first_address = 0x1020;
constexpr std::uint64_t second_address = 0x1040;
constexpr std::uint64_t result_address = 0x1060;
constexpr std::uint64_t stack_top = 0x3ff0;

int failures;

struct Operands {
	std::array<std::uint8_t, 32> accumulator;
	std::array<std::uint8_t, 32> first;
	std::array<std::uint8_t, 32> second;
};

enum class Op { vpdpbusd, vpdpbusds, vpdpwssd, vpdpwssds };

const char *name(Op op)
{
	switch (op) {
	case Op::vpdpbusd: return "VPDPBUSD";
	case Op::vpdpbusds: return "VPDPBUSDS";
	case Op::vpdpwssd: return "VPDPWSSD";
	case Op::vpdpwssds: return "VPDPWSSDS";
	}
	return "?";
}

std::int32_t saturate(std::int64_t value)
{
	if (value > std::numeric_limits<std::int32_t>::max())
		return std::numeric_limits<std::int32_t>::max();
	if (value < std::numeric_limits<std::int32_t>::min())
		return std::numeric_limits<std::int32_t>::min();
	return static_cast<std::int32_t>(value);
}

/* The architectural result, one dword at a time. The first source is the
 * unsigned one of VPDPBUSD. */
std::array<std::uint8_t, 32> reference(Op op, const Operands &in, unsigned bytes)
{
	std::array<std::uint8_t, 32> out{};
	for (unsigned lane = 0; lane < bytes / 4; ++lane) {
		std::int32_t acc;
		std::memcpy(&acc, &in.accumulator[lane * 4], 4);
		std::int64_t sum = 0;
		if (op == Op::vpdpbusd || op == Op::vpdpbusds) {
			for (unsigned j = 0; j < 4; ++j)
				sum += static_cast<std::int64_t>(in.first[lane * 4 + j]) *
					static_cast<std::int8_t>(in.second[lane * 4 + j]);
		} else {
			for (unsigned j = 0; j < 2; ++j) {
				std::int16_t a, b;
				std::memcpy(&a, &in.first[lane * 4 + j * 2], 2);
				std::memcpy(&b, &in.second[lane * 4 + j * 2], 2);
				sum += static_cast<std::int64_t>(a) * b;
			}
		}
		const bool saturating = op == Op::vpdpbusds || op == Op::vpdpwssds;
		const std::int64_t total = static_cast<std::int64_t>(acc) + sum;
		const std::int32_t result = saturating
			? saturate(total) : static_cast<std::int32_t>(static_cast<std::uint32_t>(total));
		std::memcpy(&out[lane * 4], &result, 4);
	}
	return out;
}

/* vmovdqu reg, [disp32] and vmovdqu [disp32], reg through ymm/xmm 0..2, then
 * the dot product with the accumulator in 0, the first source in 1 and the
 * second in 2, then SYSCALL to end the run. */
std::vector<std::uint8_t> program(Op op, bool wide)
{
	const std::uint8_t load_prefix = wide ? 0xfe : 0xfa;
	const auto address = [](std::uint64_t value) {
		return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(value),
			static_cast<std::uint8_t>(value >> 8),
			static_cast<std::uint8_t>(value >> 16),
			static_cast<std::uint8_t>(value >> 24)};
	};
	std::vector<std::uint8_t> bytes;
	const auto emit = [&](std::initializer_list<std::uint8_t> list) {
		bytes.insert(bytes.end(), list);
	};
	for (unsigned reg = 0; reg < 3; ++reg) {
		const std::uint64_t source = reg == 0 ? acc_address
			: reg == 1 ? first_address : second_address;
		const auto a = address(source);
		/* vmovdqu reg, [disp32]: C5 /F3 0F 6F, modrm reg, SIB absolute. */
		emit({0xc5, load_prefix, 0x6f, static_cast<std::uint8_t>(0x04 | (reg << 3)),
			0x25, a[0], a[1], a[2], a[3]});
	}
	const std::uint8_t opcode = op == Op::vpdpbusd ? 0x50
		: op == Op::vpdpbusds ? 0x51 : op == Op::vpdpwssd ? 0x52 : 0x53;
	/* VEX.66.0F38.W0 /r with vvvv = 1 (inverted 1110) and L = 128 or 256:
	 * the third byte is 0 1110 L 01. */
	emit({0xc4, 0xe2, static_cast<std::uint8_t>(wide ? 0x75 : 0x71), opcode, 0xc2});
	const auto r = address(result_address);
	emit({0xc5, load_prefix, 0x7f, 0x04, 0x25, r[0], r[1], r[2], r[3]});
	emit({0x0f, 0x05});
	return bytes;
}

bool run(const uml::platform::HostCapabilities &host, Op op, bool wide,
	const Operands &in, std::array<std::uint8_t, 32> &out)
{
	FlatMemory memory;
	const auto code = program(op, wide);
	for (std::size_t index = 0; index < code.size(); ++index)
		memory.load(index, {code[index]});
	const auto put = [&](std::uint64_t address, const std::array<std::uint8_t, 32> &data) {
		for (std::size_t index = 0; index < data.size(); ++index)
			memory.load(address + index, {data[index]});
	};
	put(acc_address, in.accumulator);
	put(first_address, in.first);
	put(second_address, in.second);

	x86_64::jit::FexEngine engine{host};
	engine.apply_changes(1, x86_64::ExecutionMode::long64,
		{.mapping_sequence = 1, .code_sequence = 1}, false);
	x86_64::CpuState cpu{};
	cpu.segment_selectors = {0, 0x33, 0x2b, 0, 0, 0};
	(void)x86_64::enter_segments(cpu);
	cpu.rip = 0;
	cpu.rflags = 0x202;
	cpu.gpr(x86_64::Register::rsp) = stack_top;
	uml::cpu::InterruptToken interrupt{};
	for (unsigned runs = 0; runs < 8; ++runs) {
		const auto result = engine.run(cpu, memory, {
			.generation = 1,
			.instruction_budget = 1000,
			.execution_context = nullptr,
			.interrupt = &interrupt,
			.orders_stores = false,
		});
		if (result.execution.reason == uml::cpu::ExitReason::system_call) {
			std::array<std::byte, 32> raw{};
			if (!memory.read(result_address, raw))
				return false;
			std::memcpy(out.data(), raw.data(), 32);
			return true;
		}
		if (result.execution.reason != uml::cpu::ExitReason::instruction_budget)
			return false;
	}
	return false;
}

std::uint64_t random_state = 0x9e3779b97f4a7c15ULL;
std::uint64_t next()
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return random_state;
}

void fill(std::array<std::uint8_t, 32> &bytes, int mode)
{
	for (unsigned index = 0; index < 32; ++index) {
		switch (mode) {
		case 0: bytes[index] = 0x00; break;
		case 1: bytes[index] = 0xff; break;
		case 2: bytes[index] = 0x80; break;
		case 3: bytes[index] = 0x7f; break;
		default: bytes[index] = static_cast<std::uint8_t>(next() >> 24); break;
		}
	}
}

void check_profile(const char *label, const uml::platform::HostCapabilities &host)
{
	unsigned cases = 0, bad = 0;
	for (const Op op : {Op::vpdpbusd, Op::vpdpbusds, Op::vpdpwssd, Op::vpdpwssds})
		for (const bool wide : {false, true})
			for (int first_mode = 0; first_mode < 9; ++first_mode)
				for (int second_mode = 0; second_mode < 9; ++second_mode)
					for (int acc_mode = 0; acc_mode < 3; ++acc_mode) {
						Operands in{};
						fill(in.first, first_mode);
						fill(in.second, second_mode);
						/* Accumulators that make the saturating forms
						 * saturate: zero, near the top and the bottom. */
						for (unsigned lane = 0; lane < 8; ++lane) {
							const std::int32_t value = acc_mode == 0 ? 0
								: acc_mode == 1 ? 0x7ffffff0
								: static_cast<std::int32_t>(0x80000010U);
							std::memcpy(&in.accumulator[lane * 4], &value, 4);
						}
						std::array<std::uint8_t, 32> got{};
						const bool finished = run(host, op, wide, in, got);
						const auto want = reference(op, in, wide ? 32 : 16);
						++cases;
						if (!finished || got != want) {
							if (bad++ < 3)
								std::printf("FAIL %s %s %s first %d second %d acc %d%s\n",
									label, name(op), wide ? "ymm" : "xmm",
									first_mode, second_mode, acc_mode,
									finished ? "" : " (did not finish)");
							++failures;
						}
					}
	std::printf("%s %s: %u cases, %u wrong\n", bad == 0 ? "ok" : "FAIL", label, cases, bad);
}

} // namespace

int main()
{
	const auto probed = uml::platform::apple::probed_host_capabilities();
	if (!x86_64::jit::fex_engine_available(probed)) {
		std::puts("SKIP the x86-64 JIT cannot run on this host");
		return 0;
	}
	/* The host as probed: the lowering the product uses. */
	check_profile("host profile", probed);
	/* Neither DotProd nor I8MM: the long NEON sequence, on any host. */
	auto software = probed;
	software.supported.reset(static_cast<std::size_t>(ArmFeature::dotprod));
	software.supported.reset(static_cast<std::size_t>(ArmFeature::i8mm));
	check_profile("no DotProd, no I8MM", software);
	/* DotProd without I8MM: the three-SDOT lowering, where the host has DotProd. */
	if (probed.has(ArmFeature::dotprod)) {
		auto dot = probed;
		dot.supported.reset(static_cast<std::size_t>(ArmFeature::i8mm));
		check_profile("DotProd, no I8MM", dot);
	}
	std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
