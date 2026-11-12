/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Darwin clears x18 when it interrupts a thread, so no host code FEXCore
 * emits may name it. This walks the blocks FEXCore translates into its code
 * buffers and checks every instruction reachable from each block's entry
 * with the register oracle of the arm64 tests (a64_register_operands.hpp).
 *
 * The buffers are found through FEXCore's executable-memory hooks, which the
 * engine installs when it makes its first context: install() wraps them, so
 * every later allocation is recorded. A code buffer holds FEXCore's JIT
 * blocks back to back from its start, each 16-byte aligned, in the layout of
 * FEXCore's Interface/Core/CPUBackend.h:
 * - JITCodeHeader: the 32-bit offset from the block to its tail;
 * - the code, its jump thunks and their data, then padding;
 * - JITCodeTail: the block's whole size, its guest RIP and more, followed
 *   by the RIP entries.
 * A block that does not parse in that layout fails the scan, so a change to
 * it cannot pass silently. Data sits in the code range, so the walk follows
 * control flow from the entry rather than reading it linearly: through
 * branches, calls and the return addresses ADR forms, stopping at indirect
 * branches, returns, traps, the exit linker's call and the words a literal
 * load names.
 *
 * Each context's dispatcher, a smaller allocation, is not walked: FEXCore
 * enters it at offsets only its C++ knows, and constant tables an ADR names
 * sit between its routines, so neither a walk nor a sweep separates its
 * code from its data. */

#include "a64_register_operands.hpp"

#include <FEXCore/Utils/AllocatorHooks.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace uml::testing::fex {

class EmittedRegisterScan final {
public:
	/* Wraps the installed hooks; call once the engine has made a context. */
	static void install()
	{
		auto &state = shared();
		std::lock_guard lock{state.mutex};
		if (state.installed)
			return;
		state.original = FEXCore::Allocator::GetExecutableMemoryHooks();
		FEXCore::Allocator::SetExecutableMemoryHooks({
			.Allocate = &allocate,
			.Free = &release,
			.SetWritable = state.original.SetWritable,
		});
		state.installed = true;
	}

	/* Checks every block placed since the previous call. */
	void scan()
	{
		auto &state = shared();
		std::lock_guard lock{state.mutex};
		for (auto &[base, buffer] : state.buffers) {
			if (buffer.size < minimum_code_buffer)
				continue; // a dispatcher (see above)
			if (!walk(base, buffer, buffer.scanned) &&
			    !walk(base, buffer, 0)) {
				fail(base, 0, 0, "code buffer does not parse as "
						 "FEXCore JIT blocks");
				buffer.scanned = buffer.size;
			}
		}
	}

	[[nodiscard]] bool passed() const noexcept
	{
		return failures_ == 0U && blocks_ != 0U;
	}

	[[nodiscard]] std::string summary() const
	{
		return "blocks=" + std::to_string(blocks_) +
			" code-words=" + std::to_string(code_words_) +
			" reached-words=" + std::to_string(reached_words_) +
			" failures=" + std::to_string(failures_ +
				(blocks_ == 0U ? 1U : 0U));
	}

private:
	static constexpr std::size_t minimum_code_buffer = 1U << 20;
	static constexpr std::size_t tail_size = 40;

	struct Buffer final {
		std::size_t size{};
		/* Where the next block not yet checked starts. */
		std::size_t scanned{};
	};

	struct State final {
		std::mutex mutex;
		bool installed{};
		FEXCore::Allocator::ExecutableMemoryHooks original{};
		std::map<std::uintptr_t, Buffer> buffers;
	};

	/* Never destroyed: the engine frees its buffers at exit. */
	static State &shared()
	{
		static auto *state = new State;
		return *state;
	}

	static void *allocate(std::size_t size)
	{
		auto &state = shared();
		void *mapping = state.original.Allocate(size);
		if (mapping != nullptr) {
			std::lock_guard lock{state.mutex};
			state.buffers[reinterpret_cast<std::uintptr_t>(mapping)] =
				Buffer{size, 0};
		}
		return mapping;
	}

