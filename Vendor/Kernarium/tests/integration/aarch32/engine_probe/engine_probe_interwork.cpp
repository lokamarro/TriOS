// SPDX-License-Identifier: GPL-2.0-or-later
#include "engine_probe.hpp"
/* Group "interwork": calls and returns between A32 and T32 in every form
 * that changes instruction set, PC reads and the Align(PC, 4) base at both
 * halfword positions, table branches, and IT blocks (every condition, a
 * skipped faulting access, and an SVC inside a block). */

#define IW_CONDITIONS(X) \
	X(eq) X(ne) X(cs) X(cc) X(mi) X(pl) X(vs) X(vc) X(hi) X(ls) X(ge) X(lt) X(gt) X(le)

extern "C" {
long iw_arm_lr();
long iw_thumb_lr();
long iw_arm_k();
long iw_thumb_k();
long iw_arm_calls(long (*function)());
long iw_thumb_calls(long (*function)());
long iw_arm_ret_pop();
long iw_arm_ret_ldr();
long iw_arm_ret_mov();
long iw_arm_ret_ldm();
long iw_arm_ret_bxcond();
long iw_arm_ret_movcond();
long iw_arm_ret_add();
long iw_thumb_ret_pop();
long iw_thumb_ret_ldr();
long iw_thumb_ret_popw();
long iw_thumb_ret_itbx();
long iw_thumb_ret_mov();
long iw_a_bx(long (*target)());
long iw_a_mov(long (*target)());
long iw_a_add(long (*target)());
long iw_a_ldr(long (**target)());
long iw_a_pop(long (*target)());
long iw_t_bx(long (*target)());
long iw_t_mov(long (*target)());
long iw_t_ldr(long (**target)());
long iw_t_pop(long (*target)());
long iw_t_tbb(long index);
long iw_t_tbh(long index);
long iw_a_pc();
long iw_t_pc();
long iw_t_adr0();
long iw_t_adr2();
long iw_t_adrw0();
long iw_t_adrw2();
long iw_t_lit0();
long iw_t_lit2();
long iw_t_litw0();
long iw_t_litw2();
long iw_t_blx0();
long iw_t_blx2();
long iw_t_bl0();
long iw_t_bl2();
void iw_it_skip(probe::u32 *out);
void iw_it_svc(probe::u32 *out);
#define X(c) long iw_ac_##c(long); long iw_tc_##c(long);
IW_CONDITIONS(X)
#undef X
extern const probe::u32 iw_pool[];
}

