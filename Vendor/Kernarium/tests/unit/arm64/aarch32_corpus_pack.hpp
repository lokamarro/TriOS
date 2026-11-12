// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The AArch32 instruction corpus pack: its memory map, the reader and the
 * decoded cases. The unit runner (aarch32_corpus_runner.cpp) checks the cases
 * against the decoder; an engine lane includes this header to drive each case
 * through its engine. tests/unit/arm64/aarch32_corpus/README.md owns the
 * format.
 *
 * A case starts from `recipes[case.recipe]` changed by `case.initial` (already
 * applied: `Case::initial` is the complete starting state), with the data
 * region filled by `data_fill(case.fill)` and then `case.data_patches`, the
 * read-only region holding `read_only_fill()`, and the program placed at
 * kCodeBase + `Case::origin`. `Case::final` is the complete state after the
 * program, and `Case::memory_patches` the data bytes it changed. code_image()
 * and data_before() lay a case out that way, and for_each_instruction() walks
 * its program through the decoder.
 */
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace uml::testing::aarch32_corpus {

/* The memory map every case runs in (tools/arm64-tests/a32_oracle.cpp). */
constexpr std::uint32_t kPackVersion = 2; /* 2: a case holds its program origin */
constexpr std::uint32_t kCodeBase = 0x4F000000;
constexpr std::uint32_t kCodeSize = 0x4000;
constexpr std::uint32_t kArmExit = kCodeBase + 0x800;   /* finishes in A32 state */
constexpr std::uint32_t kThumbExit = kCodeBase + 0x900; /* finishes in Thumb state */
/* A stub is `ldr pc, [pc, #-4]` (Thumb: `ldr.w pc, [pc]`, f8df f000) and then one of these
 * words; a program that loads a stub reads them. The stub at the end of a program is the same. */
constexpr std::uint32_t kArmEpilogue = kCodeBase + 0x3F00;
constexpr std::uint32_t kThumbEpilogue = kCodeBase + 0x3F08;
constexpr std::uint32_t kDataBase = 0x50000000;
constexpr std::uint32_t kDataSize = 0x8000;
constexpr std::uint32_t kReadOnlyBase = 0x50008000;
constexpr std::uint32_t kReadOnlySize = 0x4000;
constexpr unsigned kProfileBit = 2U; /* the 4 KiB profile */
constexpr unsigned kMaxHalfwords = 0x380;
constexpr unsigned kRegisters = 15;
constexpr unsigned kVectors = 32;

constexpr std::uint8_t kFlagThumb = 1;
constexpr std::uint8_t kFlagStraight = 2;
constexpr std::uint8_t kFlagExitThumb = 8;

constexpr unsigned kSigIll = 4;
constexpr unsigned kSigTrap = 5;
constexpr unsigned kSigBus = 7;
constexpr unsigned kSigFpe = 8;
constexpr unsigned kSigSegv = 11;
constexpr unsigned kEventSvc = 255;

/* Reading ---------------------------------------------------------------------- */

class Reader final {
public:
	explicit Reader(const std::vector<std::uint8_t> &data, std::size_t offset = 0)
		: data_(data), offset_(offset)
	{
	}

	template<typename T>
	T get()
	{
		require(offset_ + sizeof(T) <= data_.size(), "the pack is truncated");
		T value{};
		std::memcpy(&value, data_.data() + offset_, sizeof(T));
		offset_ += sizeof(T);
		return value;
	}

	std::string str16()
	{
		const auto size = get<std::uint16_t>();
		return take(size);
	}

	std::string str32()
	{
		const auto size = get<std::uint32_t>();
		return take(size);
	}

	std::string take(std::size_t size)
	{
		require(offset_ + size <= data_.size(), "the pack is truncated");
		std::string text(reinterpret_cast<const char *>(data_.data() + offset_), size);
		offset_ += size;
		return text;
	}

	void skip(std::size_t size)
	{
		require(offset_ + size <= data_.size(), "the pack is truncated");
		offset_ += size;
	}

	[[nodiscard]] bool done() const { return offset_ == data_.size(); }

private:
	const std::vector<std::uint8_t> &data_;
	std::size_t offset_;
};

struct State final {
	std::uint32_t r[kRegisters]{};
	std::uint32_t apsr{}, fpscr{}, tpidruro{}, tpidrurw{};
	std::uint64_t d[kVectors]{};
};

/* The oracle's wire layout: r0-r14, APSR, FPSCR, TPIDRURO, TPIDRURW, a word of
 * padding, D0-D31. */
inline State state_from_wire(Reader &reader)
{
	State state;
	for (auto &value : state.r)
		value = reader.get<std::uint32_t>();
	state.apsr = reader.get<std::uint32_t>();
	state.fpscr = reader.get<std::uint32_t>();
	state.tpidruro = reader.get<std::uint32_t>();
	state.tpidrurw = reader.get<std::uint32_t>();
	reader.skip(4);
	for (auto &value : state.d)
		value = reader.get<std::uint64_t>();
	return state;
}

inline State delta_into(Reader &reader, State state)
{
	const auto registers = reader.get<std::uint16_t>();
	const auto misc = reader.get<std::uint8_t>();
	const auto vectors = reader.get<std::uint32_t>();
	require(registers < 1U << kRegisters && misc < 16, "a state change names a register that does not exist");
	for (unsigned index = 0; index < kRegisters; ++index)
		if (registers >> index & 1U)
			state.r[index] = reader.get<std::uint32_t>();
	std::uint32_t *const named[] = {&state.apsr, &state.fpscr, &state.tpidruro, &state.tpidrurw};
	for (unsigned index = 0; index < 4; ++index)
		if (misc >> index & 1U)
			*named[index] = reader.get<std::uint32_t>();
	for (unsigned index = 0; index < kVectors; ++index)
		if (vectors >> index & 1U)
			state.d[index] = reader.get<std::uint64_t>();
	return state;
}

struct Patch final {
	std::uint32_t offset;
	std::uint32_t length;
	std::vector<std::uint8_t> bytes;
};

inline std::vector<Patch> read_patches(Reader &reader)
{
	std::vector<Patch> patches;
	std::uint32_t end = 0;
	const auto count = reader.get<std::uint16_t>();
	for (unsigned index = 0; index < count; ++index) {
		const auto offset = reader.get<std::uint32_t>();
		const auto length = reader.get<std::uint16_t>();
		require(length != 0 && offset >= end && offset + length <= kDataSize,
			"a data patch is outside the data region or out of order");
		const auto bytes = reader.take(length);
		patches.push_back({offset, length, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
		end = offset + length;
	}
	return patches;
}

struct Case final {
	std::string name, form;
	std::uint8_t flags{};
	std::uint16_t target{};
	std::uint16_t origin{}; /* byte offset of the program from kCodeBase */
	std::vector<std::uint16_t> halves;
	std::uint16_t recipe{};
	State initial, final;
	std::uint8_t fill{};
	std::vector<Patch> data_patches, memory_patches;
	std::uint8_t event{};
	std::uint16_t fault_instruction{}, retired{};
	std::uint32_t fault_address{};
};

struct Record final {
	std::string name, oracle;
	std::uint8_t profiles{};
	std::vector<Case> cases;
};

struct Pack final {
	std::vector<State> recipes;
	std::vector<Record> records;
};

inline Pack read_pack(const std::vector<std::uint8_t> &data)
{
	require(data.size() >= 24 && std::memcmp(data.data(), "UMLA32C\0", 8) == 0, "not an AArch32 corpus pack");
	Reader reader(data, 8);
	const auto version = reader.get<std::uint32_t>();
	reader.skip(4);
	const auto length = reader.get<std::uint64_t>();
	require(version == kPackVersion && data.size() == 24 + length, "the pack header or its version is wrong");
	reader.str32(); /* the memory map and record names, for tools */
	Pack pack;
	const auto recipes = reader.get<std::uint32_t>();
	for (std::uint32_t index = 0; index < recipes; ++index) {
		reader.str16();
		pack.recipes.push_back(state_from_wire(reader));
	}
	const auto records = reader.get<std::uint32_t>();
	for (std::uint32_t index = 0; index < records; ++index) {
		Record record;
		record.name = reader.str16();
		record.oracle = reader.str16();
		record.profiles = reader.get<std::uint8_t>();
		const auto cases = reader.get<std::uint32_t>();
		for (std::uint32_t number = 0; number < cases; ++number) {
			Case test;
			test.name = reader.str16();
			test.form = reader.str16();
			test.flags = reader.get<std::uint8_t>();
			test.target = reader.get<std::uint16_t>();
			test.origin = reader.get<std::uint16_t>();
			const auto halves = reader.get<std::uint16_t>();
			require(halves != 0 && halves <= kMaxHalfwords, "a program has a wrong length");
			for (unsigned half = 0; half < halves; ++half)
				test.halves.push_back(reader.get<std::uint16_t>());
			test.recipe = reader.get<std::uint16_t>();
			require(test.recipe < pack.recipes.size(), "a case names a state recipe that does not exist");
			test.initial = delta_into(reader, pack.recipes[test.recipe]);
			test.fill = reader.get<std::uint8_t>();
			test.data_patches = read_patches(reader);
			test.event = reader.get<std::uint8_t>();
			test.fault_instruction = reader.get<std::uint16_t>();
			test.retired = reader.get<std::uint16_t>();
			test.fault_address = reader.get<std::uint32_t>();
			test.final = delta_into(reader, test.initial);
			test.memory_patches = read_patches(reader);
			record.cases.push_back(std::move(test));
		}
		pack.records.push_back(std::move(record));
	}
	require(reader.done(), "the pack has trailing bytes");
	return pack;
}

inline std::vector<std::uint8_t> read_file(const char *path)
{
	std::ifstream input(path, std::ios::binary);
	require(input.is_open(), "the corpus pack is missing");
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/* The data region before a case runs: zero or the hash32 byte recipe. */
inline std::vector<std::uint8_t> data_fill(std::uint8_t fill)
{
	std::vector<std::uint8_t> data(kDataSize, 0);
	if (fill != 0)
		for (std::uint32_t index = 0; index < kDataSize; ++index)
			data[index] = static_cast<std::uint8_t>(((1U + index) * 0x9E3779B1U) >> 24);
	return data;
}

/* The read-only region, identical for every case. */
inline std::vector<std::uint8_t> read_only_fill()
{
	std::vector<std::uint8_t> data(kReadOnlySize);
	for (std::uint32_t index = 0; index < kReadOnlySize; ++index)
		data[index] = static_cast<std::uint8_t>(((0x5AU + index) * 0x9E3779B1U) >> 24);
	return data;
}

/* The code region as the oracle prepares it: the program at its origin, an
 * exit stub after it (a halfword NOP first when a Thumb program leaves the
 * stub's word unaligned, which `padded` reports), the stubs at the two fixed
 * exits, and the lane's own exit code, `epilogue_word`, behind the two
 * epilogues. A program at the end of the region has no room for a stub and
 * falls off it. */
inline std::vector<std::uint8_t> code_image(const Case &test, std::uint32_t epilogue_word, bool &padded)
{
	std::vector<std::uint8_t> code(kCodeSize, 0);
	const auto put16 = [&](std::uint32_t offset, std::uint16_t value) {
		std::memcpy(code.data() + offset, &value, 2);
	};
	const auto put32 = [&](std::uint32_t offset, std::uint32_t value) {
		std::memcpy(code.data() + offset, &value, 4);
	};
	const auto arm_stub = [&](std::uint32_t at) {
		put32(at, 0xe51ff004U); /* ldr pc, [pc, #-4] */
		put32(at + 4, kArmEpilogue);
	};
	const auto thumb_stub = [&](std::uint32_t at) {
		put16(at, 0xf8df); /* ldr.w pc, [pc, #0] */
		put16(at + 2, 0xf000);
		put32(at + 4, kThumbEpilogue);
	};
	for (std::size_t index = 0; index < test.halves.size(); ++index)
		put16(test.origin + static_cast<std::uint32_t>(2 * index), test.halves[index]);
	std::uint32_t end = test.origin + static_cast<std::uint32_t>(2 * test.halves.size());
	padded = false;
	if (test.flags & kFlagThumb) {
		if (end % 4 != 0 && end + 2 + 8 <= kCodeSize) {
			padded = true;
			put16(end, 0xbf00); /* nop, so the stub is word aligned */
			end += 2;
		}
		if (end + 8 <= kCodeSize)
			thumb_stub(end);
	} else if (end + 8 <= kCodeSize) {
		arm_stub(end);
	}
	arm_stub(kArmExit - kCodeBase);
	thumb_stub(kThumbExit - kCodeBase);
	put32(kArmEpilogue - kCodeBase, epilogue_word);
	put32(kThumbEpilogue - kCodeBase, epilogue_word);
	return code;
}

/* The data region a case starts from, and what it holds after the program. */
inline std::vector<std::uint8_t> data_before(const Case &test)
{
	auto data = data_fill(test.fill);
	for (const auto &patch : test.data_patches)
		std::copy(patch.bytes.begin(), patch.bytes.end(), data.begin() + patch.offset);
	return data;
}

inline std::vector<std::uint8_t> data_after(const Case &test, std::vector<std::uint8_t> data)
{
	for (const auto &patch : test.memory_patches)
		std::copy(patch.bytes.begin(), patch.bytes.end(), data.begin() + patch.offset);
	return data;
}

/* One instruction of a case's program, decoded. */
struct ProgramStep final {
	::uml::cpu::arm64::aarch32::Instruction instruction;
	/* The word as the decoder takes it: the A32 bits, or hw1 << 16 | hw2 (hw1 << 16 for a T16). */
	std::uint32_t word;
	/* The IT state before it. */
	::uml::cpu::arm64::aarch32::ItState before;
	/* The index of its first halfword. */
	unsigned half;
	bool thumb;

	/* The same word, against another profile. */
	[[nodiscard]] ::uml::cpu::arm64::aarch32::Instruction decode_at(
		const ::uml::cpu::arm64::aarch32::Profile &profile) const
	{
		return thumb ? ::uml::cpu::arm64::aarch32::decode_thumb(word, before, profile)
			     : ::uml::cpu::arm64::aarch32::decode_a32(word, profile);
	}
};

/* How a program's halfwords ended. */
enum class ProgramEnd { whole, split_word, cut_off };

/* Decodes the program in order against `profile`, carrying the IT state the
 * earlier instructions leave, and hands each instruction to `visit`, which
 * returns false to stop. Reports a program that ends in half an A32 word
 * (`split_word`) or in a Thumb-2 instruction cut off by the end of the code
 * region (`cut_off`); `whole` otherwise, and when `visit` stopped it. */
template<typename Visit>
ProgramEnd for_each_instruction(const Case &test, const ::uml::cpu::arm64::aarch32::Profile &profile, Visit &&visit)
{
	namespace dec = ::uml::cpu::arm64::aarch32;
	const bool thumb = test.flags & kFlagThumb;
	dec::ItState it;
	for (unsigned half = 0; half < test.halves.size();) {
		ProgramStep step{{}, 0, it, half, thumb};
		unsigned length = 4;
		if (!thumb) {
			if (half + 1 >= test.halves.size())
				return ProgramEnd::split_word;
			step.word = std::uint32_t{test.halves[half + 1]} << 16 | test.halves[half];
			step.instruction = dec::decode_a32(step.word, profile);
		} else {
			const std::uint16_t first = test.halves[half];
			length = dec::thumb_length(first);
			if (half + length / 2 > test.halves.size())
				return ProgramEnd::cut_off;
			step.word = length == 4 ? std::uint32_t{first} << 16 | test.halves[half + 1]
						: std::uint32_t{first} << 16;
			step.instruction = dec::decode_thumb(step.word, it, profile);
			it = step.instruction.next_it;
		}
		if (!visit(step))
			return ProgramEnd::whole;
		half += length / 2;
	}
	return ProgramEnd::whole;
}

} // namespace uml::testing::aarch32_corpus
