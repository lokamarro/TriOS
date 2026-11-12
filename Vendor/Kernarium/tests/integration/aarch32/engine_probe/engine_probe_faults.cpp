// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "faults": the memory faults an AArch32 task takes. Accesses that must
 * be aligned (LDRD, STRD, LDM, STM, LDREX of a word, halfword and doubleword)
 * raise SIGBUS with BUS_ADRALN and the address; the accesses that may be
 * unaligned (LDR, LDRH, LDRSH, STR, STRH with SCTLR.A clear) work, even across
 * a page boundary; and a load, a store and an instruction fetch outside a
 * mapping or against its protection raise SIGSEGV with the right code and
 * address, from both instruction sets. */
#include "engine_probe.hpp"

extern "C" {
void fa_ldrd_arm(probe::word address, probe::u32 *out);
void fa_strd_arm(probe::word address, probe::u32 *out);
void fa_ldm_arm(probe::word address, probe::u32 *out);
void fa_stm_arm(probe::word address, probe::u32 *out);
void fa_ldrex_arm(probe::word address, probe::u32 *out);
void fa_ldrexh_arm(probe::word address, probe::u32 *out);
void fa_ldrexb_arm(probe::word address, probe::u32 *out);
void fa_ldrexd_arm(probe::word address, probe::u32 *out);
void fa_ldr_arm(probe::word address, probe::u32 *out);
void fa_ldrh_arm(probe::word address, probe::u32 *out);
void fa_ldrsh_arm(probe::word address, probe::u32 *out);
void fa_str_arm(probe::word address, probe::u32 *out);
void fa_strh_arm(probe::word address, probe::u32 *out);
void fa_ldrd_thumb(probe::word address, probe::u32 *out);
void fa_strd_thumb(probe::word address, probe::u32 *out);
void fa_ldm_thumb(probe::word address, probe::u32 *out);
void fa_stm_thumb(probe::word address, probe::u32 *out);
void fa_ldrex_thumb(probe::word address, probe::u32 *out);
void fa_ldrexh_thumb(probe::word address, probe::u32 *out);
void fa_ldrexd_thumb(probe::word address, probe::u32 *out);
void fa_ldr_thumb(probe::word address, probe::u32 *out);
void fa_ldrh_thumb(probe::word address, probe::u32 *out);
void fa_str_thumb(probe::word address, probe::u32 *out);
extern const char fa_ldrd_thumb_insn[], fa_strd_thumb_insn[], fa_ldm_thumb_insn[],
	fa_stm_thumb_insn[], fa_ldrex_thumb_insn[], fa_ldrexh_thumb_insn[],
	fa_ldrexd_thumb_insn[], fa_ldr_thumb_insn[], fa_ldrh_thumb_insn[], fa_str_thumb_insn[];
long fa_call_arm(probe::word address);
long fa_call_thumb(probe::word address);
}

