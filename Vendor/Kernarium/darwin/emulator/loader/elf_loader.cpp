/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "elf_loader.h"
#include "kernel_resources.hpp"
#include "session_record.hpp"
#include "kernel_signal_scope.hpp"
#include "kernel_threads.hpp"
#include "uml_elf64.h"

#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

struct LoadedSegment final {
	uintptr_t begin{};
	uintptr_t end{};
	uint32_t flags{};
};

struct DynamicInfo final {
	const uml_elf64_sym *symtab{};
	size_t symbol_count{};
	const char *strtab{};
	size_t strsz{};
	const uml_elf64_rela *rela{};
	size_t rela_count{};
	const uml_elf64_rela *jmprel{};
	size_t jmprel_count{};
	size_t syment{};
	size_t relaent{};
};

/* An image placed inside the host's window reservation goes back to that
 * reservation rather than to the process: the window outlives the run, and a
 * range freed inside it is where Darwin puts the next unhinted allocation.
 * Signed text an earlier load left in place stays (reserve_kernel_image). */
[[nodiscard]] void *reserve_in_place(void *address, size_t length) noexcept
{
	return ::uml::runtime::reserve_kernel_image(address, length) == 0 ?
		address : MAP_FAILED;
}

class Reservation final {
public:
	Reservation() = default;
	Reservation(void *address, size_t length, bool in_window) noexcept
		: address_{address}, length_{length}, in_window_{in_window}
	{
	}

	/* The image's last pages are guest RAM, which a host-service view may
	 * still read after the run; the runtime keeps them while one does. */
	~Reservation()
	{
		::uml::runtime::release_kernel_image(address_, length_, in_window_);
	}

	Reservation(const Reservation &) = delete;
	Reservation &operator=(const Reservation &) = delete;

	[[nodiscard]] void *get() const noexcept { return address_; }

private:
	void *address_{MAP_FAILED};
	size_t length_{};
	bool in_window_{};
};

[[nodiscard]] bool checked_add(uintptr_t left, uint64_t right,
	uintptr_t &result) noexcept
{
	if (right > std::numeric_limits<uintptr_t>::max() - left)
		return false;
	result = left + static_cast<uintptr_t>(right);
	return true;
}

[[nodiscard]] bool checked_add_signed(uintptr_t left, int64_t right,
	uintptr_t &result) noexcept
{
	if (right >= 0)
		return checked_add(left, static_cast<uint64_t>(right), result);
	const auto magnitude = static_cast<uint64_t>(-(right + 1)) + 1U;
	if (magnitude > left)
		return false;
	result = left - static_cast<uintptr_t>(magnitude);
	return true;
}

[[nodiscard]] uintptr_t align_down(uintptr_t value,
	uintptr_t alignment) noexcept
{
	return value & ~(alignment - 1U);
}

[[nodiscard]] bool align_up(uintptr_t value, uintptr_t alignment,
	uintptr_t &result) noexcept
{
	if (value > std::numeric_limits<uintptr_t>::max() - (alignment - 1U))
		return false;
	result = (value + alignment - 1U) & ~(alignment - 1U);
	return true;
}

[[nodiscard]] bool range_inside(size_t image_size, uint64_t offset,
	uint64_t length) noexcept
{
	return offset <= image_size && length <= image_size - offset;
}

/* iOS makes the alias permanent once its code-signing monitor has associated
 * it: no mapping can replace it again, and munmap would only take its access.
 * It is kept for the life of the process, and a later load of the same image,
 * as after the guest restarts in place, reuses it; the final protection pass
 * fails the load if it has lost its access. macOS could replace the alias,
 * and keeps it the same way, so that a restart takes the path iOS takes. */
