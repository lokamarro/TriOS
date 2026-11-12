/* SPDX-License-Identifier: GPL-2.0-or-later */
/* x86 SIMD floating-point exception flags while MXCSR masks every exception.
 * Each case is one register-form instruction from XE2's request sets, with
 * the MXCSR flags an Intel Xeon 8581C left after it
 * (the generated `xeon-flags.bin`, chosen by
 * tools/x86-tests/masked-flags-subset.py). The case runs through the engine's
 * ordinary path, unchecked, in the guest loop that loads MXCSR before the
 * instruction and stores it after, and the six flags it leaves must be the
 * Xeon's. The documented inexact cases are named in `residuals`, and only
 * those may differ. `--checked` runs the gadget engine's checked path instead,
 * which corrects the three of them: every instruction runs alone with its
 * exceptions checked while MXCSR masks them all, and its flags must be the
 * Xeon's, with no residual and no fault. It then runs each case again with
 * invalid operation, a denormal operand, division by zero or precision
 * unmasked, alone and in pairs, where the Xeon's flags decide the fault: x86
 * faults with #XM when an unmasked exception was raised, before computing
 * for the first three, which leaves only their flags, and after it for
 * precision. `--show N` prints up to N unexplained cases. The same
 * executable runs XE2's whole request sets from a file the selection tool
 * writes with `--all`. */
#include "x86_64_sse_loop.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace uml::test::x86_64_sse_loop;

constexpr std::uint8_t flag_bits = 0x3fU;
/* Set by the selection tool from the Xeon's result; see `residuals`. */
constexpr std::uint8_t rounded_to_normal_mark = 0x40U;
constexpr std::uint8_t quiet_nan_fma_mark = 0x80U;
constexpr std::uint8_t invalid_operation = 0x01U;
constexpr std::uint8_t denormal_operand = 0x02U;
constexpr std::uint8_t underflow = 0x10U;
constexpr std::uint32_t denormals_are_zero = 0x40U;
#if !UML_X86_64_SSE_LOOP_JIT
/* The flags x86 checks before it computes: invalid operation, denormal
 * operand and division by zero. */
constexpr std::uint8_t precomputation_flags = 0x07U;
#endif

struct Instruction final {
	std::string name;
	std::vector<std::uint8_t> bytes;
	std::size_t kept{};
};

struct Case final {
	std::uint16_t instruction{};
	std::uint16_t mxcsr{};
	std::uint8_t flags{};
	/* The MXCSR flags a checked run unmasks. */
	std::uint8_t unmask{};
	std::array<std::byte, input_stride> input{};
};

/* What x86 does and the engine cannot, by design: each may explain one
 * flag of a case. */
struct Residual final {
	const char *description;
	bool (*explains)(const Case &, std::uint8_t flag, bool spurious);
};

constexpr std::array<Residual, 3> residuals{{
	{"DE while MXCSR.DAZ is clear: no host flag shows a denormal operand, "
		"and a software test costs the common path (user decision "
		"2026-10-07)",
		[](const Case &c, std::uint8_t flag, bool spurious) {
			return flag == denormal_operand && !spurious &&
				(c.mxcsr & denormals_are_zero) == 0;
		}},
	{"UE for a result x86 rounds up to the smallest normal: the host "
		"detects tininess before rounding",
		[](const Case &c, std::uint8_t flag, bool spurious) {
			return flag == underflow && spurious &&
				(c.flags & rounded_to_normal_mark) != 0;
		}},
	{"IE for an FMA of infinity times zero plus a quiet NaN, which the "
		"host raises and x86 does not",
		[](const Case &c, std::uint8_t flag, bool spurious) {
			return flag == invalid_operation && spurious &&
				(c.flags & quiet_nan_fma_mark) != 0;
		}},
}};

class Reader final {
public:
	explicit Reader(std::vector<char> bytes) : bytes_(std::move(bytes)) {}

	template<typename T>
	[[nodiscard]] bool read(T &value) noexcept
	{
		if (bytes_.size() - offset_ < sizeof(T))
			return false;
		std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
		offset_ += sizeof(T);
		return true;
	}

	[[nodiscard]] bool read(void *destination, std::size_t size) noexcept
	{
		if (bytes_.size() - offset_ < size)
			return false;
		std::memcpy(destination, bytes_.data() + offset_, size);
		offset_ += size;
		return true;
	}

	[[nodiscard]] bool done() const noexcept
	{
		return offset_ == bytes_.size();
	}

private:
	std::vector<char> bytes_;
	std::size_t offset_{};
};

