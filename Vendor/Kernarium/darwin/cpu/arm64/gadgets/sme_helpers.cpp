// SPDX-License-Identifier: GPL-2.0-or-later
/* The C++ halves of the gadget engine's SME instructions (engine/sme.inc).
 * They run with the host out of streaming mode and work on the vCPU's
 * register file, where guest ZA and ZT0 are whenever host code runs. */
#include "sme_layout.h"
#include "tlb.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/memory_transfer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace {

using uml::cpu::arm64::CpuState;
using uml::memory::Access;

/* Copy `size` bytes between guest memory at `address` and `data`. Both pages
 * a transfer can touch are translated before any byte moves, so a fault
 * leaves memory and the register file as they were and reports the first
 * byte of the faulting page. */
[[nodiscard]] bool transfer(uml::memory::Tlb &tlb, std::uint64_t address,
	std::byte *data, std::size_t size, Access access) noexcept
{
	constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	std::array<std::byte *, 2> pages{};
	std::array<std::size_t, 2> lengths{};
	std::size_t count = 0;
	for (std::size_t done = 0; done < size; ++count) {
		if (count == pages.size())
			return false;
		const auto current = address + done;
		pages[count] = tlb.translate_tagged(current, access);
		if (pages[count] == nullptr)
			return false;
		lengths[count] = std::min(size - done,
			page_size - static_cast<std::size_t>(current % page_size));
		done += lengths[count];
	}
	for (std::size_t index = 0, done = 0; index < count; ++index) {
		if (access == Access::write)
			std::memcpy(pages[index], data + done, lengths[index]);
		else
			std::memcpy(data + done, pages[index], lengths[index]);
		done += lengths[index];
	}
	return true;
}

/* The guest pages an access's active bytes lie in, each translated the first
 * time an active byte needs it. No access spans more than two pages. */
class Pages final {
public:
	Pages(uml::memory::Tlb &tlb, Access access) noexcept
		: tlb_{tlb}, access_{access}
	{}

	/* Whether every byte of [address, address + size) is reachable; if
	 * not, the TLB holds the fault of the first byte that is not. */
	[[nodiscard]] bool reach(std::uint64_t address, std::size_t size) noexcept
	{
		for (std::size_t done = 0; done < size;) {
			const auto current = address + done;
			const auto page = current & ~(page_size - 1U);
			if (find(page) == nullptr) {
				if (count_ == pages_.size())
					return false;
				auto *const host = tlb_.translate_tagged(current,
					access_);
				if (host == nullptr)
					return false;
				pages_[count_] = page;
				hosts_[count_++] = host - (current - page);
			}
			done += std::min<std::size_t>(size - done,
				page_size - (current - page));
		}
		return true;
	}

	/* Copy between the bytes at `address`, all reached, and `data`. */
	void move(std::uint64_t address, std::byte *data,
		std::size_t size) const noexcept
	{
		for (std::size_t done = 0; done < size;) {
			const auto current = address + done;
			const auto page = current & ~(page_size - 1U);
			const auto length = std::min<std::size_t>(size - done,
				page_size - (current - page));
			auto *const host = find(page) + (current - page);
			if (access_ == Access::write)
				std::memcpy(host, data + done, length);
			else
				std::memcpy(data + done, host, length);
			done += length;
		}
	}

private:
	static constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	static_assert(UML_GADGET_SME_VM_MAX_FOOTPRINT <= page_size);

	[[nodiscard]] std::byte *find(std::uint64_t page) const noexcept
	{
		for (unsigned index = 0; index < count_; ++index)
			if (pages_[index] == page)
				return hosts_[index];
		return nullptr;
	}

	uml::memory::Tlb &tlb_;
	Access access_;
	std::array<std::uint64_t, 2> pages_{};
	std::array<std::byte *, 2> hosts_{};
	unsigned count_ = 0;
};

[[nodiscard]] std::uint64_t file_field(std::uint64_t word,
	unsigned field) noexcept
{
	return (word >> (16U * field)) & 0xffffU;
}

