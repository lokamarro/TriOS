// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freestanding arm64/Linux probe of the guest kernel's SME task state, run as
 * the guest's /init with the Darwin host's SME policy on.
 *
 * "kernel" mode never executes an SME instruction. It reaches every state the
 * kernel keeps through rt_sigreturn frames and ptrace regsets alone, so it
 * proves the kernel's duties whatever the execution engines implement:
 * signal frame records, sizes and layout for the run's streaming vector
 * length, sigreturn and its rejection of invalid frames, the syscall SMSTOP,
 * a handler's starting state, fork, vfork, clone with and without CLONE_VM,
 * exec, the NT_ARM_SSVE, NT_ARM_ZA, NT_ARM_ZT and NT_ARM_TLS regsets, a
 * syscall-entry stop, AT_MINSIGSTKSZ, PR_SME_SET_VL and PR_SME_GET_VL, and
 * abi.sme_default_vector_length. Code that runs while PSTATE.SM is set is
 * hand-written GPR-only assembly, and the whole probe is compiled with
 * -mgeneral-regs-only.
 *
 * "spin" mode enters streaming mode with ZA live the same way and spins in a
 * GPR loop while the runner signals the emulator, then checks that the
 * kernel's copy of ZA, ZT0 and TPIDR2 is intact.
 *
 * "engine" mode executes the control core of SME (SMSTART, SMSTOP, MRS SVCR,
 * TPIDR2, RDSVL, LDR/STR ZA, ZERO ZA, LDR/STR/ZERO ZT0) and needs an engine
 * at SmeSupport control or better; "engine-spin" is the signal
 * test with ZA loaded by the engine.
 *
 * "off" mode runs with the policy off, on any host, and checks that a run
 * without SME shows none of it: no HWCAP2 bit, AT_MINSIGSTKSZ, prctl, sysctl,
 * regset or signal frame record. NT_ARM_TLS exists without SME, as natively:
 * it reads and writes TPIDR_EL0, and its TPIDR2_EL0 reads as zero and ignores
 * writes.
 *
 * No guest has non-streaming SVE: an sve_context outside streaming mode is a
 * header with vector length 0, and "off" mode's frames hold no sve_context
 * and its auxiliary vector no AT_MINSIGSTKSZ.
 *
 * SME_PROBE_MODE=<mode> on the kernel command line selects the mode, "kernel"
 * by default. Every mode prints PASS or FAIL lines, then "sme-probe: <mode>:
 * <n> failure(s) in <m> check(s)", and powers the guest off.
 */

