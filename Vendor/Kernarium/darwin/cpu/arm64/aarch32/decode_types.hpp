// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <bit>
#include <cstdint>

namespace uml::cpu::arm64::aarch32 {

/* The AArch32 instruction sets the decoder reads. A T16 word holds the one
 * halfword in its upper half (hw1 << 16); a T32 word is hw1 << 16 | hw2. */
enum class Isa : std::uint8_t { a32, t16, t32 };

/* The part of the profile a form belongs to: the integer core, VFPv4-D32 and
 * Advanced SIMD, or the Armv8 additions. A profile has all three. */
enum class Level : std::uint8_t { base, simd, complete };

/* The optional extensions a profile has, each where the host has the A64
 * feature the engines lower it onto. The order is the generator's
 * (a32_profile_policy.FEATURE_ORDER), which the generated table asserts. */
enum class Feature : std::uint8_t {
	fp16,
	aes,
	pmull,
	sha1,
	sha2,
	crc32,
	dotprod,
	fhm,
	bf16,
	i8mm,
	sb,
	rdm,
	fcma,
	jscvt,
};

class FeatureSet final {
public:
	constexpr FeatureSet() noexcept = default;

	[[nodiscard]] static constexpr FeatureSet all() noexcept
	{
		return FeatureSet{(std::uint32_t{1} << 14) - 1};
	}

	[[nodiscard]] constexpr bool has(Feature feature) const noexcept
	{
		return (bits_ >> static_cast<unsigned>(feature) & 1U) != 0;
	}

	[[nodiscard]] constexpr FeatureSet with(Feature feature) const noexcept
	{
		return FeatureSet{bits_ | std::uint32_t{1} << static_cast<unsigned>(feature)};
	}

	[[nodiscard]] constexpr FeatureSet without(Feature feature) const noexcept
	{
		return FeatureSet{bits_ & ~(std::uint32_t{1} << static_cast<unsigned>(feature))};
	}

	[[nodiscard]] constexpr bool operator==(const FeatureSet &) const noexcept = default;

private:
	explicit constexpr FeatureSet(std::uint32_t bits) noexcept : bits_(bits) {}

	std::uint32_t bits_ = 0;
};

/* The profile a decode is made against: the optional features the host has.
 * The default has all of them. */
struct Profile final {
	FeatureSet features = FeatureSet::all();
};

/* ITSTATE: the condition and mask of the IT block in progress. The value is
 * the architectural ITSTATE<7:0>; zero is outside a block. */
class ItState final {
public:
	constexpr ItState() noexcept = default;
	explicit constexpr ItState(std::uint8_t bits) noexcept : bits_(bits) {}

	/* The state an IT instruction with these operands starts. */
	[[nodiscard]] static constexpr ItState from_it(unsigned firstcond, unsigned mask) noexcept
	{
		return ItState{static_cast<std::uint8_t>(firstcond << 4 | mask)};
	}

	[[nodiscard]] constexpr std::uint8_t bits() const noexcept { return bits_; }
	[[nodiscard]] constexpr bool active() const noexcept { return (bits_ & 0xFU) != 0; }
	[[nodiscard]] constexpr bool last() const noexcept { return (bits_ & 0xFU) == 8; }
	/* The condition the next instruction of the block executes under. */
	[[nodiscard]] constexpr unsigned cond() const noexcept { return bits_ >> 4; }
	/* Instructions the block still holds, the next one included. */
	[[nodiscard]] constexpr unsigned remaining() const noexcept
	{
		return active() ? 4U - static_cast<unsigned>(std::countr_zero(
			static_cast<unsigned>(bits_ & 0xFU))) : 0U;
	}
	/* ITAdvance: the state after an instruction of the block. */
	[[nodiscard]] constexpr ItState advance() const noexcept
	{
		if ((bits_ & 7U) == 0)
			return ItState{};
		return ItState{static_cast<std::uint8_t>(
			(bits_ & 0xE0U) | ((bits_ << 1) & 0x1FU))};
	}

	[[nodiscard]] constexpr bool operator==(const ItState &) const noexcept = default;

private:
	std::uint8_t bits_ = 0;
};

enum class Outcome : std::uint8_t {
	ok,
	undefined,
	unpredictable,
	nop,
	/* The pseudocode requires a feature the profile lacks. */
	feature,
	/* The pseudocode defers to another encoding. A word the index
	 * allocates never reaches it; decode.cpp treats it as undefined. */
	see,
};

/* What a Decode pseudocode reads besides the word. */
struct Context final {
	ItState it;
	bool thumb = false;
	FeatureSet features;
};

} // namespace uml::cpu::arm64::aarch32
