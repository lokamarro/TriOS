// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freestanding arm64/Linux probe that a guest shows no SVE. The virtual CPU
 * implements none outside streaming mode, whatever the host reports, and
 * advertises none. It runs as the guest's /init and checks that the guest
 * kernel and the engine agree: no SVE bit in AT_HWCAP or AT_HWCAP2, no SVE
 * prctl, sysctl or ptrace regset, no sve_context in a signal frame without
 * SME and only the header of one outside streaming mode with it, as Linux
 * gives a CPU with SME and without SVE, and SVE instructions are undefined.
 *
 * The probe powers the guest off at the end. It prints PASS or FAIL lines,
 * then "no-sve-probe: <n> failure(s) in <m> check(s)".
 */

namespace {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long;

constexpr long nr_mkdirat = 34;
constexpr long nr_mount = 40;
constexpr long nr_openat = 56;
constexpr long nr_write = 64;
constexpr long nr_nanosleep = 101;
constexpr long nr_ptrace = 117;
constexpr long nr_kill = 129;
constexpr long nr_sigaltstack = 132;
constexpr long nr_rt_sigaction = 134;
constexpr long nr_reboot = 142;
constexpr long nr_prctl = 167;
constexpr long nr_getpid = 172;
constexpr long nr_wait4 = 260;

constexpr long at_fdcwd = -100;
constexpr long o_rdwr = 2;
constexpr long enoent = 2;
constexpr long ebadf = 9;
constexpr long einval = 22;

constexpr int sigill = 4;
constexpr int sigsegv = 11;
constexpr int sigchld = 17;
constexpr int sigstop = 19;
constexpr int ill_illopc = 1;

constexpr u64 sa_siginfo = 4;
constexpr u64 sa_onstack = 0x08000000;
constexpr u64 sa_nodefer = 0x40000000;

constexpr long pr_sve_set_vl = 50;
constexpr long pr_sve_get_vl = 51;

constexpr long ptrace_traceme = 0;
constexpr long ptrace_cont = 7;
constexpr long ptrace_getregset = 0x4204;
constexpr long nt_arm_sve = 0x405;

constexpr u64 at_hwcap = 16;
constexpr u64 at_hwcap2 = 26;
constexpr u64 at_minsigstksz = 51;
constexpr u64 hwcap_sve = 1UL << 22;
/* SVE2, SVEAES, SVEPMULL, SVEBITPERM, SVESHA3 and SVESM4; SVEI8MM,
 * SVEF32MM, SVEF64MM and SVEBF16; SVE2P1. */
constexpr u64 hwcap2_sve_bits = (0x3fUL << 1) | (0xfUL << 9) | (1UL << 36);
constexpr u64 hwcap2_sme = 1UL << 23;

/* Signal frame records (arch/arm64/include/uapi/asm/sigcontext.h). */
constexpr u32 fpsimd_magic = 0x46508001;
constexpr u32 sve_magic = 0x53564501;

/* struct rt_sigframe: siginfo, then the arm64 ucontext. */
constexpr u64 uc_offset = 128;
constexpr u64 mcontext_offset = uc_offset + 176;
constexpr u64 mc_pc_offset = mcontext_offset + 264;
constexpr u64 reserved_offset = mcontext_offset + 288;
constexpr u64 reserved_size = 4096;

/* ------------------------------------------------------------------ */
/* System calls, memory and output.                                    */

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0,
		long a3 = 0, long a4 = 0, long a5 = 0)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}

template <typename T> long ptr(T *value) { return reinterpret_cast<long>(value); }

} // namespace

extern "C" void *memset(void *destination, int value, u64 count)
{
	auto *bytes = static_cast<volatile u8 *>(destination);
	for (u64 index = 0; index < count; index++)
		bytes[index] = static_cast<u8>(value);
	return destination;
}

extern "C" void *memcpy(void *destination, const void *source, u64 count)
{
	auto *to = static_cast<volatile u8 *>(destination);
	const auto *from = static_cast<const volatile u8 *>(source);
	for (u64 index = 0; index < count; index++)
		to[index] = from[index];
	return destination;
}