[[nodiscard]] int remap_signed_executable(uintptr_t destination,
	size_t length, const unsigned char *source)
{
	if (::uml::runtime::signed_kernel_text_kept(
		    reinterpret_cast<const void *>(destination), length))
		return 0;
	vm_address_t target = destination;
	vm_prot_t current_protection = VM_PROT_NONE;
	vm_prot_t maximum_protection = VM_PROT_NONE;
	const kern_return_t result = vm_remap(mach_task_self(), &target,
		length, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
		reinterpret_cast<vm_address_t>(source), FALSE,
		&current_protection, &maximum_protection, VM_INHERIT_NONE);
	if (result == KERN_SUCCESS && target == destination) {
		if (!::uml::runtime::keep_signed_kernel_text(
			    reinterpret_cast<void *>(destination), length))
			fprintf(stderr,
				"UML ELF: cannot keep signed kernel text at %#lx; the "
				"guest cannot restart in place\n",
				static_cast<unsigned long>(destination));
		/* A panic rewrites the kernel's frames to the carrier bytes this
		 * range reads from, which the crash report can symbolize. A restart
		 * finds the alias kept and this record in place. */
		(void)::uml::runtime::record_kernel_text_alias(
			reinterpret_cast<const void *>(destination), length, source);
	}
	if (result != KERN_SUCCESS || target != destination ||
	    (current_protection & (VM_PROT_READ | VM_PROT_EXECUTE)) !=
		(VM_PROT_READ | VM_PROT_EXECUTE) ||
	    (maximum_protection & VM_PROT_EXECUTE) == 0) {
		fprintf(stderr,
			"UML ELF: cannot alias signed kernel text at %#lx: %s\n",
			static_cast<unsigned long>(destination),
			mach_error_string(result));
		return -1;
	}
	return 0;
}

/* Whether an executable segment covers the host page, which is then an alias
 * of the signed carrier. */
[[nodiscard]] bool signed_text_page(const std::vector<LoadedSegment> &segments,
	uintptr_t page, size_t page_size) noexcept
{
	return std::ranges::any_of(segments, [&](const LoadedSegment &segment) {
		uintptr_t segment_end;
		return (segment.flags & UML_PF_X) != 0U &&
			page >= align_down(segment.begin, page_size) &&
			align_up(segment.end, page_size, segment_end) &&
			page < segment_end;
	});
}

[[nodiscard]] bool loaded_range_inside(
	const std::vector<LoadedSegment> &segments, uintptr_t address,
	size_t length, uint32_t required_flags = 0) noexcept
{
	uintptr_t end;
	if (!checked_add(address, length, end))
		return false;
	return std::ranges::any_of(segments, [&](const LoadedSegment &segment) {
		return address >= segment.begin && end <= segment.end &&
			(segment.flags & required_flags) == required_flags;
	});
}

[[nodiscard]] size_t loaded_bytes_available(
	const std::vector<LoadedSegment> &segments, uintptr_t address) noexcept
{
	for (const auto &segment : segments)
		if (address >= segment.begin && address < segment.end)
			return segment.end - address;
	return 0;
}

[[nodiscard]] int segment_protection(uint32_t flags) noexcept
{
	int protection = 0;
	if ((flags & UML_PF_R) != 0U)
		protection |= PROT_READ;
	if ((flags & UML_PF_W) != 0U)
		protection |= PROT_WRITE;
	if ((flags & UML_PF_X) != 0U)
		protection |= PROT_EXEC;
	return protection;
}

[[nodiscard]] int reject_writable_executable_regions(
	uintptr_t minimum, uintptr_t maximum)
{
	vm_address_t cursor = minimum;
	for (;;) {
		vm_address_t address = cursor;
		vm_size_t size = 0;
		vm_region_basic_info_data_64_t info = {};
		mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		const kern_return_t result = vm_region_64(
			mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
			reinterpret_cast<vm_region_info_t>(&info), &count, &object);

		if (object != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), object);
		if (result == KERN_INVALID_ADDRESS || address >= maximum)
			return 0;
		if (result != KERN_SUCCESS || size == 0) {
			fprintf(stderr,
				"UML ELF: cannot audit Darwin VM protections: %s\n",
				mach_error_string(result));
			return -1;
		}
		if ((info.protection & (VM_PROT_WRITE | VM_PROT_EXECUTE)) ==
		    (VM_PROT_WRITE | VM_PROT_EXECUTE)) {
			fprintf(stderr,
				"UML ELF: writable-executable Darwin VM region at %#lx\n",
				static_cast<unsigned long>(address));
			return -1;
		}
		if (address > UINTPTR_MAX - size || address + size >= maximum)
			return 0;
		cursor = address + size;
	}
}