	/* The owner serves released memory again as it was, and a walk
	 * stops at the zeroed end of the blocks: clear what FEXCore wrote, all
	 * of a dispatcher, so no old bytes follow a later buffer's blocks. */
	static void release(void *mapping, std::size_t size)
	{
		auto &state = shared();
		{
			std::lock_guard lock{state.mutex};
			state.buffers.erase(
				reinterpret_cast<std::uintptr_t>(mapping));
		}
		{
			FEXCore::Allocator::ScopedExecutableWrite write;
			std::memset(mapping, 0, size < minimum_code_buffer ? size :
				written(reinterpret_cast<std::uintptr_t>(mapping), size));
		}
		state.original.Free(mapping, size);
	}

	/* The end of the blocks from a buffer's start, or its size when they
	 * do not parse. */
	[[nodiscard]] static std::size_t written(std::uintptr_t base,
		std::size_t buffer_size)
	{
		std::size_t offset = 0;
		while (offset + 4U <= buffer_size) {
			const auto header = read<std::uint32_t>(base + offset);
			if (header == 0U)
				return offset;
			const auto tail = offset + header;
			if (header < 8U || header % 8U != 0U ||
			    tail + tail_size > buffer_size)
				return buffer_size;
			const auto size = read<std::uint64_t>(base + tail);
			if (size < header + tail_size || size > buffer_size - offset)
				return buffer_size;
			offset = std::min(buffer_size, static_cast<std::size_t>(
				(offset + size + 15U) & ~std::uint64_t{15}));
		}
		return buffer_size;
	}

	template<typename Value>
	[[nodiscard]] static Value read(std::uintptr_t address)
	{
		Value value;
		std::memcpy(&value, reinterpret_cast<const void *>(address),
			sizeof value);
		return value;
	}

	/* Walks blocks from `from` until the unused, zeroed end of the
	 * buffer. Returns false when a block does not parse. */
	bool walk(std::uintptr_t base, Buffer &buffer, std::size_t from)
	{
		std::size_t offset = from;
		while (offset + 4U <= buffer.size) {
			const auto header = read<std::uint32_t>(base + offset);
			if (header == 0U)
				break;
			const auto tail = offset + header;
			if (header < 8U || header % 8U != 0U ||
			    tail + tail_size > buffer.size)
				return false;
			const auto size = read<std::uint64_t>(base + tail);
			const auto rip = read<std::uint64_t>(base + tail + 8U);
			if (size < header + tail_size ||
			    size > buffer.size - offset || rip == 0U)
				return false;
			check_block(base, offset, header, rip);
			offset = (offset + size + 15U) & ~std::size_t{15};
		}
		buffer.scanned = offset;
		return true;
	}

	void check_block(std::uintptr_t base, std::size_t offset,
		std::uint32_t header, std::uint64_t rip)
	{
		++blocks_;
		const auto begin = base + offset + 4U;
		const std::size_t count = (header - 4U) / 4U;
		code_words_ += count;
		const auto *words = reinterpret_cast<const std::uint32_t *>(begin);
		std::vector<bool> data(count);
		std::vector<bool> reached;
		/* Reach again until no reached word is one a literal names. */
		for (;;) {
			reached.assign(count, false);
			std::vector<std::size_t> pending{0};
			bool overlap = false;
			while (!pending.empty()) {
				const auto index = pending.back();
				pending.pop_back();
				if (index >= count || reached[index] || data[index])
					continue;
				reached[index] = true;
				const auto word = words[index];
				mark_literal(word, index, count, data);
				successors(word,
					index != 0U ? std::optional{words[index - 1U]} :
						std::nullopt,
					index, pending);
			}
			for (std::size_t index = 0; index < count; ++index)
				overlap = overlap || (reached[index] && data[index]);
			if (!overlap)
				break;
		}
		for (std::size_t index = 0; index < count; ++index) {
			if (!reached[index])
				continue;
			++reached_words_;
			const auto names =
				::uml::testing::a64::names_platform_register(
					words[index]);
			if (!names.has_value())
				fail(base, offset, rip, "unclassified word", index,
					words[index]);
			else if (*names)
				fail(base, offset, rip, "x18 operand", index,
					words[index]);
		}
	}

