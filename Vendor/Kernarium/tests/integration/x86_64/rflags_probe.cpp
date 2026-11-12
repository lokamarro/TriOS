// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding x86-64 and i386 Linux guest probe: the RFLAGS a user task can
 * change, and the value it starts with, match Linux x86.
 *
 * Linux restores only FIX_EFLAGS (CF, PF, AF, ZF, SF, TF, DF, OF, RF and AC)
 * from a signal frame, lets a tracer change NT as well (ptrace's FLAG_MASK),
 * and starts a task from execve() with IF and reserved bit 1, 0x202. IOPL,
 * VM, VIF, VIP and ID are never a task's to set that way, and IF stays set.
 * RF, which IRET at CPL 3 and a signal return may set, is cleared as the next
 * instruction starts, so a system call that instruction makes never shows it,
 * but a fault saves the flags with RF set and a trap saves them as they are,
 * and an interrupt between the elements of a repeated string instruction
 * saves RF too. TF that IRET sets raises SIGTRAP after the instruction IRET
 * returns to. POPF at CPL 3 loads only the flags a task may change, IRET
 * those and RF, and PUSHF never stores RF. IRET without REX.W pops a 32-bit
 * frame in 64-bit mode.
 *
 * The x86-64 build traces an x86-64 and an i386 child stopped after execve(),
 * and raises a signal whose handler sets privileged bits in its frame. The
 * i386 build, whose path is the first argument, is the 32-bit child and runs
 * the same signal case under the i386 ABI. Each build also sets RF and reads
 * the signal frames, and on x86-64 SYSCALL's R11, of the kill() calls that
 * follow, sets TF with IRET, raises faults and traps, and signals a REP STOSB
 * from another CPU, which needs a second CPU. The x86-64 build also reads the
 * flags of both children stopped at a fault. Prints one line per case and
 * exits with the number of failures.
 */

namespace {

using u32 = unsigned int;
using u64 = unsigned long long;
using word = unsigned long;

#if defined(__x86_64__)
constexpr long nr_read = 0;
constexpr long nr_write = 1;
constexpr long nr_rt_sigaction = 13;
constexpr long nr_pipe = 22;
constexpr long nr_nanosleep = 35;
constexpr long nr_getpid = 39;
constexpr long nr_fork = 57;
constexpr long nr_execve = 59;
constexpr long nr_wait4 = 61;
constexpr long nr_kill = 62;
constexpr long nr_ptrace = 101;
constexpr long nr_sched_setaffinity = 203;
constexpr long nr_exit_group = 231;
/* uc_mcontext.eflags: uc_flags, uc_link and a 24-byte stack_t, then
 * r8-r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp and rip. */
constexpr unsigned frame_flags_offset = 40 + 17 * 8;

inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
			long a3 = 0)
{
	long result;
	register long r10 __asm__("r10") = a3;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
			 : "rcx", "r11", "memory", "cc");
	return result;
}

word read_flags()
{
	word flags;
	__asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags) : : "memory");
	return flags;
}

/* kill(pid, SIGUSR1), whose handler runs before the syscall returns, then
 * the flags sigreturn restored, read before any other instruction can change
 * them. A popf then returns the flags a popf can change to their start. */
word raise_and_read_flags(long pid)
{
	long result;
	word flags;
	__asm__ volatile("syscall\n\t"
			 "pushfq\n\t"
			 "popq %1\n\t"
			 "pushq $0x202\n\t"
			 "popfq"
			 : "=a"(result), "=&r"(flags)
			 : "a"(nr_kill), "D"(pid), "S"(10L)
			 : "rcx", "r11", "memory", "cc");
	return flags;
}

/* uc_mcontext.rax, rcx and rip, and siginfo's si_addr. */
constexpr unsigned frame_ax_offset = 40 + 13 * 8;
constexpr unsigned frame_cx_offset = 40 + 14 * 8;
constexpr unsigned frame_ip_offset = 40 + 16 * 8;
constexpr unsigned siginfo_address_offset = 16;

/* IRETQ to the instruction after it, with RFLAGS %[image]. */
#define PROBE_IRET_TO_NEXT                                                     \
	"movq %%rsp, %%rcx\n\t"                                                \
	"pushq $0x2b\n\t"                                                      \
	"pushq %%rcx\n\t"                                                      \
	"pushq %[image]\n\t"                                                   \
	"pushq $0x33\n\t"                                                      \
	"leaq 1f(%%rip), %%rcx\n\t"                                            \
	"pushq %%rcx\n\t"                                                      \
	"iretq\n"                                                              \
	"1:\t"

/* kill(pid, SIGUSR1) as the first instruction after an IRETQ that loads
 * `image`, or as the one after that. Returns the flags SYSCALL saved in R11,
 * which sigreturn restores. */
word iret_then_raise(word image, long pid, bool later)
{
	long result = nr_kill;
	word saved;
	if (later)
		__asm__ volatile(PROBE_IRET_TO_NEXT
				 "nop\n\t"
				 "syscall\n\t"
				 "movq %%r11, %[saved]"
				 : "+a"(result), [saved] "=r"(saved)
				 : [image] "r"(image), "D"(pid), "S"(10L)
				 : "rcx", "r11", "memory", "cc");
	else
		__asm__ volatile(PROBE_IRET_TO_NEXT
				 "syscall\n\t"
				 "movq %%r11, %[saved]"
				 : "+a"(result), [saved] "=r"(saved)
				 : [image] "r"(image), "D"(pid), "S"(10L)
				 : "rcx", "r11", "memory", "cc");
	return saved;
}

/* kill(pid, SIGUSR1) twice in a row, so the first handler's frame decides
 * what the second SYSCALL starts with. Returns the flags the second one saved
 * in R11. */
word raise_twice(long pid)
{
	long result = nr_kill;
	word saved;
	__asm__ volatile("syscall\n\t"
			 "syscall\n\t"
			 "movq %%r11, %[saved]"
			 : "+a"(result), [saved] "=r"(saved)
			 : "D"(pid), "S"(10L)
			 : "rcx", "r11", "memory", "cc");
	return saved;
}

__asm__(".text\n"
	".global probe_restorer\n"
	".type probe_restorer,@function\n"
	"probe_restorer:\n"
	"	mov $15, %eax\n"
	"	syscall\n"
	"	hlt\n"
	".size probe_restorer,.-probe_restorer\n"
	".global _start\n"
	".type _start,@function\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %rsp, %rdi\n"
	"	and $-16, %rsp\n"
	"	call probe_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n"
	"	hlt\n"
	".size _start,.-_start\n");

