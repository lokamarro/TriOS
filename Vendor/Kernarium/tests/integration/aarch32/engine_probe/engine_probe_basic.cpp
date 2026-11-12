// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "basic": system calls and errno, the auxiliary vector, 64-bit
 * system call arguments, and the state a system call leaves alone. */
#include "engine_probe.hpp"

extern "C" void syscall_state_arm(probe::u32 *out);
extern "C" void syscall_state_thumb(probe::u32 *out);
extern "C" long syscall_imm_arm();

namespace probe {
namespace {

/* struct __kernel_timespec. */
struct Timespec64 {
	i64 seconds;
	i64 nanoseconds;
};

void check_syscall_state(void (*routine)(u32 *), const char *what, long pid)
{
	u32 out[9] = {};
	routine(out);
	/* Z and C set, N and V clear, Q set, GE all set. */
	check_eq(out[0] & 0xf80f0000U, 0x680f0000U, what);
	check_eq(out[1], 0x11111111, "r5 survives a system call");
	check_eq(out[2], 0x22222222, "r6 survives a system call");
	check_eq(out[3], 0x44444444, "r8 survives a system call");
	check_eq(out[4], 0x55555555, "r9 survives a system call");
	check_eq(out[5], 0x66666666, "r10 survives a system call");
	check_eq(out[6], 0x77777777, "r11 survives a system call");
	check_eq(out[7], static_cast<u32>(pid), "the call returns getpid() in r0");
}

} // namespace

void group_basic()
{
	/* The auxiliary vector of a 32-bit task. */
	check(page_size == 4096, "AT_PAGESZ is the guest page size");
	if (!under_qemu()) {
		constexpr word always = (1U << 1) | (1U << 2) | (1U << 4) | (1U << 7) |
			(1U << 15) | (1U << 17) | (1U << 18) | (1U << 20) | (1U << 21);
		check_eq(hwcap & always, always,
			 "AT_HWCAP names half, thumb, fastmult, edsp, tls, idiva, idivt, lpae and evtstrm");
		check_eq(hwcap & ((1U << 0) | (1U << 19)), 0,
			 "AT_HWCAP names neither swp nor vfpd32");
	}
	check_eq(hwcap & (hwcap_neon | hwcap_vfpv3 | hwcap_vfpv4 | (1U << 6)),
		 hwcap_neon | hwcap_vfpv3 | hwcap_vfpv4 | (1U << 6),
		 "AT_HWCAP names vfp, vfpv3, vfpv4 and neon");

	/* errno. */
	const long pid = getpid();
	check(pid >= 1, "getpid");
	check_eq(static_cast<u32>(sys(9999)), static_cast<u32>(-enosys),
		 "an unknown system call is ENOSYS");
	check_eq(static_cast<u32>(sys(nr_write, 99, addr("x"), 1)), static_cast<u32>(-ebadf),
		 "write to a closed descriptor is EBADF");
	check_eq(static_cast<u32>(sys(nr_close, 99)), static_cast<u32>(-ebadf),
		 "close of a closed descriptor is EBADF");
	check_eq(static_cast<u32>(sys(nr_open, addr("/nonexistent/probe"), 0, 0)),
		 static_cast<u32>(-enoent), "open of a missing path is ENOENT");
	check_eq(static_cast<u32>(sys(nr_munmap, 1, 4096)), static_cast<u32>(-einval),
		 "munmap of an unaligned address is EINVAL");
	check_eq(static_cast<u32>(sys(nr_kill, 0x7fffff00, 0)), static_cast<u32>(-esrch),
		 "kill of a missing process is ESRCH");
	check_eq(static_cast<u32>(sys(nr_wait4, -1, 0, 0, 0)), static_cast<u32>(-echild),
		 "wait4 with no children is ECHILD");
	check_eq(static_cast<u32>(sys(nr_execve, addr("/nonexistent/probe"), 0, 0)),
		 static_cast<u32>(-enoent), "execve of a missing path is ENOENT");

	/* A pipe: EFAULT on bad buffers, a full-width return. */
	i32 fds[2];
	check_eq(static_cast<u32>(sys(nr_pipe2, addr(fds), 0)), 0, "pipe2");
	check_eq(static_cast<u32>(sys(nr_write, fds[1], 0x10, 8)), static_cast<u32>(-efault),
		 "write from an unmapped buffer is EFAULT");
	check_eq(static_cast<u32>(sys(nr_write, fds[1], addr("pipe"), 4)), 4,
		 "write to a pipe");
	/* With data waiting: an empty pipe would block before it looks at the buffer. */
	check_eq(static_cast<u32>(sys(nr_read, fds[0], 0x10, 8)), static_cast<u32>(-efault),
		 "read into an unmapped buffer is EFAULT");
	char back[8] = {};
	check_eq(static_cast<u32>(sys(nr_read, fds[0], addr(back), 8)), 4, "read from a pipe");
	check_mem(back, "pipe", 4, "the bytes arrive");
	sys(nr_close, fds[0]);
	sys(nr_close, fds[1]);

	/* Six arguments: mmap2 with a page offset, and memory that works. */
	const word region = static_cast<word>(sys(nr_mmap2, 0, 3 * page_size,
		prot_read | prot_write, map_private | map_anonymous, -1, 0));
	check(region < static_cast<word>(-4096) && (region % page_size) == 0,
	      "mmap2 with six arguments");
	auto *words = ptr<u32>(region);
	for (u32 index = 0; index < 3 * page_size / 4; index += 64)
		words[index] = index ^ 0xa5a5a5a5U;
	bool intact = true;
	for (u32 index = 0; index < 3 * page_size / 4; index += 64)
		intact = intact && words[index] == (index ^ 0xa5a5a5a5U);
	check(intact, "anonymous memory holds what was stored");
	check_eq(static_cast<u32>(sys(nr_mprotect, region + page_size, page_size, prot_read)),
		 0, "mprotect");
	check_eq(static_cast<u32>(sys(nr_munmap, region + page_size, page_size)), 0,
		 "munmap of the middle page");
	check_eq(static_cast<u32>(sys(nr_munmap, region, page_size)), 0, "munmap of the first");
	check_eq(static_cast<u32>(sys(nr_munmap, region + 2 * page_size, page_size)), 0,
		 "munmap of the last");

	/* 64-bit system call arguments sit in even register pairs. */
	const long fd = sys(385 /* memfd_create */, addr("a32-probe"), 0);
	check(fd >= 0, "memfd_create");
	if (fd >= 0) {
		const u64 offset = 0x1'0000'0008ULL;
		const u32 low = static_cast<u32>(offset);
		const u32 high = static_cast<u32>(offset >> 32);
		check_eq(static_cast<u32>(sys(nr_ftruncate64, fd, 0, low + 0x1000, high)), 0,
			 "ftruncate64 past 4 GiB: the length is in r2:r3");
		const u8 mark[8] = {1, 2, 3, 4, 5, 6, 7, 8};
		check_eq(static_cast<u32>(sys(181 /* pwrite64 */, fd, addr(mark), 8, 0, low, high)), 8,
			 "pwrite64 at 4 GiB: the offset is in r4:r5");
		u8 got[8] = {};
		check_eq(static_cast<u32>(sys(nr_pread64, fd, addr(got), 8, 0, low, high)), 8,
			 "pread64 at 4 GiB");
		check_mem(got, mark, 8, "pread64 returns what pwrite64 stored");
		u8 hole[8] = {9, 9, 9, 9, 9, 9, 9, 9};
		const u8 zero[8] = {};
		check_eq(static_cast<u32>(sys(nr_pread64, fd, addr(hole), 8, 0, 8, 0)), 8,
			 "pread64 of the hole");
		check_mem(hole, zero, 8, "the hole reads as zeros: the high word is not dropped");
		u64 position = 0;
		check_eq(static_cast<u32>(sys(nr_llseek, fd, high, low, addr(&position), 0)), 0,
			 "_llseek");
		check_eq64(position, offset, "_llseek returns a 64-bit position through its pointer");
		sys(nr_close, fd);
	}

	/* 64-bit results through a pointer. */
	Timespec64 before{}, after{};
	check_eq(static_cast<u32>(sys(nr_clock_gettime64, 1 /* CLOCK_MONOTONIC */, addr(&before))), 0,
		 "clock_gettime64");
	sleep_ms(20);
	sys(nr_clock_gettime64, 1, addr(&after));
	const i64 elapsed = (after.seconds - before.seconds) * 1000000000LL +
		(after.nanoseconds - before.nanoseconds);
	check(elapsed >= 15000000LL && elapsed < 5000000000LL, "nanosleep sleeps about as long as asked");
	u8 random[16] = {};
	check_eq(static_cast<u32>(sys(nr_getrandom, addr(random), 16, 0)), 16, "getrandom");

	/* fork returns twice, and a child's status reaches wait4. */
	const long child = fork_child();
	if (child == 0)
		quit(41);
	check(child > 0 && child != pid, "fork returns the child's pid in the parent");
	check(exited_with(wait_child(child), 41), "wait4 returns the child's exit status");

	/* The immediate of an ARM-mode svc is ignored (qemu-user, which only
	 * validates the probe's own logic, takes it for an OABI number). */
	if (!under_qemu())
		check_eq(static_cast<u32>(syscall_imm_arm()), static_cast<u32>(pid),
			 "an ARM-mode svc with a nonzero immediate is a plain EABI call");

	/* What a system call leaves alone: flags, Q, GE and r5-r11. */
	check_syscall_state(syscall_state_arm, "A32: Q, GE and the flags survive a system call", pid);
	check_syscall_state(syscall_state_thumb, "T32: Q, GE and the flags survive a system call", pid);
}

} // namespace probe
