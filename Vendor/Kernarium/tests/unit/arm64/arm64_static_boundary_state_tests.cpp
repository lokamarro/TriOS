// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/engine_factory.hpp"
#include "platform/apple/host_features.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_static_mmu.h"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "scratch_backing.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using uml::emulator::tests::JitMapping;
using uml::testing::ScratchBacking;

constexpr std::string_view backing_name = "uml-static-boundary.XXXXXX";

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t pstate(
	unsigned immediate, unsigned op2)
{
	return 0xd500'401fU | (3U << 16) |
		((immediate & 15U) << 8) | ((op2 & 7U) << 5);
}

int enter(darwin_uml_guest_registers &registers,
	uml::testing::GuestFpTail &fp, unsigned long generation)
{
	unsigned long tls = 0x1234'5678UL;
	return darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
}

void fpmr_is_not_guest_state()
{
	/* The virtual CPU implements no FEAT_FP8, so an arm64 guest ignores the
	 * bridge's FPMR slot on entry and reports it as zero. */
	constexpr unsigned long generation = 200;
	constexpr std::uint64_t base = 0x50'0000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.fpmr = 0x0000'003f'ff7f'c1ffULL;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 4);
	require(registers.fpmr == 0);
}

/* The SVL of the register file each vCPU holds, or 0 when this run does
 * not implement SME (the policy is off or the host lacks the profile). */
[[nodiscard]] unsigned long sme_vector_length()
{
	return uml::cpu::arm64::SmeRegisterFile::for_host(
		uml::platform::apple::probed_host_capabilities()).svl();
}

void pstate_survives_boundary()
{
	/* DIT is guest state. The virtual CPU implements no FEAT_SSBS: MSR SSBS
	 * is undefined and the kernel's PSTATE.SSBS passes through untouched.
	 * Without SME, SVCR comes back zero and SMSTART is undefined. When the
	 * run implements SME (the policy on a host with the profile), the
	 * static runtime's gadget engine runs SMSTART at sme_support control
	 * and above: a task without SME storage asks for it and retires
	 * nothing, and with storage the guest SVCR survives the boundary. */
	constexpr unsigned long generation = 201;
	constexpr std::uint64_t base = 0x51'0000;
	constexpr std::uint32_t pstate_ssbs = 0x0000'1000U;
	constexpr std::uint32_t pstate_dit = 0x0100'0000U;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, pstate(1, 2));  // msr dit, #1
	backing.put32(4, 0xd503'201fU);  // nop
	backing.put32(8, 0xd400'0001U);  // svc #0
	backing.put32(12, pstate(0, 1)); // msr ssbs, #0
	backing.put32(16, pstate(7, 3)); // smstart
	backing.put32(20, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.pstate = pstate_ssbs;
	const bool sme = sme_vector_length() != 0;
	const bool smstart = sme &&
		uml::cpu::arm64::gadgets::sme_support !=
			uml::cpu::arm64::SmeSupport::none;
	registers.svcr = sme ? 0 : 3;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 12);
	require((registers.pstate & (pstate_dit | pstate_ssbs)) ==
		(pstate_dit | pstate_ssbs));
	require(registers.svcr == 0);
	require(enter(registers, fp, generation) ==
		DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION);
	require(registers.pc == base + 12);
	require((registers.pstate & pstate_ssbs) == pstate_ssbs);
	registers.pc = base + 16;
	if (!smstart) {
		require(enter(registers, fp, generation) ==
			DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION);
		require(registers.pc == base + 16);
		require(registers.svcr == 0);
		return;
	}
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SME_ACCESS);
	require(registers.pc == base + 16);
	require(registers.svcr == 0);
	const auto svl = sme_vector_length();
	std::vector<std::byte> storage(DARWIN_UML_GUEST_SME_STATE_SIZE(svl));
	fp.sme.state = reinterpret_cast<std::uintptr_t>(storage.data());
	fp.sme.svl = static_cast<std::uint32_t>(svl);
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 24);
	require(registers.svcr == 3);
}

void arm64_tail_is_exact()
{
	/* An arm64 task hands over its FP registers and its SME registers,
	 * DARWIN_UML_ABI_GUEST_ARM64_TAIL_SIZE bytes; any other size is a
	 * kernel built for another bridge. */
	constexpr unsigned long generation = 205;
	constexpr std::uint64_t base = 0x55'0000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	struct {
		uml::testing::GuestFpTail tail;
		std::uint8_t spare[48];
	} fp{};
	for (const unsigned long size : {
		     DARWIN_UML_ABI_GUEST_FP_REGISTERS_SIZE + 0UL,
		     DARWIN_UML_ABI_GUEST_ARM64_TAIL_SIZE - 8UL,
		     DARWIN_UML_ABI_GUEST_ARM64_TAIL_SIZE + 8UL,
		     /* The retired bridge ABI 21 and 22 tail. */
		     DARWIN_UML_ABI_GUEST_ARM64_TAIL_SIZE + 40UL}) {
		darwin_uml_guest_registers registers{};
		unsigned long tls = 0;
		registers.pc = base;
		require(darwin_uml_static_enter_guest(&registers, &fp, size,
			generation, &tls) == DARWIN_UML_EVENT_FATAL);
		require(registers.pc == base);
	}
	darwin_uml_guest_registers registers{};
	registers.pc = base;
	require(enter(registers, fp.tail, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 4);
}

