// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "memory/guest_address_space.hpp"

#include <cstddef>
#include <cstdint>

/*
 * The access of an SM = 0 spill word (sm0_vector_length.hpp): LDR or STR of
 * a vector register Z (16 bytes, the whole of V_n) or of a predicate register
 * P (2 bytes), with a scalar base and an immediate offset. A translated
 * block reaches it through the JIT's slow path, when the access crosses a
 * guest page or misses the TLB; the gadget engine translates the same words
 * as the SIMD&FP or halfword access they are. The virtual CPU has no
 * predicate state at SM = 0, so STR of P stores two zero bytes and LDR of P
 * performs its access, faulting as a load does, and drops the data.
 *
 * It works on CpuState alone and reaches guest memory only through the
 * caller's Sm0Memory, so neither engine's header is included.
 */

namespace uml::cpu::arm64::detail {

/*
 * Guest memory as the caller's engine reaches it. translate() returns the
 * host address of the guest byte at `address` for `access`, valid through
 * the end of its guest page, or nullptr when the page does not translate
 * that way. After a store has moved bytes the access reports each host range
 * it wrote to stored(), so an engine can invalidate the reservations they
 * overlap.
 */
class Sm0Memory {
public:
	[[nodiscard]] virtual std::byte *translate(std::uint64_t address,
		::uml::memory::MemoryAccess access) noexcept = 0;
	virtual void stored(std::byte *host, std::size_t size) noexcept = 0;

protected:
	Sm0Memory() = default;
	~Sm0Memory() = default;
	Sm0Memory(const Sm0Memory &) = default;
	Sm0Memory &operator=(const Sm0Memory &) = default;
};

enum class Sm0Event : std::uint8_t {
	/* The access completed; the caller advances PC. */
	retired,
	/* A data abort or alignment fault, at fault_address for access. */
	memory_fault,
	/* The word is not a spill word: an internal error. */
	fatal,
};

struct Sm0Result final {
	Sm0Event event{Sm0Event::retired};
	/* For memory_fault: an alignment fault (an SP base that is not 16-byte
	 * aligned) rather than a translation fault. */
	bool alignment{};
	::uml::memory::MemoryAccess access{::uml::memory::MemoryAccess::read};
	std::uint64_t fault_address{};
};

/*
 * Executes spill word `word` on `cpu`, which holds the guest's FP state or
 * defers it (the function makes it accessible). An SP base must be 16-byte
 * aligned, which is checked before the access. The address is untagged as a
 * data address (CpuState::untag_data_address) and the access is all or
 * nothing: a fault leaves every register as it was and memory unwritten. The
 * exclusive monitor is untouched. PC is not changed. Any other word is a fatal error that changes nothing.
 */
[[nodiscard]] Sm0Result execute_sm0_spill(CpuState &cpu, std::uint32_t word,
	Sm0Memory &memory) noexcept;

} // namespace uml::cpu::arm64::detail