/* One vector memory instruction (sme_layout.h), its address formed. */
struct VectorAccess final {
	std::byte *file;
	std::size_t svl;
	std::uint64_t address;
	unsigned shape;
	std::size_t msize;
	std::size_t esize;
	bool sign;
	unsigned registers;
	std::uint64_t transfer;
	const std::byte *predicate;

	[[nodiscard]] std::byte *z(unsigned index) const noexcept
	{
		return file + file_field(transfer, index);
	}

	[[nodiscard]] bool active(std::size_t element,
		std::size_t size) const noexcept
	{
		const auto bit = element * size;
		return ((std::to_integer<unsigned>(predicate[bit / 8U]) >>
			(bit % 8U)) & 1U) != 0U;
	}
};

/* A memory element of `from` bytes in `data`, extended to `to` bytes. */
void extend(std::byte *data, std::size_t from, std::size_t to,
	bool sign) noexcept
{
	if (from == to)
		return;
	const bool negative = sign &&
		(std::to_integer<unsigned>(data[from - 1U]) & 0x80U) != 0U;
	std::memset(data + from, negative ? 0xff : 0, to - from);
}

/* Visit each element the access transfers, in element order, which is
 * also address order: visit(address, bytes, register bytes, active). A
 * load writes `register bytes` from memory or, for an inactive element,
 * zeroes them; a store writes memory from them. */
template <typename Visit>
void elements(const VectorAccess &access, std::uint32_t za_slice,
	Visit &&visit) noexcept
{
	const auto svl = access.svl;
	const auto esize = access.esize;
	switch (access.shape) {
	case UML_GADGET_SME_VM_CONTIGUOUS:
		for (std::size_t element = 0; element < svl / esize; ++element)
			visit(access.address + element * access.msize,
				access.msize, access.z(0) + element * esize,
				access.active(element, esize));
		break;
	case UML_GADGET_SME_VM_STRUCTURE:
		for (std::size_t element = 0; element < svl / esize; ++element)
			for (unsigned index = 0; index < access.registers;
			     ++index)
				visit(access.address + (element *
					access.registers + index) * esize,
					esize, access.z(index) + element * esize,
					access.active(element, esize));
		break;
	case UML_GADGET_SME_VM_ZA_SLICE: {
		/* ZA tile t of esize bytes: horizontal slice s holds array
		 * vector s * esize + t; element i of vertical slice s is
		 * element s of array vector i * esize + t. */
		const unsigned tile = (za_slice >> 8) & 0xffU;
		const bool vertical = (za_slice >> 16) != 0U;
		const std::size_t slice = (za_slice & 0xffU) % (svl / esize);
		for (std::size_t element = 0; element < svl / esize; ++element) {
			const auto row = (vertical ? element : slice) * esize +
				tile;
			const auto column = vertical ? slice : element;
			visit(access.address + element * esize, esize,
				access.file + uml::cpu::arm64::sme_za_offset(svl,
					static_cast<unsigned>(row)) +
					column * esize,
				access.active(element, esize));
		}
		break;
	}
	default:
		break;
	}
}

} // namespace

/* A guest write of SVCR, with the bits in the operand's low byte taking the
 * values in its next. Leaves the native FP environment first: the write can
 * set the guest FPSR, which the environment would otherwise overwrite when it
 * is next restored. A changed mode empties the return ring, whose records
 * name code translated for the old one. */
extern "C" [[gnu::noinline]] std::uint32_t uml_arm64_gadget_sme_write_svcr(
	CpuState *cpu, std::uint64_t operand) noexcept
{
	if (cpu->sme_registers == nullptr)
		return UML_GADGET_SME_SVCR_FATAL;
	uml::cpu::arm64::restore_native_fp_environment(*cpu);
	const auto previous = cpu->svcr;
	if (!cpu->write_svcr(static_cast<std::uint8_t>(operand),
		    static_cast<std::uint8_t>(
			    operand >> UML_GADGET_SME_SVCR_VALUE_SHIFT)))
		return UML_GADGET_SME_SVCR_NEEDS_STORAGE;
	if (cpu->svcr != previous) {
		cpu->gadget_return_stack = {};
		cpu->gadget_return_top = 0;
	}
	return UML_GADGET_SME_SVCR_WRITTEN;
}