/* IRETQ with an image of TF, IF and bit 1 to the instruction after it, which
 * a NOP follows; the second form makes a getpid() system call first. Each
 * returns from the label named ..._after, where the trap lands. */
__asm__(".text\n"
	".global probe_iret_step\n"
	".global probe_iret_step_after\n"
	"probe_iret_step:\n"
	"	movq %rsp, %rcx\n"
	"	pushq $0x2b\n"
	"	pushq %rcx\n"
	"	pushq $0x302\n"
	"	pushq $0x33\n"
	"	leaq 1f(%rip), %rcx\n"
	"	pushq %rcx\n"
	"	iretq\n"
	"1:	nop\n"
	"probe_iret_step_after:\n"
	"	ret\n"
	".global probe_iret_syscall\n"
	".global probe_iret_syscall_after\n"
	"probe_iret_syscall:\n"
	"	movq %rsp, %rcx\n"
	"	pushq $0x2b\n"
	"	pushq %rcx\n"
	"	pushq $0x302\n"
	"	pushq $0x33\n"
	"	leaq 1f(%rip), %rcx\n"
	"	pushq %rcx\n"
	"	movl $39, %eax\n"
	"	iretq\n"
	"1:	syscall\n"
	"	nop\n"
	"probe_iret_syscall_after:\n"
	"	ret\n"
	".global probe_rep\n"
	".global probe_rep_insn\n"
	"probe_rep:\n"
	"	movq %rsi, %rcx\n"
	"	xorl %eax, %eax\n"
	"probe_rep_insn:\n"
	"	rep stosb\n"
	"	ret\n");
#elif defined(__i386__)
constexpr long nr_fork = 2;
constexpr long nr_read = 3;
constexpr long nr_write = 4;
constexpr long nr_getpid = 20;
constexpr long nr_kill = 37;
constexpr long nr_pipe = 42;
constexpr long nr_wait4 = 114;
constexpr long nr_nanosleep = 162;
constexpr long nr_rt_sigaction = 174;
constexpr long nr_sched_setaffinity = 241;
constexpr long nr_exit_group = 252;
/* uc_mcontext.eflags: uc_flags, uc_link and a 12-byte stack_t, then gs, fs,
 * es and ds, edi-eax, trapno, err, eip and cs. */
constexpr unsigned frame_flags_offset = 20 + 64;

inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
			long a3 = 0)
{
	long result;
	__asm__ volatile("int $0x80"
			 : "=a"(result)
			 : "a"(number), "b"(a0), "c"(a1), "d"(a2), "S"(a3)
			 : "memory", "cc");
	return result;
}

word read_flags()
{
	word flags;
	__asm__ volatile("pushfl\n\tpopl %0" : "=r"(flags) : : "memory");
	return flags;
}

word raise_and_read_flags(long pid)
{
	long result;
	word flags;
	__asm__ volatile("int $0x80\n\t"
			 "pushfl\n\t"
			 "popl %1\n\t"
			 "pushl $0x202\n\t"
			 "popfl"
			 : "=a"(result), "=&r"(flags)
			 : "a"(nr_kill), "b"(pid), "c"(10L)
			 : "memory", "cc");
	return flags;
}

/* uc_mcontext.eax, ecx and eip, and siginfo's si_addr. */
constexpr unsigned frame_ax_offset = 20 + 44;
constexpr unsigned frame_cx_offset = 20 + 40;
constexpr unsigned frame_ip_offset = 20 + 56;
constexpr unsigned siginfo_address_offset = 12;

/* IRET to the instruction after it, with EFLAGS %[image]. */
#define PROBE_IRET_TO_NEXT                                                     \
	"pushl %[image]\n\t"                                                   \
	"pushl $0x23\n\t"                                                      \
	"pushl $1f\n\t"                                                        \
	"iretl\n"                                                              \
	"1:\t"

/* The i386 versions: INT 80h saves no flags in a register, so they return
 * zero and only the signal frame shows the flags. */
word iret_then_raise(word image, long pid, bool later)
{
	long result = nr_kill;
	if (later)
		__asm__ volatile(PROBE_IRET_TO_NEXT
				 "nop\n\t"
				 "int $0x80"
				 : "+a"(result)
				 : [image] "r"(image), "b"(pid), "c"(10L)
				 : "memory", "cc");
	else
		__asm__ volatile(PROBE_IRET_TO_NEXT
				 "int $0x80"
				 : "+a"(result)
				 : [image] "r"(image), "b"(pid), "c"(10L)
				 : "memory", "cc");
	return 0;
}

word raise_twice(long pid)
{
	long result = nr_kill;
	__asm__ volatile("int $0x80\n\t"
			 "int $0x80"
			 : "+a"(result)
			 : "b"(pid), "c"(10L)
			 : "memory", "cc");
	return 0;
}

__asm__(".text\n"
	".global probe_restorer\n"
	".type probe_restorer,@function\n"
	"probe_restorer:\n"
	"	mov $173, %eax\n"
	"	int $0x80\n"
	"	hlt\n"
	".size probe_restorer,.-probe_restorer\n"
	".global _start\n"
	".type _start,@function\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %esp, %eax\n"
	"	and $-16, %esp\n"
	"	sub $12, %esp\n"
	"	push %eax\n"
	"	call probe_main\n"
	"	mov %eax, %ebx\n"
	"	mov $252, %eax\n"
	"	int $0x80\n"
	"	hlt\n"
	".size _start,.-_start\n");

/* The i386 forms of the IRET that sets TF. */
__asm__(".text\n"
	".global probe_iret_step\n"
	".global probe_iret_step_after\n"
	"probe_iret_step:\n"
	"	pushl $0x302\n"
	"	pushl $0x23\n"
	"	pushl $1f\n"
	"	iretl\n"
	"1:	nop\n"
	"probe_iret_step_after:\n"
	"	ret\n"
	".global probe_iret_syscall\n"
	".global probe_iret_syscall_after\n"
	"probe_iret_syscall:\n"
	"	pushl $0x302\n"
	"	pushl $0x23\n"
	"	pushl $1f\n"
	"	movl $20, %eax\n"
	"	iretl\n"
	"1:	int $0x80\n"
	"	nop\n"
	"probe_iret_syscall_after:\n"
	"	ret\n"
	".global probe_rep\n"
	".global probe_rep_insn\n"
	"probe_rep:\n"
	"	pushl %edi\n"
	"	movl 8(%esp), %edi\n"
	"	movl 12(%esp), %ecx\n"
	"	xorl %eax, %eax\n"
	"probe_rep_insn:\n"
	"	rep stosb\n"
	"	popl %edi\n"
	"	ret\n");