[[nodiscard]] void *resolve_symbol(const char *name)
{
	/* Every thread the kernel starts is recorded before it runs, so that the
	 * host can hold them all (kernel_threads.hpp). */
	if (strcmp(name, "pthread_create") == 0 ||
	    strcmp(name, "_pthread_create") == 0)
		return reinterpret_cast<void *>(&uml::runtime::kernel_pthread_create);
	/* The resources the kernel creates are recorded, so that a guest which
	 * restarts itself can give them back (kernel_resources.hpp). */
	if (void *const wrapper = uml::runtime::kernel_resource_import(name))
		return wrapper;
	/* The kernel's console and diagnostic output, which a session record
	 * quotes (session_record.hpp). */
	if (void *const wrapper = uml::runtime::kernel_output_import(name))
		return wrapper;
	void *address = dlsym(RTLD_DEFAULT, name);
	/* Darwin SDK headers give a few libc entry points an asm-label alias. */
	if (address == nullptr && name[0] == '_')
		address = dlsym(RTLD_DEFAULT, name + 1);
	return address;
}

[[nodiscard]] bool runtime_address(uintptr_t bias, uint64_t value,
	uintptr_t &address) noexcept
{
	return checked_add(bias, value, address);
}

[[nodiscard]] int load_dynamic_info(uintptr_t bias,
	const uml_elf64_dyn *dynamic, size_t dynamic_count,
	const std::vector<LoadedSegment> &segments, DynamicInfo &info)
{
	uint64_t symtab_address = 0;
	uint64_t strtab_address = 0;
	uint64_t rela_address = 0;
	uint64_t jmprel_address = 0;
	uint64_t relasz = 0;
	uint64_t pltrelsz = 0;
	bool terminated = false;
	bool saw_pltrel = false;

	for (size_t index = 0; index < dynamic_count; ++index) {
		const auto &entry = dynamic[index];
		if (entry.tag == UML_DT_NULL) {
			terminated = true;
			break;
		}
		switch (entry.tag) {
		case UML_DT_SYMTAB:
			symtab_address = entry.un.pointer;
			break;
		case UML_DT_STRTAB:
			strtab_address = entry.un.pointer;
			break;
		case UML_DT_STRSZ:
			info.strsz = entry.un.value;
			break;
		case UML_DT_SYMENT:
			info.syment = entry.un.value;
			break;
		case UML_DT_RELA:
			rela_address = entry.un.pointer;
			break;
		case UML_DT_RELASZ:
			relasz = entry.un.value;
			break;
		case UML_DT_RELAENT:
			info.relaent = entry.un.value;
			break;
		case UML_DT_JMPREL:
			jmprel_address = entry.un.pointer;
			break;
		case UML_DT_PLTRELSZ:
			pltrelsz = entry.un.value;
			break;
		case UML_DT_PLTREL:
			saw_pltrel = true;
			if (entry.un.value != UML_DT_RELA_VALUE) {
				fprintf(stderr, "UML ELF: non-RELA PLT relocations\n");
				return -1;
			}
			break;
		case UML_DT_RELR:
		case UML_DT_RELRSZ:
		case UML_DT_RELRENT:
			fprintf(stderr, "UML ELF: packed RELR is not supported\n");
			return -1;
		default:
			break;
		}
	}
	if (!terminated) {
		fprintf(stderr, "UML ELF: unterminated dynamic table\n");
		return -1;
	}
	if (info.relaent == 0)
		info.relaent = sizeof(uml_elf64_rela);
	if (info.syment == 0)
		info.syment = sizeof(uml_elf64_sym);
	if (info.relaent != sizeof(uml_elf64_rela) ||
	    info.syment != sizeof(uml_elf64_sym) ||
	    relasz % info.relaent != 0 || pltrelsz % info.relaent != 0) {
		fprintf(stderr, "UML ELF: unexpected dynamic entry size\n");
		return -1;
	}
	if (pltrelsz != 0 && !saw_pltrel) {
		fprintf(stderr, "UML ELF: PLT relocation encoding is unspecified\n");
		return -1;
	}

	auto assign_table = [&](uint64_t virtual_address, uint64_t byte_size,
		auto *&table, size_t &count) -> bool {
		if (byte_size == 0) {
			table = nullptr;
			count = 0;
			return true;
		}
		uintptr_t address;
		if (virtual_address == 0 || byte_size > SIZE_MAX ||
		    !runtime_address(bias, virtual_address, address) ||
		    address % alignof(std::remove_pointer_t<
			    std::remove_reference_t<decltype(table)>>) != 0 ||
		    !loaded_range_inside(segments, address,
			static_cast<size_t>(byte_size)))
			return false;
		table = reinterpret_cast<std::remove_reference_t<decltype(*table)> *>(
			address);
		count = static_cast<size_t>(byte_size) / sizeof(*table);
		return true;
	};

	if (!assign_table(rela_address, relasz, info.rela, info.rela_count) ||
	    !assign_table(jmprel_address, pltrelsz, info.jmprel,
		info.jmprel_count)) {
		fprintf(stderr, "UML ELF: relocation table is outside loaded segments\n");
		return -1;
	}
	if (info.strsz != 0) {
		uintptr_t address;
		if (strtab_address == 0 ||
		    !runtime_address(bias, strtab_address, address) ||
		    !loaded_range_inside(segments, address, info.strsz)) {
			fprintf(stderr, "UML ELF: string table is outside loaded segments\n");
			return -1;
		}
		info.strtab = reinterpret_cast<const char *>(address);
	}
	if (symtab_address != 0) {
		uintptr_t address;
		if (!runtime_address(bias, symtab_address, address) ||
		    address % alignof(uml_elf64_sym) != 0) {
			fprintf(stderr, "UML ELF: invalid symbol table address\n");
			return -1;
		}
		const auto available = loaded_bytes_available(segments, address);
		if (available < sizeof(uml_elf64_sym)) {
			fprintf(stderr, "UML ELF: symbol table is outside loaded segments\n");
			return -1;
		}
		info.symtab = reinterpret_cast<const uml_elf64_sym *>(address);
		info.symbol_count = available / sizeof(uml_elf64_sym);
	}
	return 0;
}