namespace {

u64 string_length(const char *text)
{
	u64 length = 0;
	while (text[length] != '\0')
		length++;
	return length;
}

void write_text(const char *text)
{
	u64 length = string_length(text);
	while (length > 0) {
		long written = sys(nr_write, 1, ptr(text), static_cast<long>(length));
		if (written <= 0)
			return;
		text += written;
		length -= static_cast<u64>(written);
	}
}

/* One line, assembled first so that a child's lines do not interleave. */
struct Line {
	char text[256];
	u64 length = 0;

	Line &add(const char *part)
	{
		while (*part != '\0' && length + 2 < sizeof(text))
			text[length++] = *part++;
		text[length] = '\0';
		return *this;
	}
	Line &number(long value)
	{
		char digits[24];
		int count = 0;
		bool negative = value < 0;
		u64 magnitude = negative ? static_cast<u64>(-value) : static_cast<u64>(value);
		do {
			digits[count++] = static_cast<char>('0' + magnitude % 10);
			magnitude /= 10;
		} while (magnitude != 0);
		if (negative)
			add("-");
		while (count > 0) {
			char digit[2] = {digits[--count], '\0'};
			add(digit);
		}
		return *this;
	}
	Line &hex(u64 value)
	{
		static const char nibbles[] = "0123456789abcdef";
		add("0x");
		bool started = false;
		for (int shift = 60; shift >= 0; shift -= 4) {
			unsigned nibble = (value >> shift) & 15;
			if (nibble != 0 || started || shift == 0) {
				char digit[2] = {nibbles[nibble], '\0'};
				add(digit);
				started = true;
			}
		}
		return *this;
	}
	void print()
	{
		text[length++] = '\n';
		text[length] = '\0';
		write_text(text);
		length = 0;
	}
};

int failures;
int checks;

void check(bool passed, const char *name)
{
	checks++;
	if (!passed)
		failures++;
	Line().add(passed ? "PASS " : "FAIL ").add(name).print();
}

void check_value(u64 actual, u64 expected, const char *name)
{
	checks++;
	if (actual == expected) {
		Line().add("PASS ").add(name).print();
		return;
	}
	failures++;
	Line().add("FAIL ").add(name).add(": ").hex(actual).add(" != ").hex(expected).print();
}

void check_error(long result, long error, const char *name)
{
	check_value(static_cast<u64>(result), static_cast<u64>(-error), name);
}

u64 auxv_hwcap;
u64 auxv_hwcap2;
u64 auxv_minsigstksz;

/* ------------------------------------------------------------------ */
/* Faults and what the signal frame of one holds.                      */

extern "C" {
/* A load from address 0, which the SIGSEGV handler steps over. */
void fault();
/* SVE instructions, each of which must be undefined. */
void sve_ptrue();
void sve_add();
}

__asm__(R"(
	.text
	.arch	armv8.2-a+sve
	.p2align 2
	.globl fault
fault:
	mov	x9, #0
	ldr	x9, [x9]
	ret

	.globl sve_ptrue
sve_ptrue:
	ptrue	p0.b
	ret

	.globl sve_add
sve_add:
	add	z0.b, z0.b, z1.b
	ret
)");

template <typename T> T load(const u8 *at)
{
	T value;
	memcpy(&value, at, sizeof(value));
	return value;
}

template <typename T> void store(u8 *at, T value) { memcpy(at, &value, sizeof(value)); }

struct Capture {
	bool valid;
	u64 record_count;
	u64 sve_records;
	u32 first_magic;
	u32 sve_size;
	u16 sve_vl;
	u16 sve_flags;
};

Capture capture;
volatile int ill_code;

void segv_handler(int, void *, void *context)
{
	u8 *frame = static_cast<u8 *>(context) - uc_offset;
	u8 *head = frame + reserved_offset;
	u64 offset = 0;
	capture = {};
	capture.valid = true;
	while (offset + 8 <= reserved_size) {
		u32 magic = load<u32>(head + offset);
		u32 size = load<u32>(head + offset + 4);
		if (magic == 0 || size < 16 || size > reserved_size - offset)
			break;
		if (capture.record_count == 0)
			capture.first_magic = magic;
		capture.record_count++;
		if (magic == sve_magic) {
			if (capture.sve_records == 0) {
				capture.sve_size = size;
				capture.sve_vl = load<u16>(head + offset + 8);
				capture.sve_flags = load<u16>(head + offset + 10);
			}
			capture.sve_records++;
		}
		offset += size;
	}
	store<u64>(frame + mc_pc_offset, load<u64>(frame + mc_pc_offset) + 4);
}

void ill_handler(int, void *info, void *context)
{
	ill_code = load<int>(static_cast<u8 *>(info) + 8);
	u8 *frame = static_cast<u8 *>(context) - uc_offset;
	store<u64>(frame + mc_pc_offset, load<u64>(frame + mc_pc_offset) + 4);
}

struct KernelSigaction {
	void (*handler)(int, void *, void *);
	u64 flags;
	void (*restorer)();
	u64 mask;
};

alignas(16) u8 altstack[512 * 1024];

void set_handler(int signal, void (*handler)(int, void *, void *))
{
	KernelSigaction action = {handler, sa_siginfo | sa_onstack | sa_nodefer, nullptr, 0};
	sys(nr_rt_sigaction, signal, ptr(&action), 0, 8);
}

void install_handlers()
{
	struct {
		void *sp;
		int flags;
		u64 size;
	} stack = {altstack, 0, sizeof(altstack)};
	sys(nr_sigaltstack, ptr(&stack), 0);
	set_handler(sigsegv, segv_handler);
	set_handler(sigill, ill_handler);
}

/* An undefined instruction raises SIGILL with ILL_ILLOPC. */
void check_undefined(void (*instruction)(), const char *name)
{
	ill_code = 0;
	instruction();
	check_value(static_cast<u64>(ill_code), ill_illopc, name);
}

/* ------------------------------------------------------------------ */
/* A tracee, for the ptrace regsets.                                   */

struct Iovec {
	void *base;
	u64 length;
};

alignas(16) u8 regset_buffer[4096];

long get_regset(long pid, long type)
{
	Iovec iov = {regset_buffer, sizeof(regset_buffer)};
	long result = sys(nr_ptrace, ptrace_getregset, pid, type, ptr(&iov));
	return result < 0 ? result : static_cast<long>(iov.length);
}

int wait_stop(long pid)
{
	int status = 0;
	if (sys(nr_wait4, pid, ptr(&status), 0, 0) != pid)
		return -1;
	return status;
}

bool stopped_with(int status, int signal)
{
	return (status & 0xff) == 0x7f && ((status >> 8) & 0xff) == signal;
}

int tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	sys(nr_kill, sys(nr_getpid), sigstop);
	return 0;
}