#else
#error "the RFLAGS probe is an x86-64 or i386 program"
#endif

constexpr word flag_cf = 1UL << 0;
constexpr word flag_reserved = 1UL << 1;
constexpr word flag_if = 1UL << 9;
constexpr word flag_iopl = 3UL << 12;
constexpr word flag_nt = 1UL << 14;
constexpr word flag_ac = 1UL << 18;
constexpr word flag_vif = 1UL << 19;
constexpr word flag_vip = 1UL << 20;
constexpr word flag_id = 1UL << 21;
constexpr word initial_flags = flag_if | flag_reserved;
/* A pushed image never shows VM, so the signal case leaves it out. */
constexpr word frame_privileged = flag_iopl | flag_vif | flag_vip | flag_id;

constexpr long sigusr1 = 10;
constexpr word sa_siginfo = 4;
constexpr word sa_restorer = 0x04000000;

struct KernelSigaction {
	word handler;
	word flags;
	word restorer;
	u32 mask[2];
};

void put(const char *text)
{
	long length = 0;
	while (text[length] != '\0')
		++length;
	raw_syscall(nr_write, 1, reinterpret_cast<long>(text), length);
}

void put_hex(u64 value)
{
	char text[19];
	int position = 18;
	text[position] = '\0';
	do {
		text[--position] = "0123456789abcdef"[value & 0xf];
		value >>= 4;
	} while (value != 0);
	text[--position] = 'x';
	text[--position] = '0';
	put(text + position);
}