namespace {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long;

constexpr long nr_mkdirat = 34;
constexpr long nr_mount = 40;
constexpr long nr_openat = 56;
constexpr long nr_close = 57;
constexpr long nr_read = 63;
constexpr long nr_write = 64;
constexpr long nr_exit = 93;
constexpr long nr_nanosleep = 101;
constexpr long nr_ptrace = 117;
constexpr long nr_kill = 129;
constexpr long nr_sigaltstack = 132;
constexpr long nr_rt_sigaction = 134;
constexpr long nr_reboot = 142;
constexpr long nr_prctl = 167;
constexpr long nr_getpid = 172;
constexpr long nr_execve = 221;
constexpr long nr_wait4 = 260;

constexpr long at_fdcwd = -100;
constexpr long o_rdwr = 2;
constexpr long o_wronly = 1;
constexpr long enoent = 2;
constexpr long ebadf = 9;
constexpr long einval = 22;
constexpr long eio = 5;

constexpr int sigtrap = 5;
constexpr int sigsegv = 11;
constexpr int sigchld = 17;
constexpr int sigstop = 19;

constexpr u64 sa_siginfo = 4;
constexpr u64 sa_onstack = 0x08000000;
constexpr u64 sa_nodefer = 0x40000000;

constexpr u64 clone_vm = 0x100;
constexpr u64 clone_vfork = 0x4000;

constexpr long pr_sme_set_vl = 63;
constexpr long pr_sme_get_vl = 64;
constexpr long pr_sme_set_vl_onexec = 1L << 18;
constexpr long pr_sme_vl_inherit = 1L << 17;
constexpr long pr_sme_vl_len_mask = 0xffff;

constexpr long ptrace_traceme = 0;
constexpr long ptrace_cont = 7;
constexpr long ptrace_syscall = 24;
constexpr long ptrace_setoptions = 0x4200;
constexpr long ptrace_getregset = 0x4204;
constexpr long ptrace_setregset = 0x4205;
constexpr long ptrace_o_tracesysgood = 1;

constexpr long nt_prstatus = 1;
constexpr long nt_prfpreg = 2;
constexpr long nt_arm_tls = 0x401;
constexpr long nt_arm_ssve = 0x40b;
constexpr long nt_arm_za = 0x40c;
constexpr long nt_arm_zt = 0x40d;

constexpr u64 at_hwcap2 = 26;
constexpr u64 at_minsigstksz = 51;
constexpr u64 hwcap2_sme = 1UL << 23;
constexpr u64 hwcap2_sme2 = 1UL << 37;

/* Signal frame records (arch/arm64/include/uapi/asm/sigcontext.h). */
constexpr u32 fpsimd_magic = 0x46508001;
constexpr u32 extra_magic = 0x45585401;
constexpr u32 sve_magic = 0x53564501;
constexpr u32 tpidr2_magic = 0x54504902;
constexpr u32 za_magic = 0x54366345;
constexpr u32 zt_magic = 0x5a544e01;
constexpr u16 sve_sig_flag_sm = 1;

constexpr u64 fpsimd_size = 528;
constexpr u64 terminator_size = 16;
constexpr u64 extra_size = 32;
constexpr u64 sigframe_maxsz = 256 * 1024;
constexpr u64 reserved_size = 4096;

/* struct rt_sigframe: siginfo, then the arm64 ucontext. */
constexpr u64 uc_offset = 128;
constexpr u64 uc_stack_offset = uc_offset + 16;
constexpr u64 uc_sigmask_offset = uc_offset + 40;
constexpr u64 mcontext_offset = uc_offset + 176;
constexpr u64 mc_regs_offset = mcontext_offset + 8;
constexpr u64 mc_sp_offset = mcontext_offset + 256;
constexpr u64 mc_pc_offset = mcontext_offset + 264;
constexpr u64 mc_pstate_offset = mcontext_offset + 272;
constexpr u64 reserved_offset = mcontext_offset + 288;
constexpr u64 rt_sigframe_size = reserved_offset + reserved_size;
/* sigreturn_into() and the native frame use these offsets. */
static_assert(mc_regs_offset == 312 && mc_sp_offset == 560 &&
	      mc_pc_offset == 568 && reserved_offset == 592 &&
	      rt_sigframe_size == 4688);

/* The FPSR SMSTOP leaves (ResetSVEState). */
constexpr u32 smstop_fpsr = 0x0800009f;

/* Frame layout: the Z/P/FFR part of sve_context and the ptrace SVE layout. */
constexpr u64 round16(u64 value) { return (value + 15) & ~15UL; }
constexpr u64 sve_zreg_offset(u64 vq, u64 n) { return 16 + n * vq * 16; }
constexpr u64 sve_preg_offset(u64 vq, u64 n) { return 16 + 32 * vq * 16 + n * vq * 2; }
constexpr u64 sve_ffr_offset(u64 vq) { return sve_preg_offset(vq, 16); }
constexpr u64 sve_context_size(u64 vq) { return vq ? sve_ffr_offset(vq) + vq * 2 : 16; }
constexpr u64 za_context_size(u64 vq) { return 16 + vq * 16 * vq * 16; }
constexpr u64 zt_context_size = 16 + 64;
constexpr u64 sve_pt_fpsr_offset(u64 vq) { return round16(sve_ffr_offset(vq) + vq * 2); }
constexpr u64 sve_pt_size_sve(u64 vq) { return round16(sve_pt_fpsr_offset(vq) + 8); }
constexpr u64 za_pt_size(u64 vq) { return 16 + vq * 16 * vq * 16; }
constexpr u16 sve_pt_regs_fpsimd = 0;
constexpr u16 sve_pt_regs_sve = 1;

constexpr u64 max_vl = 256;
constexpr u64 max_sve_record = 16 + 34 * max_vl + max_vl / 8;
constexpr u64 max_za_record = 16 + max_vl * max_vl;

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

extern "C" int memcmp(const void *left, const void *right, u64 count)
{
	const auto *a = static_cast<const volatile u8 *>(left);
	const auto *b = static_cast<const volatile u8 *>(right);
	for (u64 index = 0; index < count; index++)
		if (a[index] != b[index])
			return a[index] < b[index] ? -1 : 1;
	return 0;
}

namespace {

u64 string_length(const char *text)
{
	u64 length = 0;
	while (text[length] != '\0')
		length++;
	return length;
}

bool same_string(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}
	return *left == *right;
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
const char *group = "";

void check(bool passed, const char *name)
{
	checks++;
	if (!passed)
		failures++;
	Line().add(passed ? "PASS " : "FAIL ").add(group).add(": ").add(name).print();
}

void check_value(u64 actual, u64 expected, const char *name)
{
	checks++;
	if (actual == expected) {
		Line().add("PASS ").add(group).add(": ").add(name).print();
		return;
	}
	failures++;
	Line().add("FAIL ").add(group).add(": ").add(name).add(": ")
		.hex(actual).add(" != ").hex(expected).print();
}

void note(const char *text, long value)
{
	Line().add("sme-probe: ").add(text).add(" ").number(value).print();
}

[[noreturn]] void exit_task(long code)
{
	for (;;)
		sys(nr_exit, code);
}

/* ------------------------------------------------------------------ */
/* The run's SME parameters.                                           */

u64 svl;      /* streaming vector length, bytes */
u64 svq;      /* the same in quadwords */
bool sme2;
u64 auxv_minsigstksz;
u64 auxv_hwcap2;
const char *self_path = "/init";

/* Deterministic register contents. */
u8 z_byte(u64 reg, u64 byte) { return static_cast<u8>(reg * 7 + byte * 13 + 1); }
u8 p_byte(u64 reg, u64 byte) { return static_cast<u8>(0xa5 ^ (reg * 3 + byte)); }
u8 v_byte(u64 reg, u64 byte) { return static_cast<u8>(0xf0 ^ (reg + byte * 5)); }
u8 za_byte(u64 row, u64 byte, u64 seed) { return static_cast<u8>(row * 5 + byte * 3 + 0x11 + seed); }
u8 zt_byte(u64 byte, u64 seed) { return static_cast<u8>(byte * 9 + 0x42 + seed); }

/* ------------------------------------------------------------------ */
/* Assembly: sigreturn into a frame, continuations, clone, faults.     */

extern "C" {
/* Saves the callee-saved registers, SP and the return address into the
 * frame's mcontext, sets its PC to `continuation`, and runs rt_sigreturn on
 * it. The continuation runs with the frame's state and returns to this
 * function's caller. */
void sigreturn_into(u8 *frame, void (*continuation)());
/* Continuations. While PSTATE.SM may be set they use only GPRs, and they
 * leave streaming mode through a system call before returning to C++. */
void continue_return();
void continue_fault_twice_then_syscall();
void continue_syscall();
void continue_exit_zero();
void continue_spin_then_syscall();
/* A load from address 0, which the SIGSEGV handler steps over. */
void fault();
/* kill(pid, SIGSTOP), then a fault, then a system call. */
void stop_fault_syscall(long pid);
/* clone(flags, stack); the child runs function(argument) on `stack` and
 * exits with its result. Returns the child's PID (or -errno) in the parent. */
long clone_run(u64 flags, void *stack, int (*function)(void *), void *argument);
}

__asm__(R"(
	.text
	.p2align 2
	.globl sigreturn_into
sigreturn_into:
	add	x9, x0, #312
	stp	x19, x20, [x9, #152]
	stp	x21, x22, [x9, #168]
	stp	x23, x24, [x9, #184]
	stp	x25, x26, [x9, #200]
	stp	x27, x28, [x9, #216]
	stp	x29, x30, [x9, #232]
	mov	x10, sp
	str	x10, [x0, #560]
	str	x1, [x0, #568]
	mov	sp, x0
	mov	x8, #139
	svc	#0
	brk	#0x51

	.globl continue_return
continue_return:
	ret

	.globl continue_fault_twice_then_syscall
continue_fault_twice_then_syscall:
	mov	x9, #0
	ldr	x9, [x9]
	mov	x9, #0
	ldr	x9, [x9]
	mov	x8, #172
	svc	#0
	ret

	.globl continue_syscall
continue_syscall:
	mov	x8, #172
	svc	#0
	ret

	.globl continue_exit_zero
continue_exit_zero:
	mov	x0, #0
	mov	x8, #93
	svc	#0
	brk	#0x52

	.globl continue_spin_then_syscall
continue_spin_then_syscall:
	adrp	x9, spin_seconds
	ldr	x9, [x9, :lo12:spin_seconds]
	mrs	x10, cntfrq_el0
	mul	x9, x9, x10
	mrs	x10, cntvct_el0
	add	x9, x9, x10
1:	mrs	x10, cntvct_el0
	add	x11, x11, #1
	cmp	x10, x9
	b.lo	1b
	mov	x9, #0
	ldr	x9, [x9]
	mov	x8, #172
	svc	#0
	ret

	.globl fault
fault:
	mov	x9, #0
	ldr	x9, [x9]
	ret

	.globl stop_fault_syscall
stop_fault_syscall:
	mov	x1, #19
	mov	x8, #129
	svc	#0
	mov	x9, #0
	ldr	x9, [x9]
	mov	x8, #172
	svc	#0
	ret

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

} // namespace

extern "C" u64 spin_seconds;
u64 spin_seconds = 10;

namespace {

/* ------------------------------------------------------------------ */
/* The SIGSEGV handler and what it captures.                           */

struct Capture {
	bool valid;
	const char *error;      /* the first layout error, or nullptr */
	u64 frame_size;         /* from the frame to its frame record */
	u64 record_count;
	u32 magics[12];
	u64 offsets[12];        /* each record's offset in the frame */
	u32 sizes[12];
	bool extra;
	u64 fault_address;
	u32 fpsr, fpcr;
	u8 vregs[512];
	bool has_sve;
	u16 sve_vl, sve_flags;
	u32 sve_size;
	u8 sve_data[max_sve_record];
	bool has_tpidr2;
	u64 tpidr2;
	bool has_za;
	u16 za_vl;
	u32 za_size;
	u8 za_data[max_za_record];
	bool has_zt;
	u16 zt_nregs;
	u32 zt_size;
	u8 zt_data[64];
};

Capture captures[3];
volatile int capture_count;
volatile int nested_request;
u8 *altstack_top;

template <typename T> T load(const u8 *at)
{
	T value;
	memcpy(&value, at, sizeof(value));
	return value;
}

template <typename T> void store(u8 *at, T value) { memcpy(at, &value, sizeof(value)); }

void parse_frame(Capture &capture, u8 *frame, bool measure)
{
	memset(&capture, 0, sizeof(capture));
	capture.valid = true;
	if (measure) {
		u64 record = (reinterpret_cast<u64>(altstack_top) - 16) & ~15UL;
		capture.frame_size = record - reinterpret_cast<u64>(frame);
	}
	capture.fault_address = load<u64>(frame + mcontext_offset);
	u8 *base = frame + reserved_offset;
	u64 limit = reserved_size;
	u64 offset = 0;
	bool extra_seen = false;
	for (;;) {
		if (capture.record_count == 12 || limit - offset < 8) {
			capture.error = "records run past their space";
			return;
		}
		u8 *head = base + offset;
		u32 magic = load<u32>(head);
		u32 size = load<u32>(head + 4);
		if (magic == 0) {
			if (size != 0)
				capture.error = "terminator with a size";
			return;
		}
		if (size < 16 || (size & 15) != 0 || size > limit - offset) {
			capture.error = "record size";
			return;
		}
		u64 index = capture.record_count++;
		capture.magics[index] = magic;
		capture.offsets[index] = static_cast<u64>(head - frame);
		capture.sizes[index] = size;
		switch (magic) {
		case fpsimd_magic:
			memcpy(capture.vregs, head + 16, sizeof(capture.vregs));
			capture.fpsr = load<u32>(head + 8);
			capture.fpcr = load<u32>(head + 12);
			break;
		case sve_magic:
			capture.has_sve = true;
			capture.sve_vl = load<u16>(head + 8);
			capture.sve_flags = load<u16>(head + 10);
			capture.sve_size = size;
			memcpy(capture.sve_data, head, size < max_sve_record ? size : max_sve_record);
			break;
		case tpidr2_magic:
			capture.has_tpidr2 = true;
			capture.tpidr2 = load<u64>(head + 8);
			break;
		case za_magic:
			capture.has_za = true;
			capture.za_vl = load<u16>(head + 8);
			capture.za_size = size;
			memcpy(capture.za_data, head, size < max_za_record ? size : max_za_record);
			break;
		case zt_magic:
			capture.has_zt = true;
			capture.zt_nregs = load<u16>(head + 8);
			capture.zt_size = size;
			if (size >= zt_context_size)
				memcpy(capture.zt_data, head + 16, 64);
			break;
		case extra_magic: {
			u64 datap = load<u64>(head + 8);
			u32 data_size = load<u32>(head + 16);
			u8 *terminator = head + size;
			if (extra_seen || load<u64>(terminator) != 0 ||
			    datap != reinterpret_cast<u64>(terminator + 16)) {
				capture.error = "extra_context";
				return;
			}
			extra_seen = capture.extra = true;
			base = reinterpret_cast<u8 *>(datap);
			limit = data_size;
			offset = 0;
			continue;
		}
		default:
			capture.error = "unexpected record";
			return;
		}
		offset += size;
	}
}

void segv_handler(int, void *, void *context)
{
	u8 *frame = static_cast<u8 *>(context) - uc_offset;
	int index = capture_count;
	if (index < 3) {
		parse_frame(captures[index], frame, nested_request != 2);
		capture_count = index + 1;
	}
	if (nested_request == 1) {
		/* The handler's own state, seen through a fault inside it. */
		nested_request = 2;
		fault();
		nested_request = 0;
	}
	store<u64>(frame + mc_pc_offset, load<u64>(frame + mc_pc_offset) + 4);
}

struct KernelSigaction {
	void (*handler)(int, void *, void *);
	u64 flags;
	void (*restorer)();
	u64 mask;
};

alignas(16) u8 altstack[512 * 1024];

void install_handler()
{
	struct {
		void *sp;
		int flags;
		u64 size;
	} stack = {altstack, 0, sizeof(altstack)};
	sys(nr_sigaltstack, ptr(&stack), 0);
	altstack_top = altstack + sizeof(altstack);
	KernelSigaction action = {segv_handler, sa_siginfo | sa_onstack | sa_nodefer, nullptr, 0};
	sys(nr_rt_sigaction, sigsegv, ptr(&action), 0, 8);
}

void default_segv()
{
	KernelSigaction action = {nullptr, 0, nullptr, 0};
	sys(nr_rt_sigaction, sigsegv, ptr(&action), 0, 8);
}

void reset_captures()
{
	capture_count = 0;
	nested_request = 0;
}

/* ------------------------------------------------------------------ */
/* The frame layout the kernel must produce, by native's rules.        */

struct Layout {
	u64 size;
	u64 limit;
	bool extra;
	u64 count;
	u32 magics[12];
	u64 offsets[12];
	u32 sizes[12];
};

bool layout_alloc(Layout &layout, u32 magic, u64 size, bool extend = true)
{
	u64 padded = round16(size);
	if (padded > layout.limit - layout.size && !layout.extra && extend) {
		layout.limit += extra_size;
		if (!layout_alloc(layout, extra_magic, extra_size, false))
			return false;
		layout.extra = true;
		layout.size += terminator_size;
		layout.limit = sigframe_maxsz - terminator_size;
	}
	if (padded > layout.limit - layout.size)
		return false;
	layout.magics[layout.count] = magic;
	layout.offsets[layout.count] = layout.size;
	layout.sizes[layout.count] = static_cast<u32>(padded);
	layout.count++;
	layout.size += padded;
	return true;
}

/* The frame of a task with the given state; `all` is AT_MINSIGSTKSZ's. */
Layout expected_layout(bool streaming, bool za, bool all = false)
{
	Layout layout{};
	layout.size = reserved_offset;
	layout.limit = reserved_offset + reserved_size - terminator_size - extra_size;
	layout_alloc(layout, fpsimd_magic, fpsimd_size);
	layout_alloc(layout, sve_magic, sve_context_size(all || streaming ? svq : 0));
	layout_alloc(layout, tpidr2_magic, 16);
	layout_alloc(layout, za_magic, za_context_size(all || za ? svq : 0));
	if (sme2 && (all || za))
		layout_alloc(layout, zt_magic, zt_context_size);
	layout.limit += terminator_size;
	layout.size += terminator_size;
	return layout;
}

u64 frame_size(const Layout &layout)
{
	return round16(layout.size > rt_sigframe_size ? layout.size : rt_sigframe_size);
}

void check_layout(const Capture &capture, bool streaming, bool za, const char *what)
{
	Layout layout = expected_layout(streaming, za);
	Line line;
	bool same = capture.valid && capture.error == nullptr &&
		capture.extra == layout.extra;
	/* Both list an extra_context record where it lies. */
	u64 compared = 0;
	for (u64 index = 0; same && index < layout.count; index++) {
		same = compared < capture.record_count &&
			capture.magics[compared] == layout.magics[index] &&
			capture.offsets[compared] == layout.offsets[index] &&
			capture.sizes[compared] == layout.sizes[index];
		compared++;
	}
	same = same && compared == capture.record_count;
	checks++;
	if (same && (capture.frame_size == 0 || capture.frame_size == frame_size(layout))) {
		Line().add("PASS ").add(group).add(": ").add(what).add(" frame layout").print();
		return;
	}
	failures++;
	line.add("FAIL ").add(group).add(": ").add(what).add(" frame layout: ");
	if (capture.error != nullptr)
		line.add(capture.error).add(" ");
	line.add("records ").number(static_cast<long>(capture.record_count)).add(" expected ")
		.number(static_cast<long>(layout.count))
		.add(" size ").number(static_cast<long>(capture.frame_size))
		.add(" expected ").number(static_cast<long>(frame_size(layout)));
	for (u64 index = 0; index < capture.record_count; index++)
		line.add(" ").hex(capture.magics[index]).add(":")
			.number(static_cast<long>(capture.sizes[index])).add("@")
			.number(static_cast<long>(capture.offsets[index]));
	line.print();
}

/* ------------------------------------------------------------------ */
/* Frames for rt_sigreturn.                                            */

alignas(16) u8 frame_buffer[sigframe_maxsz + 4096];

struct FrameBuilder {
	u8 *frame = frame_buffer;
	Layout layout{};
	u64 extra_offset = 0;

	explicit FrameBuilder()
	{
		memset(frame_buffer, 0, sizeof(frame_buffer));
		/* Keep the alternate signal stack: sigreturn restores it. */
		store<u64>(frame + uc_stack_offset, reinterpret_cast<u64>(altstack));
		store<int>(frame + uc_stack_offset + 8, 0);
		store<u64>(frame + uc_stack_offset + 16, sizeof(altstack));
		store<u64>(frame + uc_sigmask_offset, 0);
		store<u64>(frame + mc_pstate_offset, 0);
		layout.size = reserved_offset;
		layout.limit = reserved_offset + reserved_size - terminator_size - extra_size;
	}

	u8 *add(u32 magic, u64 size)
	{
		u64 before = layout.count;
		if (!layout_alloc(layout, magic, size))
			return nullptr;
		if (layout.count == before + 2)
			extra_offset = layout.offsets[before];
		u8 *head = frame + layout.offsets[layout.count - 1];
		store<u32>(head, magic);
		store<u32>(head + 4, static_cast<u32>(round16(size)));
		return head;
	}

	u8 *fpsimd(u32 fpsr = 0, u32 fpcr = 0)
	{
		u8 *head = add(fpsimd_magic, fpsimd_size);
		for (u64 reg = 0; reg < 32; reg++)
			for (u64 byte = 0; byte < 16; byte++)
				head[16 + reg * 16 + byte] = v_byte(reg, byte);
		store<u32>(head + 8, fpsr);
		store<u32>(head + 12, fpcr);
		return head;
	}

	u8 *sve_streaming()
	{
		u8 *head = add(sve_magic, sve_context_size(svq));
		store<u16>(head + 8, static_cast<u16>(svl));
		store<u16>(head + 10, sve_sig_flag_sm);
		for (u64 reg = 0; reg < 32; reg++)
			for (u64 byte = 0; byte < svl; byte++)
				head[sve_zreg_offset(svq, reg) + byte] = z_byte(reg, byte);
		for (u64 reg = 0; reg < 16; reg++)
			for (u64 byte = 0; byte < svl / 8; byte++)
				head[sve_preg_offset(svq, reg) + byte] = p_byte(reg, byte);
		return head;
	}

	/* Outside streaming mode the header names vector length 0. */
	u8 *sve_header()
	{
		u8 *head = add(sve_magic, 16);
		store<u16>(head + 8, 0);
		return head;
	}

	u8 *tpidr2(u64 value)
	{
		u8 *head = add(tpidr2_magic, 16);
		store<u64>(head + 8, value);
		return head;
	}

	u8 *za(bool live, u64 seed = 0)
	{
		u8 *head = add(za_magic, live ? za_context_size(svq) : 16);
		store<u16>(head + 8, static_cast<u16>(svl));
		if (live)
			for (u64 row = 0; row < svl; row++)
				for (u64 byte = 0; byte < svl; byte++)
					head[16 + row * svl + byte] = za_byte(row, byte, seed);
		return head;
	}

	u8 *zt(u64 seed = 0)
	{
		u8 *head = add(zt_magic, zt_context_size);
		store<u16>(head + 8, 1);
		for (u64 byte = 0; byte < 64; byte++)
			head[16 + byte] = zt_byte(byte, seed);
		return head;
	}

	u8 *finish()
	{
		layout.limit += terminator_size;
		layout.size += terminator_size;
		if (layout.extra) {
			u8 *extra = frame + extra_offset;
			u8 *data = extra + extra_size + terminator_size;
			store<u32>(extra, extra_magic);
			store<u32>(extra + 4, static_cast<u32>(extra_size));
			store<u64>(extra + 8, reinterpret_cast<u64>(data));
			store<u32>(extra + 16, static_cast<u32>(frame + round16(layout.size) - data));
		}
		return frame;
	}
};

/* A frame with every SME state: SM with Z/P, TPIDR2, ZA and, with SME2,
 * ZT0. */
u8 *full_frame(u64 tpidr2, u64 seed, bool streaming = true, bool za = true)
{
	FrameBuilder builder;
	builder.fpsimd(0x11, 0x00400000);
	if (streaming)
		builder.sve_streaming();
	else
		builder.sve_header();
	builder.tpidr2(tpidr2);
	builder.za(za, seed);
	if (sme2 && za)
		builder.zt(seed);
	return builder.finish();
}

/* ------------------------------------------------------------------ */
/* Checks of a captured frame against the state that produced it.     */

bool za_matches(const Capture &capture, u64 seed)
{
	if (!capture.has_za || capture.za_size != za_context_size(svq))
		return false;
	for (u64 row = 0; row < svl; row++)
		for (u64 byte = 0; byte < svl; byte++)
			if (capture.za_data[16 + row * svl + byte] != za_byte(row, byte, seed))
				return false;
	return true;
}

bool zt_matches(const Capture &capture, u64 seed)
{
	if (!capture.has_zt || capture.zt_nregs != 1)
		return false;
	for (u64 byte = 0; byte < 64; byte++)
		if (capture.zt_data[byte] != zt_byte(byte, seed))
			return false;
	return true;
}

/* Streaming Z/P as a sigreturn frame set them: V supplies Z's low bytes. */
bool streaming_matches(const Capture &capture, bool v_low)
{
	if (!capture.has_sve || capture.sve_vl != svl ||
	    capture.sve_flags != sve_sig_flag_sm ||
	    capture.sve_size != round16(sve_context_size(svq)))
		return false;
	for (u64 reg = 0; reg < 32; reg++)
		for (u64 byte = 0; byte < svl; byte++) {
			u8 expected = byte < 16 && v_low ? v_byte(reg, byte) : z_byte(reg, byte);
			if (capture.sve_data[sve_zreg_offset(svq, reg) + byte] != expected)
				return false;
		}
	for (u64 reg = 0; reg < 16; reg++)
		for (u64 byte = 0; byte < svl / 8; byte++)
			if (capture.sve_data[sve_preg_offset(svq, reg) + byte] != p_byte(reg, byte))
				return false;
	for (u64 byte = 0; byte < svl / 8; byte++)
		if (capture.sve_data[sve_ffr_offset(svq) + byte] != 0)
			return false;
	return true;
}

/* Outside streaming mode: a header with vector length 0, as the guest has no
 * non-streaming SVE state. */
bool not_streaming(const Capture &capture)
{
	return capture.has_sve && capture.sve_vl == 0 && capture.sve_flags == 0 &&
		capture.sve_size == 16;
}

bool za_off(const Capture &capture)
{
	return capture.has_za && capture.za_vl == svl && capture.za_size == 16 &&
		!capture.has_zt;
}

bool vregs_zero(const Capture &capture)
{
	for (u8 byte : capture.vregs)
		if (byte != 0)
			return false;
	return true;
}

/* Clears ZA and TPIDR2 and leaves streaming mode, through a frame. */
void clear_sme_state()
{
	FrameBuilder builder;
	builder.fpsimd();
	builder.sve_header();
	builder.tpidr2(0);
	builder.za(false);
	sigreturn_into(builder.finish(), continue_return);
}

/* A fault now, captured. */
Capture &capture_now()
{
	reset_captures();
	fault();
	return captures[0];
}

/* ------------------------------------------------------------------ */
/* Kernel mode: parameters, frames and sigreturn.                      */

void test_parameters()
{
	group = "parameters";
	check((auxv_hwcap2 & hwcap2_sme) != 0, "AT_HWCAP2 has HWCAP2_SME");
	long status = sys(nr_prctl, pr_sme_get_vl);
	svl = static_cast<u64>(status) & pr_sme_vl_len_mask;
	svq = svl / 16;
	sme2 = (auxv_hwcap2 & hwcap2_sme2) != 0;
	note("streaming vector length", static_cast<long>(svl));
	note("SME2", sme2);
	check(status > 0 && svl >= 16 && svl <= max_vl && (svl & (svl - 1)) == 0,
	      "PR_SME_GET_VL reports a valid streaming vector length");
	Layout all = expected_layout(true, true, true);
	u64 expected = frame_size(all) + 16 + 16;
	note("AT_MINSIGSTKSZ", static_cast<long>(auxv_minsigstksz));
	check_value(auxv_minsigstksz, expected,
		    "AT_MINSIGSTKSZ covers the largest frame for this vector length");
}

void test_plain_frame()
{
	group = "frame";
	clear_sme_state();
	Capture &capture = capture_now();
	check_layout(capture, false, false, "SME-off");
	check(not_streaming(capture),
	      "sve_context is a header with vector length 0 outside streaming mode");
	check(capture.has_tpidr2 && capture.tpidr2 == 0, "tpidr2_context holds 0");
	check(za_off(capture), "za_context is a header with the SVL while ZA is off");
	check(capture.record_count > 0 && capture.magics[0] == fpsimd_magic,
	      "fpsimd_context comes first");
}

void test_streaming_sigreturn()
{
	group = "sigreturn-streaming";
	const u64 tpidr2 = 0x1234567890abcdefUL;
	u8 *frame = full_frame(tpidr2, 1);
	reset_captures();
	nested_request = 1;
	sigreturn_into(frame, continue_fault_twice_then_syscall);
	check(capture_count == 3, "two faults and one nested fault were taken");
	const Capture &in_streaming = captures[0];
	const Capture &in_handler = captures[1];
	const Capture &after_handler = captures[2];
	check_layout(in_streaming, true, true, "streaming, ZA live");
	check(streaming_matches(in_streaming, true),
	      "the frame's Z, P and V come back in streaming mode, FFR reads 0");
	check(in_streaming.has_tpidr2 && in_streaming.tpidr2 == tpidr2, "TPIDR2 comes back");
	check(za_matches(in_streaming, 1), "ZA comes back");
	if (sme2)
		check(zt_matches(in_streaming, 1), "ZT0 comes back");
	check(in_streaming.fpsr == 0x11 && in_streaming.fpcr == 0x00400000,
	      "FPSR and FPCR come back");
	check(not_streaming(in_handler), "a handler starts outside streaming mode");
	check(za_off(in_handler), "a handler starts with ZA off");
	check(in_handler.has_tpidr2 && in_handler.tpidr2 == 0, "a handler starts with TPIDR2 0");
	check(streaming_matches(after_handler, true) && za_matches(after_handler, 1) &&
	      after_handler.tpidr2 == tpidr2,
	      "the handler's return restores streaming mode, ZA and TPIDR2");

	group = "syscall-smstop";
	Capture &after_syscall = capture_now();
	check_layout(after_syscall, false, true, "after a syscall");
	check(not_streaming(after_syscall), "a syscall leaves streaming mode");
	check(vregs_zero(after_syscall), "and zeroes the vector registers");
	check_value(after_syscall.fpsr, smstop_fpsr, "and sets FPSR as SMSTOP does");
	check(za_matches(after_syscall, 1), "and keeps ZA");
	if (sme2)
		check(zt_matches(after_syscall, 1), "and keeps ZT0");
	check(after_syscall.tpidr2 == tpidr2, "and keeps TPIDR2");

	group = "prctl";
	long status = sys(nr_prctl, pr_sme_set_vl, static_cast<long>(svl));
	check_value(static_cast<u64>(status) & pr_sme_vl_len_mask, svl, "PR_SME_SET_VL to the SVL");
	check(za_matches(capture_now(), 1), "setting the same VL keeps ZA");
	clear_sme_state();
}

void test_za_frames()
{
	group = "sigreturn-za";
	/* ZA without streaming mode: the continuation can return to C++. */
	sigreturn_into(full_frame(77, 2, false, true), continue_return);
	Capture &live = capture_now();
	check_layout(live, false, true, "ZA live");
	check(za_matches(live, 2), "ZA from a frame without streaming mode");
	if (sme2)
		check(zt_matches(live, 2), "ZT0 likewise");
	check(not_streaming(live) && live.tpidr2 == 77, "no streaming mode, TPIDR2 set");
	sigreturn_into(full_frame(0, 0, false, false), continue_return);
	Capture &off = capture_now();
	check(za_off(off), "a ZA header alone turns ZA off");
	check_layout(off, false, false, "ZA off again");
	/* A frame without a za_context leaves ZA as it was. */
	sigreturn_into(full_frame(5, 3, false, true), continue_return);
	FrameBuilder builder;
	builder.fpsimd();
	builder.sve_header();
	sigreturn_into(builder.finish(), continue_return);
	Capture &kept = capture_now();
	check(za_matches(kept, 3) && kept.tpidr2 == 5,
	      "a frame without za_context or tpidr2_context keeps both");
	clear_sme_state();
}

/* ------------------------------------------------------------------ */
/* Invalid frames: each child dies of SIGSEGV; the valid one exits 0.  */

enum class Corruption {
	none,
	no_fpsimd,
	fpsimd_size,
	unknown_magic,
	misaligned_size,
	sve_vl,
	sve_short,
	sve_data_without_sm,
	za_vl,
	za_short,
	zt_without_za,
	zt_nregs,
	duplicate_tpidr2,
	tpidr2_size,
	extra_datap,
	extra_too_large,
};

Corruption corruption;

int invalid_frame_child(void *)
{
	default_segv();
	FrameBuilder builder;
	bool streaming = true;
	switch (corruption) {
	case Corruption::no_fpsimd:
		builder.sve_streaming();
		builder.tpidr2(0);
		builder.za(true);
		sigreturn_into(builder.finish(), continue_exit_zero);
		return 0;
	case Corruption::fpsimd_size: {
		u8 *head = builder.add(fpsimd_magic, 512);
		(void)head;
		builder.sve_header();
		sigreturn_into(builder.finish(), continue_exit_zero);
		return 0;
	}
	default:
		break;
	}
	builder.fpsimd();
	switch (corruption) {
	case Corruption::unknown_magic:
		builder.add(0x12345678, 16);
		break;
	case Corruption::misaligned_size: {
		u8 *head = builder.add(tpidr2_magic, 16);
		store<u32>(head + 4, 24);
		break;
	}
	case Corruption::sve_vl: {
		u8 *head = builder.sve_streaming();
		store<u16>(head + 8, static_cast<u16>(svl == max_vl ? svl / 2 : svl * 2));
		break;
	}
	case Corruption::sve_short: {
		u8 *head = builder.add(sve_magic, 64);
		store<u16>(head + 8, static_cast<u16>(svl));
		store<u16>(head + 10, sve_sig_flag_sm);
		break;
	}
	case Corruption::sve_data_without_sm: {
		u8 *head = builder.sve_streaming();
		store<u16>(head + 10, 0);
		streaming = false;
		break;
	}
	case Corruption::za_vl: {
		builder.sve_header();
		u8 *head = builder.za(true);
		store<u16>(head + 8, static_cast<u16>(svl == max_vl ? svl / 2 : svl * 2));
		break;
	}
	case Corruption::za_short: {
		builder.sve_header();
		u8 *head = builder.add(za_magic, 32);
		store<u16>(head + 8, static_cast<u16>(svl));
		break;
	}
	case Corruption::zt_without_za:
		builder.sve_header();
		builder.za(false);
		builder.zt();
		break;
	case Corruption::zt_nregs: {
		builder.sve_header();
		builder.za(true);
		u8 *head = builder.add(zt_magic, 16 + 128);
		store<u16>(head + 8, 2);
		break;
	}
	case Corruption::duplicate_tpidr2:
		builder.sve_header();
		builder.tpidr2(1);
		builder.tpidr2(2);
		break;
	case Corruption::tpidr2_size:
		builder.sve_header();
		builder.add(tpidr2_magic, 32);
		break;
	case Corruption::extra_datap:
	case Corruption::extra_too_large:
	case Corruption::none:
		builder.sve_streaming();
		builder.tpidr2(9);
		builder.za(true);
		if (sme2)
			builder.zt();
		break;
	default:
		break;
	}
	u8 *frame = builder.finish();
	if (corruption == Corruption::extra_datap || corruption == Corruption::extra_too_large) {
		if (!builder.layout.extra)
			return 0;  /* no extra_context at this SVL: nothing to corrupt */
		u8 *extra = frame + builder.extra_offset;
		if (corruption == Corruption::extra_datap)
			store<u64>(extra + 8, load<u64>(extra + 8) + 16);
		else
			store<u32>(extra + 16, static_cast<u32>(sigframe_maxsz));
	}
	sigreturn_into(frame, streaming ? continue_exit_zero : continue_exit_zero);
	return 0;
}

/* Runs function in a forked child; returns its wait status. */
long run_child(int (*function)(void *), void *argument = nullptr, u64 flags = sigchld,
	       void *stack = nullptr)
{
	long pid = clone_run(flags, stack, function, argument);
	if (pid < 0)
		return pid;
	int status = 0;
	sys(nr_wait4, pid, ptr(&status), 0, 0);
	return status;
}

bool exited_zero(long status) { return status == 0; }
bool killed_by(long status, int signal) { return (status & 0x7f) == signal; }

void test_invalid_frames()
{
	group = "invalid-frames";
	struct Case {
		Corruption corruption;
		const char *name;
		bool needs_sme2;
		bool needs_extra;
	};
	static const Case cases[] = {
		{Corruption::no_fpsimd, "a frame without fpsimd_context", false, false},
		{Corruption::fpsimd_size, "an fpsimd_context of the wrong size", false, false},
		{Corruption::unknown_magic, "an unknown record", false, false},
		{Corruption::misaligned_size, "a record size that is not a multiple of 16", false, false},
		{Corruption::sve_vl, "a streaming sve_context of another VL", false, false},
		{Corruption::sve_short, "a streaming sve_context too short for its registers", false, false},
		{Corruption::sve_data_without_sm, "an sve_context with data but without SM", false, false},
		{Corruption::za_vl, "a za_context of another VL", false, false},
		{Corruption::za_short, "a za_context between a header and full", false, false},
		{Corruption::zt_without_za, "a zt_context while ZA is off", false, false},
		{Corruption::zt_nregs, "a zt_context with two registers", true, false},
		{Corruption::duplicate_tpidr2, "two tpidr2_context records", false, false},
		{Corruption::tpidr2_size, "a tpidr2_context of the wrong size", false, false},
		{Corruption::extra_datap, "an extra_context whose data is not next", false, true},
		{Corruption::extra_too_large, "an extra_context past the frame bound", false, true},
	};
	corruption = Corruption::none;
	check(exited_zero(run_child(invalid_frame_child)), "a valid full frame is accepted");
	bool has_extra = expected_layout(true, true).extra;
	for (const Case &item : cases) {
		if ((item.needs_sme2 && !sme2) || (item.needs_extra && !has_extra))
			continue;
		corruption = item.corruption;
		check(killed_by(run_child(invalid_frame_child), sigsegv), item.name);
	}
}

/* ------------------------------------------------------------------ */
/* fork, vfork, clone, exec.                                           */

volatile int shared_result;

/* A child reports its first fault's state: 0 if it matches. */
int expect_inherited(void *argument)
{
	u64 seed = reinterpret_cast<u64>(argument);
	Capture &capture = capture_now();
	int result = za_matches(capture, seed) && capture.tpidr2 == 0x5eed ? 0 : 1;
	shared_result = result;
	return result;
}

int expect_cleared(void *)
{
	Capture &capture = capture_now();
	int result = za_off(capture) && capture.tpidr2 == 0 && not_streaming(capture) ? 0 : 1;
	shared_result = result;
	return result;
}

int exec_child(void *argument)
{
	const char *inherit = static_cast<const char *>(argument);
	if (inherit[0] == 'i')
		sys(nr_prctl, pr_sme_set_vl, static_cast<long>(svl) | pr_sme_vl_inherit);
	const char *argv[] = {self_path, "exec-check", inherit, nullptr};
	const char *envp[] = {nullptr};
	sys(nr_execve, ptr(self_path), ptr(argv), ptr(envp));
	return 99;
}

alignas(16) u8 thread_stack[64 * 1024];

void test_task_creation()
{
	group = "task-creation";
	sigreturn_into(full_frame(0x5eed, 4, false, true), continue_return);
	check(exited_zero(run_child(expect_inherited, reinterpret_cast<void *>(4))),
	      "fork: the child inherits ZA and TPIDR2");
	shared_result = -1;
	long status = run_child(expect_cleared, nullptr, clone_vm | sigchld,
				thread_stack + sizeof(thread_stack));
	check(exited_zero(status) && shared_result == 0,
	      "clone(CLONE_VM): the child starts with ZA off and TPIDR2 0");
	shared_result = -1;
	status = run_child(expect_cleared, nullptr, clone_vm | clone_vfork | sigchld,
			   thread_stack + sizeof(thread_stack));
	check(exited_zero(status) && shared_result == 0,
	      "vfork: the child starts with ZA off and TPIDR2 0");
	Capture &parent = capture_now();
	check(za_matches(parent, 4) && parent.tpidr2 == 0x5eed,
	      "the parent keeps ZA and TPIDR2 across all three");
	check(exited_zero(run_child(exec_child, const_cast<char *>("plain"))),
	      "exec: a task starts with no SME state");
	check(exited_zero(run_child(exec_child, const_cast<char *>("inherit"))),
	      "exec: PR_SME_VL_INHERIT survives exec");
	clear_sme_state();
}

/* Run by the exec'd image; its exit status reports the result. */
[[noreturn]] void exec_check(const char *inherit)
{
	install_handler();
	long status = sys(nr_prctl, pr_sme_get_vl);
	svl = static_cast<u64>(status) & pr_sme_vl_len_mask;
	svq = svl / 16;
	Capture &capture = capture_now();
	bool clean = za_off(capture) && capture.tpidr2 == 0 && not_streaming(capture);
	bool flag = (status & pr_sme_vl_inherit) != 0;
	exit_task(clean && flag == (inherit[0] == 'i') ? 0 : 1);
}

/* ------------------------------------------------------------------ */
/* prctl and the sysctl.                                               */

void test_prctl()
{
	group = "prctl";
	static const long lengths[] = {16, 32, 64, 128, 256, 512, 4096};
	for (long length : lengths) {
		long status = sys(nr_prctl, pr_sme_set_vl, length);
		Line name;
		name.add("PR_SME_SET_VL ").number(length).add(" resolves to the SVL");
		name.text[name.length] = '\0';
		check_value(static_cast<u64>(status), svl, name.text);
	}
	check_value(static_cast<u64>(sys(nr_prctl, pr_sme_set_vl, 17)),
		    static_cast<u64>(-einval), "PR_SME_SET_VL 17 is EINVAL");
	check_value(static_cast<u64>(sys(nr_prctl, pr_sme_set_vl, 0)),
		    static_cast<u64>(-einval), "PR_SME_SET_VL 0 is EINVAL");
	check_value(static_cast<u64>(sys(nr_prctl, pr_sme_set_vl, static_cast<long>(svl) | (1L << 20))),
		    static_cast<u64>(-einval), "an unknown PR_SME_SET_VL flag is EINVAL");
	long status = sys(nr_prctl, pr_sme_set_vl, static_cast<long>(svl) | pr_sme_vl_inherit);
	check_value(static_cast<u64>(status), svl | pr_sme_vl_inherit,
		    "PR_SME_VL_INHERIT is accepted");
	check_value(static_cast<u64>(sys(nr_prctl, pr_sme_get_vl)), svl | pr_sme_vl_inherit,
		    "PR_SME_GET_VL reports it");
	check(sys(nr_prctl, pr_sme_set_vl, static_cast<long>(svl) | pr_sme_set_vl_onexec) >= 0,
	      "PR_SME_SET_VL_ONEXEC is accepted");
	check_value(static_cast<u64>(sys(nr_prctl, pr_sme_get_vl)), svl,
		    "PR_SME_GET_VL reports the SVL alone again");
}

bool read_sysctl(char *buffer, u64 size)
{
	long fd = sys(nr_openat, at_fdcwd, ptr("/proc/sys/abi/sme_default_vector_length"), 0);
	if (fd < 0)
		return false;
	long length = sys(nr_read, fd, ptr(buffer), static_cast<long>(size - 1));
	sys(nr_close, fd);
	if (length < 0)
		return false;
	buffer[length] = '\0';
	return true;
}

long write_sysctl(const char *text)
{
	long fd = sys(nr_openat, at_fdcwd, ptr("/proc/sys/abi/sme_default_vector_length"), o_wronly);
	if (fd < 0)
		return fd;
	long result = sys(nr_write, fd, ptr(text), static_cast<long>(string_length(text)));
	sys(nr_close, fd);
	return result;
}

void test_sysctl()
{
	group = "sysctl";
	char expected[32];
	Line line;
	line.number(static_cast<long>(svl)).add("\n");
	memcpy(expected, line.text, line.length);
	expected[line.length] = '\0';
	char buffer[64];
	check(read_sysctl(buffer, sizeof(buffer)) && same_string(buffer, expected),
	      "abi.sme_default_vector_length reads the SVL");
	check(write_sysctl("32\n") == 3, "writing a valid length succeeds");
	check(read_sysctl(buffer, sizeof(buffer)) && same_string(buffer, expected),
	      "and the default stays the SVL");
	check(write_sysctl("-1\n") == 3, "writing -1 succeeds");
	check(write_sysctl("17\n") == -einval, "writing 17 is EINVAL");
	check(read_sysctl(buffer, sizeof(buffer)) && same_string(buffer, expected),
	      "the default is still the SVL");
}

/* ------------------------------------------------------------------ */
/* ptrace: regsets and the syscall-entry stop.                         */

struct Iovec {
	void *base;
	u64 length;
};

struct RegsetHeader {
	u32 size;
	u32 max_size;
	u16 vl;
	u16 max_vl;
	u16 flags;
	u16 reserved;
};

alignas(16) u8 regset_buffer[sigframe_maxsz];

long get_regset(long pid, long type, u64 length = sizeof(regset_buffer))
{
	memset(regset_buffer, 0, length);
	Iovec iov = {regset_buffer, length};
	long result = sys(nr_ptrace, ptrace_getregset, pid, type, ptr(&iov));
	return result < 0 ? result : static_cast<long>(iov.length);
}

long set_regset(long pid, long type, u64 length)
{
	Iovec iov = {regset_buffer, length};
	return sys(nr_ptrace, ptrace_setregset, pid, type, ptr(&iov));
}

int wait_stop(long pid)
{
	int status = 0;
	if (sys(nr_wait4, pid, ptr(&status), 0, 0) != pid)
		return -1;
	return status;
}

bool stopped_with(int status, int signal) { return (status & 0xff) == 0x7f && ((status >> 8) & 0xff) == signal; }

constexpr u64 traced_tpidr2 = 0x7ace7ace7ace7aceUL;
/* The TPIDR_EL0 a tracer writes through NT_ARM_TLS. */
constexpr u64 traced_tpidr = 0x00007e1d5eed7e1dUL;

u64 read_tpidr()
{
	u64 value;
	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
	return value;
}
constexpr u32 traced_fpsr = 0x00000011;
constexpr u32 traced_fpcr = 0x00800000;

/* The tracee: stop, then report what the tracer wrote through a fault. */
int regset_tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	reset_captures();
	stop_fault_syscall(sys(nr_getpid));
	const Capture &capture = captures[0];
	bool z = streaming_matches(capture, false);
	bool za = za_matches(capture, 6);
	bool zt = !sme2 || zt_matches(capture, 6);
	bool tls = capture.tpidr2 == traced_tpidr2;
	bool fp = capture.fpsr == traced_fpsr && capture.fpcr == traced_fpcr;
	bool tpidr = read_tpidr() == traced_tpidr;
	return (z ? 0 : 1) | (za ? 0 : 2) | (zt ? 0 : 4) | (tls ? 0 : 8) | (fp ? 0 : 16) |
	       (tpidr ? 0 : 32);
}

void fill_ssve(u16 flags, u16 vl)
{
	memset(regset_buffer, 0, sve_pt_size_sve(svq));
	RegsetHeader header = {static_cast<u32>(sve_pt_size_sve(svq)), 0, vl, 0, flags, 0};
	memcpy(regset_buffer, &header, sizeof(header));
	for (u64 reg = 0; reg < 32; reg++)
		for (u64 byte = 0; byte < svl; byte++)
			regset_buffer[sve_zreg_offset(svq, reg) + byte] = z_byte(reg, byte);
	for (u64 reg = 0; reg < 16; reg++)
		for (u64 byte = 0; byte < svl / 8; byte++)
			regset_buffer[sve_preg_offset(svq, reg) + byte] = p_byte(reg, byte);
	store<u32>(regset_buffer + sve_pt_fpsr_offset(svq), traced_fpsr);
	store<u32>(regset_buffer + sve_pt_fpsr_offset(svq) + 4, traced_fpcr);
}

void test_regsets()
{
	group = "regsets";
	long pid = clone_run(sigchld, nullptr, regset_tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop)) {
		check(false, "the tracee stops");
		return;
	}
	RegsetHeader header;
	long length = get_regset(pid, nt_arm_ssve);
	memcpy(&header, regset_buffer, sizeof(header));
	check(length >= static_cast<long>(sizeof(header)), "NT_ARM_SSVE can be read");
	check_value(header.vl, svl, "NT_ARM_SSVE vl is the SVL");
	check_value(header.max_vl, svl, "NT_ARM_SSVE max_vl is the SVL");
	check_value(header.flags & 1, sve_pt_regs_fpsimd, "NT_ARM_SSVE is FPSIMD outside streaming mode");
	check_value(header.size, 16, "NT_ARM_SSVE is a header outside streaming mode");
	check_value(header.max_size, sve_pt_size_sve(svq), "NT_ARM_SSVE max_size");

	length = get_regset(pid, nt_arm_za);
	memcpy(&header, regset_buffer, sizeof(header));
	check(length >= static_cast<long>(sizeof(header)), "NT_ARM_ZA can be read");
	check_value(header.vl, svl, "NT_ARM_ZA vl is the SVL");
	check_value(header.size, 16, "NT_ARM_ZA is a header while ZA is off");
	check_value(header.max_size, za_pt_size(svq), "NT_ARM_ZA max_size");

	if (sme2) {
		length = get_regset(pid, nt_arm_zt, 64);
		u8 zero[64] = {};
		check(length == 64 && memcmp(regset_buffer, zero, 64) == 0,
		      "NT_ARM_ZT reads zero while ZA is off");
	} else {
		check(get_regset(pid, nt_arm_zt, 64) < 0, "NT_ARM_ZT does not exist without SME2");
	}

	length = get_regset(pid, nt_arm_tls, 16);
	check(length == 16, "NT_ARM_TLS holds TPIDR_EL0 and TPIDR2_EL0");
	check(load<u64>(regset_buffer) == read_tpidr(), "NT_ARM_TLS reads the TPIDR_EL0 inherited");

	fill_ssve(sve_pt_regs_fpsimd, static_cast<u16>(svl));
	check_value(static_cast<u64>(set_regset(pid, nt_arm_ssve, sve_pt_size_sve(svq))),
		    static_cast<u64>(-einval), "writing NT_ARM_SSVE in FPSIMD form is EINVAL");
	if (svl < max_vl) {
		fill_ssve(sve_pt_regs_sve, static_cast<u16>(svl * 2));
		check_value(static_cast<u64>(set_regset(pid, nt_arm_ssve, sve_pt_size_sve(svq))),
			    static_cast<u64>(-eio), "writing NT_ARM_SSVE data for another VL is EIO");
	}
	fill_ssve(sve_pt_regs_sve, static_cast<u16>(svl));
	check(set_regset(pid, nt_arm_ssve, sve_pt_size_sve(svq)) == 0, "NT_ARM_SSVE can be written");
	length = get_regset(pid, nt_arm_ssve);
	memcpy(&header, regset_buffer, sizeof(header));
	check_value(header.flags & 1, sve_pt_regs_sve, "the tracee is now in streaming mode");
	check_value(header.size, sve_pt_size_sve(svq), "NT_ARM_SSVE size in streaming mode");
	bool same = true;
	for (u64 reg = 0; reg < 32; reg++)
		for (u64 byte = 0; byte < svl; byte++)
			same = same && regset_buffer[sve_zreg_offset(svq, reg) + byte] == z_byte(reg, byte);
	check(same, "NT_ARM_SSVE reads back the Z registers written");
	check(load<u32>(regset_buffer + sve_pt_fpsr_offset(svq)) == traced_fpsr,
	      "NT_ARM_SSVE reads back FPSR");
	length = get_regset(pid, nt_prfpreg, fpsimd_size);
	same = length == static_cast<long>(fpsimd_size);
	for (u64 reg = 0; same && reg < 32; reg++)
		for (u64 byte = 0; byte < 16; byte++)
			same = same && regset_buffer[reg * 16 + byte] == z_byte(reg, byte);
	check(same, "NT_PRFPREG's V are the low bytes of the Z written");

	memset(regset_buffer, 0, za_pt_size(svq));
	RegsetHeader za = {static_cast<u32>(za_pt_size(svq)), 0, static_cast<u16>(svl), 0, 0, 0};
	memcpy(regset_buffer, &za, sizeof(za));
	for (u64 row = 0; row < svl; row++)
		for (u64 byte = 0; byte < svl; byte++)
			regset_buffer[16 + row * svl + byte] = za_byte(row, byte, 6);
	check(set_regset(pid, nt_arm_za, za_pt_size(svq)) == 0, "NT_ARM_ZA can be written");
	length = get_regset(pid, nt_arm_za);
	memcpy(&header, regset_buffer, sizeof(header));
	check_value(header.size, za_pt_size(svq), "NT_ARM_ZA size while ZA is live");
	same = true;
	for (u64 row = 0; row < svl; row++)
		for (u64 byte = 0; byte < svl; byte++)
			same = same && regset_buffer[16 + row * svl + byte] == za_byte(row, byte, 6);
	check(same, "NT_ARM_ZA reads back what was written");

	if (sme2) {
		for (u64 byte = 0; byte < 64; byte++)
			regset_buffer[byte] = zt_byte(byte, 6);
		check(set_regset(pid, nt_arm_zt, 64) == 0, "NT_ARM_ZT can be written");
		get_regset(pid, nt_arm_zt, 64);
		same = true;
		for (u64 byte = 0; byte < 64; byte++)
			same = same && regset_buffer[byte] == zt_byte(byte, 6);
		check(same, "NT_ARM_ZT reads back what was written");
	}

	store<u64>(regset_buffer, traced_tpidr);
	store<u64>(regset_buffer + 8, traced_tpidr2);
	check(set_regset(pid, nt_arm_tls, 16) == 0, "NT_ARM_TLS can be written");
	get_regset(pid, nt_arm_tls, 16);
	check(load<u64>(regset_buffer) == traced_tpidr && load<u64>(regset_buffer + 8) == traced_tpidr2,
	      "NT_ARM_TLS reads back the TPIDR_EL0 and TPIDR2_EL0 written");

	/* Resume; deliver the tracee's own SIGSEGV; collect its verdict. */
	sys(nr_ptrace, ptrace_cont, pid, 0, 0);
	int status = 0;
	for (int round = 0; round < 8; round++) {
		status = wait_stop(pid);
		if (stopped_with(status, sigsegv)) {
			sys(nr_ptrace, ptrace_cont, pid, 0, sigsegv);
			continue;
		}
		break;
	}
	Line verdict;
	verdict.add("the tracee runs with the state written (status ").hex(static_cast<u64>(status)).add(")");
	verdict.text[verdict.length] = '\0';
	check((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0, verdict.text);
}

int syscall_stop_tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	sys(nr_kill, sys(nr_getpid), sigstop);
	sigreturn_into(full_frame(0x51, 7), continue_syscall);
	Capture &capture = capture_now();
	return za_matches(capture, 7) && not_streaming(capture) ? 0 : 1;
}

void test_syscall_stop()
{
	group = "syscall-stop";
	long pid = clone_run(sigchld, nullptr, syscall_stop_tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop)) {
		check(false, "the tracee stops");
		return;
	}
	sys(nr_ptrace, ptrace_setoptions, pid, 0, ptrace_o_tracesysgood);
	sys(nr_ptrace, ptrace_syscall, pid, 0, 0);
	bool entry = true;
	bool seen = false;
	int status = 0;
	for (int round = 0; round < 32; round++) {
		status = wait_stop(pid);
		if ((status & 0xff) != 0x7f)
			break;
		int signal = (status >> 8) & 0xff;
		if (signal == (sigtrap | 0x80)) {
			get_regset(pid, nt_prstatus, 34 * 8);
			u64 number = load<u64>(regset_buffer + 8 * 8);
			if (entry && number == nr_getpid && !seen) {
				seen = true;
				RegsetHeader header;
				get_regset(pid, nt_arm_ssve);
				memcpy(&header, regset_buffer, sizeof(header));
				check_value(header.flags & 1, sve_pt_regs_fpsimd,
					    "at syscall entry the tracee has left streaming mode");
				get_regset(pid, nt_prfpreg, fpsimd_size);
				bool zero = true;
				for (u64 byte = 0; byte < 512; byte++)
					zero = zero && regset_buffer[byte] == 0;
				check(zero, "its vector registers are zero");
				check_value(load<u32>(regset_buffer + 512), smstop_fpsr,
					    "its FPSR is SMSTOP's");
				get_regset(pid, nt_arm_za);
				memcpy(&header, regset_buffer, sizeof(header));
				check_value(header.size, za_pt_size(svq), "ZA is still live");
				get_regset(pid, nt_arm_tls, 16);
				check_value(load<u64>(regset_buffer + 8), 0x51, "TPIDR2 is kept");
				sys(nr_ptrace, ptrace_cont, pid, 0, 0);
				continue;
			}
			entry = !entry;
			sys(nr_ptrace, ptrace_syscall, pid, 0, 0);
			continue;
		}
		sys(nr_ptrace, seen ? ptrace_cont : ptrace_syscall, pid, 0, signal == sigstop ? 0 : signal);
	}
	check(seen, "the getpid entry stop was seen");
	check((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0, "the tracee checks its state");
}

/* ------------------------------------------------------------------ */
/* Spin mode: streaming mode with ZA live while the host is signalled. */

void run_spin()
{
	group = "spin";
	write_text("sme-probe: spinning\n");
	reset_captures();
	sigreturn_into(full_frame(0x5150, 8), continue_spin_then_syscall);
	check(capture_count == 1, "the loop ended in its fault");
	const Capture &capture = captures[0];
	check(streaming_matches(capture, true), "streaming Z and P are intact after the signals");
	check(za_matches(capture, 8), "ZA is intact");
	if (sme2)
		check(zt_matches(capture, 8), "ZT0 is intact");
	check(capture.tpidr2 == 0x5150, "TPIDR2 is intact");
	clear_sme_state();
}

/* ------------------------------------------------------------------ */
/* Engine mode: the control core of SME, executed by the engine.       */

extern "C" {
u64 engine_read_svcr();
u64 engine_rdsvl();
void engine_smstart();
void engine_smstop();
void engine_smstart_za();
void engine_smstop_za();
void engine_write_tpidr2(u64 value);
u64 engine_read_tpidr2();
void engine_load_za(const u8 *rows, u64 svl);
void engine_store_za(u8 *rows, u64 svl);
void engine_zero_za();
void engine_load_zt0(const u8 *bytes);
void engine_store_zt0(u8 *bytes);
void engine_spin_streaming(const u8 *rows, u64 svl, u8 *out);
/* The SM = 0 vector-length family (detail/sm0_vector_length.hpp): x0 is the
 * operand of the ones that scale or count into it. */
u64 engine_cntb();
u64 engine_cntd();
u64 engine_rdvl();
u64 engine_addvl(u64 base);
u64 engine_addpl(u64 base);
u64 engine_incd(u64 value);
u64 engine_decb(u64 value);
u64 engine_cntb_streaming();
/* ... and its LDR and STR of Z and P: x0 is where to store, x1 where to load
 * from. */
void engine_spill_z(u8 *out, const u8 *in);
void engine_spill_p(u8 *out);
}

__asm__(R"(
	.text
	.p2align 2
	.globl engine_read_svcr
engine_read_svcr:
	.inst	0xd53b4240	// mrs x0, svcr
	ret
	.globl engine_rdsvl
engine_rdsvl:
	.inst	0x04bf5820	// rdsvl x0, #1
	ret
	.globl engine_cntb
engine_cntb:
	.inst	0x0420e3e0	// cntb x0
	ret
	.globl engine_cntd
engine_cntd:
	.inst	0x04e0e3e0	// cntd x0
	ret
	.globl engine_rdvl
engine_rdvl:
	.inst	0x04bf5020	// rdvl x0, #1
	ret
	.globl engine_addvl
engine_addvl:
	.inst	0x04205020	// addvl x0, x0, #1
	ret
	.globl engine_addpl
engine_addpl:
	.inst	0x04605020	// addpl x0, x0, #1
	ret
	.globl engine_incd
engine_incd:
	.inst	0x04f0e3e0	// incd x0
	ret
	.globl engine_decb
engine_decb:
	.inst	0x0430e7e0	// decb x0
	ret
	.globl engine_spill_z
engine_spill_z:
	.inst	0x85804030	// ldr z16, [x1]
	.inst	0x85804431	// ldr z17, [x1, #1, mul vl]
	.inst	0xe5804810	// str z16, [x0, #2, mul vl]
	.inst	0xe5bd5411	// str z17, [x0, #-19, mul vl]
	ret
	.globl engine_spill_p
engine_spill_p:
	.inst	0xe5800c04	// str p4, [x0, #3, mul vl]
	.inst	0x85bf1c05	// ldr p5, [x0, #-1, mul vl]
	ret
	.globl engine_cntb_streaming
engine_cntb_streaming:
	.inst	0xd503477f	// smstart
	.inst	0x0420e3e0	// cntb x0
	.inst	0xd503467f	// smstop
	ret
	.globl engine_smstart
engine_smstart:
	.inst	0xd503477f	// smstart
	ret
	.globl engine_smstop
engine_smstop:
	.inst	0xd503467f	// smstop
	ret
	.globl engine_smstart_za
engine_smstart_za:
	.inst	0xd503457f	// smstart za
	ret
	.globl engine_smstop_za
engine_smstop_za:
	.inst	0xd503447f	// smstop za
	ret
	.globl engine_write_tpidr2
engine_write_tpidr2:
	.inst	0xd51bd0a0	// msr tpidr2_el0, x0
	ret
	.globl engine_read_tpidr2
engine_read_tpidr2:
	.inst	0xd53bd0a0	// mrs x0, tpidr2_el0
	ret
	.globl engine_load_za
engine_load_za:			// x0 rows, x1 svl
	mov	w12, #0
1:	.inst	0xe1000000	// ldr za[w12, 0], [x0]
	add	x0, x0, x1
	add	w12, w12, #1
	cmp	x12, x1
	b.lo	1b
	ret
	.globl engine_store_za
engine_store_za:
	mov	w12, #0
1:	.inst	0xe1200000	// str za[w12, 0], [x0]
	add	x0, x0, x1
	add	w12, w12, #1
	cmp	x12, x1
	b.lo	1b
	ret
	.globl engine_zero_za
engine_zero_za:
	.inst	0xc00800ff	// zero {za}
	ret
	.globl engine_load_zt0
engine_load_zt0:
	.inst	0xe11f8000	// ldr zt0, [x0]
	ret
	.globl engine_store_zt0
engine_store_zt0:
	.inst	0xe13f8000	// str zt0, [x0]
	ret
	// smstart; load ZA; spin for spin_seconds; store ZA to x2; smstop.
	.globl engine_spin_streaming
engine_spin_streaming:
	.inst	0xd503477f	// smstart
	mov	x3, x0
	mov	w12, #0
1:	.inst	0xe1000060	// ldr za[w12, 0], [x3]
	add	x3, x3, x1
	add	w12, w12, #1
	cmp	x12, x1
	b.lo	1b
	adrp	x9, spin_seconds
	ldr	x9, [x9, :lo12:spin_seconds]
	mrs	x10, cntfrq_el0
	mul	x9, x9, x10
	mrs	x10, cntvct_el0
	add	x9, x9, x10
2:	mrs	x10, cntvct_el0
	cmp	x10, x9
	b.lo	2b
	mov	w12, #0
3:	.inst	0xe1200040	// str za[w12, 0], [x2]
	add	x2, x2, x1
	add	w12, w12, #1
	cmp	x12, x1
	b.lo	3b
	.inst	0xd503467f	// smstop
	ret
)");

alignas(16) u8 za_rows[max_vl * max_vl];
alignas(16) u8 za_out[max_vl * max_vl];

void fill_rows(u64 seed)
{
	for (u64 row = 0; row < svl; row++)
		for (u64 byte = 0; byte < svl; byte++)
			za_rows[row * svl + byte] = za_byte(row, byte, seed);
}

int engine_tpidr2_child(void *argument)
{
	return engine_read_tpidr2() == reinterpret_cast<u64>(argument) ? 0 : 1;
}

u64 handler_svcr;
u64 handler_tpidr2;

void engine_handler(int, void *, void *context)
{
	handler_svcr = engine_read_svcr();
	handler_tpidr2 = engine_read_tpidr2();
	u8 *frame = static_cast<u8 *>(context) - uc_offset;
	store<u64>(frame + mc_pc_offset, load<u64>(frame + mc_pc_offset) + 4);
}

void run_engine()
{
	group = "engine";
	check_value(engine_rdsvl(), svl, "RDSVL reads the SVL");
	check_value(engine_read_svcr(), 0, "SVCR starts at 0");
	/* A deliberate departure from the architecture: without SVE, the
	 * engine counts and scales a vector length of 16 bytes at SM = 0, and
	 * the streaming vector length in streaming mode. */
	check_value(engine_cntb(), 16, "CNTB counts a 16-byte vector at SM = 0");
	check_value(engine_cntd(), 2, "CNTD counts two doublewords at SM = 0");
	check_value(engine_rdvl(), 16, "RDVL #1 is 16 at SM = 0");
	check_value(engine_addvl(100), 116, "ADDVL scales by 16 at SM = 0");
	check_value(engine_addpl(100), 102, "ADDPL scales by 2 at SM = 0");
	check_value(engine_incd(10), 12, "INCD adds 2 at SM = 0");
	check_value(engine_decb(40), 24, "DECB subtracts 16 at SM = 0");
	/* LDR and STR of Z and P move 16 and 2 bytes: Z through V, and P as
	 * zeros, wherever the offset reaches. */
	u8 spill_in[32], spill_out[512];
	for (u64 index = 0; index < sizeof spill_in; index++)
		spill_in[index] = static_cast<u8>(index * 7 + 1);
	memset(spill_out, 0xaa, sizeof spill_out);
	engine_spill_z(spill_out + 320, spill_in);
	bool spilled = memcmp(spill_out + 16, spill_in + 16, 16) == 0 &&
		memcmp(spill_out + 352, spill_in, 16) == 0;
	for (u64 index = 0; index < sizeof spill_out; index++)
		if ((index < 16 || (index >= 32 && index < 352) || index >= 368) &&
		    spill_out[index] != 0xaa)
			spilled = false;
	check(spilled, "STR and LDR of Z move 16 bytes at SM = 0");
	memset(spill_out, 0xaa, sizeof spill_out);
	engine_spill_p(spill_out + 16);
	bool predicate = spill_out[22] == 0 && spill_out[23] == 0;
	for (u64 index = 0; index < 64; index++)
		if (index != 22 && index != 23 && spill_out[index] != 0xaa)
			predicate = false;
	check(predicate, "STR of P stores two zero bytes and LDR of P loads two");
	check_value(engine_cntb_streaming(), svl, "CNTB reads the SVL in streaming mode");
	check_value(engine_read_svcr(), 0, "and streaming mode ended");
	engine_smstart_za();
	check_value(engine_read_svcr(), 2, "SMSTART ZA sets PSTATE.ZA");
	engine_store_za(za_out, svl);
	bool zero = true;
	for (u64 index = 0; index < svl * svl; index++)
		zero = zero && za_out[index] == 0;
	check(zero, "the first SMSTART ZA of a task gives a zeroed ZA");
	fill_rows(10);
	engine_load_za(za_rows, svl);
	sys(nr_getpid);
	check_value(engine_read_svcr(), 2, "a syscall keeps PSTATE.ZA");
	engine_store_za(za_out, svl);
	check(memcmp(za_rows, za_out, svl * svl) == 0, "LDR and STR ZA round-trip across a syscall");
	if (sme2) {
		u8 bytes[64], back[64];
		for (u64 byte = 0; byte < 64; byte++)
			bytes[byte] = zt_byte(byte, 10);
		engine_load_zt0(bytes);
		engine_store_zt0(back);
		check(memcmp(bytes, back, 64) == 0, "LDR and STR ZT0 round-trip");
	}
	Capture &capture = capture_now();
	check(za_matches(capture, 10), "a signal frame carries the ZA the engine loaded");
	engine_zero_za();
	engine_store_za(za_out, svl);
	zero = true;
	for (u64 index = 0; index < svl * svl; index++)
		zero = zero && za_out[index] == 0;
	check(zero, "ZERO {ZA}");
	engine_smstop_za();
	check_value(engine_read_svcr(), 0, "SMSTOP ZA clears PSTATE.ZA");

	engine_write_tpidr2(0xabcdef);
	check_value(engine_read_tpidr2(), 0xabcdef, "TPIDR2 round-trips");
	check(exited_zero(run_child(engine_tpidr2_child, reinterpret_cast<void *>(0xabcdef))),
	      "a forked child reads the parent's TPIDR2");
	shared_result = -1;
	check(exited_zero(run_child(engine_tpidr2_child, nullptr, clone_vm | sigchld,
				    thread_stack + sizeof(thread_stack))),
	      "a CLONE_VM child reads TPIDR2 0");

	KernelSigaction action = {engine_handler, sa_siginfo | sa_onstack | sa_nodefer, nullptr, 0};
	sys(nr_rt_sigaction, sigsegv, ptr(&action), 0, 8);
	engine_smstart();
	fault();
	u64 after = engine_read_svcr();
	engine_smstop();
	check_value(handler_svcr, 0, "a handler starts with SVCR 0");
	check_value(handler_tpidr2, 0, "a handler starts with TPIDR2 0");
	check_value(after, 3, "the handler's return restores SM and ZA");
	check_value(engine_read_tpidr2(), 0xabcdef, "and TPIDR2");
	engine_write_tpidr2(0);
	install_handler();
}

void run_engine_spin()
{
	group = "engine-spin";
	fill_rows(11);
	write_text("sme-probe: spinning\n");
	engine_spin_streaming(za_rows, svl, za_out);
	check(memcmp(za_rows, za_out, svl * svl) == 0,
	      "ZA loaded before the signals reads back after them");
	check_value(engine_read_svcr(), 0, "streaming mode ended");
}

/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Off mode: a run that does not implement SME.                        */

/* The tracee: stop, then report whether it runs with the TPIDR_EL0 the
 * tracer wrote. */
int off_tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	sys(nr_kill, sys(nr_getpid), sigstop);
	return read_tpidr() == traced_tpidr ? 0 : 1;
}

void check_error(long result, long error, const char *name)
{
	check_value(static_cast<u64>(result), static_cast<u64>(-error), name);
}

void test_off()
{
	group = "off";
	check((auxv_hwcap2 & (hwcap2_sme | hwcap2_sme2)) == 0, "AT_HWCAP2 has no SME bit");
	check_value(auxv_minsigstksz, 0, "the auxiliary vector has no AT_MINSIGSTKSZ");
	check_error(sys(nr_prctl, pr_sme_get_vl), einval, "PR_SME_GET_VL fails with EINVAL");
	check_error(sys(nr_prctl, pr_sme_set_vl, 16), einval, "PR_SME_SET_VL fails with EINVAL");
	mount_proc();
	check_error(sys(nr_openat, at_fdcwd, ptr("/proc/sys/abi/sme_default_vector_length"), 0),
		    enoent, "abi.sme_default_vector_length does not exist");
	const Capture &capture = capture_now();
	check(capture.valid && capture.error == nullptr && capture.record_count == 1 &&
		      capture.magics[0] == fpsimd_magic,
	      "a signal frame holds fpsimd_context alone");
	long pid = clone_run(sigchld, nullptr, off_tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop)) {
		check(false, "the tracee stops");
		return;
	}
	check_error(get_regset(pid, nt_arm_ssve), einval, "NT_ARM_SSVE does not exist");
	check_error(get_regset(pid, nt_arm_za), einval, "NT_ARM_ZA does not exist");
	check_error(get_regset(pid, nt_arm_zt, 64), einval, "NT_ARM_ZT does not exist");
	check(get_regset(pid, nt_arm_tls, 16) == 16, "NT_ARM_TLS holds TPIDR_EL0 and TPIDR2_EL0");
	check(load<u64>(regset_buffer) == read_tpidr(), "NT_ARM_TLS reads the TPIDR_EL0 inherited");
	check_value(load<u64>(regset_buffer + 8), 0, "NT_ARM_TLS reads TPIDR2_EL0 as zero");
	store<u64>(regset_buffer, ~traced_tpidr);
	store<u64>(regset_buffer + 8, traced_tpidr2);
	check(set_regset(pid, nt_arm_tls, 16) == 0, "NT_ARM_TLS can be written");
	get_regset(pid, nt_arm_tls, 16);
	check(load<u64>(regset_buffer) == ~traced_tpidr && load<u64>(regset_buffer + 8) == 0,
	      "NT_ARM_TLS reads back the TPIDR_EL0 written and ignores TPIDR2_EL0");
	store<u64>(regset_buffer, traced_tpidr);
	check(set_regset(pid, nt_arm_tls, 8) == 0, "NT_ARM_TLS can be written in part");
	get_regset(pid, nt_arm_tls, 16);
	check(load<u64>(regset_buffer) == traced_tpidr && load<u64>(regset_buffer + 8) == 0,
	      "NT_ARM_TLS reads back the TPIDR_EL0 written alone");
	sys(nr_ptrace, ptrace_cont, pid, 0, 0);
	check(wait_stop(pid) == 0, "the tracee runs with the TPIDR_EL0 written and exits");
}

const char *find_argument(char **argv, int index)
{
	for (int position = 0; position <= index; position++)
		if (argv[position] == nullptr)
			return nullptr;
	return argv[index];
}

[[noreturn]] void power_off()
{
	for (;;) {
		sys(nr_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0);
		struct {
			long seconds, nanoseconds;
		} delay = {1, 0};
		sys(nr_nanosleep, ptr(&delay), 0);
	}
}

/* The mode comes from SME_PROBE_MODE=<mode> on the kernel command line,
 * which the kernel passes to init as an environment variable. */
const char *find_mode(char **envp)
{
	static const char prefix[] = "SME_PROBE_MODE=";
	static const char *const modes[] = {"kernel", "spin", "engine", "engine-spin", "off"};
	for (; *envp != nullptr; envp++) {
		const char *entry = *envp;
		u64 index = 0;
		while (prefix[index] != '\0' && entry[index] == prefix[index])
			index++;
		if (prefix[index] != '\0')
			continue;
		for (const char *mode : modes)
			if (same_string(entry + index, mode))
				return mode;
		return "unknown";
	}
	return "kernel";
}

} // namespace

extern "C" [[noreturn]] void probe_main(u64 *initial)
{
	int argc = static_cast<int>(initial[0]);
	char **argv = reinterpret_cast<char **>(initial + 1);
	char **envp = argv + argc + 1;
	u64 *auxv = initial + 1 + argc + 1;
	while (*auxv != 0)
		auxv++;
	auxv++;
	for (; auxv[0] != 0; auxv += 2) {
		if (auxv[0] == at_hwcap2)
			auxv_hwcap2 = auxv[1];
		else if (auxv[0] == at_minsigstksz)
			auxv_minsigstksz = auxv[1];
	}
	if (argc > 0 && argv[0] != nullptr)
		self_path = argv[0];
	const char *first = find_argument(argv, 1);
	if (first != nullptr && same_string(first, "exec-check"))
		exec_check(find_argument(argv, 2) != nullptr ? argv[2] : "plain");

	setup_console();
	install_handler();
	const char *mode = find_mode(envp);
	Line().add("sme-probe: mode ").add(mode).print();
	if (same_string(mode, "off")) {
		test_off();
	} else {
		test_parameters();
		check(!same_string(mode, "unknown"), "SME_PROBE_MODE names a mode");
	}
	if (svl != 0 && (auxv_hwcap2 & hwcap2_sme) != 0 && !same_string(mode, "unknown") &&
	    !same_string(mode, "off")) {
		if (same_string(mode, "kernel")) {
			mount_proc();
			test_plain_frame();
			test_streaming_sigreturn();
			test_za_frames();
			test_invalid_frames();
			test_task_creation();
			test_prctl();
			test_sysctl();
			test_regsets();
			test_syscall_stop();
		} else if (same_string(mode, "spin")) {
			run_spin();
		} else if (same_string(mode, "engine")) {
			run_engine();
		} else {
			run_engine_spin();
		}
	}
	Line().add("sme-probe: ").add(mode).add(": ").number(failures).add(" failure(s) in ")
		.number(checks).add(" check(s)").print();
	power_off();
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
