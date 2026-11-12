// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jit_test = uml::cpu::arm64::jit::testing;

extern "C" {
void uml_arm64_gadget_sysreg_cntfrq_read();
void uml_arm64_gadget_sysreg_cntvct_read();
void uml_arm64_gadget_sysreg_fpsr_read();
}

namespace {

constexpr std::uint64_t code_base = 0x4fe0'0000ULL;
constexpr std::uint32_t cntfrq = 0xd53b'e000U;
constexpr std::uint32_t cntvct = 0xd53b'e040U;

using uml::testing::require;

[[nodiscard]] constexpr bool is_counter_read(
	std::uint32_t word, std::uint32_t system) noexcept
{
	return (word & 0xffff'ffe0U) == system;
}

[[nodiscard]] constexpr bool is_call(std::uint32_t word) noexcept
{
	const bool immediate = (word & 0xfc00'0000U) == 0x9400'0000U;
	const bool register_call =
		(word & 0xffff'fc1fU) == 0xd63f'0000U;
	return immediate || register_call;
}

[[nodiscard]] constexpr bool is_cached_host_gpr(unsigned reg) noexcept
{
	return (reg >= 3U && reg <= 7U) || reg == 12U || reg == 13U ||
		reg == 17U;
}

template <typename First, typename Last>
[[nodiscard]] std::span<const std::uint32_t>
gadget_words(First first, Last last)
{
	const auto begin = reinterpret_cast<std::uintptr_t>(first);
	const auto end = reinterpret_cast<std::uintptr_t>(last);
	require(end > begin, "counter gadget link order changed");
	require((end - begin) % sizeof(std::uint32_t) == 0,
		"counter gadget is not word aligned");
	require(end - begin <= 128U, "counter gadget unexpectedly expanded");
	return {
		reinterpret_cast<const std::uint32_t *>(begin),
		(end - begin) / sizeof(std::uint32_t),
	};
}

void gadget_counter_reads_are_direct()
{
	const auto frequency = gadget_words(
		uml_arm64_gadget_sysreg_cntfrq_read,
		uml_arm64_gadget_sysreg_cntvct_read);
	const auto counter = gadget_words(
		uml_arm64_gadget_sysreg_cntvct_read,
		uml_arm64_gadget_sysreg_fpsr_read);

	require(std::ranges::count_if(frequency, [](std::uint32_t word) {
			return is_counter_read(word, cntfrq);
		}) == 1,
		"CNTFRQ gadget does not contain exactly one native MRS");
	require(std::ranges::count_if(counter, [](std::uint32_t word) {
			return is_counter_read(word, cntvct);
		}) == 1,
		"CNTVCT gadget does not contain exactly one native MRS");
	require(std::ranges::none_of(frequency, is_call),
		"CNTFRQ gadget still calls a helper");
	require(std::ranges::none_of(counter, is_call),
		"CNTVCT gadget still calls a helper");
}

[[nodiscard]] constexpr std::uint32_t add_immediate(
	unsigned destination, unsigned source, unsigned immediate) noexcept
{
	return 0x9100'0000U | ((immediate & 0xfffU) << 10U) |
		((source & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] std::span<const std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled, std::size_t guest_count)
{
	require(compiled.guest_instructions == guest_count,
		"JIT did not compile the complete counter block");
	require(compiled.hot_words <= compiled.words.size(),
		"JIT hot-word boundary exceeds generated counter code");
	return std::span<const std::uint32_t>{compiled.words}.first(
		compiled.hot_words);
}

void jit_counter_reads_are_one_to_one_and_resident()
{
	constexpr unsigned frequency_guest = 18;
	constexpr unsigned counter_guest = 17;
	constexpr std::array guest{
		cntfrq | frequency_guest,
		add_immediate(5, frequency_guest, 1),
		cntvct | counter_guest,
		add_immediate(6, counter_guest, 1),
	};
	const auto compiled = jit_test::compile_block(code_base, guest);
	const auto hot = hot_code(compiled, guest.size());

	require(std::ranges::count_if(hot, [](std::uint32_t word) {
			return is_counter_read(word, cntfrq);
		}) == 1,
		"JIT did not emit exactly one CNTFRQ MRS");
	require(std::ranges::count_if(hot, [](std::uint32_t word) {
			return is_counter_read(word, cntvct);
		}) == 1,
		"JIT did not emit exactly one CNTVCT MRS");
	require(std::ranges::none_of(hot, is_call),
		"JIT counter hot path still calls a helper");

	constexpr std::uint32_t add_register_mask = (31U << 5U) | 31U;
	bool found = false;
	for (std::size_t index = 0; index + guest.size() <= hot.size(); ++index) {
		const auto candidate = hot.subspan(index, guest.size());
		if (!is_counter_read(candidate[0], cntfrq) ||
		    (candidate[1] & ~add_register_mask) !=
			    (guest[1] & ~add_register_mask) ||
		    !is_counter_read(candidate[2], cntvct) ||
		    (candidate[3] & ~add_register_mask) !=
			    (guest[3] & ~add_register_mask))
			continue;

		const unsigned frequency_host = candidate[0] & 31U;
		const unsigned counter_host = candidate[2] & 31U;
		require(frequency_host != frequency_guest,
			"CNTFRQ destination was not remapped");
		require(counter_host != counter_guest,
			"CNTVCT destination was not remapped");
		require(is_cached_host_gpr(frequency_host) &&
			    is_cached_host_gpr(counter_host),
			"counter result escaped the resident host-register bank");
		require(((candidate[1] >> 5U) & 31U) == frequency_host,
			"CNTFRQ result was staged before its consumer");
		require(((candidate[3] >> 5U) & 31U) == counter_host,
			"CNTVCT result was staged before its consumer");
		require(!found, "JIT emitted the counter sequence more than once");
		found = true;
	}
	require(found,
		"JIT counter reads are not consecutive one-to-one native operations");
}

void discarded_counter_reads_emit_no_native_work()
{
	constexpr std::array guest{
		cntfrq | 31U,
		cntvct | 31U,
	};
	const auto compiled =
		jit_test::compile_block(code_base + 0x1000U, guest);
	const auto hot = hot_code(compiled, guest.size());
	require(std::ranges::none_of(hot, [](std::uint32_t word) {
			return is_counter_read(word, cntfrq) ||
				is_counter_read(word, cntvct);
		}),
		"discarded counter read emitted a native MRS");
	require(std::ranges::none_of(hot, is_call),
		"discarded counter read emitted a helper call");
}

} // namespace

int main()
{
	gadget_counter_reads_are_direct();
	jit_counter_reads_are_one_to_one_and_resident();
	discarded_counter_reads_emit_no_native_work();
	return 0;
}