void sme_tail_crosses_boundary()
{
	/* With SME, SVCR, TPIDR2_EL0 and the storage the tail names are the
	 * task's and come back as the kernel handed them, here unread by any
	 * engine. SVCR may name SM or ZA only with storage, storage must be
	 * sized for this run's vector length, and SVCR's other bits stay zero.
	 * Without SME the tail is the kernel's alone: an engine neither reads
	 * nor writes it, and SVCR comes back zero. */
	constexpr unsigned long generation = 206;
	constexpr std::uint64_t base = 0x56'0000;
	constexpr std::uint64_t tpidr2 = 0x1234'5678'9abc'def0ULL;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	const unsigned long svl = sme_vector_length();
	const auto run = [&](std::uint64_t svcr, std::uint64_t state,
		std::uint32_t storage_svl, std::uint64_t &out_svcr,
		std::uint64_t &out_tpidr2) {
		darwin_uml_guest_registers registers{};
		uml::testing::GuestFpTail fp{};
		registers.pc = base;
		registers.svcr = svcr;
		fp.sme.tpidr2 = tpidr2;
		fp.sme.state = state;
		fp.sme.svl = storage_svl;
		const int event = enter(registers, fp, generation);
		out_svcr = registers.svcr;
		out_tpidr2 = fp.sme.tpidr2;
		require(fp.sme.state == state && fp.sme.svl == storage_svl);
		return event;
	};
	std::uint64_t svcr = 0;
	std::uint64_t out_tpidr2 = 0;
	if (svl == 0) {
		require(run(3, 0, 0, svcr, out_tpidr2) ==
			DARWIN_UML_EVENT_SYSCALL);
		require(svcr == 0 && out_tpidr2 == tpidr2);
		return;
	}
	std::vector<std::byte> storage(DARWIN_UML_GUEST_SME_STATE_SIZE(svl));
	for (std::size_t index = 0; index < storage.size(); ++index)
		storage[index] = static_cast<std::byte>(index * 7U);
	/* The FP registers' V own each Z's low 16 bytes, all zero here, so an
	 * engine that loads and stores Z hands back the same bytes. */
	for (unsigned n = 0; n < 32; ++n)
		std::memset(storage.data() + DARWIN_UML_GUEST_SME_Z_OFFSET(svl, n),
			0, 16);
	const auto copy = storage;
	const auto address = reinterpret_cast<std::uint64_t>(storage.data());
	const auto sized = static_cast<std::uint32_t>(svl);
	for (const std::uint64_t mode : {0U, 1U, 2U, 3U}) {
		require(run(mode, address, sized, svcr, out_tpidr2) ==
			DARWIN_UML_EVENT_SYSCALL);
		require(svcr == mode && out_tpidr2 == tpidr2);
		require(storage == copy);
	}
	require(run(0, 0, 0, svcr, out_tpidr2) == DARWIN_UML_EVENT_SYSCALL);
	require(svcr == 0 && out_tpidr2 == tpidr2);
	require(run(2, 0, 0, svcr, out_tpidr2) == DARWIN_UML_EVENT_FATAL);
	require(run(1, address, sized * 2U, svcr, out_tpidr2) ==
		DARWIN_UML_EVENT_FATAL);
	require(run(4, address, sized, svcr, out_tpidr2) ==
		DARWIN_UML_EVENT_FATAL);
}

void exclusive_monitor_is_cleared_at_boundary()
{
	constexpr unsigned long generation = 202;
	constexpr std::uint64_t base = 0x52'0000;
	constexpr std::uint64_t data = base + 0x800;
	constexpr std::uint64_t original = 0x1122'3344'5566'7788ULL;
	constexpr std::uint64_t replacement = 0x8877'6655'4433'2211ULL;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xc85f'7c22U);  // ldxr x2, [x1]
	backing.put32(4, 0xd400'0001U);  // svc #0
	backing.put32(8, 0xc805'fc23U);  // stlxr w5, x3, [x1]
	backing.put32(12, 0xd400'0001U); // svc #0
	backing.put64(0x800, original);
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.x[1] = data;
	registers.x[3] = replacement;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[2] == original);
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[5] == 1);
	require(backing.get64(0x800) == original);
}

void deferred_fp_source_is_refreshed_at_boundary()
{
	constexpr unsigned long generation = 203;
	constexpr std::uint64_t base = 0x53'0000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x4f00'041fU); // movi v31.4s, #0
	backing.put32(4, 0xd400'0001U); // svc #0
	backing.put32(8, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	std::memset(fp.vectors[31], 0xff, sizeof(fp.vectors[31]));

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	for (const auto byte : fp.vectors[31])
		require(byte == 0);
	std::memset(fp.vectors[31], 0x5a, sizeof(fp.vectors[31]));
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	for (const auto byte : fp.vectors[31])
		require(byte == 0x5a);
}

void deferred_fp_source_is_materialized_on_linked_target()
{
	constexpr unsigned long generation = 204;
	constexpr std::uint64_t base = 0x54'0000;
	constexpr std::uint64_t first_value = 0x0123'4567'89ab'cdefULL;
	constexpr std::uint64_t second_value = 0xfedc'ba98'7654'3210ULL;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x1400'0004U);  // b base + 16
	backing.put32(16, 0x9e66'0000U); // fmov x0, d0
	backing.put32(20, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};

	/* Translate and activate the FP target before its integer predecessor. */
	registers.pc = base + 16;
	std::memcpy(fp.vectors[0], &first_value, sizeof(first_value));
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == first_value);

	/* A new entry defers a different kernel frame. The cached predecessor now
	 * chains through the FP prefix and must observe this new source. */
	registers.pc = base;
	std::memcpy(fp.vectors[0], &second_value, sizeof(second_value));
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == second_value);
	require(registers.pc == base + 24);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	fpmr_is_not_guest_state();
	pstate_survives_boundary();
	arm64_tail_is_exact();
	sme_tail_crosses_boundary();
	exclusive_monitor_is_cleared_at_boundary();
	deferred_fp_source_is_refreshed_at_boundary();
	deferred_fp_source_is_materialized_on_linked_target();
}