bool equal(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

/* A written value keeps the bits no task may change, takes the ones it may,
 * and always has IF and bit 1. Prints why not. */
bool accepted_only(const char *what, u64 before, u64 requested, u64 after,
		   u64 privileged, u64 accepted)
{
	const bool ok = (after & privileged) == (before & privileged) &&
			(after & accepted) == (requested & accepted) &&
			(after & initial_flags) == initial_flags;
	if (!ok) {
		put("  ");
		put(what);
		put(": before ");
		put_hex(before);
		put(", requested ");
		put_hex(requested);
		put(", read back ");
		put_hex(after);
		put("\n");
	}
	return ok;
}

extern "C" void probe_restorer();

void set_privileged_frame_flags(int, void *, void *context)
{
	auto *flags = reinterpret_cast<word *>(static_cast<char *>(context) +
					       frame_flags_offset);
	*flags = (*flags | frame_privileged | flag_nt | flag_ac | flag_cf) &
		 ~flag_if;
}

/* A handler sets IOPL, NT, VIF, VIP, ID, AC and CF in its frame and clears
 * IF. sigreturn restores AC and CF only. */
bool sigreturn_restores_only_fix_eflags()
{
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(&set_privileged_frame_flags);
	action.flags = sa_siginfo | sa_restorer;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	if (raw_syscall(nr_rt_sigaction, sigusr1,
			reinterpret_cast<long>(&action), 0, 8) != 0) {
		put("  rt_sigaction failed\n");
		return false;
	}
	const word before = read_flags();
	const word after = raise_and_read_flags(raw_syscall(nr_getpid));
	const word requested =
		(before | frame_privileged | flag_nt | flag_ac | flag_cf) &
		~flag_if;
	/* NT is not in FIX_EFLAGS, so the frame cannot change it either. */
	return accepted_only("frame flags", before, requested, after,
			     frame_privileged | flag_nt, flag_ac | flag_cf);
}

constexpr word flag_rf = 1UL << 16;

volatile word resume_frame_flags;
volatile int resume_frames;
volatile bool resume_on_return;

/* Records the frame's flags. Armed, it also sets RF in the frame and has the
 * instruction sigreturn returns to make the same kill() again. */
void record_resume_frame(int, void *, void *context)
{
	auto *frame = static_cast<char *>(context);
	auto *flags = reinterpret_cast<word *>(frame + frame_flags_offset);
	resume_frame_flags = *flags;
	resume_frames = resume_frames + 1;
	if (resume_on_return) {
		resume_on_return = false;
		*flags |= flag_rf;
		*reinterpret_cast<word *>(frame + frame_ax_offset) = nr_kill;
	}
}

/* `frames` handlers ran since `before`, and neither the last frame nor the
 * flags the last system call saved show RF. Prints why not. */
bool resume_cleared(const char *what, int before, int frames, word saved)
{
	const bool ok = resume_frames == before + frames &&
			(resume_frame_flags & flag_rf) == 0 &&
			(saved & flag_rf) == 0;
	if (!ok) {
		put("  ");
		put(what);
		put(": handlers ");
		put_hex(static_cast<u32>(resume_frames - before));
		put(", frame flags ");
		put_hex(resume_frame_flags);
		put(", saved flags ");
		put_hex(saved);
		put("\n");
	}
	return ok;
}

/* RF from IRET's image, or from a signal frame through sigreturn, is gone
 * when the next instruction makes a system call: kill() as the first
 * instruction after IRET, as the one after that, and as the first after a
 * sigreturn that restored RF. */
bool resume_flag_clears_at_next_instruction()
{
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(&record_resume_frame);
	action.flags = sa_siginfo | sa_restorer;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	if (raw_syscall(nr_rt_sigaction, sigusr1,
			reinterpret_cast<long>(&action), 0, 8) != 0) {
		put("  rt_sigaction failed\n");
		return false;
	}
	const long pid = raw_syscall(nr_getpid);
	const word image = initial_flags | flag_rf;
	int before = resume_frames;
	word saved = iret_then_raise(image, pid, false);
	bool ok = resume_cleared("kill() right after IRET", before, 1, saved);
	before = resume_frames;
	saved = iret_then_raise(image, pid, true);
	ok = resume_cleared("kill() an instruction after IRET", before, 1,
			    saved) && ok;
	before = resume_frames;
	resume_on_return = true;
	saved = raise_twice(pid);
	return resume_cleared("kill() right after sigreturn", before, 2,
			      saved) && ok;
}

#if defined(__x86_64__)
/* PUSHFQ right after an IRETQ that loads `image`, then a POPFQ of 0x202.
 * The image may set AC, so every stack access stays aligned until then. */
word iret_then_read_flags(word image)
{
	word flags;
	__asm__ volatile(PROBE_IRET_TO_NEXT
			 "pushfq\n\t"
			 "popq %[flags]\n\t"
			 "pushq $0x202\n\t"
			 "popfq"
			 : [flags] "=&r"(flags)
			 : [image] "r"(image)
			 : "rcx", "memory", "cc");
	return flags;
}

/* PUSHFQ after a POPFQ of `image`, or after a POPFQ of ID, IF and bit 1
 * and a POPFW of `image`, then a POPFQ of 0x202. */
word popf_then_read_flags(word image, bool narrow)
{
	word flags;
	if (narrow)
		__asm__ volatile("pushq $0x200202\n\t"
				 "popfq\n\t"
				 "pushw %w[image]\n\t"
				 "popfw\n\t"
				 "pushfq\n\t"
				 "popq %[flags]\n\t"
				 "pushq $0x202\n\t"
				 "popfq"
				 : [flags] "=&r"(flags)
				 : [image] "r"(image)
				 : "memory", "cc");
	else
		__asm__ volatile("pushq %[image]\n\t"
				 "popfq\n\t"
				 "pushfq\n\t"
				 "popq %[flags]\n\t"
				 "pushq $0x202\n\t"
				 "popfq"
				 : [flags] "=&r"(flags)
				 : [image] "r"(image)
				 : "memory", "cc");
	return flags;
}

bool flags_read(const char *what, word flags, word expected)
{
	if (flags == expected)
		return true;
	put("  ");
	put(what);
	put(": read ");
	put_hex(flags);
	put(", expected ");
	put_hex(expected);
	put("\n");
	return false;
}

/* At CPL 3 POPF loads only CF, PF, AF, ZF, SF, TF, DF, OF, NT, AC and ID,
 * and a 16-bit POPF only those in the low half; IRET loads them and RF. IF,
 * IOPL, VM, VIF and VIP keep their values. PUSHF stores RF and VM as zero.
 * TF stays clear, for it would trap after the next instruction. */
bool user_flag_loads()
{
	const word image = 0xfffffffffffefeffUL;
	bool ok = flags_read("PUSHFQ after POPFQ of every flag but TF and RF",
			     popf_then_read_flags(image, false), 0x244ed7);
	ok &= flags_read("PUSHFQ after POPFQ of zero",
			 popf_then_read_flags(0, false), initial_flags);
	ok &= flags_read("PUSHFQ after POPFW of every flag but TF",
			 popf_then_read_flags(image, true), 0x204ed7);
	ok &= flags_read("PUSHFQ after IRETQ of every flag but TF and RF",
			 iret_then_read_flags(image), 0x244ed7);
	ok &= flags_read("PUSHFQ after IRETQ of RF",
			 iret_then_read_flags(initial_flags | flag_rf),
			 initial_flags);
	return ok;
}

/* IRET without REX.W in 64-bit mode pops 32-bit EIP, CS, EFLAGS, ESP and
 * SS, each zero-extended. Its frame and the stack it returns to sit below
 * 4 GiB, in the static image. */
alignas(16) u32 iretl_frame[8];
alignas(16) word iretl_stack[8];
word iretl_saved_sp;

bool iretl_pops_a_32_bit_frame()
{
	word flags;
	word sp;
	__asm__ volatile("movq %%rsp, %[saved]\n\t"
			 "leaq %[frame], %%rsp\n\t"
			 "leaq 1f(%%rip), %%rcx\n\t"
			 "movl %%ecx, (%%rsp)\n\t"
			 "movl $0x33, 4(%%rsp)\n\t"
			 "movl $0xfffefeff, 8(%%rsp)\n\t"
			 "leaq %[stack], %%rcx\n\t"
			 "movl %%ecx, 12(%%rsp)\n\t"
			 "movl $0x2b, 16(%%rsp)\n\t"
			 "iretl\n"
			 "1:\tpushfq\n\t"
			 "popq %[flags]\n\t"
			 "movq %%rsp, %[sp]\n\t"
			 "movq %[saved], %%rsp\n\t"
			 "pushq $0x202\n\t"
			 "popfq"
			 : [flags] "=&r"(flags), [sp] "=&r"(sp),
			   [saved] "=m"(iretl_saved_sp), [frame] "+m"(iretl_frame)
			 : [stack] "m"(iretl_stack[4])
			 : "rcx", "memory", "cc");
	bool ok = flags_read("PUSHFQ after IRETL of every flag but TF and RF",
			     flags, 0x244ed7);
	ok &= flags_read("RSP after IRETL",
			 sp, reinterpret_cast<word>(&iretl_stack[4]));
	return ok;
}

/* RDRAND and RDSEED, which CPUID advertises, always succeed here: CF set
 * and OF, SF, ZF, AF and PF clear, whatever they held. A 16-bit destination
 * keeps the register's upper bits and a 32-bit one is zero-extended; two
 * 64-bit reads differ. */
bool random_reads()
{
	u32 leaf1 = 1;
	u32 leaf1_ecx;
	u32 leaf7 = 7;
	u32 leaf7_ebx;
	u32 subleaf = 0;
	__asm__ volatile("cpuid"
			 : "+a"(leaf1), "=c"(leaf1_ecx)
			 :
			 : "ebx", "edx");
	__asm__ volatile("cpuid"
			 : "+a"(leaf7), "=b"(leaf7_ebx), "+c"(subleaf)
			 :
			 : "edx");
	bool ok = flags_read("CPUID.1:ECX.RDRAND", (leaf1_ecx >> 30) & 1, 1);
	ok &= flags_read("CPUID.7.0:EBX.RDSEED", (leaf7_ebx >> 18) & 1, 1);
	for (int seed = 0; seed < 2; ++seed) {
		word flags;
		word first;
		word second;
		word narrow = 0x1111111122223333UL;
		word middle = ~0UL;
		if (seed != 0)
			__asm__ volatile("pushq $0xad6\n\t"
					 "popfq\n\t"
					 "rdseed %[first]\n\t"
					 "pushfq\n\t"
					 "popq %[flags]\n\t"
					 "rdseed %w[narrow]\n\t"
					 "rdseed %k[middle]\n\t"
					 "rdseed %[second]\n\t"
					 "pushq $0x202\n\t"
					 "popfq"
					 : [flags] "=&r"(flags), [first] "=&r"(first),
					   [second] "=&r"(second), [narrow] "+r"(narrow),
					   [middle] "+r"(middle)
					 :
					 : "memory", "cc");
		else
			__asm__ volatile("pushq $0xad6\n\t"
					 "popfq\n\t"
					 "rdrand %[first]\n\t"
					 "pushfq\n\t"
					 "popq %[flags]\n\t"
					 "rdrand %w[narrow]\n\t"
					 "rdrand %k[middle]\n\t"
					 "rdrand %[second]\n\t"
					 "pushq $0x202\n\t"
					 "popfq"
					 : [flags] "=&r"(flags), [first] "=&r"(first),
					   [second] "=&r"(second), [narrow] "+r"(narrow),
					   [middle] "+r"(middle)
					 :
					 : "memory", "cc");
		bool reads = flags_read("RFLAGS after it", flags, 0x203);
		reads &= flags_read("upper bits of a 16-bit destination",
				    narrow >> 16, 0x111111112222UL);
		reads &= flags_read("upper half of a 32-bit destination",
				    middle >> 32, 0);
		reads &= flags_read("two 64-bit reads differ", first != second, 1);
		if (!reads) {
			put(seed != 0 ? "  in RDSEED\n" : "  in RDRAND\n");
			ok = false;
		}
	}
	return ok;
}
#endif

constexpr word flag_tf = 1UL << 8;
constexpr long trap_signal = 5;
constexpr int trap_trace = 2;

extern "C" void probe_iret_step();
extern "C" char probe_iret_step_after[];
extern "C" void probe_iret_syscall();
extern "C" char probe_iret_syscall_after[];

volatile int step_traps;
volatile int step_code;
volatile word step_ip;
volatile word step_address;
volatile word step_flags;

/* Records the #DB and clears TF in its frame, so the task runs on. */
void record_step(int, void *info, void *context)
{
	auto *frame = static_cast<char *>(context);
	auto *flags = reinterpret_cast<word *>(frame + frame_flags_offset);
	auto *details = static_cast<char *>(info);
	step_traps = step_traps + 1;
	step_code = *reinterpret_cast<int *>(details + 8);
	step_address =
		*reinterpret_cast<word *>(details + siginfo_address_offset);
	step_ip = *reinterpret_cast<word *>(frame + frame_ip_offset);
	step_flags = *flags;
	*flags &= ~flag_tf;
}

/* The stub raised exactly one SIGTRAP, TRAP_TRACE at `after`, whose frame
 * holds the IRET image: TF, IF and bit 1, and no RF. A system call after the
 * stub delivers a trap the engine left pending. Prints why not. */
bool stepped_to(const char *what, void (*stub)(), const char *after)
{
	step_traps = 0;
	stub();
	raw_syscall(nr_getpid);
	const word expected = reinterpret_cast<word>(after);
	const bool ok = step_traps == 1 && step_code == trap_trace &&
			step_ip == expected && step_address == expected &&
			step_flags == (flag_tf | initial_flags);
	if (!ok) {
		put("  ");
		put(what);
		put(": traps ");
		put_hex(static_cast<u32>(step_traps));
		put(", si_code ");
		put_hex(static_cast<u32>(step_code));
		put(", ip ");
		put_hex(step_ip);
		put(" (expected ");
		put_hex(expected);
		put("), frame flags ");
		put_hex(step_flags);
		put("\n");
	}
	return ok;
}

/* IRET that sets TF raises #DB after the instruction it returns to, as POPF
 * does; after a system call instead, the trap follows the instruction after
 * it, because the kernel entry swallows the step of the call itself. */
bool iret_trap_flag_steps_one_instruction()
{
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(&record_step);
	action.flags = sa_siginfo | sa_restorer;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	if (raw_syscall(nr_rt_sigaction, trap_signal,
			reinterpret_cast<long>(&action), 0, 8) != 0) {
		put("  rt_sigaction failed\n");
		return false;
	}
	const bool ok = stepped_to("the instruction after IRET",
				   probe_iret_step, probe_iret_step_after);
	return stepped_to("a system call after IRET", probe_iret_syscall,
			  probe_iret_syscall_after) && ok;
}

/* One instruction of each exception class the engine raises, at ..._insn in
 * a stub that returns from ..._end. The same code assembles for both ABIs. */
__asm__(".text\n"
	".global probe_ud2\n"
	".global probe_ud2_insn\n"
	".global probe_ud2_end\n"
	"probe_ud2:\n"
	"probe_ud2_insn:\n"
	"	ud2\n"
	"probe_ud2_end:\n"
	"	ret\n"
	".global probe_hlt\n"
	".global probe_hlt_insn\n"
	".global probe_hlt_end\n"
	"probe_hlt:\n"
	"probe_hlt_insn:\n"
	"	hlt\n"
	"probe_hlt_end:\n"
	"	ret\n"
	".global probe_divide\n"
	".global probe_divide_insn\n"
	".global probe_divide_end\n"
	"probe_divide:\n"
	"	xorl %eax, %eax\n"
	"	xorl %ecx, %ecx\n"
	"	xorl %edx, %edx\n"
	"probe_divide_insn:\n"
	"	divl %ecx\n"
	"probe_divide_end:\n"
	"	ret\n"
	".global probe_page\n"
	".global probe_page_insn\n"
	".global probe_page_end\n"
	"probe_page:\n"
	"probe_page_insn:\n"
	"	movl 0, %eax\n"
	"probe_page_end:\n"
	"	ret\n"
	".global probe_int3\n"
	".global probe_int3_insn\n"
	".global probe_int3_end\n"
	"probe_int3:\n"
	"probe_int3_insn:\n"
	"	int3\n"
	"probe_int3_end:\n"
	"	ret\n"
	".global probe_int4\n"
	".global probe_int4_insn\n"
	".global probe_int4_end\n"
	"probe_int4:\n"
	"probe_int4_insn:\n"
	"	int $4\n"
	"probe_int4_end:\n"
	"	ret\n");

#define PROBE_STUB(name)                                                       \
	extern "C" void probe_##name();                                        \
	extern "C" char probe_##name##_insn[];                                 \
	extern "C" char probe_##name##_end[];
PROBE_STUB(ud2)
PROBE_STUB(hlt)
PROBE_STUB(divide)
PROBE_STUB(page)
PROBE_STUB(int3)
PROBE_STUB(int4)
#undef PROBE_STUB

constexpr long sigill = 4;
constexpr long sigfpe = 8;
constexpr long sigsegv = 11;

volatile int fault_signal;
volatile word fault_ip;
volatile word fault_flags;
volatile word fault_resume;

/* Records the frame and resumes at the stub's return. */
void record_fault(int signal, void *, void *context)
{
	auto *frame = static_cast<char *>(context);
	auto *ip = reinterpret_cast<word *>(frame + frame_ip_offset);
	fault_signal = signal;
	fault_ip = *ip;
	fault_flags = *reinterpret_cast<word *>(frame + frame_flags_offset);
	*ip = fault_resume;
}

struct ExceptionCase {
	const char *name;
	void (*stub)();
	const char *instruction;
	const char *end;
	long signal;
	bool fault;
};

/* A fault's frame names the instruction and has RF set; a trap's names the
 * next one and has it clear. Prints why not. */
bool exception_frame(const ExceptionCase &shape)
{
	fault_signal = 0;
	fault_resume = reinterpret_cast<word>(shape.end);
	shape.stub();
	const word ip = reinterpret_cast<word>(
		shape.fault ? shape.instruction : shape.end);
	const bool ok = fault_signal == shape.signal && fault_ip == ip &&
			((fault_flags & flag_rf) != 0) == shape.fault;
	if (!ok) {
		put("  ");
		put(shape.name);
		put(": signal ");
		put_hex(static_cast<u32>(fault_signal));
		put(", ip ");
		put_hex(fault_ip);
		put(" (expected ");
		put_hex(ip);
		put("), frame flags ");
		put_hex(fault_flags);
		put(shape.fault ? ", Linux sets RF\n" : ", Linux clears RF\n");
	}
	return ok;
}

/* Hardware sets RF in the flags it saves for every fault but an instruction
 * breakpoint, so the resumed instruction does not break again; a trap saves
 * the flags as they are. */
bool fault_frames_set_resume_flag()
{
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(&record_fault);
	action.flags = sa_siginfo | sa_restorer;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	const long signals[] = {sigill, trap_signal, sigfpe, sigsegv};
	for (const long signal : signals) {
		if (raw_syscall(nr_rt_sigaction, signal,
				reinterpret_cast<long>(&action), 0, 8) != 0) {
			put("  rt_sigaction failed\n");
			return false;
		}
	}
	const ExceptionCase cases[] = {
		{"#UD from UD2", probe_ud2, probe_ud2_insn, probe_ud2_end, sigill,
		 true},
		{"#GP from HLT", probe_hlt, probe_hlt_insn, probe_hlt_end, sigsegv,
		 true},
		{"#DE from DIV by zero", probe_divide, probe_divide_insn,
		 probe_divide_end, sigfpe, true},
		{"#PF from a load at address zero", probe_page, probe_page_insn,
		 probe_page_end, sigsegv, true},
		{"#BP from INT3", probe_int3, probe_int3_insn, probe_int3_end,
		 trap_signal, false},
		{"#OF from INT 4", probe_int4, probe_int4_insn, probe_int4_end,
		 sigsegv, false},
	};
	bool ok = true;
	for (const auto &shape : cases)
		ok = exception_frame(shape) && ok;
	return ok;
}

/* REP STOSB of `count` bytes of zero at `buffer`, at probe_rep_insn. */
extern "C" void probe_rep(void *buffer, word count);
extern "C" char probe_rep_insn[];

constexpr long sigusr2 = 12;
constexpr word rep_bytes = 1UL << 22;
constexpr word rep_minimum = 20;
constexpr int rep_signals = 200;

struct Timespec {
	long seconds;
	long nanoseconds;
};

alignas(4096) unsigned char rep_buffer[rep_bytes];
volatile word rep_resumed;
volatile word rep_cleared;
volatile word rep_elsewhere;
volatile bool rep_done;

/* Classifies where a signal found the task: inside the REP STOSB, with
 * elements done and elements left, or anywhere else. */
void record_rep(int, void *, void *context)
{
	auto *frame = static_cast<char *>(context);
	const word ip = *reinterpret_cast<word *>(frame + frame_ip_offset);
	const word count = *reinterpret_cast<word *>(frame + frame_cx_offset);
	const word flags = *reinterpret_cast<word *>(frame + frame_flags_offset);
	if (ip != reinterpret_cast<word>(probe_rep_insn) || count == 0 ||
	    count == rep_bytes)
		rep_elsewhere = rep_elsewhere + 1;
	else if ((flags & flag_rf) != 0)
		rep_resumed = rep_resumed + 1;
	else
		rep_cleared = rep_cleared + 1;
}

void stop_rep(int, void *, void *)
{
	rep_done = true;
}

bool install(long signal, void (*handler)(int, void *, void *))
{
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(handler);
	action.flags = sa_siginfo | sa_restorer;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	return raw_syscall(nr_rt_sigaction, signal,
			   reinterpret_cast<long>(&action), 0, 8) == 0;
}

bool pin_to(long cpu)
{
	word mask = 1UL << cpu;
	return raw_syscall(nr_sched_setaffinity, 0, sizeof(mask),
			   reinterpret_cast<long>(&mask)) == 0;
}

/* The child repeats the REP STOSB on CPU 0 until SIGUSR2 and exits 0 when
 * enough of the SIGUSR1s found it mid-string and every one showed RF. */
int repeat_until_stopped(long ready)
{
	if (!pin_to(0) || !install(sigusr1, record_rep) ||
	    !install(sigusr2, stop_rep))
		return 2;
	const char byte = 1;
	raw_syscall(nr_write, ready, reinterpret_cast<long>(&byte), 1);
	for (long round = 0; !rep_done && round < 100000; ++round)
		probe_rep(rep_buffer, rep_bytes);
	if (rep_resumed >= rep_minimum && rep_cleared == 0)
		return 0;
	put("  signals mid-string with RF ");
	put_hex(rep_resumed);
	put(", without RF ");
	put_hex(rep_cleared);
	put(", elsewhere ");
	put_hex(rep_elsewhere);
	put("\n");
	return 1;
}

/* A signal from another CPU interrupts a REP STOSB between elements. The
 * frame names the instruction with its count part done, and hardware saves
 * RF there, so the resumed instruction does not break again. */
bool interrupted_rep_string_sets_resume_flag()
{
	int ready[2] = {};
	if (raw_syscall(nr_pipe, reinterpret_cast<long>(ready)) != 0) {
		put("  pipe failed\n");
		return false;
	}
	const long child = raw_syscall(nr_fork);
	if (child == 0)
		raw_syscall(nr_exit_group, repeat_until_stopped(ready[1]));
	char byte = 0;
	const bool started = child > 0 && pin_to(1) &&
		raw_syscall(nr_read, ready[0], reinterpret_cast<long>(&byte),
			    1) == 1;
	if (started) {
		const Timespec gap = {0, 1000000};
		for (int sent = 0; sent < rep_signals; ++sent) {
			raw_syscall(nr_kill, child, sigusr1);
			raw_syscall(nr_nanosleep,
				    reinterpret_cast<long>(&gap), 0);
		}
		raw_syscall(nr_kill, child, sigusr2);
	} else {
		put("  the REP STOSB child did not start on another CPU\n");
		if (child > 0)
			raw_syscall(nr_kill, child, 9);
	}
	int status = 0;
	if (child > 0)
		raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0,
			    0);
	return started && (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0;
}

#if defined(__x86_64__)
constexpr word flag_vm = 1UL << 17;
constexpr word tracer_privileged = frame_privileged | flag_vm;
constexpr long nr_sigkill = 9;
constexpr long sigtrap = 5;
constexpr long ptrace_traceme = 0;
constexpr long ptrace_peekuser = 3;
constexpr long ptrace_pokeuser = 6;
constexpr long ptrace_getregs = 12;
constexpr long ptrace_setregs = 13;
constexpr long ptrace_getregset = 0x4204;
constexpr long ptrace_setregset = 0x4205;
constexpr long nt_prstatus = 1;
/* struct user_regs_struct: eflags follows r15-r8, rax-rdi, orig_rax, rip
 * and cs. The i386 layout has it after ebx-eax, ds-gs, orig_eax, eip, cs. */
constexpr unsigned user_flags_index = 18;
constexpr unsigned user_regs_words = 27;
constexpr unsigned user_regs32_flags_index = 14;
constexpr unsigned user_regs32_words = 17;

struct Iovec {
	void *base;
	word length;
};

long wait_for(long pid, int &status)
{
	return raw_syscall(nr_wait4, pid, reinterpret_cast<long>(&status), 0, 0);
}

/* Start a child that is traced from execve(): it stops with SIGTRAP after
 * the new program is loaded and before it runs an instruction. */
long start_traced(const char *path, const char *mode)
{
	const char *arguments[] = {path, mode, nullptr};
	const char *environment[] = {nullptr};
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		raw_syscall(nr_ptrace, ptrace_traceme, 0, 0, 0);
		raw_syscall(nr_execve, reinterpret_cast<long>(path),
			    reinterpret_cast<long>(arguments),
			    reinterpret_cast<long>(environment));
		raw_syscall(nr_exit_group, 127);
	}
	int status = 0;
	if (child < 0 || wait_for(child, status) != child ||
	    (status & 0xff) != 0x7f || ((status >> 8) & 0xff) != sigtrap) {
		put("  no stop after execve(): status ");
		put_hex(static_cast<u32>(status));
		put("\n");
		return -1;
	}
	return child;
}

