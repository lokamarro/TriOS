// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_static_mmu.h"
#include "runtime/execution_engine.hpp"
#include "guest_register_tail.hpp"
#include "scratch_backing.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <span>
#include <sys/mman.h>
#include <unistd.h>

namespace uml::emulator::tests {

[[nodiscard]] inline bool jit_available() noexcept
{
	return ::uml::runtime::execution_engine_available(
		::uml::runtime::ExecutionEngine::jit);
}

using uml::testing::require;

/* Guest pages of scratch backing for the JIT tests. */
class JitBacking final : public ::uml::testing::ScratchBacking {
public:
	explicit JitBacking(std::size_t pages)
		: ScratchBacking{"uml-jit-test.XXXXXX",
			  pages * DARWIN_UML_GUEST_PAGE_SIZE}
	{
	}
};

class JitMapping final {
public:
	JitMapping(unsigned long generation, std::uint64_t base,
		   const ::uml::testing::ScratchBacking &backing,
		   std::size_t pages,
		   int protection = PROT_READ | PROT_WRITE | PROT_EXEC)
		: generation_{generation}
		, protection_{protection}
	{
		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.descriptor(), 0,
			pages * DARWIN_UML_GUEST_PAGE_SIZE, protection, 1,
			(protection & PROT_WRITE) != 0) == 0);
	}

	/* Put the first page of another backing behind one mapped page, as a
	 * walker does after Linux replaces the page a PTE names. */
	void replace_page(std::uint64_t address,
		const ::uml::testing::ScratchBacking &backing)
	{
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(address), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, protection_, 1,
			(protection_ & PROT_WRITE) != 0) == 0);
	}

	~JitMapping()
	{
		require(darwin_uml_static_vm_destroy(generation_) == 0);
	}

	JitMapping(const JitMapping &) = delete;
	JitMapping &operator=(const JitMapping &) = delete;

private:
	unsigned long generation_;
	int protection_;
};

/* `pages` guest pages of `backing` at `address` in one generation, unmapped
 * again when it goes out of scope. Guest code is tracked for changes; data
 * is guest-writable. */
class GenerationMapping final {
public:
	GenerationMapping(unsigned long generation, std::uint64_t address,
		const ::uml::testing::ScratchBacking &backing,
		std::size_t pages, int protection, bool code)
		: generation_{generation}, address_{address},
		  length_{pages * DARWIN_UML_GUEST_PAGE_SIZE}
	{
		require(darwin_uml_static_vm_map_generation(generation_,
			reinterpret_cast<void *>(address_), backing.descriptor(),
			0, length_, protection, code ? 1 : 0, code ? 0 : 1) == 0);
	}

	~GenerationMapping()
	{
		require(darwin_uml_static_vm_unmap_generation(generation_,
			reinterpret_cast<void *>(address_), length_) == 0);
	}

	GenerationMapping(const GenerationMapping &) = delete;
	GenerationMapping &operator=(const GenerationMapping &) = delete;

private:
	unsigned long generation_;
	std::uint64_t address_;
	std::size_t length_;
};

inline int enter_jit(darwin_uml_guest_registers &registers,
		     uml::testing::GuestFpTail &fp,
		     unsigned long generation)
{
	unsigned long tls = 0;
	return darwin_uml_jit_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
}

} // namespace uml::emulator::tests