extern "C" {
/* clone(flags, stack); the child runs function(argument) on `stack` and
 * exits with its result. Returns the child's PID (or -errno) in the parent. */
long clone_run(u64 flags, void *stack, int (*function)(void *), void *argument);
}

__asm__(R"(
	.text
	.p2align 2
	.globl clone_run
clone_run:
	mov	x10, x2
	mov	x11, x3
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #220
	svc	#0
	cbnz	x0, 1f
	mov	x0, x11
	blr	x10
	mov	x8, #93
	svc	#0
	brk	#0x53
1:	ret
)");

/* ------------------------------------------------------------------ */

const char sysctl_path[] = "/proc/sys/abi/sve_default_vector_length";

void test_no_sve()
{
	check((auxv_hwcap & hwcap_sve) == 0, "AT_HWCAP has no HWCAP_SVE");
	check((auxv_hwcap2 & hwcap2_sve_bits) == 0, "AT_HWCAP2 has no SVE bit");
	check_error(sys(nr_prctl, pr_sve_get_vl), einval, "PR_SVE_GET_VL fails with EINVAL");
	check_error(sys(nr_prctl, pr_sve_set_vl, 16), einval, "PR_SVE_SET_VL fails with EINVAL");
	check_error(sys(nr_openat, at_fdcwd, ptr(sysctl_path), 0), enoent,
		    "abi.sve_default_vector_length does not exist");

	/* Outside streaming mode a signal frame starts with fpsimd_context.
	 * With SME it also holds Linux's sve_context for a CPU with SME and
	 * without SVE, which is only a header: its vector length and flags are
	 * zero, since streaming mode is the one place SVE state exists. Without
	 * SME there is no sve_context, and the auxiliary vector sizes a signal
	 * stack for no SVE or SME record at all. */
	capture = {};
	fault();
	check(capture.valid && capture.first_magic == fpsimd_magic,
	      "a signal frame starts with fpsimd_context");
	if ((auxv_hwcap2 & hwcap2_sme) != 0) {
		check_value(capture.sve_records, 1, "a signal frame holds one sve_context with SME");
		check_value(capture.sve_size, 16, "the sve_context is a bare header");
		check_value(capture.sve_vl, 0, "the sve_context has vector length 0");
		check_value(capture.sve_flags, 0, "the sve_context is not streaming");
	} else {
		check_value(capture.sve_records, 0, "a signal frame holds no sve_context");
		check_value(auxv_minsigstksz, 0, "the auxiliary vector has no AT_MINSIGSTKSZ");
		check_value(capture.record_count, 1, "a signal frame holds fpsimd_context alone");
	}

	check_undefined(sve_ptrue, "PTRUE is an undefined instruction");
	check_undefined(sve_add, "ADD (vectors, unpredicated) is an undefined instruction");

	long pid = clone_run(sigchld, nullptr, tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop)) {
		check(false, "the tracee stops");
		return;
	}
	check_error(get_regset(pid, nt_arm_sve), einval, "NT_ARM_SVE does not exist");
	sys(nr_ptrace, ptrace_cont, pid, 0, 0);
	check(wait_stop(pid) == 0, "the tracee exits");
}