void finish(long child)
{
	int status = 0;
	raw_syscall(nr_kill, child, nr_sigkill);
	wait_for(child, status);
}

bool get_regs(long child, u64 (&regs)[user_regs_words])
{
	return raw_syscall(nr_ptrace, ptrace_getregs, child, 0,
			   reinterpret_cast<long>(regs)) == 0;
}

bool set_regs(long child, u64 (&regs)[user_regs_words])
{
	return raw_syscall(nr_ptrace, ptrace_setregs, child, 0,
			   reinterpret_cast<long>(regs)) == 0;
}

bool exec_stop_flags(long child, u64 &flags)
{
	u64 regs[user_regs_words] = {};
	if (!get_regs(child, regs))
		return false;
	flags = regs[user_flags_index];
	if (flags == initial_flags)
		return true;
	put("  RFLAGS at the execve() stop ");
	put_hex(flags);
	put(", Linux starts a task with 0x202\n");
	return false;
}

u64 tracer_request(u64 flags)
{
	return (flags | tracer_privileged | flag_nt | flag_ac | flag_cf) &
	       ~flag_if;
}

bool setregs_changes_only_flag_mask(long child)
{
	u64 regs[user_regs_words] = {};
	if (!get_regs(child, regs))
		return false;
	const u64 before = regs[user_flags_index];
	regs[user_flags_index] = tracer_request(before);
	if (!set_regs(child, regs) || !get_regs(child, regs))
		return false;
	const u64 after = regs[user_flags_index];
	regs[user_flags_index] = before;
	const bool restored = set_regs(child, regs);
	return accepted_only("PTRACE_SETREGS", before, tracer_request(before),
			     after, tracer_privileged,
			     flag_nt | flag_ac | flag_cf) && restored;
}

