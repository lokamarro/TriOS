// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "cacheflush": code the program writes and then runs. After the
 * cacheflush system call an engine has to run the new instructions, whichever
 * instruction set they are in: rewriting one location again and again, the
 * same bytes of memory run as A32 and then as T32 and back, a 32-bit Thumb
 * instruction that straddles a page boundary rewritten in its second half
 * only, code that rewrites an instruction it is about to run, a page whose
 * protection is flipped between writes and runs, and a mapping replaced at
 * the same address. */
#include "engine_probe.hpp"

extern "C" {
extern const char cf_self_arm_start[], cf_self_arm_end[];
extern const char cf_self_thumb_start[], cf_self_thumb_end[];
}

namespace probe {
namespace {

constexpr word rwx = prot_read | prot_write | prot_exec;

using Function = long (*)();

long call_arm(word address)
{
	return reinterpret_cast<Function>(address)();
}

long call_thumb(word address)
{
	return reinterpret_cast<Function>(address | 1)();
}

long flush(word address, word length)
{
	return sys(nr_cacheflush, address, address + length, 0);
}

/* mov r0, #n; bx lr */
void write_arm(word address, u32 n)
{
	auto *code = ptr<u32>(address);
	code[0] = 0xe3a00000U | n;
	code[1] = 0xe12fff1eU;
	flush(address, 8);
}

/* movs r0, #n; bx lr */
void write_thumb(word address, u32 n)
{
	auto *code = ptr<u16>(address);
	code[0] = static_cast<u16>(0x2000U | n);
	code[1] = 0x4770;
	flush(address, 4);
}

} // namespace

void group_cacheflush()
{
	const word region = mmap_pages(4 * page_size, rwx);
	check(region < static_cast<word>(-4096), "an anonymous mapping may be writable and executable");
	if (region > static_cast<word>(-4096))
		return;

	/* One location rewritten again and again. */
	u32 wrong = 0;
	for (u32 n = 1; n <= 64; ++n) {
		write_arm(region, n);
		if (call_arm(region) != static_cast<long>(n))
			++wrong;
	}
	check_eq(wrong, 0, "A32 code rewritten 64 times runs its latest version");
	wrong = 0;
	for (u32 n = 1; n <= 64; ++n) {
		write_thumb(region + 16, n);
		if (call_thumb(region + 16) != static_cast<long>(n))
			++wrong;
	}
	check_eq(wrong, 0, "T32 code rewritten 64 times runs its latest version");

	/* The same bytes of memory as A32, then T32, then A32, then T32. */
	write_arm(region + 64, 11);
	check_eq(static_cast<u32>(call_arm(region + 64)), 11, "A32 at an address");
	write_thumb(region + 64, 22);
	check_eq(static_cast<u32>(call_thumb(region + 64)), 22, "T32 at the same address");
	write_arm(region + 64, 33);
	check_eq(static_cast<u32>(call_arm(region + 64)), 33, "A32 again at the same address");
	write_thumb(region + 64, 44);
	check_eq(static_cast<u32>(call_thumb(region + 64)), 44, "T32 again");
	write_thumb(region + 66, 55);
	check_eq(static_cast<u32>(call_thumb(region + 66)), 55, "T32 at a halfword offset");
	write_thumb(region + 66, 66);
	check_eq(static_cast<u32>(call_thumb(region + 66)), 66, "and rewritten there");

	/* A 32-bit Thumb instruction across a page boundary, changed in its
	 * second half. movw r0, #n: f240 00nn; then bx lr. */
	{
		const word boundary = region + page_size;
		auto *low = ptr<u16>(boundary - 2);
		auto *high = ptr<u16>(boundary);
		wrong = 0;
		for (u32 n = 1; n <= 40; ++n) {
			low[0] = 0xf240;
			high[0] = static_cast<u16>(n);
			high[1] = 0x4770;
			flush(boundary - 2, 6);
			if (call_thumb(boundary - 2) != static_cast<long>(n))
				++wrong;
		}
		check_eq(wrong, 0, "a 32-bit T32 instruction across a page boundary runs as written");
		wrong = 0;
		for (u32 n = 100; n < 140; ++n) {
			high[0] = static_cast<u16>(n);
			flush(boundary, 2);
			if (call_thumb(boundary - 2) != static_cast<long>(n))
				++wrong;
		}
		check_eq(wrong, 0, "and when only its second half is rewritten and flushed");
	}

	/* Code that rewrites the next instruction and runs it. */
	{
		const word buffer = region + 2 * page_size;
		const word arm_size = static_cast<word>(cf_self_arm_end - cf_self_arm_start);
		memcpy(ptr<void>(buffer), cf_self_arm_start, arm_size);
		flush(buffer, arm_size);
		wrong = 0;
		for (u32 n = 0; n < 60; ++n) {
			const u32 instruction = 0xe3a00000U | (n + 3); /* mov r0, #n + 3 */
			using SelfFunction = long (*)(u32);
			if (reinterpret_cast<SelfFunction>(buffer)(instruction) != static_cast<long>(n + 3))
				++wrong;
		}
		check_eq(wrong, 0, "A32 code that rewrites its next instruction runs the new one");
		const word thumb_buffer = buffer + 256;
		const word thumb_size = static_cast<word>(cf_self_thumb_end - cf_self_thumb_start);
		memcpy(ptr<void>(thumb_buffer), cf_self_thumb_start, thumb_size);
		flush(thumb_buffer, thumb_size);
		wrong = 0;
		for (u32 n = 0; n < 60; ++n) {
			using SelfFunction = long (*)(u32);
			if (reinterpret_cast<SelfFunction>(thumb_buffer | 1)(0x2000U | (n + 3)) !=
			    static_cast<long>(n + 3))
				++wrong;
		}
		check_eq(wrong, 0, "T32 code that rewrites its next instruction runs the new one");
	}

	/* A page written while writable, run while executable, and back. */
	{
		const word page = mmap_pages(page_size, prot_read | prot_write);
		wrong = 0;
		for (u32 n = 1; n <= 20; ++n) {
			sys(nr_mprotect, page, page_size, prot_read | prot_write);
			write_arm(page, n);
			sys(nr_mprotect, page, page_size, prot_read | prot_exec);
			if (call_arm(page) != static_cast<long>(n))
				++wrong;
		}
		check_eq(wrong, 0, "a page flipped between writable and executable runs the latest code");
		sys(nr_munmap, page, page_size);
	}

	/* A mapping replaced at the same address. */
	{
		const word first = mmap_pages(page_size, rwx);
		write_arm(first, 7);
		check_eq(static_cast<u32>(call_arm(first)), 7, "code in a mapping");
		sys(nr_munmap, first, page_size);
		const long again = sys(nr_mmap2, first, page_size, rwx, map_private | map_anonymous | map_fixed, -1, 0);
		check_eq(static_cast<u32>(again), first, "a mapping at the same address");
		write_thumb(first, 9);
		check_eq(static_cast<u32>(call_thumb(first)), 9, "runs the code written into it");
		sys(nr_munmap, first, page_size);
	}

	/* The system call's arguments. */
	if (!under_qemu()) {
		check_eq(static_cast<u32>(sys(nr_cacheflush, region + 8, region, 0)),
			 static_cast<u32>(-einval), "cacheflush with end below start is EINVAL");
		check_eq(static_cast<u32>(sys(nr_cacheflush, region, region + 8, 1)),
			 static_cast<u32>(-einval), "cacheflush with nonzero flags is EINVAL");
	}
	check_eq(static_cast<u32>(sys(nr_cacheflush, region, region + 8, 0)), 0, "cacheflush");
}

} // namespace probe