[[nodiscard]] bool load(const char *path, std::vector<Instruction> &instructions,
	std::vector<Case> &cases)
{
	std::ifstream file{path, std::ios::binary};
	if (!file)
		return false;
	Reader reader{std::vector<char>{std::istreambuf_iterator<char>{file},
		std::istreambuf_iterator<char>{}}};
	std::array<char, 4> magic{};
	std::uint32_t count{};
	if (!reader.read(magic.data(), magic.size()) ||
		std::string_view{magic.data(), magic.size()} != "MXF1" ||
		!reader.read(count))
		return false;
	instructions.resize(count);
	for (auto &instruction : instructions) {
		std::uint8_t length{};
		if (!reader.read(length))
			return false;
		instruction.name.resize(length);
		if (!reader.read(instruction.name.data(), length) ||
			!reader.read(length) || length == 0 || length > 15)
			return false;
		instruction.bytes.resize(length);
		std::uint8_t kept{};
		if (!reader.read(instruction.bytes.data(), length) ||
			!reader.read(kept) || (kept != 16 && kept != 32))
			return false;
		instruction.kept = kept;
	}
	if (!reader.read(count))
		return false;
	cases.resize(count);
	for (auto &c : cases) {
		if (!reader.read(c.instruction) || !reader.read(c.mxcsr) ||
			!reader.read(c.flags) || c.instruction >= instructions.size())
			return false;
		const auto kept = instructions[c.instruction].kept;
		for (std::size_t source = 0; source < 3; ++source)
			if (!reader.read(c.input.data() + source * 32, kept))
				return false;
	}
	return reader.done();
}

[[nodiscard]] std::string flag_name(std::uint8_t flag)
{
	static constexpr std::array<const char *, 6> names{
		"IE", "DE", "ZE", "OE", "UE", "PE"};
	for (std::size_t bit = 0; bit < names.size(); ++bit)
		if (flag == (1U << bit))
			return names[bit];
	return "?";
}

} // namespace

int main(int argc, char **argv)
{
	std::size_t show = 20;
	bool checked = false;
	const char *path = nullptr;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument{argv[index]};
		if (argument == "--show" && index + 1 < argc)
			show = std::strtoull(argv[++index], nullptr, 10);
#if !UML_X86_64_SSE_LOOP_JIT
		else if (argument == "--checked")
			checked = true;
#endif
		else if (path == nullptr && !argument.starts_with("--"))
			path = argv[index];
		else {
#if UML_X86_64_SSE_LOOP_JIT
			std::fprintf(stderr, "usage: %s [--show N] DATA\n", argv[0]);
#else
			std::fprintf(stderr, "usage: %s [--show N] [--checked] DATA\n",
				argv[0]);
#endif
			return 2;
		}
	}
	std::vector<Instruction> instructions;
	std::vector<Case> cases;
	if (path == nullptr || !load(path, instructions, cases)) {
		std::fprintf(stderr, "x86-64 masked flags: cannot read %s\n",
			path == nullptr ? "(no data)" : path);
		return 2;
	}
#if !UML_X86_64_SSE_LOOP_JIT
	if (checked) {
		/* The exceptions whose faults the Xeon's flags decide. Underflow
		 * and overflow are not here: unmasked, they change which
		 * results are tiny or overflow, and so what the Xeon raises. */
		static constexpr std::array<std::uint8_t, 7> unmasks{
			0x00U, 0x01U, 0x02U, 0x04U, 0x20U, 0x21U, 0x27U};
		std::vector<Case> expanded;
		expanded.reserve(cases.size() * unmasks.size());
		for (const auto &c : cases)
			for (const auto unmask : unmasks) {
				expanded.push_back(c);
				expanded.back().unmask = unmask;
				expanded.back().mxcsr = static_cast<std::uint16_t>(
					c.mxcsr & ~(std::uint32_t{unmask} << 7U));
			}
		cases = std::move(expanded);
	}
#endif
	std::vector<std::size_t> order(cases.size());
	for (std::size_t index = 0; index < order.size(); ++index)
		order[index] = index;
	std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
		return std::tie(cases[left].instruction, cases[left].mxcsr) <
			std::tie(cases[right].instruction, cases[right].mxcsr);
	});

	std::array<std::uint64_t, residuals.size()> explained{};
	std::uint64_t unexplained{};
	std::uint64_t matched{};
	std::map<std::tuple<std::string, std::string, bool>, std::uint64_t> tally;
	std::size_t shown{};
	bool failed = false;
	FlatMemory memory{memory_size};
#if UML_X86_64_SSE_LOOP_JIT
	auto &executor = jit_engine();
#else
	std::optional<uml::cpu::x86_64::gadgets::Executor> executor;
