// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "kuser": every helper of the kernel's user-space page, called from
 * A32 and from T32 with a known register and flag state, the contract on
 * which registers they clobber, the fault a bad pointer raises inside one, and
 * the page's protection. */
#include "engine_probe.hpp"

extern "C" {
probe::u32 ku_cmpxchg_arm(probe::u32 old_value, probe::u32 new_value, probe::u32 *pointer,
			  probe::u32 *carry);
probe::u32 ku_cmpxchg_thumb(probe::u32 old_value, probe::u32 new_value, probe::u32 *pointer,
			    probe::u32 *carry);
probe::u32 ku_cmpxchg64_arm(probe::u64 *old_value, probe::u64 *new_value, probe::u64 *pointer,
			    probe::u32 *carry);
probe::u32 ku_cmpxchg64_thumb(probe::u64 *old_value, probe::u64 *new_value, probe::u64 *pointer,
			      probe::u32 *carry);
void ku_preserves_arm(probe::u32 helper, probe::u32 *out, probe::u32 r0, probe::u32 r1);
void ku_preserves_thumb(probe::u32 helper, probe::u32 *out, probe::u32 r0, probe::u32 r1);
void ku_store(probe::u32 *address);
}

namespace probe {
namespace {

constexpr word ku_version = 0xffff0ffc;
constexpr u32 ku_get_tls = 0xffff0fe0;
constexpr u32 ku_cmpxchg = 0xffff0fc0;
constexpr u32 ku_barrier = 0xffff0fa0;
constexpr word page_start = 0xffff0000;

word read_tpidruro()
{
	word value;
	asm volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
	return value;
}

word fault_pc, fault_address;
u32 fault_count;

/* Records a fault and returns to the caller of the helper. */
void return_handler(int, SigInfo *info, UContext *context)
{
	++fault_count;
	fault_pc = context->mcontext.pc;
	fault_address = info->address;
	context->mcontext.pc = context->mcontext.lr & ~1U;
	context->mcontext.cpsr = (context->mcontext.lr & 1U) ?
		(context->mcontext.cpsr | (1U << 5)) : (context->mcontext.cpsr & ~(1U << 5));
}

const char *labelled(char *buffer, const char *prefix, const char *text)
{
	word length = 0;
	for (const char *p = prefix; *p; ++p)
		buffer[length++] = *p;
	for (const char *p = text; *p; ++p)
		buffer[length++] = *p;
	buffer[length] = 0;
	return buffer;
}

} // namespace

void group_kuser()
{
	char name[128];
	const u32 version = *reinterpret_cast<volatile u32 *>(ku_version);
	check(version >= 5, "__kuser_helper_version names cmpxchg64 (5 or more)");

	/* get_tls returns TPIDRURO, set by the set_tls system call. */
	check_eq(static_cast<u32>(sys(nr_set_tls, 0x12345678)), 0, "set_tls");
	check_eq(read_tpidruro(), 0x12345678, "TPIDRURO holds what set_tls set");

	struct {
		void (*routine)(u32, u32 *, u32, u32);
		const char *isa;
		bool thumb;
	} callers[] = {
		{ku_preserves_arm, "A32: ", false},
		{ku_preserves_thumb, "T32: ", true},
	};
	for (auto &caller : callers) {
		u32 out[24] = {};
		caller.routine(ku_get_tls, out, 0xdead0001, 0xdead0002);
		check_eq(out[10], 0x12345678, labelled(name, caller.isa, "get_tls returns TPIDRURO"));
		bool preserved = true;
		for (unsigned index = 0; index < 8; ++index)
			preserved = preserved && out[index] == (index + 4) * 0x11111111U;
		preserved = preserved && out[8] == 0xdead0002 && out[9] == addr(&out[16]);
		check(preserved, labelled(name, caller.isa, "get_tls clobbers no register"));
		check_eq(out[11] & 0xf0000000U, 0xa0000000U,
			 labelled(name, caller.isa, "get_tls leaves the flags"));

		u32 barrier[24] = {};
		caller.routine(ku_barrier, barrier, 0xdead0001, 0xdead0002);
		preserved = barrier[10] == 0xdead0001 && barrier[8] == 0xdead0002;
		for (unsigned index = 0; index < 8; ++index)
			preserved = preserved && barrier[index] == (index + 4) * 0x11111111U;
		check(preserved, labelled(name, caller.isa, "memory_barrier clobbers no register"));
		check_eq(barrier[11] & 0xf0000000U, 0xa0000000U,
			 labelled(name, caller.isa, "memory_barrier leaves the flags"));

		/* cmpxchg: success then failure; r3, ip and the flags may change. */
		u32 swap[24] = {};
		swap[16] = 0x1000;
		caller.routine(ku_cmpxchg, swap, 0x1000, 0x2000);
		check_eq(swap[10], 0, labelled(name, caller.isa, "cmpxchg returns 0 when it swaps"));
		check_eq(swap[16], 0x2000, labelled(name, caller.isa, "cmpxchg stores the new value"));
		preserved = swap[8] == 0x2000 && swap[9] == addr(&swap[16]);
		for (unsigned index = 0; index < 8; ++index)
			preserved = preserved && swap[index] == (index + 4) * 0x11111111U;
		check(preserved, labelled(name, caller.isa, "cmpxchg preserves r1, r2 and r4-r11"));
		check_eq((swap[11] >> 29) & 1, 1, labelled(name, caller.isa, "cmpxchg sets C on success"));
		swap[16] = 0x3000;
		caller.routine(ku_cmpxchg, swap, 0x1000, 0x2000);
		check(swap[10] != 0, labelled(name, caller.isa, "cmpxchg returns nonzero when it does not swap"));
		check_eq(swap[16], 0x3000, labelled(name, caller.isa, "cmpxchg leaves a different value"));
		check_eq((swap[11] >> 29) & 1, 0, labelled(name, caller.isa, "cmpxchg clears C on failure"));
	}

	/* The plain wrappers: success and failure from each instruction set. */
	u32 (*const swaps[])(u32, u32, u32 *, u32 *) = {ku_cmpxchg_arm, ku_cmpxchg_thumb};
	for (unsigned index = 0; index < 2; ++index) {
		const char *isa = index ? "T32: " : "A32: ";
		u32 value = 7, carry = 9;
		check_eq(swaps[index](7, 8, &value, &carry), 0, labelled(name, isa, "cmpxchg(7 -> 8) swaps"));
		check_eq(value, 8, labelled(name, isa, "the word holds 8"));
		check_eq(carry, 1, labelled(name, isa, "and C is set"));
		check(swaps[index](7, 9, &value, &carry) != 0, labelled(name, isa, "cmpxchg(7 -> 9) of 8 fails"));
		check_eq(value, 8, labelled(name, isa, "the word still holds 8"));
		check_eq(carry, 0, labelled(name, isa, "and C is clear"));
	}

	/* cmpxchg64 with distinct halves, from each instruction set. */
	u32 (*const swaps64[])(u64 *, u64 *, u64 *, u32 *) = {ku_cmpxchg64_arm, ku_cmpxchg64_thumb};
	for (unsigned index = 0; index < 2; ++index) {
		const char *isa = index ? "T32: " : "A32: ";
		alignas(8) u64 value = 0x1122334455667788ULL;
		alignas(8) u64 expected = 0x1122334455667788ULL;
		alignas(8) u64 replacement = 0x99aabbccddeeff00ULL;
		u32 carry = 9;
		check_eq(swaps64[index](&expected, &replacement, &value, &carry), 0,
			 labelled(name, isa, "cmpxchg64 swaps"));
		check_eq64(value, 0x99aabbccddeeff00ULL, labelled(name, isa, "cmpxchg64 stores 64 bits"));
		check_eq(carry, 1, labelled(name, isa, "cmpxchg64 sets C"));
		expected = 0x1122334455667788ULL;
		replacement = 0x0102030405060708ULL;
		check(swaps64[index](&expected, &replacement, &value, &carry) != 0,
		      labelled(name, isa, "cmpxchg64 does not swap a different value"));
		check_eq64(value, 0x99aabbccddeeff00ULL, labelled(name, isa, "and leaves it"));
		check_eq(carry, 0, labelled(name, isa, "cmpxchg64 clears C"));
		/* A difference in only the high or only the low half is a difference. */
		expected = 0x99aabbccddeeff01ULL;
		check(swaps64[index](&expected, &replacement, &value, &carry) != 0,
		      labelled(name, isa, "cmpxchg64 compares the low half"));
		expected = 0x99aabbcddeeeff00ULL;
		check(swaps64[index](&expected, &replacement, &value, &carry) != 0,
		      labelled(name, isa, "cmpxchg64 compares the high half"));
		check_eq64(value, 0x99aabbccddeeff00ULL, labelled(name, isa, "and the word is untouched"));
	}

	/* A bad pointer faults inside the helper, at the pointer. */
	fault_count = 0;
	set_handler(sigsegv, return_handler);
	u32 carry = 0;
	ku_cmpxchg_arm(1, 2, ptr<u32>(0x20), &carry);
	reset_handler(sigsegv);
	check_eq(fault_count, 1, "cmpxchg on an unmapped word raises SIGSEGV");
	check_eq(fault_address, 0x20, "si_addr is the pointer");
	if (!under_qemu())
		check(fault_pc >= ku_cmpxchg && fault_pc < page_start + 0x1000,
		      "and the fault is inside the helper's code");

	/* The page is read-only. */
	catch_signal(sigsegv, 4);
	ku_store(ptr<u32>(ku_cmpxchg));
	release_signal(sigsegv);
	check_eq(caught.count, 1, "a store to the kuser page raises SIGSEGV");
	check_eq(static_cast<u32>(caught.code), 2, "si_code is SEGV_ACCERR");
	check_eq(caught.address, ku_cmpxchg, "si_addr is the address");
	if (!under_qemu()) {
		word start = 0, end = 0;
		check(find_mapping("[vectors]", &start, &end) && start == page_start,
		      "/proc/self/maps names [vectors] at 0xffff0000");
	}
}

} // namespace probe