bool pokeuser_changes_only_flag_mask(long child)
{
	const long offset = user_flags_index * sizeof(u64);
	u64 before = 0;
	u64 after = 0;
	if (raw_syscall(nr_ptrace, ptrace_peekuser, child, offset,
			reinterpret_cast<long>(&before)) != 0 ||
	    raw_syscall(nr_ptrace, ptrace_pokeuser, child, offset,
			static_cast<long>(tracer_request(before))) != 0 ||
	    raw_syscall(nr_ptrace, ptrace_peekuser, child, offset,
			reinterpret_cast<long>(&after)) != 0)
		return false;
	return accepted_only("PTRACE_POKEUSER", before, tracer_request(before),
			     after, tracer_privileged,
			     flag_nt | flag_ac | flag_cf);
}

/* An i386 tracee's general registers are its i386 regset, which 32-bit
 * tracers also write. */
bool setregset32_changes_only_flag_mask(long child)
{
	u32 regs[user_regs32_words] = {};
	Iovec vector = {regs, sizeof(regs)};
	if (raw_syscall(nr_ptrace, ptrace_getregset, child, nt_prstatus,
			reinterpret_cast<long>(&vector)) != 0 ||
	    vector.length != sizeof(regs))
		return false;
	const u32 before = regs[user_regs32_flags_index];
	const u32 requested = static_cast<u32>(tracer_request(before));
	regs[user_regs32_flags_index] = requested;
	if (raw_syscall(nr_ptrace, ptrace_setregset, child, nt_prstatus,
			reinterpret_cast<long>(&vector)) != 0 ||
	    raw_syscall(nr_ptrace, ptrace_getregset, child, nt_prstatus,
			reinterpret_cast<long>(&vector)) != 0)
		return false;
	return accepted_only("i386 NT_PRSTATUS", before, requested,
			     regs[user_regs32_flags_index], tracer_privileged,
			     flag_nt | flag_ac | flag_cf);
}