#endif
	for (std::size_t begin = 0; begin < order.size();) {
		const auto &instruction = instructions[cases[order[begin]].instruction];
		const auto mxcsr = cases[order[begin]].mxcsr;
		std::size_t end = begin;
		while (end < order.size() && end - begin < chunk &&
			cases[order[end]].instruction == cases[order[begin]].instruction &&
			cases[order[end]].mxcsr == mxcsr)
			++end;
		/* Each instruction gets its own code, and the gadget engine a
		 * fresh executor for it; the JIT starts a generation per run. */
		if (begin == 0 || cases[order[begin - 1]].instruction !=
			cases[order[begin]].instruction) {
			const auto code = loop_code(instruction.bytes);
			(void)memory.write(code_address, code);
#if !UML_X86_64_SSE_LOOP_JIT
			executor.emplace();
			if (checked)
				executor->check_exceptions(
					uml::cpu::x86_64::gadgets::check_simd);
			executor->prepare(memory);
#endif
		}
		const std::uint32_t control = mxcsr;
		(void)memory.write(control_address,
			std::as_bytes(std::span{&control, 1}));
		for (std::size_t row = begin; row < end; ++row) {
			std::memcpy(memory.at(input_address + (row - begin) * input_stride),
				cases[order[row]].input.data(), input_stride);
			std::memset(memory.at(output_address +
				(row - begin) * output_stride), 0, output_stride);
		}
#if UML_X86_64_SSE_LOOP_JIT
		const bool ran = run_engine(memory, executor, mxcsr, end - begin);
#else
		std::vector<std::uint8_t> trapped(end - begin);
		const bool ran = run_engine(memory, *executor, mxcsr, end - begin,
			checked, instruction.bytes.size(), &trapped,
			uml::cpu::x86_64::gadgets::check_simd);
#endif
		if (!ran) {
			std::fprintf(stderr, "x86-64 masked flags: %s under MXCSR %#x "
				"did not run to its end\n", instruction.name.c_str(), mxcsr);
			failed = true;
			begin = end;
			continue;
		}
		for (std::size_t row = begin; row < end; ++row) {
			const auto &c = cases[order[row]];
			std::uint32_t stored{};
			std::memcpy(&stored, memory.at(output_address +
				(row - begin) * output_stride + 48), 4);
			const auto actual = static_cast<std::uint8_t>(stored & flag_bits);
			auto expected = static_cast<std::uint8_t>(c.flags & flag_bits);
#if !UML_X86_64_SSE_LOOP_JIT
			const bool fault = (expected & c.unmask) != 0;
			if ((trapped[row - begin] != 0) != fault) {
				++unexplained;
				std::fprintf(stderr, "  %s mxcsr %#06x: %s\n",
					instruction.name.c_str(), c.mxcsr,
					fault ? "did not fault" : "faulted");
				continue;
			}
			/* A fault before the computation leaves only the flags
			 * checked before it. A dot product checks in steps, whose
			 * flags the Xeon's union does not split. */
			if (fault && (expected & c.unmask & precomputation_flags) != 0)
				expected &= precomputation_flags;
			if (fault && instruction.name.find("dpp") != std::string::npos) {
				++matched;
				continue;
			}
#endif
			if (actual == expected) {
				++matched;
				continue;
			}
			bool case_explained = true;
			for (std::uint8_t flag = 1; flag <= 0x20U; flag <<= 1U) {
				if (((actual ^ expected) & flag) == 0)
					continue;
				const bool spurious = (actual & flag) != 0;
				bool flag_explained = false;
				for (std::size_t index = 0;
					!checked && index < residuals.size(); ++index)
					if (residuals[index].explains(c, flag, spurious)) {
						++explained[index];
						flag_explained = true;
						break;
					}
				if (!flag_explained) {
					case_explained = false;
					++tally[{instruction.name, flag_name(flag), spurious}];
				}
			}
			if (case_explained)
				continue;
			++unexplained;
			if (shown < show) {
				++shown;
				std::fprintf(stderr, "  %s mxcsr %#06x: engine %#04x x86 "
					"%#04x", instruction.name.c_str(), c.mxcsr, actual,
					expected);
				for (std::size_t source = 0; source < 3; ++source) {
					std::fprintf(stderr, " | ymm%zu", source);
					for (std::size_t lane = 0; lane < instruction.kept / 8;
						++lane) {
						std::uint64_t value{};
						std::memcpy(&value, c.input.data() + source * 32 +
							lane * 8, 8);
						std::fprintf(stderr, " %016llx",
							static_cast<unsigned long long>(value));
					}
				}
				std::fprintf(stderr, "\n");
			}
		}
		begin = end;
	}
	for (const auto &[key, count] : tally)
		std::printf("unexplained %-24s %s %-8s %llu\n",
			std::get<0>(key).c_str(), std::get<1>(key).c_str(),
			std::get<2>(key) ? "spurious" : "missing",
			static_cast<unsigned long long>(count));
	for (std::size_t index = 0; index < residuals.size(); ++index)
		std::printf("residual %llu: %s\n",
			static_cast<unsigned long long>(explained[index]),
			residuals[index].description);
	std::printf("TOTAL cases %zu matched %llu unexplained %llu\n",
		cases.size(), static_cast<unsigned long long>(matched),
		static_cast<unsigned long long>(unexplained));
	return failed || unexplained != 0 ? 1 : 0;
}