[[nodiscard]] int apply_relocations(uintptr_t bias,
	const DynamicInfo &info, const uml_elf64_rela *relocations, size_t count,
	const std::vector<LoadedSegment> &segments)
{
	for (size_t index = 0; index < count; ++index) {
		const auto &rela = relocations[index];
		const uint32_t type = UML_ELF64_R_TYPE(rela.info);
		const uint32_t symbol_index = UML_ELF64_R_SYM(rela.info);
		uintptr_t destination_address;
		if (!runtime_address(bias, rela.offset, destination_address) ||
		    destination_address % alignof(uintptr_t) != 0 ||
		    !loaded_range_inside(segments, destination_address,
			sizeof(uintptr_t))) {
			fprintf(stderr, "UML ELF: relocation destination is out of range\n");
			return -1;
		}
		auto *destination = reinterpret_cast<uintptr_t *>(destination_address);
		uintptr_t value;

		if (type == UML_R_AARCH64_RELATIVE) {
			if (!checked_add_signed(bias, rela.addend, value)) {
				fprintf(stderr, "UML ELF: relative relocation overflows\n");
				return -1;
			}
			*destination = value;
			continue;
		}
		if (type != UML_R_AARCH64_ABS64 &&
		    type != UML_R_AARCH64_GLOB_DAT &&
		    type != UML_R_AARCH64_JUMP_SLOT) {
			fprintf(stderr, "UML ELF: unsupported relocation %u\n", type);
			return -1;
		}
		if (info.symtab == nullptr || info.strtab == nullptr ||
		    symbol_index >= info.symbol_count) {
			fprintf(stderr, "UML ELF: relocation has invalid symbol index\n");
			return -1;
		}

		const auto &symbol = info.symtab[symbol_index];
		if (symbol.name >= info.strsz ||
		    memchr(info.strtab + symbol.name, '\0',
			info.strsz - symbol.name) == nullptr) {
			fprintf(stderr, "UML ELF: invalid symbol name\n");
			return -1;
		}
		if (symbol.shndx != UML_SHN_UNDEF) {
			if (!runtime_address(bias, symbol.value, value) ||
			    !loaded_range_inside(segments, value,
				std::max<size_t>(1, symbol.size))) {
				fprintf(stderr, "UML ELF: defined symbol is out of range\n");
				return -1;
			}
		} else {
			const char *name = info.strtab + symbol.name;
			void *resolved = resolve_symbol(name);
			if (resolved == nullptr &&
			    UML_ELF64_ST_BIND(symbol.info) != UML_STB_WEAK) {
				fprintf(stderr, "UML ELF: unresolved Darwin symbol: %s\n",
					name);
				return -1;
			}
			value = reinterpret_cast<uintptr_t>(resolved);
		}
		if (!checked_add_signed(value, rela.addend, value)) {
			fprintf(stderr, "UML ELF: symbol relocation overflows\n");
			return -1;
		}
		*destination = value;
	}
	return 0;
}