namespace probe {
namespace {

using Access = void (*)(word, u32 *);

struct Case {
	Access routine;
	/* The address of the access instruction in the routine. */
	const char *instruction;
	word skip;
	bool aligned_only;
	const char *name;
};

#define ARM_CASE(routine, aligned, text) {routine, nullptr, 4, aligned, text}
#define THUMB_CASE(routine, label, skip, aligned, text) {routine, label, skip, aligned, text}

const Case cases[] = {
	ARM_CASE(fa_ldrd_arm, true, "A32 ldrd"),
	ARM_CASE(fa_strd_arm, true, "A32 strd"),
	ARM_CASE(fa_ldm_arm, true, "A32 ldm"),
	ARM_CASE(fa_stm_arm, true, "A32 stm"),
	ARM_CASE(fa_ldrex_arm, true, "A32 ldrex"),
	ARM_CASE(fa_ldrexh_arm, true, "A32 ldrexh"),
	ARM_CASE(fa_ldrexd_arm, true, "A32 ldrexd"),
	THUMB_CASE(fa_ldrd_thumb, fa_ldrd_thumb_insn, 4, true, "T32 ldrd"),
	THUMB_CASE(fa_strd_thumb, fa_strd_thumb_insn, 4, true, "T32 strd"),
	THUMB_CASE(fa_ldm_thumb, fa_ldm_thumb_insn, 4, true, "T32 ldm"),
	THUMB_CASE(fa_stm_thumb, fa_stm_thumb_insn, 4, true, "T32 stm"),
	THUMB_CASE(fa_ldrex_thumb, fa_ldrex_thumb_insn, 4, true, "T32 ldrex"),
	THUMB_CASE(fa_ldrexh_thumb, fa_ldrexh_thumb_insn, 4, true, "T32 ldrexh"),
	THUMB_CASE(fa_ldrexd_thumb, fa_ldrexd_thumb_insn, 4, true, "T32 ldrexd"),
};

/* The alignment each case needs: word for most, halfword for LDREXH, and
 * doubleword for LDREXD (an LDRD only needs a word). */
bool address_is_valid(const Case &fault, word offset)
{
	const char *name = fault.name;
	if (streq(name + 4, "ldrexh"))
		return (offset & 1) == 0;
	if (streq(name + 4, "ldrexd"))
		return (offset & 7) == 0;
	return (offset & 3) == 0;
}

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

} // namespace

void group_faults()
{
	char name[128];
	const word area = mmap_pages(4 * page_size, prot_read | prot_write);
	auto *bytes = ptr<u8>(area);
	for (u32 index = 0; index < 4 * page_size; ++index)
		bytes[index] = static_cast<u8>(index * 7 + 3);

	/* Accesses that must be aligned. */
	for (const Case &fault : cases) {
		u32 wrong_alignment = 0, wrongly_ok = 0, bad_address = 0, bad_pc = 0, bad_code = 0;
		for (word offset = 0; offset < 16; ++offset) {
			const word address = area + 64 + offset;
			u32 out[4] = {};
			catch_signal(sigbus, fault.skip);
			fault.routine(address, out);
			release_signal(sigbus);
			const bool should_fault = !address_is_valid(fault, offset);
			if (should_fault) {
				if (caught.count != 1)
					++wrongly_ok;
				else {
					if (caught.address != address)
						++bad_address;
					if (caught.code != 1)
						++bad_code;
					const word start = fault.instruction ? addr(fault.instruction) :
						(addr(fault.routine) + 16);
					if (caught.pc != start)
						++bad_pc;
				}
			} else if (caught.count != 0) {
				++wrong_alignment;
			}
		}
		check_eq(wrongly_ok, 0, labelled(name, fault.name, "a misaligned access raises SIGBUS"));
		check_eq(wrong_alignment, 0, labelled(name, fault.name, "an aligned access does not fault"));
		check_eq(bad_address, 0, labelled(name, fault.name, "si_addr is the access address"));
		check_eq(bad_code, 0, labelled(name, fault.name, "si_code is BUS_ADRALN"));
		check_eq(bad_pc, 0, labelled(name, fault.name, "the frame's pc is the faulting instruction"));
	}

	/* Accesses that may be unaligned work and see little-endian data. */
	{
		const word base = area + 128;
		u32 bad = 0;
		for (word offset = 0; offset < 8; ++offset) {
			u32 out[4] = {};
			fa_ldr_arm(base + offset, out);
			const u32 expected = static_cast<u32>(bytes[128 + offset]) |
				static_cast<u32>(bytes[129 + offset]) << 8 |
				static_cast<u32>(bytes[130 + offset]) << 16 |
				static_cast<u32>(bytes[131 + offset]) << 24;
			if (out[0] != expected)
				++bad;
			fa_ldr_thumb(base + offset, out);
			if (out[0] != expected)
				++bad;
			fa_ldrh_arm(base + offset, out);
			if (out[0] != (static_cast<u32>(bytes[128 + offset]) |
				       static_cast<u32>(bytes[129 + offset]) << 8))
				++bad;
			fa_ldrh_thumb(base + offset, out);
			if (out[0] != (static_cast<u32>(bytes[128 + offset]) |
				       static_cast<u32>(bytes[129 + offset]) << 8))
				++bad;
			fa_ldrsh_arm(base + offset, out);
			const u32 half = static_cast<u32>(bytes[128 + offset]) |
				static_cast<u32>(bytes[129 + offset]) << 8;
			if (out[0] != (half & 0x8000 ? half | 0xffff0000U : half))
				++bad;
		}
		check_eq(bad, 0, "unaligned LDR, LDRH and LDRSH read little-endian data");
		bad = 0;
		for (word offset = 1; offset < 4; ++offset) {
			const word target = area + 4 * page_size - 8 + offset;
			u32 out[4] = {};
			fa_str_arm(target, out);
			fa_str_thumb(target, out);
			fa_strh_arm(target, out);
			bad += *ptr<u32>(area) == 0xdeadbeef;
		}
		check_eq(bad, 0, "unaligned STR and STRH work");
		u32 stored[2] = {};
		fa_str_arm(area + 4 * page_size - 6, stored);
		check_eq(static_cast<u32>(bytes[4 * page_size - 6]), 0x11, "an unaligned store writes its low byte first");
		check_eq(static_cast<u32>(bytes[4 * page_size - 3]), 0x11, "and its high byte last");
	}

	/* A load, store or fetch outside a mapping or against its protection. */
	{
		const word block = mmap_pages(4 * page_size, prot_read | prot_write);
		sys(nr_mprotect, block + page_size, page_size, prot_read);
		sys(nr_mprotect, block + 2 * page_size, page_size, 0);
		sys(nr_munmap, block + 3 * page_size, page_size);
		u32 out[4] = {};

		catch_signal(sigsegv, 4);
		fa_str_arm(block + page_size + 8, out);
		release_signal(sigsegv);
		check_eq(caught.count, 1, "a store to a read-only page raises SIGSEGV");
		check_eq(static_cast<u32>(caught.code), 2, "store to read-only: si_code is SEGV_ACCERR");
		check_eq(caught.address, block + page_size + 8, "and the address");

		catch_signal(sigsegv, 4);
		fa_ldr_arm(block + 2 * page_size + 12, out);
		release_signal(sigsegv);
		check_eq(caught.count, 1, "a load from a PROT_NONE page raises SIGSEGV");
		check_eq(static_cast<u32>(caught.code), 2, "load from PROT_NONE: si_code is SEGV_ACCERR");
		check_eq(caught.address, block + 2 * page_size + 12, "and the address");

		catch_signal(sigsegv, 4);
		fa_ldr_arm(block + 3 * page_size + 16, out);
		release_signal(sigsegv);
		check_eq(caught.count, 1, "a load from an unmapped page raises SIGSEGV");
		check_eq(static_cast<u32>(caught.code), 1, "load from unmapped: si_code is SEGV_MAPERR");
		check_eq(caught.address, block + 3 * page_size + 16, "and the address");

		catch_signal(sigsegv, 4);
		fa_str_thumb(block + page_size + 40, out);
		release_signal(sigsegv);
		check_eq(caught.count, 1, "a T32 store to a read-only page raises SIGSEGV");
		check_eq(caught.address, block + page_size + 40, "at the address");

		/* An unaligned word access that straddles into an unmapped page. */
		const word pair = mmap_pages(2 * page_size, prot_read | prot_write);
		sys(nr_munmap, pair + page_size, page_size);
		catch_signal(sigsegv, 4);
		fa_ldr_arm(pair + page_size - 2, out);
		release_signal(sigsegv);
		check_eq(caught.count, 1, "an unaligned load across into an unmapped page raises SIGSEGV");
		check_eq(static_cast<u32>(caught.code), 1, "straddling load: si_code is SEGV_MAPERR");

		/* Fetches: from a page that is not executable and from none. */
		catch_signal(sigsegv, return_to_lr);
		fa_call_arm(block);
		release_signal(sigsegv);
		check_eq(caught.count, 1, "an A32 fetch from a non-executable page raises SIGSEGV");
		check_eq(static_cast<u32>(caught.code), 2, "A32 fetch non-exec: si_code is SEGV_ACCERR");
		check_eq(caught.address, block, "at the target");
		check_eq(caught.pc, block, "and the frame's pc is the target");
		catch_signal(sigsegv, return_to_lr);
		fa_call_thumb(block | 1);
		release_signal(sigsegv);
		check_eq(caught.count, 1, "a T32 fetch from a non-executable page raises SIGSEGV");
		check_eq(caught.address, block, "at the target (without the T bit)");
		check_eq(caught.cpsr & (1U << 5), 1U << 5, "the frame's T bit is set");
		catch_signal(sigsegv, return_to_lr);
		fa_call_arm(block + 3 * page_size);
		release_signal(sigsegv);
		check_eq(caught.count, 1, "a fetch from an unmapped page raises SIGSEGV");
		if (!under_qemu())
			check_eq(static_cast<u32>(caught.code), 1, "fetch unmapped: si_code is SEGV_MAPERR");
		catch_signal(sigsegv, return_to_lr);
		fa_call_arm(0x100);
		release_signal(sigsegv);
		check_eq(caught.count, 1, "a jump to a low unmapped address raises SIGSEGV");
		check_eq(caught.address, 0x100, "at the target");
	}
}

} // namespace probe
