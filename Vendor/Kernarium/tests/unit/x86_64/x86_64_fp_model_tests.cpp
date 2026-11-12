/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The model answers some questions in integers where SoftFloat is the
 * reference: whether a square root is exact, and whether an integer fits the
 * float it converts to. This holds both to SoftFloat's own flags. A square
 * root is exact when the odd part of the significand is a perfect square and
 * the exponent is even, which depends on nothing else, so every single
 * significand is checked with an even and an odd exponent, the other
 * exponents by a stride, and doubles by the squares of integers and their
 * neighbours, and random significands. A DPPS or DPPD adds each distinct sum
 * once, and this holds its results and flags to the sums of every lane, one
 * operation at a time. */
#include "cpu/x86_64/fp_model.hpp"
#include "require.hpp"

extern "C" {
#include <SoftFloat-3e/softfloat.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace uml::cpu::x86_64;
using uml::testing::require;

constexpr std::uint32_t precision = 0x20U;

/* The flags the model raises for one lane of `kind`, and what it raises
 * before the computation. */
struct Raised final {
	std::uint32_t before{};
	std::uint32_t after{};
};

[[nodiscard]] Raised raise(const FpOperation &operation,
	std::uint64_t lane)
{
	FpValues values{};
	std::memcpy(values.sources[0].data(), &lane, sizeof lane);
	const auto steps = x86_fp_compute(operation, 0x1f80U, values);
	return {steps.before[0], steps.after[0]};
}

/* SoftFloat's verdict on whether the square root of the lane is inexact. */
[[nodiscard]] bool sqrt_inexact(bool dbl, std::uint64_t lane)
{
	softfloat_state state{};
	const auto wide = dbl ?
		extF80_to_f128(&state, f64_to_extF80(&state, float64_t{lane})) :
		f32_to_f128(&state, float32_t{static_cast<std::uint32_t>(lane)});
	softfloat_state root{};
	(void)f128_sqrt(&root, wide);
	return (root.exceptionFlags & softfloat_flag_inexact) != 0;
}

void check_sqrt(bool dbl, std::uint64_t lane, std::atomic<std::uint64_t> &failed)
{
	const FpOperation operation{FpKind::sqrt, dbl ? 8U : 4U, 1U, 0, 0};
	const auto raised = raise(operation, lane);
	const bool inexact = (raised.after & precision) != 0;
	if (raised.before != 0 || inexact != sqrt_inexact(dbl, lane))
		failed.store(lane | 1ULL << 63U);
}

template <typename Body>
void in_parallel(std::uint64_t count, Body body)
{
	const auto workers = std::clamp<unsigned>(
		std::thread::hardware_concurrency(), 1U, 8U);
	constexpr std::uint64_t block = 1U << 16U;
	std::atomic<std::uint64_t> next{0};
	std::vector<std::thread> threads;
	for (unsigned worker = 0; worker < workers; ++worker)
		threads.emplace_back([&] {
			for (;;) {
				const auto begin = next.fetch_add(block);
				if (begin >= count)
					return;
				for (auto index = begin;
					index < std::min(begin + block, count); ++index)
					body(index);
			}
		});
	for (auto &thread : threads)
		thread.join();
}

void test_single_roots()
{
	std::atomic<std::uint64_t> failed{0};
	/* Every significand with the exponents 127 and 128. */
	in_parallel(2ULL << 23U, [&](std::uint64_t index) {
		const auto exponent = 127U + static_cast<std::uint32_t>(index >> 23U);
		check_sqrt(false, (exponent << 23U) | (index & 0x7fffffU), failed);
	});
	require(failed.load() == 0, "a single square root's precision differs");
	/* Every exponent, a stride of significands. */
	in_parallel(254ULL << 12U, [&](std::uint64_t index) {
		const auto exponent = 1U + static_cast<std::uint32_t>(index >> 12U);
		const auto fraction = static_cast<std::uint32_t>(
			(index & 0xfffU) * 2053U) & 0x7fffffU;
		check_sqrt(false, (exponent << 23U) | fraction, failed);
	});
	require(failed.load() == 0, "a single square root's precision differs "
		"at some exponent");
}

void test_double_roots()
{
	std::atomic<std::uint64_t> failed{0};
	/* The squares of integers and their neighbours, in every exponent
	 * parity, with the trailing zeros a square root leaves. */
	in_parallel(1ULL << 22U, [&](std::uint64_t index) {
		const std::uint64_t root = (index >> 3U) * 61U + 1U;
		for (const std::uint64_t square :
			{root * root, root * root + 1U, root * root - 1U}) {
			if (square == 0 || square >= (1ULL << 53U))
				continue;
			/* Normalise the square to a 53-bit significand. */
			const unsigned top = 63U - static_cast<unsigned>(
				__builtin_clzll(square));
			const auto significand = (square << (52U - top)) &
				((1ULL << 52U) - 1U);
			const std::uint64_t exponent = 1023U + (index & 7U) * 37U + top;
			check_sqrt(true, (exponent << 52U) | significand, failed);
		}
	});
	require(failed.load() == 0, "a double square root's precision differs "
		"for a square");
	std::vector<std::mt19937_64> engines;
	for (unsigned seed = 0; seed < 64; ++seed)
		engines.emplace_back(seed);
	in_parallel(64, [&](std::uint64_t seed) {
		auto &engine = engines[seed];
		for (unsigned round = 0; round < 1U << 17U; ++round) {
			const std::uint64_t exponent = 1U + engine() % 2046U;
			check_sqrt(true, (exponent << 52U) | (engine() >> 12U), failed);
		}
	});
	require(failed.load() == 0, "a double square root's precision differs");
}

/* An integer as a binary128, which holds any 64-bit integer exactly. */
[[nodiscard]] float128_t exact_binary128(std::int64_t integer) noexcept
{
	float128_t value{};
	if (integer == 0)
		return value;
	const bool negative = integer < 0;
	const auto magnitude = negative ? 0 - static_cast<std::uint64_t>(integer) :
		static_cast<std::uint64_t>(integer);
	const unsigned top = 63U - static_cast<unsigned>(__builtin_clzll(magnitude));
	/* The bits below the leading one, as the top of 112 fraction bits. */
	const auto fraction = static_cast<unsigned __int128>(
		magnitude & ~(1ULL << top)) << (112U - top);
	value.v[0] = static_cast<std::uint64_t>(fraction);
	value.v[1] = static_cast<std::uint64_t>(fraction >> 64U) |
		(static_cast<std::uint64_t>(16383U + top) << 48U) |
		(negative ? 1ULL << 63U : 0U);
	return value;
}

/* SoftFloat's verdict on whether `integer` converts inexactly. */
[[nodiscard]] bool conversion_inexact(bool dbl, std::int64_t integer)
{
	softfloat_state state{};
	state.roundingMode = softfloat_round_near_even;
	const auto exact = exact_binary128(integer);
	if (dbl)
		(void)f128_to_f64(&state, exact);
	else
		(void)f128_to_f32(&state, exact);
	return (state.exceptionFlags & softfloat_flag_inexact) != 0;
}

void check_conversion(bool dbl, bool wide, std::int64_t integer,
	std::atomic<std::uint64_t> &failed)
{
	std::uint8_t flags{};
	if (wide)
		flags = fp_from_int64;
	const FpOperation operation{FpKind::from_int, dbl ? 8U : 4U, 1U, 0, flags};
	const auto raised = raise(operation, static_cast<std::uint64_t>(integer));
	if (raised.before != 0 ||
		((raised.after & precision) != 0) != conversion_inexact(dbl, integer))
		failed.store(static_cast<std::uint64_t>(integer) | 1ULL);
}

void test_conversions()
{
	std::atomic<std::uint64_t> failed{0};
	/* Every 32-bit integer of up to 26 bits, and a stride of the rest. */
	in_parallel(1ULL << 27U, [&](std::uint64_t index) {
		const auto integer = static_cast<std::int32_t>(index) -
			(1 << 26);
		check_conversion(false, false, integer, failed);
		check_conversion(true, false, integer, failed);
	});
	in_parallel(1ULL << 22U, [&](std::uint64_t index) {
		const auto integer = static_cast<std::int32_t>(
			static_cast<std::uint32_t>(index * 1021U));
		check_conversion(false, false, integer, failed);
		check_conversion(true, false, integer, failed);
	});
	require(failed.load() == 0, "a 32-bit conversion's precision differs");
	/* 64-bit integers around every power of two, with trailing zeros of
	 * every length, and random ones. */
	in_parallel(64ULL * 64U * 16U, [&](std::uint64_t index) {
		const unsigned top = static_cast<unsigned>(index >> 10U);
		const unsigned zeros = static_cast<unsigned>((index >> 4U) & 63U);
		const auto low = static_cast<std::uint64_t>(index & 15U);
		if (zeros > top)
			return;
		const std::uint64_t base = (1ULL << top) | (low << zeros);
		for (const auto sign : {1, -1}) {
			const auto integer = static_cast<std::int64_t>(
				sign < 0 ? 0 - base : base);
			check_conversion(false, true, integer, failed);
			check_conversion(true, true, integer, failed);
		}
	});
	std::mt19937_64 engine{11};
	for (unsigned round = 0; round < 1U << 20U; ++round) {
		const auto integer = static_cast<std::int64_t>(
			engine() >> (engine() % 64U) << (engine() % 40U));
		check_conversion(false, true, integer, failed);
		check_conversion(true, true, integer, failed);
	}
	require(failed.load() == 0, "a 64-bit conversion's precision differs");
}

/* A lane of a kind of one operation: its bits, and the flags it raises. */
struct Lane final {
	std::uint64_t bits{};
	std::uint32_t before{};
	std::uint32_t after{};
};

[[nodiscard]] Lane one_operation(FpKind kind, bool dbl, std::uint64_t a,
	std::uint64_t b, std::uint32_t mxcsr)
{
	FpValues values{};
	std::memcpy(values.sources[0].data(), &a, dbl ? 8U : 4U);
	std::memcpy(values.sources[1].data(), &b, dbl ? 8U : 4U);
	const FpOperation operation{kind, dbl ? 8U : 4U, 1U, 0, 0};
	const auto steps = x86_fp_compute(operation, mxcsr, values);
	Lane lane{0, steps.before[0], steps.after[0]};
	std::memcpy(&lane.bits, values.result.data(), dbl ? 8U : 4U);
	return lane;
}

/* DPPS or DPPD one operation at a time, in x86's order: the selected
 * products, the sums of pairs in each lane's order, and for DPPS the sums of
 * those. */
struct DotProduct final {
	std::array<std::uint64_t, 4> lanes{};
	FpSteps steps{};
};

[[nodiscard]] DotProduct reference_dot_product(bool dbl, std::uint8_t control,
	const FpValues &values, std::uint32_t mxcsr)
{
	DotProduct result{};
	const std::size_t count = dbl ? 2U : 4U;
	const auto lane = [&](const FpVector &vector, std::size_t index) {
		std::uint64_t bits{};
		std::memcpy(&bits, vector.data() + index * (dbl ? 8U : 4U),
			dbl ? 8U : 4U);
		return bits;
	};
	const auto operate = [&](FpKind kind, std::uint64_t a, std::uint64_t b,
		std::size_t step) {
		const auto done = one_operation(kind, dbl, a, b, mxcsr);
		result.steps.before[step] |= done.before;
		result.steps.after[step] |= done.after;
		return done.bits;
	};
	std::array<std::uint64_t, 4> products{};
	for (std::size_t index = 0; index < count; ++index)
		if (((control >> (4U + index)) & 1U) != 0)
			products[index] = operate(FpKind::mul,
				lane(values.sources[0], index),
				lane(values.sources[1], index), 0);
	std::array<std::uint64_t, 4> sums{};
	if (dbl) {
		for (std::size_t index = 0; index < 2; ++index)
			sums[index] = operate(FpKind::add, products[index],
				products[index ^ 1U], 1);
	} else {
		std::array<std::uint64_t, 4> pairs{};
		for (std::size_t index = 0; index < 4; ++index)
			pairs[index] = operate(FpKind::add, products[index ^ 1U],
				products[index], 1);
		for (std::size_t index = 0; index < 4; ++index)
			sums[index] = operate(FpKind::add, pairs[index],
				pairs[index ^ 2U], 2);
	}
	for (std::size_t index = 0; index < count; ++index)
		result.lanes[index] = ((control >> index) & 1U) != 0 ? sums[index] : 0U;
	return result;
}

/* A lane that reaches the corners of DPPS: NaNs of either kind, infinities,
 * zeros, denormals, and magnitudes that overflow or underflow in a product
 * or a sum. */
[[nodiscard]] std::uint64_t corner_lane(bool dbl, std::mt19937_64 &engine)
{
	const unsigned fraction_bits = dbl ? 52U : 23U;
	const std::uint64_t exponent_max = dbl ? 0x7ffU : 0xffU;
	const std::uint64_t bias = dbl ? 1023U : 127U;
	const std::uint64_t sign = (engine() & 1U) << (dbl ? 63U : 31U);
	const std::uint64_t fraction = engine() & ((1ULL << fraction_bits) - 1U);
	const std::uint64_t quiet = 1ULL << (fraction_bits - 1U);
	std::uint64_t exponent{};
	switch (engine() % 10U) {
	case 0: return engine() & (dbl ? ~0ULL : 0xffffffffULL);
	case 1: return sign;
	case 2: return sign | (exponent_max << fraction_bits);
	case 3: return sign | (exponent_max << fraction_bits) | quiet | (fraction & (quiet - 1U));
	case 4: return sign | (exponent_max << fraction_bits) | ((fraction & (quiet - 1U)) | 1U);
	case 5: return sign | fraction;
	case 6: exponent = bias - 3U + engine() % 7U; break;
	case 7: exponent = exponent_max - 1U - engine() % (bias / 2U); break;
	case 8: exponent = 1U + engine() % (bias / 2U); break;
	default: exponent = bias - 30U + engine() % 60U; break;
	}
	return sign | (exponent << fraction_bits) | fraction;
}

void test_dot_products()
{
	std::atomic<std::uint64_t> failed{0};
	/* MXCSR with every exception masked, and with DAZ, FTZ and each
	 * rounding control. */
	const std::uint32_t masks[]{0x1f80U, 0x1f80U | 0x40U, 0x1f80U | 0x8000U,
		0x1f80U | 0x8040U | 0x2000U, 0x1f80U | 0x6000U, 0x1f80U | 0x4000U,
		0x1f80U | 0x2000U | 0x8000U};
	std::vector<std::mt19937_64> engines;
	for (unsigned seed = 0; seed < 64; ++seed)
		engines.emplace_back(100U + seed);
	in_parallel(64, [&](std::uint64_t seed) {
		auto &engine = engines[seed];
		for (unsigned round = 0; round < 1U << 15U; ++round) {
			const bool dbl = (engine() & 1U) != 0;
			const std::size_t count = dbl ? 2U : 4U;
			const std::size_t bytes = dbl ? 8U : 4U;
			FpValues values{};
			for (auto &vector : values.sources)
				for (std::size_t index = 0; index < count; ++index) {
					const auto bits = corner_lane(dbl, engine);
					std::memcpy(vector.data() + index * bytes, &bits, bytes);
				}
			const auto control = static_cast<std::uint8_t>(engine());
			const auto mxcsr = masks[engine() % std::size(masks)];
			const auto expected = reference_dot_product(dbl, control, values,
				mxcsr);
			auto computed = values;
			const FpOperation operation{FpKind::dpp, dbl ? 8U : 4U,
				dbl ? 2U : 4U, control, 0};
			const auto steps = x86_fp_compute(operation, mxcsr, computed);
			bool same = true;
			for (std::size_t step = 0; step < FpSteps::count; ++step)
				same = same && steps.before[step] == expected.steps.before[step] &&
					steps.after[step] == expected.steps.after[step];
			for (std::size_t index = 0; index < count; ++index) {
				std::uint64_t bits{};
				std::memcpy(&bits, computed.result.data() + index * bytes, bytes);
				same = same && bits == expected.lanes[index];
			}
			if (!same)
				failed.store(1U + seed);
		}
	});
	require(failed.load() == 0, "a dot product differs from its sums one at "
		"a time");
}

} // namespace

int main()
{
	test_single_roots();
	test_double_roots();
	test_conversions();
	test_dot_products();
	return 0;
}