/* Every ZA and ZT0 access and zeroing, on the register file, as the
 * descriptor (sme_layout.h) names it. Returns 0 after recording a fault in
 * the TLB, which then reports it as the native access would: an SP base
 * that is not 16-byte aligned, or the first byte of a page the access
 * cannot reach. */
extern "C" [[gnu::noinline]] std::uint32_t uml_arm64_gadget_sme_memory(
	CpuState *cpu, uml::memory::Tlb *tlb, std::uint64_t descriptor) noexcept
{
	const std::size_t svl = cpu->sme_svl;
	std::byte *const file = cpu->sme_registers;
	const auto operation = static_cast<unsigned>(
		(descriptor >> UML_GADGET_SME_OPERATION_SHIFT) & 7U);
	if (operation == UML_GADGET_SME_ZA_ZERO) {
		const auto mask = static_cast<unsigned>(
			(descriptor >> UML_GADGET_SME_MASK_SHIFT) & 0xffU);
		/* ZAk.D holds the array vectors whose number is k modulo 8. */
		for (std::size_t row = 0; row < svl; ++row)
			if ((mask >> (row % 8U)) & 1U)
				std::memset(file + uml::cpu::arm64::sme_za_offset(
					svl, static_cast<unsigned>(row)), 0, svl);
		return 1;
	}
	if (operation == UML_GADGET_SME_ZT0_ZERO) {
		std::memset(file + uml::cpu::arm64::sme_zt0_offset(svl), 0,
			uml::cpu::arm64::sme_zt0_bytes);
		return 1;
	}
	const bool base_sp =
		((descriptor >> UML_GADGET_SME_BASE_SP_BIT) & 1U) != 0U;
	const auto access =
		operation == UML_GADGET_SME_ZA_STORE ||
		operation == UML_GADGET_SME_ZT0_STORE ? Access::write : Access::read;
	std::uint64_t base;
	std::memcpy(&base, reinterpret_cast<const std::byte *>(cpu) +
		(descriptor & 0xffffU), sizeof(base));
	if (base_sp && (base & 0xfU) != 0U) {
		uml_arm64_alignment_fault(tlb, base,
			static_cast<std::uint32_t>(access));
		return 0;
	}
	std::uint64_t address = base;
	std::byte *data;
	std::size_t size;
	if (operation == UML_GADGET_SME_ZA_LOAD ||
	    operation == UML_GADGET_SME_ZA_STORE) {
		const auto offset = static_cast<std::uint32_t>(
			(descriptor >> 32) & 0xfU);
		std::uint32_t index;
		std::memcpy(&index, reinterpret_cast<const std::byte *>(cpu) +
			((descriptor >> 16) & 0xffffU), sizeof(index));
		address += std::uint64_t{offset} * svl;
		const auto row = static_cast<unsigned>(
			(index + offset) & (svl - 1U));
		data = file + uml::cpu::arm64::sme_za_offset(svl, row);
		size = svl;
	} else {
		data = file + uml::cpu::arm64::sme_zt0_offset(svl);
		size = uml::cpu::arm64::sme_zt0_bytes;
	}
	address = cpu->untag_data_address(address);
	if (!transfer(*tlb, address, data, size, access))
		return 0;
	if (access == Access::write)
		cpu->exclusive_size = 0;
	return 1;
}

/* Every vector memory access a streaming program's gadget cannot complete
 * natively, on the register file, from its stream (sme_layout.h). Only the
 * active elements' bytes are touched, and every page they lie in is
 * translated before any byte moves, so a fault leaves memory and the
 * registers unchanged. The fault is reported as M4 hardware reports it: a
 * predicated access at the first faulting active byte rounded down to
 * 4 KiB, an unpredicated one (LDR and STR of a vector, a predicate, or a
 * SIMD&FP register) at that byte, and an SP base that is not 16-byte
 * aligned as an alignment fault. Returns 0 after recording the fault. */
