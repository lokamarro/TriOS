// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's VFP and Advanced SIMD memory forms: VLDR, VSTR,
// VLDM, VSTM (and FLDMX, FSTMX, which transfer the same doublewords) and the
// structure loads and stores VLDn and VSTn (semantics.hpp).

#include "cpu/arm64/aarch32/semantics_internal.hpp"

#include <array>

namespace uml::cpu::arm64::aarch32::detail {

namespace {

using ::uml::memory::MemoryAccess;

/* VLDR and VSTR: one single or double register at base +/- an immediate.
 * The access is MemA, so the address needs the alignment of its words. */
bool exec_register_transfer(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	const bool load = info.op == Op::vldr;
	const unsigned n = static_cast<unsigned>(o.n);
	const unsigned size = static_cast<unsigned>(o.esize) / 8U;
	/* VLDR (literal) reads the word-aligned PC; every other base is R[n]. */
	const std::uint32_t base = load && n == reg_pc ? x.pc_aligned() : x.reg(n);
	const auto offset = static_cast<std::uint32_t>(o.imm32);
	const std::uint32_t address = o.add ? base + offset : base - offset;
	const MemoryAccess access = load ? MemoryAccess::read : MemoryAccess::write;
	const std::uint32_t alignment = size == 8U ? 4U : size;
	if ((address & (alignment - 1U)) != 0U)
		return x.raise_alignment(address, access);
	const unsigned d = static_cast<unsigned>(o.d);
	/* The transfer moves the low `size` bytes of a doubleword: H(d) and S(d)
	 * are the low halfword and word of S(d), and a load zeroes the rest. */
	std::uint64_t value = 0;
	if (load) {
		if (!x.read(address, &value, size))
			return false;
		if (size == 8U)
			x.set_d_reg(d, value);
		else
			x.set_s_reg(d, static_cast<std::uint32_t>(value));
		return true;
	}
	value = size == 8U ? x.d_reg(d) : x.s_reg(d);
	return x.write(address, &value, size);
}

/* VLDM and VSTM, increment-after or decrement-before, with optional
 * writeback; FLDMX and FSTMX are the same transfer of the doublewords with
 * the extra word counted in the writeback only. */
bool exec_block_transfer(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	const bool load = info.op == Op::vldm;
	const unsigned n = static_cast<unsigned>(o.n);
	const unsigned regs = static_cast<unsigned>(o.regs);
	const unsigned words = o.single_regs ? regs : 2U * regs;
	const auto offset = static_cast<std::uint32_t>(o.imm32);
	const std::uint32_t base = x.reg(n);
	const std::uint32_t start = o.add ? base : base - offset;
	const MemoryAccess access = load ? MemoryAccess::read : MemoryAccess::write;
	/* MemA: every element needs word alignment, so an unaligned block faults
	 * at its first word. */
	if ((start & 3U) != 0U)
		return x.raise_alignment(start, access);
	const unsigned d = static_cast<unsigned>(o.d);
	std::array<std::uint32_t, 32> buffer{};
	if (load) {
		if (!x.read(start, buffer.data(), 4U * words))
			return false;
		for (unsigned r = 0; r < regs; ++r) {
			if (o.single_regs)
				x.set_s_reg(d + r, buffer[r]);
			else
				x.set_d_reg(d + r, std::uint64_t{buffer[2U * r + 1U]} << 32 | buffer[2U * r]);
		}
	} else {
		for (unsigned r = 0; r < regs; ++r) {
			if (o.single_regs) {
				buffer[r] = x.s_reg(d + r);
			} else {
				const std::uint64_t value = x.d_reg(d + r);
				buffer[2U * r] = static_cast<std::uint32_t>(value);
				buffer[2U * r + 1U] = static_cast<std::uint32_t>(value >> 32);
			}
		}
		if (!x.write_words(start, buffer.data(), words))
			return false;
	}
	if (o.wback)
		x.set_reg(n, o.add ? base + offset : base - offset);
	return true;
}

/* One element of a structure transfer: the byte offset in D(reg) of the
 * element that memory element k reads or writes. */
struct Slot final {
	std::uint8_t reg;
	std::uint8_t byte;
};

/* VLDn and VSTn. Memory holds the elements consecutively and in the order
 * the slots list them; a load reads all of it before it changes a register
 * and a store gathers all of it before it writes any byte, so a fault leaves
 * both as they were. */
bool exec_structure(Exec &x, const OpInfo &info)
{
	const Operands &o = x.o;
	const bool load = info.op == Op::vld;
	const unsigned structs = info.structs;
	const unsigned ebytes = static_cast<unsigned>(o.ebytes);
	const unsigned n = static_cast<unsigned>(o.n);
	const std::array<unsigned, 4> dregs{static_cast<unsigned>(o.d), static_cast<unsigned>(o.d2),
		static_cast<unsigned>(o.d3), static_cast<unsigned>(o.d4)};
	std::array<Slot, 32> slots{};
	unsigned count = 0;
	const auto add_slot = [&](unsigned reg, unsigned byte) {
		slots[count++] = Slot{static_cast<std::uint8_t>(reg), static_cast<std::uint8_t>(byte)};
	};

	unsigned bytes = structs * ebytes;
	switch (info.structure) {
	case StructureKind::multiple: {
		const unsigned elements = 8U / ebytes;
		if (structs == 1U) {
			for (unsigned r = 0; r < static_cast<unsigned>(o.regs); ++r)
				for (unsigned e = 0; e < elements; ++e)
					add_slot(dregs[0] + r, e * ebytes);
		} else if (structs == 2U) {
			for (unsigned r = 0; r < static_cast<unsigned>(o.pairs); ++r)
				for (unsigned e = 0; e < elements; ++e) {
					add_slot(dregs[0] + r, e * ebytes);
					add_slot(dregs[1] + r, e * ebytes);
				}
		} else {
			for (unsigned e = 0; e < elements; ++e)
				for (unsigned s = 0; s < structs; ++s)
					add_slot(dregs[s], e * ebytes);
		}
		bytes = count * ebytes;
		break;
	}
	case StructureKind::single_lane:
		for (unsigned s = 0; s < structs; ++s)
			add_slot(dregs[s], static_cast<unsigned>(o.index_value) * ebytes);
		break;
	case StructureKind::all_lanes:
		break;
	}

	const std::uint32_t base = x.reg(n);
	const std::int32_t alignment = o.alignment;
	const MemoryAccess access = load ? MemoryAccess::read : MemoryAccess::write;
	if (alignment > 1 && (base & static_cast<std::uint32_t>(alignment - 1)) != 0U)
		return x.raise_alignment(base, access);

	std::array<std::uint8_t, 32> buffer{};
	if (load) {
		if (!x.read(base, buffer.data(), bytes))
			return false;
		if (info.structure == StructureKind::all_lanes) {
			/* Each element is replicated across a whole register: one
			 * element into regs registers for VLD1, and element k into
			 * the k-th register otherwise. */
			const unsigned registers = structs == 1U ? static_cast<unsigned>(o.regs) : structs;
			for (unsigned r = 0; r < registers; ++r) {
				const unsigned reg = structs == 1U ? dregs[0] + r : dregs[r];
				const unsigned source = structs == 1U ? 0U : r;
				std::byte *destination = x.d_bytes(reg);
				for (unsigned e = 0; e < 8U / ebytes; ++e)
					std::memcpy(destination + e * ebytes, buffer.data() + source * ebytes, ebytes);
			}
		} else {
			for (unsigned k = 0; k < count; ++k)
				std::memcpy(x.d_bytes(slots[k].reg) + slots[k].byte,
					buffer.data() + k * ebytes, ebytes);
		}
	} else {
		for (unsigned k = 0; k < count; ++k)
			std::memcpy(buffer.data() + k * ebytes,
				x.d_bytes(slots[k].reg) + slots[k].byte, ebytes);
		if (!x.write(base, buffer.data(), bytes))
			return false;
	}
	if (o.wback)
		x.set_reg(n, o.register_index ? base + x.reg(static_cast<unsigned>(o.m)) : base + bytes);
	return true;
}

} // namespace

bool exec_vector_memory(Exec &x, const OpInfo &info)
{
	switch (info.op) {
	case Op::vldr:
	case Op::vstr:
		return exec_register_transfer(x, info);
	case Op::vldm:
	case Op::vstm:
		return exec_block_transfer(x, info);
	case Op::vld:
	case Op::vst:
		return exec_structure(x, info);
	default:
		return x.raise_fatal();
	}
}

} // namespace uml::cpu::arm64::aarch32::detail
