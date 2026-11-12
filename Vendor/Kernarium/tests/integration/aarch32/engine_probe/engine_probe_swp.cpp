// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "swp": SWP and SWPB, which the engines raise as undefined
 * instructions and the kernel emulates while abi.swp is 1, as arm64 Linux
 * does. At 0 every form raises SIGILL at the instruction, a failing
 * condition included. At 1 the exchange returns the old contents zero-
 * extended and stores the new ones; every condition holds exactly when it
 * should, a failing one touches nothing, not even an invalid address, and
 * retires the instruction; Rt equal to Rt2 or Rn works. An access that fails
 * raises SIGSEGV with the pc at the instruction, SEGV_ACCERR when any mapping
 * lies at or above the address and SEGV_MAPERR when none does, the address in
 * si_addr, nothing in the frame's fault address, and nothing changed. An
 * unaligned SWP fails the same way and an unaligned SWPB works. The words the
 * kernel does not emulate raise SIGILL: a pc operand, condition 0xf, a
 * non-zero SBZ field. Pages that were never touched are mapped, and a page
 * shared copy-on-write with a parent is copied. */
#include "engine_probe.hpp"

extern "C" {
probe::u32 sw_word(probe::u32 *p, probe::u32 v, probe::u32 sentinel);
probe::u32 sw_byte(probe::u8 *p, probe::u32 v, probe::u32 sentinel);
probe::u32 sw_same(probe::u32 *p, probe::u32 v);
probe::u32 sw_rn_is_rt(probe::u32 *p, probe::u32 v);
probe::u32 sw_rn_is_rt2(probe::u32 *p, probe::u32 v);
extern const char sw_word_insn[], sw_byte_insn[], sw_same_insn[], sw_rn_is_rt_insn[],
	sw_rn_is_rt2_insn[];
#define DECLARE_COND(cc)                                                              \
	probe::u32 sw_c_##cc(probe::u32 flags, probe::u32 *p, probe::u32 v, probe::u32 sentinel); \
	extern const char sw_c_##cc##_insn[]
DECLARE_COND(eq); DECLARE_COND(ne); DECLARE_COND(cs); DECLARE_COND(cc); DECLARE_COND(mi);
DECLARE_COND(pl); DECLARE_COND(vs); DECLARE_COND(vc); DECLARE_COND(hi); DECLARE_COND(ls);
DECLARE_COND(ge); DECLARE_COND(lt); DECLARE_COND(gt); DECLARE_COND(le); DECLARE_COND(al);
#undef DECLARE_COND
probe::u32 sw_cb_eq(probe::u32 flags, probe::u8 *p, probe::u32 v, probe::u32 sentinel);
probe::u32 sw_cb_ne(probe::u32 flags, probe::u8 *p, probe::u32 v, probe::u32 sentinel);
extern const char sw_cb_eq_insn[], sw_cb_ne_insn[];
#define DECLARE_WORD(name) void name(probe::u32 flags); extern const char name##_insn[]
DECLARE_WORD(sw_u_pc_rt); DECLARE_WORD(sw_u_pc_rt2); DECLARE_WORD(sw_u_pc_rn);
DECLARE_WORD(sw_u_pcb_rt); DECLARE_WORD(sw_u_pc_rt_ne); DECLARE_WORD(sw_u_uncond);
DECLARE_WORD(sw_u_sbz); DECLARE_WORD(sw_u_sbz2); DECLARE_WORD(sw_u_bit20);
#undef DECLARE_WORD
}