extern "C" [[gnu::noinline]] std::uint32_t uml_arm64_gadget_sme_vector_memory(
	CpuState *cpu, uml::memory::Tlb *tlb, const std::uint64_t *words) noexcept
{
	const auto address_word = words[0];
	const auto shape_word = words[3];
	const auto *const state = reinterpret_cast<const std::byte *>(cpu);
	const auto read_state = [state](std::uint64_t offset) noexcept {
		std::uint64_t value;
		std::memcpy(&value, state + offset, sizeof(value));
		return value;
	};
	const bool store =
		((shape_word >> UML_GADGET_SME_VM_STORE_BIT) & 1U) != 0U;
	const auto direction = store ? Access::write : Access::read;
	const bool base_sp =
		((address_word >> UML_GADGET_SME_VM_BASE_SP_BIT) & 1U) != 0U;
	const auto base = read_state(address_word & 0xffffU);
	if (base_sp && (base & 0xfU) != 0U) {
		uml_arm64_alignment_fault(tlb, base,
			static_cast<std::uint32_t>(direction));
		return 0;
	}
	const auto immediate = words[1];
	std::uint64_t address = base;
	if (((address_word >> UML_GADGET_SME_VM_POST_BIT) & 1U) == 0U)
		address += immediate;
	if (((address_word >> UML_GADGET_SME_VM_INDEX_BIT) & 1U) != 0U) {
		auto index = read_state((address_word >> 16) & 0xffffU);
		const bool word =
			((address_word >> UML_GADGET_SME_VM_WORD_BIT) & 1U) != 0U;
		const bool sign =
			((address_word >> UML_GADGET_SME_VM_SIGNED_BIT) & 1U) != 0U;
		if (word && sign)
			index = static_cast<std::uint64_t>(std::int64_t{
				static_cast<std::int32_t>(index)});
		else if (word)
			index &= 0xffff'ffffU;
		address += index << ((address_word >>
			UML_GADGET_SME_VM_SHIFT_SHIFT) & 7U);
	}
	address = cpu->untag_data_address(address);

	VectorAccess access{};
	access.file = cpu->sme_registers;
	access.svl = cpu->sme_svl;
	access.address = address;
	access.shape = static_cast<unsigned>(
		(shape_word >> UML_GADGET_SME_VM_SHAPE_SHIFT) & 0xfU);
	access.msize = std::size_t{1} <<
		((shape_word >> UML_GADGET_SME_VM_MSIZE_SHIFT) & 7U);
	access.esize = std::size_t{1} <<
		((shape_word >> UML_GADGET_SME_VM_ESIZE_SHIFT) & 7U);
	access.sign = ((shape_word >> UML_GADGET_SME_VM_SIGN_BIT) & 1U) != 0U;
	access.registers = static_cast<unsigned>(
		(shape_word >> UML_GADGET_SME_VM_REGISTERS_SHIFT) & 7U);
	access.transfer = words[2];
	access.predicate = access.file + (shape_word & 0xffffU);
	const auto svl = access.svl;
	const auto esize = access.esize;

	Pages pages{*tlb, direction};
	std::uint64_t fault_mask = ~std::uint64_t{0};
	bool reached = true;
	std::uint32_t za_slice = 0;
	if (access.shape == UML_GADGET_SME_VM_ZA_SLICE) {
		const auto slice = static_cast<std::uint32_t>(
			read_state(file_field(access.transfer, 0)) +
			file_field(access.transfer, 1));
		/* The slice index modulo the slices, which divide 256. */
		za_slice = (slice & 0xffU) |
			(static_cast<std::uint32_t>(
				file_field(access.transfer, 2)) << 8) |
			(static_cast<std::uint32_t>(
				file_field(access.transfer, 3)) << 16);
	}
	switch (access.shape) {
	case UML_GADGET_SME_VM_CONTIGUOUS:
	case UML_GADGET_SME_VM_STRUCTURE:
	case UML_GADGET_SME_VM_ZA_SLICE:
		fault_mask = ~std::uint64_t{0xfff};
		elements(access, za_slice, [&](std::uint64_t at,
			std::size_t size, std::byte *, bool active) noexcept {
			if (active && reached)
				reached = pages.reach(at, size);
		});
		if (!reached)
			break;
		elements(access, za_slice, [&](std::uint64_t at,
			std::size_t size, std::byte *data, bool active) noexcept {
			if (store) {
				if (active)
					pages.move(at, data, size);
				return;
			}
			const auto lane = access.shape ==
				UML_GADGET_SME_VM_CONTIGUOUS ? esize : size;
			if (!active) {
				std::memset(data, 0, lane);
				return;
			}
			pages.move(at, data, size);
			extend(data, size, lane, access.sign);
		});
		break;
	case UML_GADGET_SME_VM_REPLICATE: {
		/* LD1R: one element when any element is active, to every
		 * active element. */
		fault_mask = ~std::uint64_t{0xfff};
		bool any = false;
		for (std::size_t element = 0; element < svl / esize; ++element)
			any = any || access.active(element, esize);
		std::array<std::byte, 8> value{};
		if (any) {
			reached = pages.reach(address, access.msize);
			if (!reached)
				break;
			pages.move(address, value.data(), access.msize);
			extend(value.data(), access.msize, esize, access.sign);
		}
		auto *const z = access.z(0);
		for (std::size_t element = 0; element < svl / esize; ++element) {
			if (access.active(element, esize))
				std::memcpy(z + element * esize, value.data(),
					esize);
			else
				std::memset(z + element * esize, 0, esize);
		}
		break;
	}
	case UML_GADGET_SME_VM_REPLICATE_QUAD: {
		/* LD1RQ: the active elements of the first quadword, in every
		 * quadword. */
		fault_mask = ~std::uint64_t{0xfff};
		std::array<std::byte, 16> quad{};
		for (std::size_t element = 0; element < 16U / esize; ++element)
			if (access.active(element, esize) && reached)
				reached = pages.reach(address + element * esize,
					esize);
		if (!reached)
			break;
		for (std::size_t element = 0; element < 16U / esize; ++element)
			if (access.active(element, esize))
				pages.move(address + element * esize,
					quad.data() + element * esize, esize);
		auto *const z = access.z(0);
		for (std::size_t offset = 0; offset < svl; offset += 16U)
			std::memcpy(z + offset, quad.data(), quad.size());
		break;
	}
	case UML_GADGET_SME_VM_FILL_Z:
	case UML_GADGET_SME_VM_FILL_P: {
		const auto size = access.shape == UML_GADGET_SME_VM_FILL_Z ?
			svl : svl / 8U;
		reached = pages.reach(address, size);
		if (reached)
			pages.move(address, access.z(0), size);
		break;
	}
	case UML_GADGET_SME_VM_FP: {
		/* A SIMD&FP register or pair: a load zeroes the rest of each
		 * Z register, as the V register write does. */
		const auto size = access.msize;
		reached = pages.reach(address, size * access.registers);
		if (!reached)
			break;
		for (unsigned index = 0; index < access.registers; ++index) {
			auto *const z = access.z(index);
			if (!store)
				std::memset(z, 0, svl);
			pages.move(address + index * size, z, size);
		}
		if (((address_word >> UML_GADGET_SME_VM_WRITEBACK_BIT) & 1U) !=
		    0U) {
			const auto written = base + immediate;
			std::memcpy(reinterpret_cast<std::byte *>(cpu) +
				(address_word & 0xffffU), &written,
				sizeof(written));
		}
		break;
	}
	default:
		return 0;
	}
	if (!reached) {
		tlb->fault_address &= fault_mask;
		return 0;
	}
	if (store)
		cpu->exclusive_size = 0;
	return 1;
}