constexpr long ptrace_cont = 7;

/* The traced child raises #UD. At its signal-delivery stop a tracer reads
 * the flags the fault saved, with RF set. */
bool fault_stop_shows_resume_flag(long child, bool i386)
{
	int status = 0;
	if (raw_syscall(nr_ptrace, ptrace_cont, child, 0, 0) != 0 ||
	    wait_for(child, status) != child || (status & 0xff) != 0x7f ||
	    ((status >> 8) & 0xff) != sigill) {
		put("  no SIGILL stop: status ");
		put_hex(static_cast<u32>(status));
		put("\n");
		return false;
	}
	u64 flags = 0;
	if (i386) {
		u32 regs[user_regs32_words] = {};
		Iovec vector = {regs, sizeof(regs)};
		if (raw_syscall(nr_ptrace, ptrace_getregset, child, nt_prstatus,
				reinterpret_cast<long>(&vector)) != 0)
			return false;
		flags = regs[user_regs32_flags_index];
	} else {
		u64 regs[user_regs_words] = {};
		if (!get_regs(child, regs))
			return false;
		flags = regs[user_flags_index];
	}
	if ((flags & flag_rf) != 0)
		return true;
	put("  RFLAGS at the SIGILL stop ");
	put_hex(flags);
	put(", Linux sets RF\n");
	return false;
}