void setup_console()
{
	if (sys(nr_write, 1, ptr(""), 0) != -ebadf)
		return;
	sys(nr_mkdirat, at_fdcwd, ptr("/dev"), 0755);
	sys(nr_mount, ptr("devtmpfs"), ptr("/dev"), ptr("devtmpfs"), 0, 0);
	for (int fd = 0; fd < 3; fd++)
		sys(nr_openat, at_fdcwd, ptr("/dev/console"), o_rdwr);
}

void mount_proc()
{
	sys(nr_mkdirat, at_fdcwd, ptr("/proc"), 0555);
	sys(nr_mount, ptr("proc"), ptr("/proc"), ptr("proc"), 0, 0);
}

[[noreturn]] void finish()
{
	for (;;) {
		sys(nr_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0);
		struct {
			long seconds, nanoseconds;
		} delay = {1, 0};
		sys(nr_nanosleep, ptr(&delay), 0);
	}
}

} // namespace

extern "C" [[noreturn]] void probe_main(u64 *initial)
{
	int argc = static_cast<int>(initial[0]);
	u64 *auxv = initial + 1 + argc + 1;
	while (*auxv != 0)
		auxv++;
	auxv++;
	for (; auxv[0] != 0; auxv += 2) {
		if (auxv[0] == at_hwcap)
			auxv_hwcap = auxv[1];
		else if (auxv[0] == at_hwcap2)
			auxv_hwcap2 = auxv[1];
		else if (auxv[0] == at_minsigstksz)
			auxv_minsigstksz = auxv[1];
	}
	setup_console();
	mount_proc();
	install_handlers();
	test_no_sve();
	Line().add("no-sve-probe: ").number(failures).add(" failure(s) in ")
		.number(checks).add(" check(s)").print();
	finish();
}

__asm__(R"(
	.text
	.p2align 2
	.globl _start
_start:
	mov	x0, sp
	bl	probe_main
	brk	#0x50
)");