[[nodiscard]] int reject_signed_text_relocations(
	uintptr_t bias, const uml_elf64_rela *relocations, size_t count,
	uintptr_t minimum, size_t page_size,
	const std::vector<unsigned char> &page_protections)
{
	for (size_t index = 0; index < count; ++index) {
		uintptr_t destination;
		if (!runtime_address(bias, relocations[index].offset, destination) ||
		    destination < minimum)
			continue;
		const auto page = (destination - minimum) / page_size;
		if (page < page_protections.size() &&
		    (page_protections[page] & PROT_EXEC) != 0) {
			fprintf(stderr,
				"UML ELF: relocation targets immutable signed text\n");
			return -1;
		}
	}
	return 0;
}

[[nodiscard]] int run_elf(const void *image, size_t image_size,
	uintptr_t load_bias, uintptr_t window_start, uintptr_t window_end,
	int argc, char **argv, char **envp)
{
	if (image == nullptr || image_size < sizeof(uml_elf64_ehdr) ||
	    reinterpret_cast<uintptr_t>(image) % alignof(uml_elf64_ehdr) != 0) {
		fprintf(stderr, "UML ELF: invalid AArch64 ET_DYN image\n");
		return 126;
	}
	const auto *bytes = static_cast<const unsigned char *>(image);
	const auto *header = static_cast<const uml_elf64_ehdr *>(image);
	if (header->ident[0] != UML_ELFMAG0 ||
	    header->ident[1] != UML_ELFMAG1 ||
	    header->ident[2] != UML_ELFMAG2 ||
	    header->ident[3] != UML_ELFMAG3 ||
	    header->ident[4] != UML_ELFCLASS64 ||
	    header->ident[5] != UML_ELFDATA2LSB || header->type != UML_ET_DYN ||
	    header->machine != UML_EM_AARCH64 ||
	    header->phentsize != sizeof(uml_elf64_phdr) ||
	    header->phnum == 0 ||
	    header->phoff % alignof(uml_elf64_phdr) != 0 ||
	    !range_inside(image_size, header->phoff,
		static_cast<uint64_t>(header->phnum) * header->phentsize)) {
		fprintf(stderr, "UML ELF: invalid AArch64 ET_DYN image\n");
		return 126;
	}
	const auto *programs = reinterpret_cast<const uml_elf64_phdr *>(
		bytes + header->phoff);
	const size_t page_size = static_cast<size_t>(getpagesize());
	if ((load_bias & (page_size - 1U)) != 0U) {
		fprintf(stderr, "UML ELF: load bias is not host-page aligned\n");
		return 126;
	}
	/* The image is the signed carrier when it is the process's own
	 * __TEXT,__uml_elf section and host-page aligned: its executable pages
	 * are then aliased from it instead of copied. iOS can make no other
	 * code, so it accepts nothing else. A macOS image that is anything else,
	 * such as a test's buffer, is copied into anonymous memory. The machine
	 * check above admits only an AArch64 image: the guest's x86 userspace is
	 * translated by the cpu engines and is never loaded here. */
	const bool carrier =
		::uml::runtime::is_embedded_kernel_image(image, image_size) &&
		reinterpret_cast<uintptr_t>(image) % page_size == 0;
#if DARWIN_UML_SIGNED_KERNEL_CARRIER
	if (!carrier) {
		fprintf(stderr,
			"UML ELF: image does not match the signed kernel carrier\n");
		return 126;
	}
#endif
	uintptr_t minimum = UINTPTR_MAX;
	uintptr_t maximum = 0;
	std::vector<LoadedSegment> segments;
	segments.reserve(header->phnum);

	for (uint16_t index = 0; index < header->phnum; ++index) {
		const auto &program = programs[index];
		if (program.type != UML_PT_LOAD)
			continue;
		uintptr_t segment_begin;
		uintptr_t segment_end;
		uintptr_t page_end;
		if (program.filesz > program.memsz || program.memsz == 0 ||
		    !range_inside(image_size, program.offset, program.filesz) ||
		    !runtime_address(load_bias, program.vaddr, segment_begin) ||
		    !checked_add(segment_begin, program.memsz, segment_end) ||
		    !align_up(segment_end, page_size, page_end)) {
			fprintf(stderr, "UML ELF: invalid load segment\n");
			return 126;
		}
		const auto page_begin = align_down(segment_begin, page_size);
		minimum = std::min(minimum, page_begin);
		maximum = std::max(maximum, page_end);
		segments.push_back({segment_begin, segment_end, program.flags});
	}
	if (segments.empty() || maximum <= minimum ||
	    maximum - minimum > SIZE_MAX) {
		fprintf(stderr, "UML ELF: image has no valid loadable segments\n");
		return 126;
	}
	uintptr_t entry_address;
	if (!runtime_address(load_bias, header->entry, entry_address) ||
	    entry_address % alignof(uint32_t) != 0 ||
	    !loaded_range_inside(segments, entry_address,
		sizeof(uint32_t), UML_PF_X)) {
		fprintf(stderr, "UML ELF: entry point is outside executable segments\n");
		return 126;
	}

	/* The whole ET_DYN image is biased as one unit, preserving the UML
	 * physical/virtual relationships. Inside a window the host has already
	 * reserved, the image replaces that part of the reservation in place.
	 * Otherwise it is placed by a non-fixed hint, which never replaces an
	 * existing host mapping; if Darwin chooses another address, release it
	 * and fail. When the image is the signed carrier, always on iOS,
	 * executable pages are replaced with aliases of its immutable,
	 * code-signed __TEXT pages; no anonymous page is made code. The aliases
	 * stay in the window for the process, and a later load of the image
	 * reuses them.
	 */
	const bool in_window = window_end != 0U;
	if (in_window && (minimum < window_start || maximum > window_end)) {
		fprintf(stderr,
			"UML ELF: kernel image %#llx..%#llx lies outside the "
			"reserved window %#llx..%#llx\n",
			static_cast<unsigned long long>(minimum),
			static_cast<unsigned long long>(maximum),
			static_cast<unsigned long long>(window_start),
			static_cast<unsigned long long>(window_end));
		return 126;
	}
	const size_t reservation_length = maximum - minimum;
	void *mapped = in_window ?
		reserve_in_place(reinterpret_cast<void *>(minimum),
			reservation_length) :
		mmap(reinterpret_cast<void *>(minimum), reservation_length,
			PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	Reservation reservation{mapped, reservation_length, in_window};
	if (mapped == MAP_FAILED) {
		perror("UML ELF mmap");
		return 126;
	}
	if (reinterpret_cast<uintptr_t>(mapped) != minimum) {
		fprintf(stderr,
			"UML ELF: preferred kernel address unavailable: "
			"wanted %#llx..%#llx, Darwin offered %#llx\n",
			static_cast<unsigned long long>(minimum),
			static_cast<unsigned long long>(maximum),
			static_cast<unsigned long long>(
				reinterpret_cast<uintptr_t>(mapped)));
		return 126;
	}

	const size_t page_count = reservation_length / page_size;
	std::vector<unsigned char> page_protections(page_count);
	for (uint16_t index = 0; index < header->phnum; ++index) {
		const auto &program = programs[index];
		if (program.type != UML_PT_LOAD)
			continue;
		uintptr_t start;
		uintptr_t data_end;
		uintptr_t end;
		(void)runtime_address(load_bias, program.vaddr, start);
		(void)checked_add(start, program.memsz, data_end);
		(void)align_up(data_end, page_size, end);
		start = align_down(start, page_size);

		if (carrier && (program.flags & UML_PF_X) != 0U) {
			uintptr_t file_end;
			uintptr_t source_end;
			if ((program.flags & UML_PF_W) != 0U ||
			    program.filesz != program.memsz ||
			    (program.offset & (page_size - 1U)) !=
				(program.vaddr & (page_size - 1U)) ||
			    !checked_add(static_cast<uintptr_t>(program.offset),
				program.filesz, file_end) ||
			    !align_up(file_end, page_size, source_end)) {
				fprintf(stderr,
					"UML ELF: signed text segment is not immutable\n");
				return 126;
			}
			const auto source_begin = align_down(
				static_cast<uintptr_t>(program.offset), page_size);
			if (source_end > image_size || source_end - source_begin !=
				end - start ||
			    remap_signed_executable(start, end - start,
				bytes + source_begin) != 0)
				return 126;
		} else {
			uintptr_t write_begin = start;
			uintptr_t write_end = end;
			/* A page shared with signed text is that text's alias, which
			 * holds the same file bytes and can never be made writable,
			 * and a later load of the image finds it already in place:
			 * the segment is written around it. */
			if (carrier) {
				if (signed_text_page(segments, write_begin, page_size))
					write_begin += page_size;
				if (write_end > write_begin &&
				    signed_text_page(segments, write_end - page_size,
					page_size))
					write_end -= page_size;
			}
			if (write_end > write_begin &&
			    mprotect(reinterpret_cast<void *>(write_begin),
				write_end - write_begin, PROT_READ | PROT_WRITE) != 0) {
				perror("UML ELF mprotect load");
				return 126;
			}
			const uintptr_t destination = start +
				(program.vaddr & (page_size - 1U));
			const uintptr_t file_end = destination + program.filesz;
			const uintptr_t copy_begin = std::max(destination, write_begin);
			const uintptr_t copy_end = std::min(file_end, write_end);
			if (copy_begin < copy_end)
				memcpy(reinterpret_cast<void *>(copy_begin),
					bytes + program.offset + (copy_begin - destination),
					copy_end - copy_begin);
			const uintptr_t zero_begin = std::max(file_end, write_begin);
			const uintptr_t zero_end = std::min(data_end, write_end);
			if (zero_begin < zero_end)
				memset(reinterpret_cast<void *>(zero_begin), 0,
					zero_end - zero_begin);
		}

		const auto first_page = (start - minimum) / page_size;
		const auto last_page = (end - minimum) / page_size;
		if (last_page > page_count) {
			fprintf(stderr, "UML ELF: segment page range is invalid\n");
			return 126;
		}
		for (size_t page = first_page; page < last_page; ++page)
			page_protections[page] |= segment_protection(program.flags);
	}

	const uml_elf64_dyn *dynamic = nullptr;
	size_t dynamic_count = 0;
	for (uint16_t index = 0; index < header->phnum; ++index) {
		const auto &program = programs[index];
		if (program.type != UML_PT_DYNAMIC)
			continue;
		if (dynamic != nullptr || program.filesz == 0 ||
		    program.filesz % sizeof(uml_elf64_dyn) != 0 ||
		    program.vaddr % alignof(uml_elf64_dyn) != 0 ||
		    program.filesz > SIZE_MAX) {
			fprintf(stderr, "UML ELF: invalid dynamic segment\n");
			return 126;
		}
		uintptr_t address;
		if (!runtime_address(load_bias, program.vaddr, address) ||
		    !loaded_range_inside(segments, address,
			static_cast<size_t>(program.filesz))) {
			fprintf(stderr, "UML ELF: dynamic segment is outside loaded data\n");
			return 126;
		}
		dynamic = reinterpret_cast<const uml_elf64_dyn *>(address);
		dynamic_count = program.filesz / sizeof(uml_elf64_dyn);
	}
	if (dynamic != nullptr) {
		DynamicInfo info;
		if (load_dynamic_info(load_bias, dynamic, dynamic_count, segments,
			info) != 0
		    || (carrier &&
			(reject_signed_text_relocations(load_bias, info.rela,
			    info.rela_count, minimum, page_size,
			    page_protections) != 0 ||
			 reject_signed_text_relocations(load_bias, info.jmprel,
			    info.jmprel_count, minimum, page_size,
			    page_protections) != 0))
		    || apply_relocations(load_bias, info, info.rela, info.rela_count,
			segments) != 0 ||
		    apply_relocations(load_bias, info, info.jmprel,
			info.jmprel_count,
			segments) != 0)
			return 126;
	}

	const auto entry_page =
		(align_down(entry_address, page_size) - minimum) / page_size;
	for (size_t page = 0; page < page_count; ++page) {
		const uintptr_t address = minimum + page * page_size;
		int protection = page_protections[page];
		/* lld may place tiny .exit.text beside writable init metadata. */
		if ((protection & (PROT_WRITE | PROT_EXEC)) ==
		    (PROT_WRITE | PROT_EXEC))
			protection &= ~PROT_EXEC;
		if (page == entry_page && (protection & PROT_EXEC) == 0) {
			fprintf(stderr,
				"UML ELF: entry page cannot be made executable safely\n");
			return 126;
		}
		if ((protection & PROT_EXEC) != 0)
			sys_icache_invalidate(reinterpret_cast<void *>(address), page_size);
		if (mprotect(reinterpret_cast<void *>(address), page_size,
			protection) != 0) {
			perror("UML ELF mprotect final");
			return 126;
		}
	}
	if (reject_writable_executable_regions(minimum, maximum) != 0)
		return 126;
	const auto entry = reinterpret_cast<int (*)(int, char **, char **)>(
		entry_address);
	/* The kernel leaves its signal handlers and interval timer installed
	 * when it returns. Declared after the reservation, this scope takes them
	 * back before the image they point into is released. */
	const ::uml::runtime::KernelSignalScope kernel_signals{minimum, maximum};
	return entry(argc, argv, envp);
}

} // namespace

extern "C" int uml_elf_run(const void *image, size_t image_size,
	uintptr_t load_bias, uintptr_t window_start, uintptr_t window_end,
	int argc, char **argv, char **envp)
{
	try {
		return run_elf(image, image_size, load_bias, window_start,
			window_end, argc, argv, envp);
	} catch (...) {
		fprintf(stderr, "UML ELF: loader allocation or host failure\n");
		return 126;
	}
}
