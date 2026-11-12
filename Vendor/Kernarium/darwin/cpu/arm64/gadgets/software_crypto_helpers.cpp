// SPDX-License-Identifier: GPL-2.0-or-later
#include "../cpu_state.hpp"
#include "software_helpers.hpp"

#include "../detail/instruction_primitives.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace uml::cpu::arm64 {

using gadgets::vector_at;

namespace {

void store_word(VectorRegister &destination, std::uint32_t value)
{
	destination.bytes.fill(std::byte{});
	detail::encode_little_endian(value,
		std::span{destination.bytes}.first(sizeof(value)));
}

[[nodiscard]] std::array<std::uint32_t, 4> load_words(
	const VectorRegister &source)
{
	std::array<std::uint32_t, 4> result{};
	for (std::size_t lane = 0; lane < result.size(); ++lane)
		result[lane] = static_cast<std::uint32_t>(
			detail::decode_little_endian(
				std::span{source.bytes}.subspan(lane * 4, 4)));
	return result;
}

void store_words(VectorRegister &destination,
	const std::array<std::uint32_t, 4> &words)
{
	for (std::size_t lane = 0; lane < words.size(); ++lane)
		detail::encode_little_endian(words[lane],
			std::span{destination.bytes}.subspan(lane * 4, 4));
}

[[nodiscard]] std::array<std::uint64_t, 2> load_doublewords(
	const VectorRegister &source)
{
	return {detail::decode_little_endian(
			std::span{source.bytes}.first<8>()),
		detail::decode_little_endian(
			std::span{source.bytes}.subspan<8, 8>())};
}

void store_doublewords(VectorRegister &destination,
	const std::array<std::uint64_t, 2> &words)
{
	detail::encode_little_endian(words[0],
		std::span{destination.bytes}.first<8>());
	detail::encode_little_endian(words[1],
		std::span{destination.bytes}.subspan<8, 8>());
}

struct CarrylessProduct final {
	std::uint64_t low{};
	std::uint64_t high{};
};

/* Keep the portable size=11 path independent of optional host instructions.
 * Out of line, this body cannot inherit a caller's optional-extension target
 * attributes, so the software helper remains safe on hosts without the
 * optional polynomial-multiply extension.
 */
[[gnu::noinline]]
[[nodiscard]] CarrylessProduct carryless_multiply_64(
	std::uint64_t left, std::uint64_t right) noexcept
{
	CarrylessProduct result{};
	for (unsigned bit = 0; bit < 64; ++bit) {
		if ((right & (std::uint64_t{1} << bit)) == 0)
			continue;
		result.low ^= left << bit;
		if (bit != 0)
			result.high ^= left >> (64U - bit);
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t multiply_finite_byte(
	std::uint8_t left, std::uint8_t right) noexcept
{
	std::uint8_t result = 0;
	for (unsigned bit = 0; bit < 8U; ++bit) {
		if ((right & 1U) != 0)
			result ^= left;
		const bool high = (left & 0x80U) != 0;
		left = static_cast<std::uint8_t>(left << 1U);
		if (high)
			left ^= 0x1bU;
		right = static_cast<std::uint8_t>(right >> 1U);
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t invert_finite_byte(
	std::uint8_t value) noexcept
{
	if (value == 0)
		return 0;
	std::uint8_t result = 1;
	std::uint8_t power = value;
	unsigned exponent = 254U;
	while (exponent != 0) {
		if ((exponent & 1U) != 0)
			result = multiply_finite_byte(result, power);
		power = multiply_finite_byte(power, power);
		exponent >>= 1U;
	}
	return result;
}

[[nodiscard]] constexpr std::uint8_t rotate_byte(
	std::uint8_t value, unsigned amount) noexcept
{
	return static_cast<std::uint8_t>(
		(value << amount) | (value >> (8U - amount)));
}

[[nodiscard]] constexpr std::uint8_t substitute_byte(
	std::uint8_t value, bool inverse) noexcept
{
	if (inverse) {
		const auto affine = static_cast<std::uint8_t>(
			rotate_byte(value, 1U) ^ rotate_byte(value, 3U) ^
			rotate_byte(value, 6U) ^ 0x05U);
		return invert_finite_byte(affine);
	}
	const auto reciprocal = invert_finite_byte(value);
	return static_cast<std::uint8_t>(
		reciprocal ^ rotate_byte(reciprocal, 1U) ^
		rotate_byte(reciprocal, 2U) ^
		rotate_byte(reciprocal, 3U) ^
		rotate_byte(reciprocal, 4U) ^ 0x63U);
}

template<bool inverse>
[[nodiscard]] consteval std::array<std::uint8_t, 256>
make_substitution_table()
{
	std::array<std::uint8_t, 256> result{};
	for (unsigned value = 0; value < result.size(); ++value)
		result[value] = substitute_byte(
			static_cast<std::uint8_t>(value), inverse);
	return result;
}

inline constexpr auto forward_substitution =
	make_substitution_table<false>();
inline constexpr auto inverse_substitution =
	make_substitution_table<true>();

template<bool inverse>
[[nodiscard]] std::array<std::byte, 16> shift_and_substitute(
	const std::array<std::byte, 16> &input)
{
	const auto &table =
		inverse ? inverse_substitution : forward_substitution;
	std::array<std::byte, 16> result{};
	for (unsigned column = 0; column < 4U; ++column)
		for (unsigned row = 0; row < 4U; ++row) {
			const auto source_column = inverse ?
				(column + 4U - row) & 3U :
				(column + row) & 3U;
			result[column * 4U + row] =
				static_cast<std::byte>(table[
					std::to_integer<std::uint8_t>(
						input[source_column * 4U + row])]);
		}
	return result;
}

template<bool inverse>
[[nodiscard]] std::array<std::byte, 16> mix_columns(
	const std::array<std::byte, 16> &input)
{
	std::array<std::byte, 16> result{};
	const std::array<std::uint8_t, 4> first_row =
		inverse ?
			std::array<std::uint8_t, 4>{14, 11, 13, 9} :
			std::array<std::uint8_t, 4>{2, 3, 1, 1};
	for (unsigned column = 0; column < 4U; ++column)
		for (unsigned row = 0; row < 4U; ++row) {
			std::uint8_t value = 0;
			for (unsigned source_row = 0; source_row < 4U;
			     ++source_row)
				value ^= multiply_finite_byte(
					std::to_integer<std::uint8_t>(
						input[column * 4U + source_row]),
					first_row[
						(source_row + 4U - row) & 3U]);
			result[column * 4U + row] =
				static_cast<std::byte>(value);
		}
	return result;
}

template<unsigned operation>
void execute_aes_software(VectorRegister &destination,
	const VectorRegister &source)
{
	static_assert(operation >= 4U && operation <= 7U);
	const auto old_destination = destination.bytes;
	const auto input = source.bytes;
	if constexpr (operation <= 5U) {
		std::array<std::byte, 16> combined{};
		for (std::size_t byte = 0; byte < combined.size(); ++byte)
			combined[byte] = old_destination[byte] ^ input[byte];
		destination.bytes =
			shift_and_substitute<operation == 5U>(combined);
	} else {
		destination.bytes = mix_columns<operation == 7U>(input);
	}
}

[[nodiscard]] constexpr std::uint32_t sha_choose(
	std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
	return ((y ^ z) & x) ^ z;
}

[[nodiscard]] constexpr std::uint32_t sha_majority(
	std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
	return (x & y) | ((x | y) & z);
}

[[nodiscard]] std::array<std::uint32_t, 4> sha1_hash(
	std::array<std::uint32_t, 4> x, std::uint32_t y,
	const std::array<std::uint32_t, 4> &words, unsigned operation)
{
	for (std::size_t round = 0; round < 4; ++round) {
		const std::uint32_t function = operation == 0U ?
			sha_choose(x[1], x[2], x[3]) : operation == 1U ?
			(x[1] ^ x[2] ^ x[3]) : sha_majority(x[1], x[2], x[3]);
		const std::uint32_t next = y + std::rotl(x[0], 5) + function +
			words[round];
		x[1] = std::rotl(x[1], 30);
		y = x[3];
		x = {next, x[0], x[1], x[2]};
	}
	return x;
}

[[nodiscard]] std::array<std::uint32_t, 4> sha256_hash(
	std::array<std::uint32_t, 4> x,
	std::array<std::uint32_t, 4> y,
	const std::array<std::uint32_t, 4> &words, bool part1)
{
	for (std::size_t round = 0; round < 4; ++round) {
		const auto sigma0 = std::rotr(x[0], 2) ^ std::rotr(x[0], 13) ^
			std::rotr(x[0], 22);
		const auto sigma1 = std::rotr(y[0], 6) ^ std::rotr(y[0], 11) ^
			std::rotr(y[0], 25);
		const std::uint32_t temporary = y[3] + sigma1 +
			sha_choose(y[0], y[1], y[2]) + words[round];
		x[3] += temporary;
		y[3] = temporary + sigma0 + sha_majority(x[0], x[1], x[2]);
		const auto old_x = x;
		const auto old_y = y;
		x = {old_y[3], old_x[0], old_x[1], old_x[2]};
		y = {old_x[3], old_y[0], old_y[1], old_y[2]};
	}
	return part1 ? x : y;
}

} // namespace

template<unsigned operation>
void execute_aes_software_direct(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	execute_aes_software<operation>(vector_at(cpu, destination_offset),
		vector_at(cpu, source_offset));
}

extern "C" void uml_arm64_simd_aese_software(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	execute_aes_software_direct<4>(
		cpu, destination_offset, source_offset);
}

extern "C" void uml_arm64_simd_aesd_software(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	execute_aes_software_direct<5>(
		cpu, destination_offset, source_offset);
}

extern "C" void uml_arm64_simd_aesmc_software(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	execute_aes_software_direct<6>(
		cpu, destination_offset, source_offset);
}

extern "C" void uml_arm64_simd_aesimc_software(
	CpuState *cpu, std::uintptr_t destination_offset,
	std::uintptr_t source_offset) noexcept
{
	execute_aes_software_direct<7>(
		cpu, destination_offset, source_offset);
}

extern "C" [[gnu::noinline]]
void uml_arm64_simd_pmull_software(
	CpuState *cpu, std::uintptr_t operand, unsigned high) noexcept
{
	const auto destination_offset = operand & 0xffffU;
	const auto left_offset = (operand >> 16) & 0xffffU;
	const auto right_offset = (operand >> 32) & 0xffffU;
	const auto lane_offset = high != 0 ? std::size_t{8} : std::size_t{};
	const auto *bytes = reinterpret_cast<const std::byte *>(cpu);
	const auto lhs = detail::decode_little_endian(
		std::span{bytes + left_offset + lane_offset, std::size_t{8}});
	const auto rhs = detail::decode_little_endian(
		std::span{bytes + right_offset + lane_offset, std::size_t{8}});
	const auto product = carryless_multiply_64(lhs, rhs);
	std::array<std::byte, 16> result{};
	detail::encode_little_endian(product.low,
		std::span{result}.first<8>());
	detail::encode_little_endian(product.high,
		std::span{result}.subspan<8, 8>());
	std::memcpy(reinterpret_cast<std::byte *>(cpu) + destination_offset,
		result.data(), result.size());
}

namespace {

template <unsigned operation>
void execute_sha12_three_software(CpuState *cpu,
                                  std::uintptr_t destination_offset,
                                  std::uintptr_t first_offset,
                                  std::uintptr_t second_offset) noexcept {
  auto &destination = vector_at(cpu, destination_offset);
  const auto first = load_words(vector_at(cpu, first_offset));
  const auto second = load_words(vector_at(cpu, second_offset));
  const auto old_destination = load_words(destination);
  std::array<std::uint32_t, 4> result{};
  if constexpr (operation == 0U)
    result = sha1_hash(old_destination, first[0], second, 0);
  else if constexpr (operation == 1U)
    result = sha1_hash(old_destination, first[0], second, 1);
  else if constexpr (operation == 2U)
    result = sha1_hash(old_destination, first[0], second, 2);
  else if constexpr (operation == 3U)
    result = {
        second[0] ^ old_destination[0] ^ old_destination[2],
        second[1] ^ old_destination[1] ^ old_destination[3],
        second[2] ^ old_destination[2] ^ first[0],
        second[3] ^ old_destination[3] ^ first[1],
    };
  else if constexpr (operation == 4U)
    result = sha256_hash(old_destination, first, second, true);
  else if constexpr (operation == 5U)
    result = sha256_hash(first, old_destination, second, false);
  else {
    const std::array temporary{
        first[1],
        first[2],
        first[3],
        second[0],
    };
    for (std::size_t lane = 0; lane < 2; ++lane) {
      const auto value = second[lane + 2];
      result[lane] =
          (std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10)) +
          old_destination[lane] + temporary[lane];
    }
    for (std::size_t lane = 2; lane < 4; ++lane) {
      const auto value = result[lane - 2];
      result[lane] =
          (std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10)) +
          old_destination[lane] + temporary[lane];
    }
  }
  store_words(destination, result);
}

template <unsigned operation>
void execute_sha12_two_software(CpuState *cpu,
                                std::uintptr_t destination_offset,
                                std::uintptr_t source_offset) noexcept {
  auto &destination = vector_at(cpu, destination_offset);
  const auto source = load_words(vector_at(cpu, source_offset));
  if constexpr (operation == 0U) {
    store_word(destination, std::rotl(source[0], 30));
  } else if constexpr (operation == 1U) {
    const auto old_destination = load_words(destination);
    const std::array temporary{
        old_destination[0] ^ source[1],
        old_destination[1] ^ source[2],
        old_destination[2] ^ source[3],
        old_destination[3],
    };
    store_words(destination,
                {
                    std::rotl(temporary[0], 1),
                    std::rotl(temporary[1], 1),
                    std::rotl(temporary[2], 1),
                    std::rotl(temporary[3], 1) ^ std::rotl(temporary[0], 2),
                });
  } else {
    const auto old_destination = load_words(destination);
    const std::array temporary{
        old_destination[1],
        old_destination[2],
        old_destination[3],
        source[0],
    };
    std::array<std::uint32_t, 4> result{};
    for (std::size_t lane = 0; lane < result.size(); ++lane) {
      const auto value = temporary[lane];
      result[lane] =
          old_destination[lane] +
          (std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3));
    }
    store_words(destination, result);
  }
}

template <unsigned operation>
void execute_sha512_three_software(CpuState *cpu,
                                   std::uintptr_t destination_offset,
                                   std::uintptr_t first_offset,
                                   std::uintptr_t second_offset) noexcept {
  auto &destination = vector_at(cpu, destination_offset);
  const auto first = load_doublewords(vector_at(cpu, first_offset));
  const auto second = load_doublewords(vector_at(cpu, second_offset));
  const auto old_destination = load_doublewords(destination);
  std::array<std::uint64_t, 2> result{};
  if constexpr (operation == 0U) {
    const auto sigma1 = [](std::uint64_t value) {
      return std::rotr(value, 14) ^ std::rotr(value, 18) ^ std::rotr(value, 41);
    };
    result[1] = (second[1] & first[0]) ^ (~second[1] & first[1]);
    result[1] += sigma1(second[1]) + old_destination[1];
    const auto temporary = result[1] + second[0];
    result[0] = (temporary & second[1]) ^ (~temporary & first[0]);
    result[0] += sigma1(temporary) + old_destination[0];
  } else if constexpr (operation == 1U) {
    const auto sigma0 = [](std::uint64_t value) {
      return std::rotr(value, 28) ^ std::rotr(value, 34) ^ std::rotr(value, 39);
    };
    result[1] = (first[0] & second[1]) | ((first[0] | second[1]) & second[0]);
    result[1] += sigma0(second[0]) + old_destination[1];
    result[0] = (result[1] & second[0]) | ((result[1] | second[0]) & second[1]);
    result[0] += sigma0(result[1]) + old_destination[0];
  } else {
    for (std::size_t lane = 0; lane < result.size(); ++lane) {
      const auto value = first[lane];
      const auto sigma1 =
          std::rotr(value, 19) ^ std::rotr(value, 61) ^ (value >> 6);
      result[lane] = old_destination[lane] + sigma1 + second[lane];
    }
  }
  store_doublewords(destination, result);
}

void execute_sha512_two_software(CpuState *cpu,
                                 std::uintptr_t destination_offset,
                                 std::uintptr_t source_offset) noexcept {
  auto &destination = vector_at(cpu, destination_offset);
  const auto old_destination = load_doublewords(destination);
  const auto source = load_doublewords(vector_at(cpu, source_offset));
  const auto sigma0 = [](std::uint64_t value) {
    return std::rotr(value, 1) ^ std::rotr(value, 8) ^ (value >> 7);
  };
  store_doublewords(destination,
                    {
                        old_destination[0] + sigma0(old_destination[1]),
                        old_destination[1] + sigma0(source[0]),
                    });
}

} // namespace

#define UML_DEFINE_SHA12_THREE_SOFTWARE(name, operation)                       \
  extern "C" void uml_arm64_simd_##name##_software(                            \
      CpuState *cpu, std::uintptr_t destination_offset,                        \
      std::uintptr_t first_offset, std::uintptr_t second_offset) noexcept {    \
    execute_sha12_three_software<operation>(cpu, destination_offset,           \
                                            first_offset, second_offset);      \
  }