namespace probe {
namespace {

constexpr u32 sentinel = 0x5e5e5e5e;
constexpr u32 ill_illopc = 1;
constexpr u32 segv_maperr = 1;
constexpr u32 segv_accerr = 2;
constexpr u32 flag_n = 1U << 31;
constexpr u32 flag_z = 1U << 30;
constexpr u32 flag_c = 1U << 29;
constexpr u32 flag_v = 1U << 28;

const char *labelled(char *buffer, const char *prefix, const char *text)
{
	word length = 0;
	for (const char *p = prefix; *p; ++p)
		buffer[length++] = *p;
	buffer[length++] = ':';
	buffer[length++] = ' ';
	for (const char *p = text; *p; ++p)
		buffer[length++] = *p;
	buffer[length] = 0;
	return buffer;
}

/* Whether condition `code` (the encoding's top four bits) holds for flags. */
bool condition_holds(unsigned code, u32 flags)
{
	const bool n = flags & flag_n, z = flags & flag_z, c = flags & flag_c, v = flags & flag_v;
	switch (code) {
	case 0: return z;
	case 1: return !z;
	case 2: return c;
	case 3: return !c;
	case 4: return n;
	case 5: return !n;
	case 6: return v;
	case 7: return !v;
	case 8: return c && !z;
	case 9: return !c || z;
	case 10: return n == v;
	case 11: return n != v;
	case 12: return !z && n == v;
	case 13: return z || n != v;
	default: return true;
	}
}

struct Condition {
	const char *name;
	u32 (*run)(u32, u32 *, u32, u32);
};

#define COND(cc) {#cc, sw_c_##cc}
/* In the order of the encoding: EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE,
 * LT, GT, LE, AL. */
const Condition conditions[] = {
	COND(eq), COND(ne), COND(cs), COND(cc), COND(mi), COND(pl), COND(vs), COND(vc),
	COND(hi), COND(ls), COND(ge), COND(lt), COND(gt), COND(le), COND(al),
};
#undef COND

struct Word {
	const char *name;
	void (*run)(u32);
	const char *insn;
	u32 flags;
};

const Word undefined_words[] = {
	{"swp with the pc as Rt", sw_u_pc_rt, sw_u_pc_rt_insn, 0},
	{"swp with the pc as Rt2", sw_u_pc_rt2, sw_u_pc_rt2_insn, 0},
	{"swp with the pc as Rn", sw_u_pc_rn, sw_u_pc_rn_insn, 0},
	{"swpb with the pc as Rt", sw_u_pcb_rt, sw_u_pcb_rt_insn, 0},
	/* The decode refuses a pc operand whatever the condition says. */
	{"swpne with the pc as Rt, its condition false", sw_u_pc_rt_ne, sw_u_pc_rt_ne_insn, flag_z},
	{"swp with condition 0xf", sw_u_uncond, sw_u_uncond_insn, 0},
	{"swp with bit 8 of its SBZ field set", sw_u_sbz, sw_u_sbz_insn, 0},
	{"swp with bits 11:8 of its SBZ field set", sw_u_sbz2, sw_u_sbz2_insn, 0},
	{"a swp-like word with bit 20 set", sw_u_bit20, sw_u_bit20_insn, 0},
};

/* Runs the words the kernel never emulates and checks each raises SIGILL at
 * its label. */
void undefined_words_raise_sigill()
{
	char label[160];
	for (const Word &word : undefined_words) {
		catch_signal(sigill, 4);
		word.run(word.flags);
		release_signal(sigill);
		check_eq(caught.count, 1, labelled(label, word.name, "raises SIGILL"));
		check_eq(caught.address, addr(word.insn), labelled(label, word.name, "si_addr is the instruction"));
		check_eq(static_cast<u32>(caught.code), ill_illopc, labelled(label, word.name, "si_code is ILL_ILLOPC"));
	}
}

/* abi.swp is 0: every form is UNDEFINED, and a failing condition does not
 * change that. */
void swp_off()
{
	char label[160];
	auto *cell = ptr<u32>(mmap_pages(4096, prot_read | prot_write));
	cell[0] = 0x01020304;

#define EXPECT_SIGILL(text, insn, call)                                                \
	do {                                                                           \
		catch_signal(sigill, 4);                                               \
		(void)(call);                                                          \
		release_signal(sigill);                                                \
		check_eq(caught.count, 1, labelled(label, text, "raises SIGILL"));     \
		check_eq(caught.address, addr(insn), labelled(label, text, "si_addr is the instruction")); \
		check_eq(caught.pc, addr(insn), labelled(label, text, "the frame's pc is the instruction")); \
		check_eq(static_cast<u32>(caught.code), ill_illopc, labelled(label, text, "si_code is ILL_ILLOPC")); \
		check_eq(cell[0], 0x01020304, labelled(label, text, "memory is unchanged")); \
	} while (0)

	EXPECT_SIGILL("abi.swp 0: swp", sw_word_insn, sw_word(cell, 0x99, sentinel));
	EXPECT_SIGILL("abi.swp 0: swpb", sw_byte_insn, sw_byte(reinterpret_cast<u8 *>(cell), 0x99, sentinel));
	EXPECT_SIGILL("abi.swp 0: swp with Rt == Rt2", sw_same_insn, sw_same(cell, 0x99));
	EXPECT_SIGILL("abi.swp 0: swp with Rn == Rt", sw_rn_is_rt_insn, sw_rn_is_rt(cell, 0x99));
	/* A condition that fails still traps: the engines raise the event for the
	 * word, and with the switch off the kernel has nothing to emulate. */
	EXPECT_SIGILL("abi.swp 0: swpne with Z set (its condition false)", sw_c_ne_insn,
		      sw_c_ne(flag_z, cell, 0x99, sentinel));
	EXPECT_SIGILL("abi.swp 0: swpeq with Z set (its condition true)", sw_c_eq_insn,
		      sw_c_eq(flag_z, cell, 0x99, sentinel));
	EXPECT_SIGILL("abi.swp 0: swpbne with Z set (its condition false)", sw_cb_ne_insn,
		      sw_cb_ne(flag_z, reinterpret_cast<u8 *>(cell), 0x99, sentinel));
#undef EXPECT_SIGILL
	undefined_words_raise_sigill();
}

/* Runs `run`, which must raise SIGSEGV at `insn` with `code` and `address`,
 * and checks the rest of what a failed SWP leaves. */
template<typename F>
void swap_faults(const char *text, F run, word address, u32 code, const char *insn)
{
	char label[160];
	catch_signal(sigsegv, 4);
	const u32 result = run();
	release_signal(sigsegv);
	check_eq(caught.count, 1, labelled(label, text, "raises SIGSEGV"));
	check_eq(result, sentinel, labelled(label, text, "retires nothing: Rt keeps its value"));
	check_eq(caught.address, address, labelled(label, text, "si_addr is the address"));
	check_eq(static_cast<u32>(caught.code), code,
		 labelled(label, text, code == segv_accerr ? "si_code is SEGV_ACCERR" : "si_code is SEGV_MAPERR"));
	check_eq(caught.pc, addr(insn), labelled(label, text, "the frame's pc is the instruction"));
	check_eq(caught.fault_address, 0, labelled(label, text, "the frame's fault address is zero"));
}

/* abi.swp is 1. */
void swp_on()
{
	char label[160];
	auto *memory = ptr<u8>(mmap_pages(16384, prot_read | prot_write));
	auto *cell = reinterpret_cast<u32 *>(memory + 64);

	/* The word exchange. */
	cell[-1] = 0x11111111;
	cell[0] = 0x01020304;
	cell[1] = 0x22222222;
	check_eq(sw_word(cell, 0xa5b6c7d8, sentinel), 0x01020304, "swp returns the old word");
	check_eq(cell[0], 0xa5b6c7d8, "swp stores the new word");
	check(cell[-1] == 0x11111111 && cell[1] == 0x22222222, "swp leaves the neighbouring words alone");
	/* Rt == Rt2, which is the form CTS runs. */
	check_eq(sw_same(cell, 0x0badf00d), 0xa5b6c7d8, "swp r0, r0, [Rn] returns the old word");
	check_eq(cell[0], 0x0badf00d, "and stores the register's old value");
	/* Rn == Rt: the address is read before Rt is written. */
	check_eq(sw_rn_is_rt(cell, 0x13572468), 0x0badf00d, "swp Rt, Rt2, [Rt] returns the old word");
	check_eq(cell[0], 0x13572468, "and stores at the address Rt held");
	/* Rn == Rt2: the data is the address. */
	cell[0] = 0x31415926;
	check_eq(sw_rn_is_rt2(cell, 0), 0x31415926, "swp Rt, Rn, [Rn] returns the old word");
	check_eq(cell[0], addr(cell), "and stores the address");

	/* The byte exchange: each byte of a word, zero-extended. */
	for (unsigned offset = 0; offset < 4; ++offset) {
		cell[0] = 0xf5e4d3c2;
		u8 expected[4] = {0xc2, 0xd3, 0xe4, 0xf5};
		const u32 old = sw_byte(memory + 64 + offset, 0x1234abcd, sentinel);
		check_eq(old, expected[offset], "swpb returns the old byte, zero-extended");
		expected[offset] = 0xcd;
		check_mem(cell, expected, 4, "swpb stores the low byte of Rt2 and no other byte");
	}
	/* An unaligned address is fine for SWPB. */
	cell[0] = 0;
	check_eq(sw_byte(memory + 64 + 3, 0x80, sentinel), 0, "swpb at an unaligned address works");

	/* Conditions: all fifteen, with every flag combination. */
	for (unsigned code = 0; code < 15; ++code) {
		u32 wrong = 0;
		for (u32 nibble = 0; nibble < 16; ++nibble) {
			const u32 flags = nibble << 28;
			cell[0] = 0xaaaaaaaa;
			const u32 result = conditions[code].run(flags, cell, 0x12345678, sentinel);
			const bool runs = condition_holds(code, flags);
			if (result != (runs ? 0xaaaaaaaa : sentinel) || cell[0] != (runs ? 0x12345678U : 0xaaaaaaaaU))
				++wrong;
		}
		labelled(label, "swp", conditions[code].name);
		check_eq(wrong, 0, label);
	}
	/* A failing condition makes no access, so an invalid address is no fault. */
	catch_signal(sigsegv, 4);
	const u32 skipped = sw_c_ne(flag_z, ptr<u32>(0xfffff000), 0x99, sentinel);
	release_signal(sigsegv);
	check_eq(caught.count, 0, "swpne with a false condition makes no access to an invalid address");
	check_eq(skipped, sentinel, "and leaves Rt alone");
	{
		cell[0] = 0x0a0b0c0d;
		check_eq(sw_cb_eq(flag_z, memory + 64, 0xee, sentinel), 0x0d, "swpbeq with Z set runs");
		check_eq(cell[0], 0x0a0b0cee, "and stores a byte");
		check_eq(sw_cb_eq(0, memory + 64, 0x55, sentinel), sentinel, "swpbeq with Z clear is skipped");
		check_eq(cell[0], 0x0a0b0cee, "and stores nothing");
		check_eq(sw_cb_ne(flag_z, memory + 64, 0x55, sentinel), sentinel, "swpbne with Z set is skipped");
	}

	/* Failed accesses. */
	cell[0] = 0x01020304;
	for (unsigned offset = 1; offset < 4; ++offset) {
		swap_faults(offset == 1 ? "swp at +1" : offset == 2 ? "swp at +2" : "swp at +3",
			    [&] { return sw_word(ptr<u32>(addr(cell) + offset), 0x99, sentinel); },
			    addr(cell) + offset, segv_accerr, sw_word_insn);
		check_eq(cell[0], 0x01020304, "an unaligned swp changes nothing");
	}
	const word read_only = mmap_pages(4096, prot_read);
	swap_faults("swp on a read-only page", [&] { return sw_word(ptr<u32>(read_only), 0x99, sentinel); },
		    read_only, segv_accerr, sw_word_insn);
	swap_faults("swpb on a read-only page", [&] { return sw_byte(ptr<u8>(read_only + 5), 0x99, sentinel); },
		    read_only + 5, segv_accerr, sw_byte_insn);
	check_eq(*ptr<u32>(read_only), 0, "and the page is unchanged");
	const word no_access = mmap_pages(4096, 0);
	swap_faults("swp on a PROT_NONE page", [&] { return sw_word(ptr<u32>(no_access), 0x99, sentinel); },
		    no_access, segv_accerr, sw_word_insn);
	/* find_vma(): any mapping above the address makes it an access error. */
	swap_faults("swp at address 0", [&] { return sw_word(ptr<u32>(0), 0x99, sentinel); }, 0, segv_accerr,
		    sw_word_insn);
	swap_faults("swp on the kuser helpers page", [&] { return sw_word(ptr<u32>(0xffff0000), 0x99, sentinel); },
		    0xffff0000, segv_accerr, sw_word_insn);
	/* Above every mapping: not mapped at all. */
	swap_faults("swp at 0xfffff000", [&] { return sw_word(ptr<u32>(0xfffff000), 0x99, sentinel); },
		    0xfffff000, segv_maperr, sw_word_insn);
	swap_faults("swpb at 0xfffff003", [&] { return sw_byte(ptr<u8>(0xfffff003), 0x99, sentinel); },
		    0xfffff003, segv_maperr, sw_byte_insn);
	swap_faults("swp at 0xfffffffc", [&] { return sw_word(ptr<u32>(0xfffffffc), 0x99, sentinel); },
		    0xfffffffc, segv_maperr, sw_word_insn);

	/* The words the kernel never emulates. */
	undefined_words_raise_sigill();

	/* A page nothing has touched is mapped by the exchange. */
	{
		auto *fresh = ptr<u32>(mmap_pages(8192, prot_read | prot_write));
		check_eq(sw_word(fresh, 0x77665544, sentinel), 0, "swp on a page nothing has touched reads zero");
		check_eq(fresh[0], 0x77665544, "and stores to the new page");
		auto *fresh_byte = ptr<u8>(mmap_pages(4096, prot_read | prot_write));
		check_eq(sw_byte(fresh_byte + 4095, 0x99, sentinel), 0, "swpb on an untouched page reads zero");
		check_eq(fresh_byte[4095], 0x99, "and stores to the new page");
		/* A page that was only read maps the shared zero page: the
		 * exchange has to copy it. */
		auto *zero = ptr<u32>(mmap_pages(4096, prot_read | prot_write));
		const u32 seen = *static_cast<volatile u32 *>(zero);
		check_eq(seen, 0, "a page that was only read is zero");
		check_eq(sw_word(zero + 8, 0x1357, sentinel), 0, "swp on a page that was only read reads zero");
		check_eq(zero[8], 0x1357, "and stores to its own copy");
	}

	/* A private page shared copy-on-write with a parent is copied. */
	{
		auto *priv = ptr<u32>(mmap_pages(4096, prot_read | prot_write));
		priv[0] = 0xc0ffee00;
		const long child = fork_child();
		if (child == 0) {
			const u32 old = sw_word(priv, 0xbadc0de1, sentinel);
			quit(old == 0xc0ffee00 && priv[0] == 0xbadc0de1 ? 0 : 1);
		}
		check(child > 0, "fork starts the child");
		const long status = wait_child(child);
		check(exited_with(status, 0), "in the child, swp on a copy-on-write page returns the shared word and stores its own");
		check_eq(priv[0], 0xc0ffee00, "and the parent's copy is unchanged");
	}
	/* A shared page is shared: the parent sees the child's exchange. */
	{
		auto *shared = ptr<u32>(mmap_pages(4096, prot_read | prot_write, map_shared | map_anonymous));
		shared[0] = 0x10203040;
		const long child = fork_child();
		if (child == 0)
			quit(sw_word(shared, 0x50607080, sentinel) == 0x10203040 ? 0 : 1);
		const long status = wait_child(child);
		check(exited_with(status, 0), "in the child, swp on a shared page returns the word");
		check_eq(shared[0], 0x50607080, "and the parent sees the child's store");
	}
}

} // namespace

void group_swp()
{
	/* qemu-user executes SWP, so it has no emulation to check. */
	if (under_qemu())
		return;
	const long original = swp_setting();
	check(original == 0 || original == 1, "abi.swp is present and 0 or 1");
	if (original < 0)
		return;
	/* Root can set either value; a shell can only see the one there is. */
	const bool writable = swp_set(static_cast<int>(original));
	if (!writable)
		line("NOTE swp: abi.swp is not writable by this process; only its current value is tested");
	if (original == 0 || writable) {
		if (original != 0)
			check(swp_set(0), "abi.swp accepts 0");
		swp_off();
	}
	if (original == 1 || writable) {
		if (swp_setting() != 1)
			check(swp_set(1), "abi.swp accepts 1");
		swp_on();
	}
	restore_swp();
	if (writable)
		check_eq(static_cast<u32>(swp_setting()), static_cast<u32>(original), "abi.swp is back where it was");
}

} // namespace probe
