// SPDX-License-Identifier: GPL-2.0-or-later
/* The runtime layer of the AArch32 engine probe (engine_probe.cpp): system
 * calls, the kernel's ARM EABI structures, output and the checks. The probe
 * is freestanding armhf C++ in one translation unit, linked by link_elf32.py,
 * and runs as the guest's /init, so nothing here may use a library. */
#pragma once

/* The compiler may call these for any block copy or fill. */
extern "C" {
void *memcpy(void *destination, const void *source, unsigned long length);
void *memmove(void *destination, const void *source, unsigned long length);
void *memset(void *destination, int value, unsigned long length);
int memcmp(const void *a, const void *b, unsigned long length);
}

namespace probe {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i32 = int;
using i64 = long long;
using word = unsigned long;
static_assert(sizeof(void *) == 4 && sizeof(word) == 4);

/* ARM EABI system call numbers (arch/arm/tools/syscall.tbl). */
enum : long {
	nr_exit = 1,
	nr_fork = 2,
	nr_read = 3,
	nr_write = 4,
	nr_open = 5,
	nr_close = 6,
	nr_execve = 11,
	nr_getpid = 20,
	nr_mount = 21,
	nr_ptrace = 26,
	nr_kill = 37,
	nr_dup = 41,
	nr_pipe = 42,
	nr_brk = 45,
	nr_dup2 = 63,
	nr_getppid = 64,
	nr_sigaction = 67,
	nr_reboot = 88,
	nr_munmap = 91,
	nr_setitimer = 104,
	nr_wait4 = 114,
	nr_sigreturn = 119,
	nr_clone = 120,
	nr_mprotect = 125,
	nr_personality = 136,
	nr_llseek = 140,
	nr_sched_yield = 158,
	nr_nanosleep = 162,
	nr_prctl = 172,
	nr_rt_sigreturn = 173,
	nr_rt_sigaction = 174,
	nr_rt_sigprocmask = 175,
	nr_rt_sigsuspend = 179,
	nr_pread64 = 180,
	nr_sigaltstack = 186,
	nr_mmap2 = 192,
	nr_ftruncate64 = 194,
	nr_fcntl64 = 221,
	nr_gettid = 224,
	nr_tkill = 238,
	nr_futex = 240,
	nr_exit_group = 248,
	nr_clock_gettime = 263,
	nr_tgkill = 268,
	nr_openat = 322,
	nr_pipe2 = 359,
	nr_getrandom = 384,
	nr_clock_gettime64 = 403,
	nr_cacheflush = 0xf0002,
	nr_set_tls = 0xf0005,
};

enum : int {
	eperm = 1,
	enoent = 2,
	esrch = 3,
	eintr = 4,
	ebadf = 9,
	echild = 10,
	eagain = 11,
	enomem = 12,
	eacces = 13,
	efault = 14,
	einval = 22,
	erange = 34,
	enosys = 38,
};

enum : int {
	sigill = 4,
	sigtrap = 5,
	sigbus = 7,
	sigsegv = 11,
	sigusr1 = 10,
	sigusr2 = 12,
	sigalrm = 14,
	sigchld = 17,
	sigcont = 18,
	sigstop = 19,
};

enum : word {
	sa_siginfo = 0x4,
	sa_restart = 0x10000000,
	sa_nodefer = 0x40000000,
	sa_onstack = 0x08000000,
	sa_restorer = 0x04000000,
	sa_resethand = 0x80000000,
};

enum : word {
	prot_read = 1,
	prot_write = 2,
	prot_exec = 4,
	map_shared = 1,
	map_private = 2,
	map_fixed = 0x10,
	map_anonymous = 0x20,
	clone_vm = 0x100,
	clone_fs = 0x200,
	clone_files = 0x400,
	clone_sighand = 0x800,
	clone_settls = 0x80000,
	clone_thread = 0x10000,
	clone_sysvsem = 0x40000,
	clone_child_cleartid = 0x200000,
	clone_parent_settid = 0x100000,
};

/* The fixed-length system calls the probe makes: each argument is passed in
 * r0-r5 and the number in r7, as EABI does. */
[[gnu::always_inline]] inline long sys(long nr, long a0 = 0, long a1 = 0,
	long a2 = 0, long a3 = 0, long a4 = 0, long a5 = 0)
{
	register long r0 asm("r0") = a0;
	register long r1 asm("r1") = a1;
	register long r2 asm("r2") = a2;
	register long r3 asm("r3") = a3;
	register long r4 asm("r4") = a4;
	register long r5 asm("r5") = a5;
	register long r7 asm("r7") = nr;
	asm volatile("svc #0"
		     : "+r"(r0)
		     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
		     : "memory");
	return r0;
}

template<typename T>
[[gnu::always_inline]] inline word addr(T *pointer)
{
	return reinterpret_cast<word>(pointer);
}

template<typename T = void>
[[gnu::always_inline]] inline T *ptr(word value)
{
	return reinterpret_cast<T *>(value);
}

/* ---- the kernel's structures (ARM EABI) ---- */

struct SigSet {
	u32 bits[2];
};

/* rt_sigaction's struct sigaction (arch/arm/include/uapi/asm/signal.h). */
struct KernelSigaction {
	word handler;
	word flags;
	word restorer;
	SigSet mask;
};
static_assert(sizeof(KernelSigaction) == 20);

struct SigInfo {
	i32 signo;
	i32 error;
	i32 code;
	/* The union starts at offset 12: si_addr for faults, si_pid and si_uid
	 * for kill(), the child for SIGCHLD. */
	union {
		word address;
		struct {
			i32 pid;
			u32 uid;
		} sender;
		u32 raw[29];
	};
};
static_assert(sizeof(SigInfo) == 128);

struct StackT {
	word sp;
	i32 flags;
	word size;
};

/* struct sigcontext (arch/arm/include/uapi/asm/sigcontext.h). */
struct SigContext {
	word trap_no;
	word error_code;
	word oldmask;
	word r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10;
	word fp, ip, sp, lr, pc, cpsr;
	word fault_address;
};
static_assert(sizeof(SigContext) == 84);

/* The VFP part of the ucontext's register space (the kernel's
 * struct vfp_sigframe): a magic and a size, then D0-D31, FPSCR (the user_vfp
 * block ends at an 8-byte boundary), and FPEXC with the two instruction
 * registers, padded to 8 bytes. */
struct VfpFrame {
	u32 magic;
	u32 size;
	u64 d[32];
	u32 fpscr;
	u32 pad0;
	u32 fpexc;
	u32 fpinst;
	u32 fpinst2;
	u32 pad1;
};
static_assert(sizeof(VfpFrame) == 288);
inline constexpr u32 vfp_magic = 0x56465001;

struct UContext {
	word flags;
	UContext *link;
	StackT stack;
	SigContext mcontext;
	SigSet sigmask;
	i32 unused[30];
	alignas(8) u8 regspace[512];
};
static_assert(__builtin_offsetof(UContext, mcontext) == 20);
static_assert(__builtin_offsetof(UContext, regspace) == 232);

/* ---- shared state of one probe process ---- */

struct Stats {
	u32 checks;
	u32 failures;
	/* SIGILLs the probe expected and caught. */
	u32 sigill_expected;
	/* SIGILLs it did not expect: reported, and fatal to the group. */
	u32 sigill_unexpected;
};

extern Stats stats;
extern const char *group_name;
/* AT_HWCAP, AT_HWCAP2, AT_PAGESZ of the process. */
extern word hwcap;
extern word hwcap2;
extern word page_size;

/* AT_HWCAP and AT_HWCAP2 bits (arch/arm/include/uapi/asm/hwcap.h). */
enum : word {
	hwcap_neon = 1U << 12,
	hwcap_fphp = 1U << 22,
	hwcap_asimdhp = 1U << 23,
	hwcap_asimddp = 1U << 24,
	hwcap_asimdfhm = 1U << 25,
	hwcap_asimdbf16 = 1U << 26,
	hwcap_i8mm = 1U << 27,
	hwcap_vfpv3 = 1U << 13,
	hwcap_vfpv4 = 1U << 16,
	hwcap2_aes = 1U << 0,
	hwcap2_pmull = 1U << 1,
	hwcap2_sha1 = 1U << 2,
	hwcap2_sha2 = 1U << 3,
	hwcap2_crc32 = 1U << 4,
};

/* ---- output and checks ---- */

void put(const char *text);
void put_dec(u32 value);
void put_hex(u32 value);
void line(const char *a, const char *b = nullptr, const char *c = nullptr);

void check(bool passed, const char *name);
/* check() with the values shown on failure. */
void check_eq(u32 got, u32 want, const char *name);
void check_eq64(u64 got, u64 want, const char *name);
void check_mem(const void *got, const void *want, u32 length, const char *name);
[[noreturn]] void quit(long status);
/* Ends the running group now with the totals it has reached: for a group that
 * cannot go on, such as one whose threads spin and do not finish. */
[[noreturn]] void end_group();

/* ---- small library ---- */

word strlen(const char *text);
bool streq(const char *a, const char *b);
/* The value of environment variable `name` of the process, or null. */
const char *environment(const char *name);
/* Whether the program runs under qemu-user (A32_PROBE_QEMU), which checks the
 * probe's own logic only: it has no sigpage, no sigcontext fault address and
 * no SWP or SETEND trap. */
bool under_qemu();
/* The first mapping of /proc/self/maps whose line ends with `name` (such as
 * "[sigpage]"): its start and end. */
bool find_mapping(const char *name, word *start, word *end);

/* ---- abi.swp ---- */

/* The value of /proc/sys/abi/swp (0: SWP and SWPB raise SIGILL, 1: the kernel
 * emulates them), or a negative errno. */
long swp_setting();
/* Writes it; false when the write is refused, as it is to a shell. The first
 * write of a process remembers the value it found, and restore_swp() puts that
 * back; quit() calls it, and only the process that wrote it restores it. */
bool swp_set(int value);
void restore_swp();

/* ---- helpers every group uses ---- */

word mmap_pages(word length, word protection, word flags = map_private | map_anonymous);
long set_handler(int signal, void (*handler)(int, SigInfo *, UContext *),
		 word flags = sa_siginfo, word restorer = 0);
long reset_handler(int signal);
long block_signal(int signal, bool block);
long fork_child();
/* Waits for `pid`; returns the status word, or -errno. */
long wait_child(long pid);
bool exited_with(long status, int code);
bool killed_by(long status, int signal);
long raise_signal(int signal);
long getpid();
long gettid();
void yield();
/* Sleeps about `ms` milliseconds. */
void sleep_ms(u32 ms);

/* ---- threads ---- */

struct Thread {
	long tid;
	/* Cleared by the kernel when the thread exits. */
	volatile i32 alive;
	word stack;
	word stack_size;
};
/* Starts a thread running fn(arg) on its own 64 KiB stack in this process's
 * address space; `tls` is its TPIDRURO (CLONE_SETTLS) when not zero. Returns
 * false if clone failed. */
bool thread_start(Thread &thread, long (*function)(void *), void *argument, word tls = 0);
/* Waits up to `seconds` for the thread to exit; false on timeout. */
bool thread_join(Thread &thread, u32 seconds = 60);

/* ---- catching a signal the probe provokes ---- */

/* What the last caught signal said. */
struct Caught {
	u32 count;
	i32 signo;
	i32 code;
	word address;
	word pc;
	word cpsr;
	/* The sigcontext's fault_address. */
	word fault_address;
};
extern Caught caught;

/* Install a handler for `signal` that records the signal in `caught`, counts
 * a SIGILL as expected, and resumes `skip` bytes after the faulting pc (0
 * resumes at the pc itself, for a handler that fixed the cause; return_to_lr
 * resumes at the interrupted link register, in its instruction set, for a
 * fault in a called routine). */
inline constexpr word return_to_lr = ~0UL;
void catch_signal(int signal, word skip);
/* Back to the default: a SIGILL is a failure, any other signal kills. */
void release_signal(int signal);

/* ---- probes the groups share (engine_probe.cpp) ---- */

struct Group {
	const char *name;
	void (*run)();
};

extern const Group groups[];
extern const unsigned group_count;

void group_basic();
void group_interwork();
void group_signals();
void group_kuser();
void group_tls();
void group_cacheflush();
void group_faults();
void group_ill();
void group_ptrace();
void group_swp();
void group_atomics();
void group_simd();
void group_armv8_fp();
void group_absent();
void group_complete();

}