UML_DEFINE_SHA12_THREE_SOFTWARE(sha1c, 0)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha1p, 1)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha1m, 2)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha1su0, 3)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha256h, 4)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha256h2, 5)
UML_DEFINE_SHA12_THREE_SOFTWARE(sha256su1, 6)
#undef UML_DEFINE_SHA12_THREE_SOFTWARE

#define UML_DEFINE_SHA12_TWO_SOFTWARE(name, operation)                         \
  extern "C" void uml_arm64_simd_##name##_software(                            \
      CpuState *cpu, std::uintptr_t destination_offset,                        \
      std::uintptr_t source_offset) noexcept {                                 \
    execute_sha12_two_software<operation>(cpu, destination_offset,             \
                                          source_offset);                      \
  }
UML_DEFINE_SHA12_TWO_SOFTWARE(sha1h, 0)
UML_DEFINE_SHA12_TWO_SOFTWARE(sha1su1, 1)
UML_DEFINE_SHA12_TWO_SOFTWARE(sha256su0, 2)
#undef UML_DEFINE_SHA12_TWO_SOFTWARE

#define UML_DEFINE_SHA512_THREE_SOFTWARE(name, operation)                      \
  extern "C" void uml_arm64_simd_##name##_software(                            \
      CpuState *cpu, std::uintptr_t destination_offset,                        \
      std::uintptr_t first_offset, std::uintptr_t second_offset) noexcept {    \
    execute_sha512_three_software<operation>(cpu, destination_offset,          \
                                             first_offset, second_offset);     \
  }
UML_DEFINE_SHA512_THREE_SOFTWARE(sha512h, 0)
UML_DEFINE_SHA512_THREE_SOFTWARE(sha512h2, 1)
UML_DEFINE_SHA512_THREE_SOFTWARE(sha512su1, 2)
#undef UML_DEFINE_SHA512_THREE_SOFTWARE

extern "C" void
uml_arm64_simd_sha512su0_software(CpuState *cpu,
                                  std::uintptr_t destination_offset,
                                  std::uintptr_t source_offset) noexcept {
  execute_sha512_two_software(cpu, destination_offset, source_offset);
}

} // namespace uml::cpu::arm64