	[[nodiscard]] static std::int64_t signed_field(std::uint32_t word,
		unsigned low, unsigned width)
	{
		const auto value = (word >> low) & ((1U << width) - 1U);
		const auto sign = 1U << (width - 1U);
		return static_cast<std::int64_t>(value ^ sign) -
			static_cast<std::int64_t>(sign);
	}

	/* Marks the words a literal load or ADR names as data. */
	static void mark_literal(std::uint32_t word, std::size_t index,
		std::size_t count, std::vector<bool> &data)
	{
		std::int64_t target = -1;
		std::size_t words = 0;
		if ((word & 0x3b00'0000U) == 0x1800'0000U) { // LDR (literal)
			const auto opc = word >> 30;
			const bool vector = (word & (1U << 26)) != 0U;
			if (!vector && opc == 3U)
				return; // PRFM
			words = vector ? (std::size_t{1} << opc) :
				(opc == 1U ? 2U : 1U);
			target = static_cast<std::int64_t>(index) +
				signed_field(word, 5, 19);
		}
		for (std::size_t member = 0; member < words; ++member) {
			const auto at = target + static_cast<std::int64_t>(member);
			if (at >= 0 && static_cast<std::size_t>(at) < count)
				data[static_cast<std::size_t>(at)] = true;
		}
	}

	static void successors(std::uint32_t word,
		std::optional<std::uint32_t> previous, std::size_t index,
		std::vector<std::size_t> &pending)
	{
		const auto push = [&](std::int64_t delta) {
			const auto at = static_cast<std::int64_t>(index) + delta;
			if (at >= 0)
				pending.push_back(static_cast<std::size_t>(at));
		};
		if ((word & 0x9f00'0000U) == 0x1000'0000U) { // ADR: a return address
			const auto bytes = (signed_field(word, 5, 19) << 2) |
				((word >> 29) & 3U);
			if (bytes % 4 == 0)
				push(bytes / 4);
			push(1);
			return;
		}
		if ((word & 0x7c00'0000U) == 0x1400'0000U) { // B, BL
			push(signed_field(word, 0, 26));
			if ((word >> 31) != 0U)
				push(1);
			return;
		}
		if ((word & 0xff00'0000U) == 0x5400'0000U || // B.cond
		    (word & 0x7e00'0000U) == 0x3400'0000U) { // CBZ, CBNZ
			push(signed_field(word, 5, 19));
			push(1);
			return;
		}
		if ((word & 0x7e00'0000U) == 0x3600'0000U) { // TBZ, TBNZ
			push(signed_field(word, 5, 14));
			push(1);
			return;
		}
		if ((word & 0xfe00'0000U) == 0xd600'0000U) { // branch (register)
			/* The exit linker, called through a literal, reads the
			 * thunk's data from the return address and never returns. */
			const auto opc = (word >> 21) & 15U;
			const bool linker = previous.has_value() &&
				(*previous & 0xff00'0000U) == 0x5800'0000U &&
				(*previous & 31U) == ((word >> 5) & 31U);
			if ((opc == 1U || opc == 9U) && !linker) // BLR, BLRAA/BLRAB
				push(1);
			return;
		}
		if ((word >> 16) == 0U || // UDF
		    (word & 0xff00'0000U) == 0xd400'0000U) // BRK, HLT, SVC
			return;
		push(1);
	}

	void fail(std::uintptr_t base, std::size_t offset, std::uint64_t rip,
		const char *why, std::size_t index = 0, std::uint32_t word = 0)
	{
		++failures_;
		if (failures_ <= 25U)
			std::fprintf(stderr,
				"FAIL FEXCore block at %#zx+%#zx (guest RIP %#llx) "
				"word %zu: %s %#010x\n",
				static_cast<std::size_t>(base), offset,
				static_cast<unsigned long long>(rip), index, why,
				word);
	}

	std::size_t blocks_{};
	std::size_t code_words_{};
	std::size_t reached_words_{};
	std::size_t failures_{};
};

} // namespace uml::testing::fex