namespace probe {
namespace {

using Function = long (*)();

/* The condition codes against an NZCV value (bit 3 = N ... bit 0 = V). */
bool condition_holds(unsigned condition, unsigned nzcv)
{
	const bool n = nzcv & 8, z = nzcv & 4, c = nzcv & 2, v = nzcv & 1;
	switch (condition) {
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
	default: return z || n != v;
	}
}

} // namespace

void group_interwork()
{
	/* A32 calls T32 and the reverse: the link register names the caller's
	 * instruction set in bit 0, and the return restores it. */
	check_eq(static_cast<u32>(iw_arm_calls(iw_thumb_lr)) & 1, 0,
		 "A32 blx to T32: lr is an A32 address");
	check_eq(static_cast<u32>(iw_thumb_calls(iw_arm_lr)) & 1, 1,
		 "T32 blx to A32: lr has bit 0 set");
	check_eq(static_cast<u32>(iw_thumb_calls(iw_thumb_lr)) & 1, 1,
		 "T32 blx to T32: lr has bit 0 set");
	check_eq(static_cast<u32>(iw_arm_calls(iw_arm_lr)) & 1, 0, "A32 blx to A32");
	check_eq(static_cast<u32>(iw_arm_k()), 0x41, "bl from A32 to A32");
	check_eq(static_cast<u32>(iw_thumb_k()), 0x54, "bl from A32 to T32 (blx immediate)");
	check_eq(static_cast<u32>(iw_t_blx0()), 0x41, "blx immediate from T32 at 0 mod 4");
	check_eq(static_cast<u32>(iw_t_blx2()), 0x41, "blx immediate from T32 at 2 mod 4");
	check_eq(static_cast<u32>(iw_t_bl0()), 0x54, "bl from T32 at 0 mod 4");
	check_eq(static_cast<u32>(iw_t_bl2()), 0x54, "bl from T32 at 2 mod 4");

	/* Every instruction that returns. */
	static const struct {
		Function function;
		u32 expected;
		const char *name;
	} arm_returns[] = {
		{iw_arm_ret_pop, 2, "pop {pc}"},
		{iw_arm_ret_ldr, 3, "ldr pc, [sp], #4"},
		{iw_arm_ret_mov, 4, "mov pc, lr"},
		{iw_arm_ret_ldm, 5, "ldm sp!, {..., pc}"},
		{iw_arm_ret_bxcond, 6, "bxeq lr"},
		{iw_arm_ret_movcond, 7, "moveq pc, lr"},
		{iw_arm_ret_add, 8, "add pc, lr, #0"},
	};
	for (const auto &entry : arm_returns) {
		char name[64] = "A32 returns by ";
		memcpy(name + 15, entry.name, strlen(entry.name) + 1);
		check_eq(static_cast<u32>(iw_arm_calls(entry.function)), entry.expected,
			 name);
		char name_t[80] = "A32 returns to T32 by ";
		memcpy(name_t + 21, entry.name, strlen(entry.name) + 1);
		check_eq(static_cast<u32>(iw_thumb_calls(entry.function)), entry.expected,
			 name_t);
	}
	static const struct {
		Function function;
		u32 expected;
		const char *name;
	} thumb_returns[] = {
		{iw_thumb_ret_pop, 2, "pop {pc}"},
		{iw_thumb_ret_ldr, 3, "ldr pc, [sp], #4"},
		{iw_thumb_ret_popw, 4, "pop.w {..., pc}"},
		{iw_thumb_ret_itbx, 5, "bxeq lr in an IT block"},
	};
	for (const auto &entry : thumb_returns) {
		char name[64] = "T32 returns by ";
		memcpy(name + 15, entry.name, strlen(entry.name) + 1);
		check_eq(static_cast<u32>(iw_thumb_calls(entry.function)), entry.expected,
			 name);
		char name_a[80] = "T32 returns to A32 by ";
		memcpy(name_a + 22, entry.name, strlen(entry.name) + 1);
		check_eq(static_cast<u32>(iw_arm_calls(entry.function)), entry.expected,
			 name_a);
	}
	check_eq(static_cast<u32>(iw_thumb_calls(iw_thumb_ret_mov)), 6,
		 "T32 mov pc, lr keeps T32");

	/* Every instruction that jumps, to each instruction set. */
	const Function arm_target = iw_arm_k;
	const Function thumb_target = iw_thumb_k;
	check_eq(static_cast<u32>(iw_a_bx(arm_target)), 0x41, "A32 bx to A32");
	check_eq(static_cast<u32>(iw_a_bx(thumb_target)), 0x54, "A32 bx to T32");
	check_eq(static_cast<u32>(iw_a_mov(arm_target)), 0x41, "A32 mov pc to A32");
	check_eq(static_cast<u32>(iw_a_mov(thumb_target)), 0x54, "A32 mov pc to T32 interworks");
	check_eq(static_cast<u32>(iw_a_add(thumb_target)), 0x54, "A32 add pc to T32 interworks");
	check_eq(static_cast<u32>(iw_a_pop(thumb_target)), 0x54, "A32 pop {pc} to T32 interworks");
	check_eq(static_cast<u32>(iw_a_ldr(const_cast<Function *>(&thumb_target))), 0x54,
		 "A32 ldr pc to T32 interworks");
	check_eq(static_cast<u32>(iw_a_ldr(const_cast<Function *>(&arm_target))), 0x41,
		 "A32 ldr pc to A32");
	check_eq(static_cast<u32>(iw_t_bx(arm_target)), 0x41, "T32 bx to A32");
	check_eq(static_cast<u32>(iw_t_bx(thumb_target)), 0x54, "T32 bx to T32");
	check_eq(static_cast<u32>(iw_t_pop(arm_target)), 0x41, "T32 pop {pc} to A32 interworks");
	check_eq(static_cast<u32>(iw_t_pop(thumb_target)), 0x54, "T32 pop {pc} to T32");
	check_eq(static_cast<u32>(iw_t_ldr(const_cast<Function *>(&arm_target))), 0x41,
		 "T32 ldr pc to A32 interworks");
	check_eq(static_cast<u32>(iw_t_ldr(const_cast<Function *>(&thumb_target))), 0x54,
		 "T32 ldr pc to T32");
	check_eq(static_cast<u32>(iw_t_mov(thumb_target)), 0x54,
		 "T32 mov pc to a T32 address with bit 0 set stays T32");
	for (u32 index = 0; index < 4; ++index)
		check_eq(static_cast<u32>(iw_t_tbb(static_cast<long>(index))), 10 + index, "tbb");
	for (u32 index = 0; index < 3; ++index)
		check_eq(static_cast<u32>(iw_t_tbh(static_cast<long>(index))), 20 + index, "tbh");

	/* The PC reads as the instruction plus 8 in A32 and plus 4 in T32, and
	 * Align(PC, 4) is the base of the PC-relative forms. */
	check_eq(static_cast<u32>(iw_a_pc()), 8, "A32 reads the PC as the instruction + 8");
	check_eq(static_cast<u32>(iw_t_pc()), 4, "T32 reads the PC as the instruction + 4");
	const word pool = addr(iw_pool);
	check_eq(static_cast<u32>(iw_t_adr0()), pool, "T32 adr at 0 mod 4");
	check_eq(static_cast<u32>(iw_t_adr2()), pool, "T32 adr at 2 mod 4: Align(PC, 4)");
	check_eq(static_cast<u32>(iw_t_adrw0()), pool, "T32 adr.w at 0 mod 4");
	check_eq(static_cast<u32>(iw_t_adrw2()), pool, "T32 adr.w at 2 mod 4");
	check_eq(static_cast<u32>(iw_t_lit0()), 0x1badf00d, "T32 ldr literal at 0 mod 4");
	check_eq(static_cast<u32>(iw_t_lit2()), 0x1badf00d, "T32 ldr literal at 2 mod 4");
	check_eq(static_cast<u32>(iw_t_litw0()), 0x1badf00d, "T32 ldr.w literal at 0 mod 4");
	check_eq(static_cast<u32>(iw_t_litw2()), 0x1badf00d, "T32 ldr.w literal at 2 mod 4");

	/* Conditional execution: every condition against every NZCV, in A32 and
	 * in an IT block. */
	static const struct {
		long (*arm)(long);
		long (*thumb)(long);
	} conditions[] = {
#define X(c) {iw_ac_##c, iw_tc_##c},
		IW_CONDITIONS(X)
#undef X
	};
	u32 arm_bad = 0, thumb_bad = 0;
	for (unsigned condition = 0; condition < 14; ++condition) {
		for (unsigned nzcv = 0; nzcv < 16; ++nzcv) {
			const long want = condition_holds(condition, nzcv) ? 1 : 0;
			if (conditions[condition].arm(nzcv) != want) {
				if (!arm_bad++) {
					put("  first A32 mismatch: condition ");
					put_dec(condition);
					put(" nzcv ");
					put_dec(nzcv);
					put("\n");
				}
			}
			if (conditions[condition].thumb(nzcv) != want) {
				if (!thumb_bad++) {
					put("  first T32 mismatch: condition ");
					put_dec(condition);
					put(" nzcv ");
					put_dec(nzcv);
					put("\n");
				}
			}
		}
	}
	check_eq(arm_bad, 0, "A32 conditional execution agrees for 14 conditions x 16 flag states");
	check_eq(thumb_bad, 0, "T32 IT blocks agree for 14 conditions x 16 flag states");

	/* A skipped IT slot does not execute its access, and an SVC inside a
	 * block runs and keeps the block's state. */
	u32 out[4] = {};
	iw_it_skip(out);
	check_eq(out[0], 0, "a skipped IT slot executes nothing (no fault)");
	check_eq(out[1], 7, "the code after the block runs");
	u32 svc[4] = {};
	iw_it_svc(svc);
	check_eq(svc[0], static_cast<u32>(getpid()), "SVC in an IT block returns getpid()");
	check_eq(svc[1], 1, "the IT slot after an SVC executes once");
	check_eq(svc[2], 0x66, "a skipped SVC in an IT block makes no system call");
	check_eq(svc[3], 11, "the else slot of a skipped SVC executes");
}

} // namespace probe