/* The signal cases run in a child, since no popf can clear an IOPL a
 * sigreturn wrongly installed, and no instruction the task has can clear an
 * RF that stays set. */
bool in_child(const char *path, const char *mode)
{
	const char *arguments[] = {path, mode, nullptr};
	const char *environment[] = {nullptr};
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		raw_syscall(nr_execve, reinterpret_cast<long>(path),
			    reinterpret_cast<long>(arguments),
			    reinterpret_cast<long>(environment));
		raw_syscall(nr_exit_group, 127);
	}
	int status = 0;
	if (child < 0 || wait_for(child, status) != child)
		return false;
	if ((status & 0x7f) != 0) {
		put("  killed by signal ");
		put_hex(static_cast<u32>(status & 0x7f));
		put("\n");
		return false;
	}
	return ((status >> 8) & 0xff) == 0;
}

int failures;

void report(const char *name, bool ok)
{
	put(ok ? "PASS " : "FAIL ");
	put(name);
	put("\n");
	failures += ok ? 0 : 1;
}

int run_all(const char *self, const char *i386_probe)
{
	u64 flags = 0;
	long child = start_traced(self, "idle");
	report("x86-64 execve() starts with RFLAGS 0x202",
	       child > 0 && exec_stop_flags(child, flags));
	report("x86-64 PTRACE_SETREGS changes only FLAG_MASK bits",
	       child > 0 && setregs_changes_only_flag_mask(child));
	report("x86-64 PTRACE_POKEUSER changes only FLAG_MASK bits",
	       child > 0 && pokeuser_changes_only_flag_mask(child));
	if (child > 0)
		finish(child);
	report("x86-64 sigreturn restores only FIX_EFLAGS bits",
	       in_child(self, "sigreturn"));
	report("x86-64 RF from IRET or sigreturn is gone at the next instruction",
	       in_child(self, "resume"));
	report("x86-64 TF set by IRET traps after one instruction",
	       in_child(self, "step"));
	report("x86-64 POPF and IRET load only user flags, PUSHF no RF",
	       in_child(self, "user-loads"));
	report("x86-64 IRET without REX.W pops a 32-bit frame",
	       in_child(self, "iretl"));
	report("x86-64 RDRAND and RDSEED succeed and set only CF",
	       in_child(self, "random"));
	report("x86-64 fault frames set RF and trap frames do not",
	       in_child(self, "faults"));
	report("x86-64 a REP string interrupted between elements shows RF",
	       in_child(self, "rep"));
	child = start_traced(self, "ud2");
	report("x86-64 a tracer sees RF at a fault's stop",
	       child > 0 && fault_stop_shows_resume_flag(child, false));
	if (child > 0)
		finish(child);

	child = start_traced(i386_probe, "idle");
	report("i386 execve() starts with EFLAGS 0x202",
	       child > 0 && exec_stop_flags(child, flags));
	report("i386 NT_PRSTATUS write changes only FLAG_MASK bits",
	       child > 0 && setregset32_changes_only_flag_mask(child));
	if (child > 0)
		finish(child);
	report("i386 sigreturn restores only FIX_EFLAGS bits",
	       in_child(i386_probe, "sigreturn"));
	report("i386 RF from IRET or sigreturn is gone at the next instruction",
	       in_child(i386_probe, "resume"));
	report("i386 TF set by IRET traps after one instruction",
	       in_child(i386_probe, "step"));
	report("i386 fault frames set RF and trap frames do not",
	       in_child(i386_probe, "faults"));
	report("i386 a REP string interrupted between elements shows RF",
	       in_child(i386_probe, "rep"));
	child = start_traced(i386_probe, "ud2");
	report("i386 a tracer sees RF at a fault's stop",
	       child > 0 && fault_stop_shows_resume_flag(child, true));
	if (child > 0)
		finish(child);
	return failures;
}
#endif

} // namespace

extern "C" int probe_main(word *stack)
{
	const long count = static_cast<long>(stack[0]);
	char **arguments = reinterpret_cast<char **>(stack + 1);
	if (count == 2 && equal(arguments[1], "idle"))
		return 0;
	if (count == 2 && equal(arguments[1], "sigreturn"))
		return sigreturn_restores_only_fix_eflags() ? 0 : 1;
	if (count == 2 && equal(arguments[1], "resume"))
		return resume_flag_clears_at_next_instruction() ? 0 : 1;
	if (count == 2 && equal(arguments[1], "step"))
		return iret_trap_flag_steps_one_instruction() ? 0 : 1;
	if (count == 2 && equal(arguments[1], "faults"))
		return fault_frames_set_resume_flag() ? 0 : 1;
	if (count == 2 && equal(arguments[1], "rep"))
		return interrupted_rep_string_sets_resume_flag() ? 0 : 1;
	if (count == 2 && equal(arguments[1], "ud2")) {
		probe_ud2();
		return 1;
	}
#if defined(__x86_64__)
	if (count == 2 && equal(arguments[1], "user-loads"))
		return user_flag_loads() ? 0 : 1;
	if (count == 2 && equal(arguments[1], "iretl"))
		return iretl_pops_a_32_bit_frame() ? 0 : 1;
	if (count == 2 && equal(arguments[1], "random"))
		return random_reads() ? 0 : 1;
	if (count == 2)
		return run_all(arguments[0], arguments[1]);
	put("usage: rflags-probe I386_PROBE\n");
#endif
	return 100;
}

using size_type = decltype(sizeof(0));

extern "C" void *memset(void *destination, int value, size_type length)
{
	auto *bytes = static_cast<volatile unsigned char *>(destination);
	for (size_type index = 0; index < length; ++index)
		bytes[index] = static_cast<unsigned char>(value);
	return destination;
}

extern "C" void *memcpy(void *destination, const void *source,
			size_type length)
{
	auto *to = static_cast<volatile unsigned char *>(destination);
	const auto *from = static_cast<const unsigned char *>(source);
	for (size_type index = 0; index < length; ++index)
		to[index] = from[index];
	return destination;
}
