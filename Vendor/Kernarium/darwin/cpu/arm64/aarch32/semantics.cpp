// SPDX-License-Identifier: GPL-2.0-or-later
//
// The AArch32 semantic library's entry point: the condition, the memory
// accesses with their fault ordering, the dispatch to the operation
// families (semantics.hpp).

#include "cpu/arm64/aarch32/semantics_internal.hpp"
#if UML_A32_LIBRARY_EXIT_COUNTERS
#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"
#endif

#include <array>
#include <cstring>

namespace uml::cpu::arm64::aarch32 {

namespace detail {

namespace {

using ::uml::memory::MemoryAccess;

/* The bytes of the page the address lies in, from the address on. */
[[nodiscard]] constexpr std::size_t page_remaining(std::uint32_t address) noexcept
{
	return guest_page_size - (address & (guest_page_size - 1U));
}

/* A single-copy atomic load or store of an aligned 2, 4 or 8 bytes: one
 * host access, which is the architecture's guarantee for an aligned
 * halfword, word, and (LPAE) doubleword. */
void copy_in(void *destination, const std::byte *host, std::size_t size) noexcept
{
	const auto address = reinterpret_cast<std::uintptr_t>(host);
	if (size == 4U && (address & 3U) == 0U) {
		const auto value = __atomic_load_n(
			reinterpret_cast<const std::uint32_t *>(host), __ATOMIC_RELAXED);
		std::memcpy(destination, &value, 4);
	} else if (size == 2U && (address & 1U) == 0U) {
		const auto value = __atomic_load_n(
			reinterpret_cast<const std::uint16_t *>(host), __ATOMIC_RELAXED);
		std::memcpy(destination, &value, 2);
	} else if (size == 8U && (address & 7U) == 0U) {
		const auto value = __atomic_load_n(
			reinterpret_cast<const std::uint64_t *>(host), __ATOMIC_RELAXED);
		std::memcpy(destination, &value, 8);
	} else {
		std::memcpy(destination, host, size);
	}
}

void copy_out(std::byte *host, const void *source, std::size_t size) noexcept
{
	const auto address = reinterpret_cast<std::uintptr_t>(host);
	if (size == 4U && (address & 3U) == 0U) {
		std::uint32_t value;
		std::memcpy(&value, source, 4);
		__atomic_store_n(reinterpret_cast<std::uint32_t *>(host), value,
			__ATOMIC_RELAXED);
	} else if (size == 2U && (address & 1U) == 0U) {
		std::uint16_t value;
		std::memcpy(&value, source, 2);
		__atomic_store_n(reinterpret_cast<std::uint16_t *>(host), value,
			__ATOMIC_RELAXED);
	} else if (size == 8U && (address & 7U) == 0U) {
		std::uint64_t value;
		std::memcpy(&value, source, 8);
		__atomic_store_n(reinterpret_cast<std::uint64_t *>(host), value,
			__ATOMIC_RELAXED);
	} else {
		std::memcpy(host, source, size);
	}
}

} // namespace

bool Exec::condition_holds(unsigned cond) const noexcept
{
	bool result;
	switch (cond >> 1) {
	case 0: result = flag_z(); break;
	case 1: result = flag_c(); break;
	case 2: result = flag_n(); break;
	case 3: result = flag_v(); break;
	case 4: result = flag_c() && !flag_z(); break;
	case 5: result = flag_n() == flag_v(); break;
	case 6: result = flag_n() == flag_v() && !flag_z(); break;
	default: return true;
	}
	return (cond & 1U) != 0U ? !result : result;
}

bool Exec::read(std::uint32_t address, void *destination, std::size_t size) noexcept
{
	/* The common access lies in one page. */
	if (size <= page_remaining(address)) [[likely]] {
		const std::byte *host = mem.translate(address, MemoryAccess::read);
		if (host == nullptr) [[unlikely]]
			return raise_fault(address, MemoryAccess::read);
		copy_in(destination, host, size);
		return true;
	}
	auto *out = static_cast<std::byte *>(destination);
	while (size != 0U) {
		const std::size_t count = std::min(size, page_remaining(address));
		const std::byte *host = mem.translate(address, MemoryAccess::read);
		if (host == nullptr)
			return raise_fault(address, MemoryAccess::read);
		copy_in(out, host, count);
		out += count;
		size -= count;
		address += static_cast<std::uint32_t>(count);
	}
	return true;
}

bool Exec::write(std::uint32_t address, const void *source, std::size_t size) noexcept
{
	if (size <= page_remaining(address)) [[likely]] {
		std::byte *host = mem.translate(address, MemoryAccess::write);
		if (host == nullptr) [[unlikely]]
			return raise_fault(address, MemoryAccess::write);
		copy_out(host, source, size);
		mem.stored(host, size);
		return true;
	}
	/* A store is one access: translate every page it touches before the
	 * first host store, so a fault on a later page exposes no partial
	 * update. A transfer spans at most two pages. */
	std::array<std::byte *, 2> hosts{};
	std::array<std::size_t, 2> counts{};
	std::size_t pages = 0;
	std::uint32_t current = address;
	for (std::size_t left = size; left != 0U;) {
		if (pages == hosts.size())
			return raise_fatal();
		const std::size_t count = std::min(left, page_remaining(current));
		std::byte *host = mem.translate(current, MemoryAccess::write);
		if (host == nullptr)
			return raise_fault(current, MemoryAccess::write);
		hosts[pages] = host;
		counts[pages] = count;
		++pages;
		left -= count;
		current += static_cast<std::uint32_t>(count);
	}
	const auto *in = static_cast<const std::byte *>(source);
	for (std::size_t page = 0; page < pages; ++page) {
		copy_out(hosts[page], in, counts[page]);
		mem.stored(hosts[page], counts[page]);
		in += counts[page];
	}
	return true;
}

bool Exec::write_words(std::uint32_t address, const std::uint32_t *words,
	unsigned count) noexcept
{
	unsigned done = 0;
	while (done < count) {
		const std::uint32_t current = address + 4U * done;
		std::byte *host = mem.translate(current, MemoryAccess::write);
		if (host == nullptr)
			return raise_fault(current, MemoryAccess::write);
		const unsigned in_page = static_cast<unsigned>(
			std::min<std::size_t>(count - done, page_remaining(current) / 4U));
		for (unsigned index = 0; index < in_page; ++index)
			copy_out(host + 4U * index, words + done + index, 4);
		mem.stored(host, 4U * in_page);
		done += in_page;
	}
	return true;
}

std::byte *Exec::atomic_pointer(std::uint32_t address, unsigned size,
	MemoryAccess access) noexcept
{
	std::byte *host = mem.translate(address, access);
	if (host == nullptr) {
		(void)raise_fault(address, access);
		return nullptr;
	}
	if ((reinterpret_cast<std::uintptr_t>(host) & (size - 1U)) != 0U) {
		(void)raise_fatal();
		return nullptr;
	}
	return host;
}

[[nodiscard]] bool covered(const FormInfo &form, const OpInfo &info) noexcept
{
	return form.availability == Availability::implemented && info.op != Op::none;
}

namespace {

/* The family that executes an operation. */
bool dispatch(Exec &x, const OpInfo &info)
{
	switch (info.op) {
	case Op::none:
	case Op::library:
		return x.raise_fatal();
	case Op::adc: case Op::add: case Op::addw: case Op::adr: case Op::and_:
	case Op::bic: case Op::cmn: case Op::cmp: case Op::eor: case Op::mov:
	case Op::movw: case Op::movt: case Op::mvn: case Op::orn: case Op::orr:
	case Op::rsb: case Op::rsc: case Op::sbc: case Op::sub: case Op::subw:
	case Op::teq: case Op::tst:
		return exec_data_processing(x, info);
	case Op::bfc: case Op::bfi: case Op::sbfx: case Op::ubfx: case Op::clz:
	case Op::rbit: case Op::rev: case Op::rev16: case Op::revsh:
	case Op::sxtb: case Op::sxth: case Op::uxtb: case Op::uxth:
	case Op::sxtab: case Op::sxtah: case Op::uxtab: case Op::uxtah:
	case Op::sxtb16: case Op::uxtb16: case Op::sxtab16: case Op::uxtab16:
	case Op::pkh: case Op::sel:
		return exec_bits_and_extend(x, info);
	case Op::mul: case Op::mla: case Op::mls: case Op::umull: case Op::umlal:
	case Op::smull: case Op::smlal: case Op::umaal: case Op::sdiv:
	case Op::udiv: case Op::smla_xy: case Op::smul_xy: case Op::smlal_xy:
	case Op::smlaw: case Op::smulw: case Op::smlad: case Op::smlsd:
	case Op::smuad: case Op::smusd: case Op::smlald: case Op::smlsld:
	case Op::smmla: case Op::smmls: case Op::smmul: case Op::usad8:
	case Op::usada8:
		return exec_multiply(x, info);
	case Op::qadd: case Op::qsub: case Op::qdadd: case Op::qdsub:
	case Op::ssat: case Op::usat: case Op::ssat16: case Op::usat16:
	case Op::parallel:
		return exec_saturate_and_parallel(x, info);
	case Op::b: case Op::bl: case Op::blx: case Op::bx: case Op::cbz:
	case Op::cbnz: case Op::tb:
		return exec_branch(x, info);
	case Op::ldr: case Op::ldrb: case Op::ldrh: case Op::ldrsb:
	case Op::ldrsh: case Op::ldrd: case Op::str: case Op::strb:
	case Op::strh: case Op::strd: case Op::ldrt: case Op::ldrbt:
	case Op::ldrht: case Op::ldrsbt: case Op::ldrsht: case Op::strt:
	case Op::strbt: case Op::strht: case Op::prefetch:
		return exec_single_transfer(x, info);
	case Op::ldm: case Op::stm:
		return exec_multiple_transfer(x, info);
	case Op::ldrex: case Op::ldrexb: case Op::ldrexh: case Op::ldrexd:
	case Op::strex: case Op::strexb: case Op::strexh: case Op::strexd:
	case Op::ldaex: case Op::ldaexb: case Op::ldaexh: case Op::ldaexd:
	case Op::stlex: case Op::stlexb: case Op::stlexh: case Op::stlexd:
	case Op::lda: case Op::ldab: case Op::ldah: case Op::stl: case Op::stlb:
	case Op::stlh: case Op::clrex:
		return exec_exclusive(x, info);
	case Op::svc: case Op::bkpt: case Op::udf: case Op::mrs: case Op::msr:
	case Op::coprocessor: case Op::nop: case Op::dmb: case Op::dsb:
	case Op::isb: case Op::it:
		return exec_system(x, info);
	case Op::vldr: case Op::vstr: case Op::vldm: case Op::vstm: case Op::vld:
	case Op::vst:
		return exec_vector_memory(x, info);
	case Op::vector:
		return info.vector(x);
	}
	return x.raise_fatal();
}

} // namespace

} // namespace detail

bool a32_semantics_covers(Form form) noexcept
{
	if (form == Form::Undefined || form == Form::ReservedHint)
		return true;
	return detail::covered(kForms[static_cast<std::size_t>(form)],
		detail::op_info(form));
}

StepResult execute_a32(CpuState &cpu, const Instruction &instruction,
	A32Memory &memory) noexcept
{
	using namespace detail;
#if UML_A32_LIBRARY_EXIT_COUNTERS
	note_library_exit(instruction.form);
#endif
	StepResult fatal;
	fatal.event = StepEvent::fatal;
	if (!key_is_aarch32(cpu.pc)) [[unlikely]]
		return fatal;
	if (instruction.status == Status::undefined) [[unlikely]]
		return StepResult{.event = StepEvent::undefined};
	const FormInfo &form = instruction.info();
	const OpInfo &info = op_info(instruction.form);
	/* A reserved hint is a pseudo-form that retires as a NOP; every other
	 * form is a table row, which the library covers when it has an
	 * operation for it. */
	if (instruction.form != Form::ReservedHint && !covered(form, info)) [[unlikely]]
		return fatal;

	Exec x(cpu, instruction, memory);
	const bool completed = instruction.status == Status::nop
		|| (form.conditional && !x.condition_passed()) || dispatch(x, info);
	if (!completed)
		return x.result;
	if (!x.pc_written)
		cpu.pc = x.thumb ? t32_key(x.next_pc()) : a32_key(x.next_pc());
	cpu.a32_itstate = instruction.next_it.bits();
	return x.result;
}

StepResult step_a32(CpuState &cpu, A32Memory &memory, const Profile &profile) noexcept
{
	using ::uml::memory::MemoryAccess;
	StepResult result;
	const std::uint64_t key = cpu.pc;
	if (!key_is_aarch32(key)) {
		result.event = StepEvent::fatal;
		return result;
	}
	const std::uint32_t pc = key_pc(key);
	if (!key_is_canonical(key)) {
		result.event = StepEvent::pc_alignment;
		result.access = MemoryAccess::execute;
		result.fault_address = pc;
		return result;
	}
	const auto fetch = [&](std::uint32_t address, void *out, std::size_t size) {
		const std::byte *host = memory.translate(address, MemoryAccess::execute);
		if (host == nullptr) {
			result.event = StepEvent::memory_fault;
			result.access = MemoryAccess::execute;
			result.fault_address = address;
			return false;
		}
		std::memcpy(out, host, size);
		return true;
	};
	Instruction instruction;
	if (!key_is_thumb(key)) {
		std::uint32_t word = 0;
		if (!fetch(pc, &word, 4))
			return result;
		instruction = decode_a32(word, profile);
	} else {
		std::uint16_t first = 0;
		if (!fetch(pc, &first, 2))
			return result;
		std::uint16_t second = 0;
		const bool wide = thumb_length(first) == 4U;
		if (wide && !fetch(pc + 2U, &second, 2))
			return result;
		instruction = decode_thumb(
			wide ? std::uint32_t{first} << 16 | second : std::uint32_t{first} << 16,
			ItState{cpu.a32_itstate}, profile);
	}
	return execute_a32(cpu, instruction, memory);
}

} // namespace uml::cpu::arm64::aarch32
