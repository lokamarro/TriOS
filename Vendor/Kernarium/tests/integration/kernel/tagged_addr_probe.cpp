// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest probe: the AArch64 tagged-address ABI as a task sees it.
//
// arm64 Linux lets a task pass pointers with a tag in their top byte to
// system calls once it has asked for that with
// prctl(PR_SET_TAGGED_ADDR_CTRL, PR_TAGGED_ADDR_ENABLE)
// (Documentation/arch/arm64/tagged-address-abi.rst). The probe passes tagged
// pointers to the system calls that read and write user memory, to the
// memory-management calls that untag whether or not the ABI is on, and to
// futex, process_vm_readv, process_vm_writev and ptrace, and it prints one
// name=value row for each result. It does the same with the ABI off and with
// it on, and it follows the ABI through fork, exec, threads and ptrace and
// into an AArch32 task, which has none.
//
// Every row is compared with the row native arm64 Linux 6.18.50 printed for
// this probe (a Raspberry Pi 4 with 4 KiB pages and no MTE), listed in
// native_rows below, so the table is the contract. Two rows differ on purpose
// (deviations): SA_EXPOSE_TAGBITS shows a fault's address without its tag
// because the engines report the address they translated, never the one the
// instruction presented. The probe also checks what only this kernel has, the
// native window that holds guest RAM and the kernel's own mappings, and so
// refuses fixed placements and has to ignore hints.
//
// The expected row set depends on the run's top-byte-ignore, which the tagged
// pointers of the probe itself need, so the test says which it started:
//
//	probe --tbi=on	the engines ignore the top byte, as they do by default:
//			the ABI can be enabled and every row above is checked
//	probe --tbi=off	they do not (--guest-no-top-byte-ignore): enabling the
//			ABI fails with EINVAL, through
//			prctl and through NT_ARM_TAGGED_ADDR_CTRL, a tagged access
//			faults, and the rows that never touch a tagged address in
//			user mode (the ABI off) are those of native Linux
//
// Prints the rows, then one line for each that differs, and exits with the
// number of differences.
//
// Build in the guest: c++ -std=c++20 -O2 -pthread tagged_addr_probe.cpp

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(__aarch64__)
#error "an arm64 guest probe"
#endif

#ifndef PR_SET_TAGGED_ADDR_CTRL
#define PR_SET_TAGGED_ADDR_CTRL 55
#define PR_GET_TAGGED_ADDR_CTRL 56
#define PR_TAGGED_ADDR_ENABLE (1UL << 0)
#endif
#ifndef SA_EXPOSE_TAGBITS
#define SA_EXPOSE_TAGBITS 0x00000800
#endif
#ifndef NT_ARM_TAGGED_ADDR_CTRL
#define NT_ARM_TAGGED_ADDR_CTRL 0x409
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace {

// Rows as the native arm64 oracle printed them. A row marked optional is not
// printed by a run whose engines keep the top byte, because it needs a tagged
// access in user mode or the ABI on.
struct Row {
	const char *name;
	const char *value;
	int optional;
};

constexpr Row native_rows[] = {
	{"sysctl_tagged_addr_disabled", "0", 0},
	{"get_initial", "0", 0},
	{"get_badarg", "-22", 0},
	{"set_badbit", "-22", 0},
	{"set_badarg", "-22", 0},
	{"tbi_deref_faults", "0", 1},
	{"tbi_deref_value_ok", "1", 1},
	{"off_write", "-14", 0},
	{"off_write_cross_page", "-14", 0},
	{"off_read", "-14", 0},
	{"off_read_data_ok", "0", 0},
	{"off_uname", "-14", 0},
	{"off_clock_gettime", "-14", 0},
	{"off_openat_path", "-14", 0},
	{"off_readv", "-14", 0},
	{"off_process_vm_readv", "-14", 0},
	{"off_process_vm_writev_remote_tagged", "4", 0},
	{"off_process_vm_writev_data_ok", "1", 0},
	{"off_futex_wait_mismatch", "-14", 0},
	{"off_futex_wait_timeout", "-14", 0},
	{"off_futex_wake", "-14", 0},
	{"off_rt_sigaction", "-14", 0},
	{"off_mprotect", "0", 0},
	{"off_madvise", "0", 0},
	{"off_madvise_zeroed", "0", 0},
	{"off_mincore", "0", 0},
	{"off_mlock", "0", 0},
	{"off_munlock", "0", 0},
	{"off_msync", "0", 0},
	{"off_mremap_old_tagged_ok", "1", 0},
	{"off_mremap_new_tagged", "-22", 0},
	{"off_mmap_fixed_tagged", "-12", 0},
	{"off_mmap_hint_tagged", "1", 0},
	{"off_brk_tagged_unchanged", "1", 0},
	{"off_munmap", "0", 0},
	{"off_segv_count", "1", 1},
	{"off_segv_code", "1", 1},
	{"off_segv_si_addr_minus_page", "0x0000000000000018", 1},
	{"off_segv_fault_address_minus_page", "0x0000000000000018", 1},
	{"off_expose_segv_count", "1", 1},
	{"off_expose_si_addr_minus_page", "0x0b00000000000018", 1},
	{"off_expose_fault_address_minus_page", "0x0000000000000018", 1},
	{"set_enable", "0", 0},
	{"get_after_enable", "1", 0},
	{"on_write", "15", 1},
	{"on_write_cross_page", "10", 1},
	{"on_read", "25", 1},
	{"on_read_data_ok", "1", 1},
	{"on_uname", "0", 1},
	{"on_clock_gettime", "0", 1},
	{"on_openat_path", "0", 1},
	{"on_readv", "4", 1},
	{"on_process_vm_readv", "4", 1},
	{"on_process_vm_writev_remote_tagged", "4", 1},
	{"on_process_vm_writev_data_ok", "1", 1},
	{"on_futex_wait_mismatch", "-11", 1},
	{"on_futex_wait_timeout", "-110", 1},
	{"on_futex_wake", "0", 1},
	{"on_rt_sigaction", "0", 1},
	{"on_mprotect", "0", 1},
	{"on_madvise", "0", 1},
	{"on_madvise_zeroed", "0", 1},
	{"on_mincore", "0", 1},
	{"on_mlock", "0", 1},
	{"on_munlock", "0", 1},
	{"on_msync", "0", 1},
	{"on_mremap_old_tagged_ok", "1", 1},
	{"on_mremap_new_tagged", "-22", 1},
	{"on_mmap_fixed_tagged", "-12", 1},
	{"on_mmap_hint_tagged", "1", 1},
	{"on_brk_tagged_unchanged", "1", 1},
	{"on_munmap", "0", 1},
	{"on_segv_count", "1", 1},
	{"on_segv_code", "1", 1},
	{"on_segv_si_addr_minus_page", "0x0000000000000018", 1},
	{"on_segv_fault_address_minus_page", "0x0000000000000018", 1},
	{"on_expose_segv_count", "1", 1},
	{"on_expose_si_addr_minus_page", "0x0b00000000000018", 1},
	{"on_expose_fault_address_minus_page", "0x0000000000000018", 1},
	{"fork_child_ctrl", "1", 1},
	{"exec_child_ctrl", "0", 1},
	{"ptrace_getregset", "0", 1},
	{"ptrace_getregset_value", "1", 1},
	{"ptrace_getregset_len", "8", 1},
	{"ptrace_peek_tagged", "0", 1},
	{"ptrace_peek_tagged_ok", "1", 1},
	{"ptrace_poke_tagged", "0", 1},
	{"ptrace_poke_tagged_ok", "1", 1},
	{"ptrace_setregset", "0", 1},
	{"ptrace_child_ctrl_after_set0", "0", 1},
	{"thread_ctrl_inherited", "1", 1},
	{"thread_ctrl_after_own_set0", "0", 1},
	{"thread_parent_ctrl_after", "1", 1},
	{"disable", "0", 0},
	{"write_after_disable", "-14", 1},
	{"a32_get_einval", "1", 0},
	{"a32_set_enable_einval", "1", 0},
	{"a32_set_zero_einval", "1", 0},
};

// The rows where this kernel differs from native arm64 Linux.
struct Override {
	const char *name;
	const char *value;
	const char *reason;
};

constexpr Override deviations[] = {
	{"off_expose_si_addr_minus_page", "0x0000000000000018",
	 "SA_EXPOSE_TAGBITS: the engines report the address they translated, "
	 "which has no tag"},
	{"on_expose_si_addr_minus_page", "0x0000000000000018",
	 "SA_EXPOSE_TAGBITS: the engines report the address they translated, "
	 "which has no tag"},
};

// The rows that differ when the engines keep the top byte. A tagged access
// faults at its first instruction, which the probe skips, so its store and
// its load make two faults; and the ABI cannot be enabled.
constexpr Override tbi_off_overrides[] = {
	{"tbi_deref_faults", "2", "a tagged access faults"},
	{"tbi_deref_value_ok", "0", "a tagged access faults"},
	{"set_enable", "-22", "the host keeps the top byte"},
	{"get_after_enable", "0", "the host keeps the top byte"},
};

// What a run with top-byte-ignore off prints in addition: enabling the ABI
// through a tracer fails the same way.
constexpr Row tbi_off_rows[] = {
	{"ptrace_setregset_enable", "-22", 0},
};

// What only this kernel has: the native window, and the ways a mapping near
// or above it is refused or ignored. Each row is 1 when it holds.
constexpr Row window_rows[] = {
	{"window_found", "1", 0},
	{"window_fixed_refused", "1", 0},
	{"window_fixed_edge_refused", "1", 0},
	{"window_fixed_beyond_task_enomem", "1", 0},
	{"window_hint_inside_ignored", "1", 0},
	{"window_hint_straddling_ignored", "1", 0},
	{"window_hint_beyond_task_ignored", "1", 0},
	{"window_hint_free_honored", "1", 0},
};

int failures;
bool tbi_on;
std::vector<std::pair<std::string, std::string>> rows;

void say(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	fflush(stdout);
}

void report(const std::string &name, long value)
{
	rows.emplace_back(name, std::to_string(value));
	say("%s=%ld\n", name.c_str(), value);
}

void report_hex(const std::string &name, uint64_t value)
{
	char text[32];

	snprintf(text, sizeof(text), "0x%016lx", (unsigned long)value);
	rows.emplace_back(name, text);
	say("%s=%s\n", name.c_str(), text);
}

template <typename T>
long word(T value)
{
	if constexpr (std::is_pointer_v<T>)
		return (long)(uintptr_t)value;
	else
		return (long)value;
}

// A system call straight from the kernel: its result or minus its errno, and
// the arguments exactly as given, tags included.
template <typename... Arguments>
long sys(long number, Arguments... arguments)
{
	long result = syscall(number, word(arguments)...);

	return result == -1 ? -(long)errno : result;
}

constexpr uint64_t tag_mask = 0x00ffffffffffffffUL;
constexpr uint64_t probe_tag = 0x0b;

void *tagged(const void *pointer, uint64_t tag = probe_tag)
{
	return (void *)(((uint64_t)(uintptr_t)pointer & tag_mask) | (tag << 56));
}

long get_control()
{
	long result = prctl(PR_GET_TAGGED_ADDR_CTRL, 0, 0, 0, 0);

	return result < 0 ? -(long)errno : result;
}

long set_control(unsigned long value)
{
	return prctl(PR_SET_TAGGED_ADDR_CTRL, value, 0, 0, 0) < 0 ?
		-(long)errno : 0;
}

char *map_pages(size_t length)
{
	void *address = mmap(nullptr, length, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return address == MAP_FAILED ? nullptr : (char *)address;
}

// ---- faults ----

volatile uint64_t signal_count, signal_address, signal_fault_address,
	signal_code;

void on_signal(int, siginfo_t *info, void *context)
{
	auto *frame = (ucontext_t *)context;

	signal_count = signal_count + 1;
	signal_code = (uint64_t)info->si_code;
	signal_address = (uint64_t)(uintptr_t)info->si_addr;
	signal_fault_address = frame->uc_mcontext.fault_address;
	// Skip the faulting instruction.
	frame->uc_mcontext.pc += 4;
}

bool install(int number, int extra_flags)
{
	struct sigaction action = {};

	action.sa_sigaction = on_signal;
	action.sa_flags = SA_SIGINFO | extra_flags;
	sigemptyset(&action.sa_mask);
	return sigaction(number, &action, nullptr) == 0;
}

uint64_t load_via(volatile uint64_t *pointer)
{
	uint64_t value;

	asm volatile("mov x9, #0\n\tldr x9, [%1]\n\tmov %0, x9"
		     : "=r"(value)
		     : "r"(pointer)
		     : "x9", "memory");
	return value;
}

void store_via(volatile uint64_t *pointer, uint64_t value)
{
	asm volatile("str %1, [%0]" : : "r"(pointer), "r"(value) : "memory");
}

void tbi_deref_test()
{
	auto *page = (volatile uint64_t *)map_pages(4096);

	install(SIGSEGV, 0);
	signal_count = 0;
	store_via((volatile uint64_t *)tagged((void *)page, 0x0b), 0x5a5a);
	uint64_t value = load_via((volatile uint64_t *)tagged((void *)page, 0xb4));
	report("tbi_deref_faults", (long)signal_count);
	report("tbi_deref_value_ok",
	       signal_count == 0 && value == 0x5a5a && *page == 0x5a5a);
}

void fault_tests(const std::string &prefix)
{
	char *page = map_pages(4096);

	munmap(page, 4096);
	install(SIGSEGV, 0);
	signal_count = 0;
	load_via((volatile uint64_t *)tagged(page + 0x18));
	report(prefix + "segv_count", (long)signal_count);
	report(prefix + "segv_code", (long)signal_code);
	report_hex(prefix + "segv_si_addr_minus_page",
		   signal_address - (uint64_t)(uintptr_t)page);
	report_hex(prefix + "segv_fault_address_minus_page",
		   signal_fault_address - (uint64_t)(uintptr_t)page);
	install(SIGSEGV, SA_EXPOSE_TAGBITS);
	signal_count = 0;
	load_via((volatile uint64_t *)tagged(page + 0x18));
	report(prefix + "expose_segv_count", (long)signal_count);
	report_hex(prefix + "expose_si_addr_minus_page",
		   signal_address - (uint64_t)(uintptr_t)page);
	report_hex(prefix + "expose_fault_address_minus_page",
		   signal_fault_address - (uint64_t)(uintptr_t)page);
	install(SIGSEGV, 0);
}

// ---- memory management, which untags whether or not the ABI is on ----

void mm_tests(const std::string &prefix)
{
	char *p = map_pages(4 * 4096);

	memset(p, 1, 4 * 4096);
	report(prefix + "mprotect", sys(SYS_mprotect, tagged(p), 4096, PROT_READ));
	sys(SYS_mprotect, p, 4096, PROT_READ | PROT_WRITE);
	report(prefix + "madvise",
	       sys(SYS_madvise, tagged(p + 4096), 4096, MADV_DONTNEED));
	report(prefix + "madvise_zeroed", p[4096]);
	unsigned char vec[4];

	report(prefix + "mincore", sys(SYS_mincore, tagged(p), 4 * 4096, vec));
	report(prefix + "mlock", sys(SYS_mlock, tagged(p), 4096));
	report(prefix + "munlock", sys(SYS_munlock, tagged(p), 4096));
	report(prefix + "msync", sys(SYS_msync, tagged(p), 4096, MS_SYNC));
	long r = sys(SYS_mremap, tagged(p + 2 * 4096), 4096, 4096, MREMAP_MAYMOVE,
		     0);

	report(prefix + "mremap_old_tagged_ok", r > 0 && ((uint64_t)r >> 56) == 0);
	char *q = map_pages(4096);

	r = sys(SYS_mremap, p + 3 * 4096, 4096, 4096,
		MREMAP_MAYMOVE | MREMAP_FIXED, tagged(q));
	report(prefix + "mremap_new_tagged", r < 0 ? r : 0);
	r = sys(SYS_mmap, tagged(q), 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	report(prefix + "mmap_fixed_tagged",
	       r < 0 ? r : (((uint64_t)r >> 56) ? 2 : 1));
	// A hint is only a hint: a tagged one names no place the task can map,
	// so the kernel picks one.
	r = sys(SYS_mmap, tagged(q), 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	report(prefix + "mmap_hint_tagged",
	       r < 0 ? r : (((uint64_t)r >> 56) ? 2 : 1));
	long current = sys(SYS_brk, 0);

	r = sys(SYS_brk, tagged((char *)current + 4096));
	report(prefix + "brk_tagged_unchanged", r == current);
	report(prefix + "munmap", sys(SYS_munmap, tagged(q), 4096));
	munmap(p, 4 * 4096);
}

// ---- system calls that read and write user memory ----

constexpr int futex_wait_private = 128;
constexpr int futex_wake_private = 129;

void signal_handler_for_sigaction(int) {}

void io_tests(const std::string &prefix)
{
	int fds[2];

	if (pipe(fds))
		return;
	char *buf = map_pages(8192);

	memcpy(buf, "tagged pointer", 15);
	report(prefix + "write", sys(SYS_write, fds[1], tagged(buf), 15));
	memcpy(buf + 4090, "0123456789", 10);
	report(prefix + "write_cross_page",
	       sys(SYS_write, fds[1], tagged(buf + 4090), 10));
	memset(buf + 4096 + 100, 0, 64);
	long r = sys(SYS_read, fds[0], tagged(buf + 4096 + 100), 64);

	report(prefix + "read", r);
	report(prefix + "read_data_ok",
	       r == 25 && !memcmp(buf + 4196, "tagged pointer\0" "0123456789", 25));
	char uts[390];

	report(prefix + "uname", sys(SYS_uname, tagged(uts)));
	uint64_t ts[2];

	report(prefix + "clock_gettime", sys(SYS_clock_gettime, 1, tagged(ts)));
	char *path = buf + 2048;

	memcpy(path, "/proc/self/stat", 16);
	r = sys(SYS_openat, -100, tagged(path), 0, 0);
	report(prefix + "openat_path", r < 0 ? r : 0);
	if (r >= 0)
		close((int)r);
	// An iovec array and its base, both tagged.
	auto *iov = (uint64_t *)(buf + 3000);

	sys(SYS_write, fds[1], "abcd", 4);
	iov[0] = (uint64_t)(uintptr_t)tagged(buf + 5000);
	iov[1] = 4;
	report(prefix + "readv", sys(SYS_readv, fds[0], tagged(iov), 1));
	// process_vm_readv on ourselves, with the local and the remote base
	// both tagged.
	uint64_t local[2] = {(uint64_t)(uintptr_t)tagged(buf + 6000), 4};
	uint64_t remote[2] = {(uint64_t)(uintptr_t)tagged(buf), 4};

	report(prefix + "process_vm_readv",
	       sys(SYS_process_vm_readv, getpid(), local, 1, remote, 1, 0));
	// process_vm_writev to ourselves, the local side untagged and the remote
	// side tagged. The remote address is never access_ok()-checked, so
	// native Linux takes it with the ABI off too, and once get_user_pages()
	// untags it the pages the kernel pins must be named untagged when it
	// tells the host which code to forget.
	memcpy(buf + 6100, "WXYZ", 4);
	uint64_t write_local[2] = {(uint64_t)(uintptr_t)(buf + 6100), 4};
	uint64_t write_remote[2] = {(uint64_t)(uintptr_t)tagged(buf + 6200), 4};

	report(prefix + "process_vm_writev_remote_tagged",
	       sys(SYS_process_vm_writev, getpid(), write_local, 1, write_remote,
		   1, 0));
	report(prefix + "process_vm_writev_data_ok", !memcmp(buf + 6200, "WXYZ", 4));
	// A futex word through a tagged pointer.
	auto *futex_word = (volatile uint32_t *)(buf + 7000);

	*futex_word = 5;
	report(prefix + "futex_wait_mismatch",
	       sys(SYS_futex, tagged((void *)futex_word), futex_wait_private, 4, 0,
		   0, 0));
	uint64_t timeout[2] = {0, 1000000};

	report(prefix + "futex_wait_timeout",
	       sys(SYS_futex, tagged((void *)futex_word), futex_wait_private, 5,
		   timeout, 0, 0));
	report(prefix + "futex_wake",
	       sys(SYS_futex, tagged((void *)futex_word), futex_wake_private, 1, 0,
		   0, 0));
	// rt_sigaction with a tagged act pointer.
	// The kernel's struct sigaction: handler, flags, restorer, mask.
	struct {
		void *handler;
		uint64_t flags;
		void *restorer;
		uint64_t mask;
	} kernel_action = {(void *)signal_handler_for_sigaction, 0, nullptr, 0};

	report(prefix + "rt_sigaction",
	       sys(SYS_rt_sigaction, SIGBUS, tagged(&kernel_action), 0, 8));
	close(fds[0]);
	close(fds[1]);
	munmap(buf, 8192);
}

// ---- children ----

long wait_exit(pid_t pid)
{
	int status = 0;

	if (waitpid(pid, &status, 0) < 0)
		return -(long)errno;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return 1000 + (status & 0x7f);
}

volatile uint64_t peek_target = 0x1122334455667788UL;
volatile uint64_t poke_target = 0x0102030405060708UL;
const char *self_path = "/proc/self/exe";

void process_tests()
{
	pid_t pid = fork();

	if (pid == 0)
		_exit((int)(get_control() & 0xff));
	report("fork_child_ctrl", wait_exit(pid));

	pid = fork();
	if (pid == 0) {
		const char *arguments[] = {self_path, "--exec-child", nullptr};

		execv(self_path, (char *const *)arguments);
		_exit(99);
	}
	report("exec_child_ctrl", wait_exit(pid));

	pid = fork();
	if (pid == 0) {
		sys(SYS_ptrace, 0 /* PTRACE_TRACEME */, 0, 0, 0);
		kill(getpid(), SIGSTOP);
		_exit((int)(get_control() & 0xff));
	}
	int status = 0;

	waitpid(pid, &status, 0);
	uint64_t value = 77;
	struct {
		void *base;
		size_t length;
	} vector = {&value, 8};

	report("ptrace_getregset",
	       sys(SYS_ptrace, 0x4204 /* PTRACE_GETREGSET */, pid,
		   NT_ARM_TAGGED_ADDR_CTRL, &vector));
	report("ptrace_getregset_value", (long)value);
	report("ptrace_getregset_len", (long)vector.length);
	uint64_t data = 0;

	// The tracee is a copy of this process, so the target has one address
	// in both.
	report("ptrace_peek_tagged",
	       sys(SYS_ptrace, 2 /* PTRACE_PEEKDATA */, pid,
		   tagged((const void *)&peek_target), &data));
	report("ptrace_peek_tagged_ok", data == peek_target);
	report("ptrace_poke_tagged",
	       sys(SYS_ptrace, 5 /* PTRACE_POKEDATA */, pid,
		   tagged((const void *)&poke_target), 0xa5a5a5a5a5a5a5a5UL));
	data = 0;
	sys(SYS_ptrace, 2 /* PTRACE_PEEKDATA */, pid, (const void *)&poke_target,
	    &data);
	report("ptrace_poke_tagged_ok", data == 0xa5a5a5a5a5a5a5a5UL);
	value = 0;
	vector.length = 8;
	report("ptrace_setregset",
	       sys(SYS_ptrace, 0x4205 /* PTRACE_SETREGSET */, pid,
		   NT_ARM_TAGGED_ADDR_CTRL, &vector));
	sys(SYS_ptrace, 7 /* PTRACE_CONT */, pid, 0, 0);
	report("ptrace_child_ctrl_after_set0", wait_exit(pid));
}

// With top-byte-ignore off, a tracer cannot enable the ABI either.
void tracer_refusal_test()
{
	pid_t pid = fork();

	if (pid == 0) {
		sys(SYS_ptrace, 0 /* PTRACE_TRACEME */, 0, 0, 0);
		kill(getpid(), SIGSTOP);
		_exit((int)(get_control() & 0xff));
	}
	int status = 0;

	waitpid(pid, &status, 0);
	uint64_t value = 1;
	struct {
		void *base;
		size_t length;
	} vector = {&value, 8};

	report("ptrace_setregset_enable",
	       sys(SYS_ptrace, 0x4205 /* PTRACE_SETREGSET */, pid,
		   NT_ARM_TAGGED_ADDR_CTRL, &vector));
	sys(SYS_ptrace, 7 /* PTRACE_CONT */, pid, 0, 0);
	wait_exit(pid);
}

// ---- a thread of this process ----

volatile long thread_before, thread_after;

void *thread_body(void *)
{
	thread_before = get_control();
	set_control(0);
	thread_after = get_control();
	return nullptr;
}

void thread_test()
{
	pthread_t thread;

	if (pthread_create(&thread, nullptr, thread_body, nullptr)) {
		report("thread_spawn", -1);
		return;
	}
	pthread_join(thread, nullptr);
	report("thread_ctrl_inherited", thread_before);
	report("thread_ctrl_after_own_set0", thread_after);
	report("thread_parent_ctrl_after", get_control());
}

void sysctl_test()
{
	char text[16] = "";
	int fd = open("/proc/sys/abi/tagged_addr_disabled", O_RDONLY);

	if (fd < 0) {
		report("sysctl_tagged_addr_disabled_open", -(long)errno);
		return;
	}
	ssize_t n = read(fd, text, sizeof(text));

	close(fd);
	report("sysctl_tagged_addr_disabled", n > 0 ? text[0] - '0' : -1000);
}

// ---- an AArch32 task has no tagged-address ABI ----

// _start: r8 = 0; for each of prctl(PR_GET_TAGGED_ADDR_CTRL),
// prctl(PR_SET_TAGGED_ADDR_CTRL, 1) and prctl(PR_SET_TAGGED_ADDR_CTRL, 0),
// r8 |= 1, 2 and 4 when the call returned -EINVAL; exit_group(r8).
constexpr uint32_t a32_code[] = {
	0xe3a08000, 0xe3a070ac, 0xe3a00038, 0xe3a01000, 0xe3a02000, 0xe3a03000,
	0xe3a04000, 0xef000000, 0xe3700016, 0x03888001, 0xe3a00037, 0xe3a01001,
	0xe3a02000, 0xe3a03000, 0xe3a04000, 0xef000000, 0xe3700016, 0x03888002,
	0xe3a00037, 0xe3a01000, 0xe3a02000, 0xe3a03000, 0xe3a04000, 0xef000000,
	0xe3700016, 0x03888004, 0xe1a00008, 0xe3a070f8, 0xef000000,
};
constexpr uint32_t a32_load_address = 0x10000;
constexpr uint32_t a32_code_offset = sizeof(Elf32_Ehdr) + 2 * sizeof(Elf32_Phdr);
constexpr uint32_t a32_size = a32_code_offset + sizeof(a32_code);

bool write_a32_program(const char *path)
{
	unsigned char image[a32_size] = {};
	Elf32_Ehdr header{};
	Elf32_Phdr load{}, stack{};

	memcpy(header.e_ident, ELFMAG, SELFMAG);
	header.e_ident[EI_CLASS] = ELFCLASS32;
	header.e_ident[EI_DATA] = ELFDATA2LSB;
	header.e_ident[EI_VERSION] = EV_CURRENT;
	header.e_type = ET_EXEC;
	header.e_machine = EM_ARM;
	header.e_version = EV_CURRENT;
	header.e_entry = a32_load_address + a32_code_offset;
	header.e_phoff = sizeof(header);
	header.e_flags = 0x05000000;	// EF_ARM_EABI_VER5
	header.e_ehsize = sizeof(header);
	header.e_phentsize = sizeof(Elf32_Phdr);
	header.e_phnum = 2;
	load.p_type = PT_LOAD;
	load.p_vaddr = load.p_paddr = a32_load_address;
	load.p_filesz = load.p_memsz = a32_size;
	load.p_flags = PF_R | PF_X;
	load.p_align = 0x1000;
	stack.p_type = PT_GNU_STACK;
	stack.p_flags = PF_R | PF_W;
	stack.p_align = 16;
	memcpy(image, &header, sizeof(header));
	memcpy(image + sizeof(header), &load, sizeof(load));
	memcpy(image + sizeof(header) + sizeof(load), &stack, sizeof(stack));
	memcpy(image + a32_code_offset, a32_code, sizeof(a32_code));
	FILE *file = fopen(path, "wb");

	if (!file)
		return false;
	bool written = fwrite(image, 1, sizeof(image), file) == sizeof(image);

	return fclose(file) == 0 && written && chmod(path, 0755) == 0;
}

void aarch32_test()
{
	const char *program = "/tmp/tagged-addr-a32";
	long status = -1;

	if (write_a32_program(program)) {
		pid_t pid = fork();

		if (pid == 0) {
			execl(program, program, (char *)nullptr);
			_exit(127);
		}
		status = wait_exit(pid);
	}
	report("a32_get_einval", (status & 1) != 0);
	report("a32_set_enable_einval", (status & 2) != 0);
	report("a32_set_zero_einval", (status & 4) != 0);
	unlink(program);
}

// ---- the native window of this kernel ----

// Whether mapping [start, end) at exactly those addresses is refused as
// reaching the native window. A range past the task's address space is
// ENOMEM, a range that is free is mapped and removed again, and a range
// something else holds is EEXIST.
bool refuses_window(uint64_t start, uint64_t end)
{
	long r = sys(SYS_mmap, start, end - start, PROT_NONE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
			     MAP_FIXED_NOREPLACE,
		     -1, 0);

	if (r > 0)
		sys(SYS_munmap, r, end - start);
	return r == -EINVAL;
}

// The first page of the native window, or 0 when the kernel has none.
uint64_t find_window()
{
	constexpr uint64_t low = 1UL << 32;
	constexpr uint64_t high = 1UL << 47;
	const uint64_t page = 4096;

	if (!refuses_window(low, high))
		return 0;
	// The smallest end such that [low, end) reaches the window.
	uint64_t lo = low / page, hi = high / page;

	while (hi - lo > 1) {
		uint64_t mid = lo + (hi - lo) / 2;

		if (refuses_window(low, mid * page))
			hi = mid;
		else
			lo = mid;
	}
	return (hi - 1) * page;
}

// Whether a non-fixed mapping of length bytes at a hint is made somewhere
// else, as a hint is only a hint.
bool hint_ignored(uint64_t hint, uint64_t length)
{
	long r = sys(SYS_mmap, hint, length, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (r <= 0)
		return false;
	bool other = (uint64_t)r != hint &&
		((uint64_t)r + length <= hint || (uint64_t)r >= hint + length);

	sys(SYS_munmap, r, length);
	return other;
}

void window_tests()
{
	uint64_t window = find_window();
	const uint64_t page = 4096;

	report("window_found", window != 0);
	if (!window)
		return;
	report("window_fixed_refused",
	       sys(SYS_mmap, window, page, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == -EINVAL);
	report("window_fixed_edge_refused",
	       sys(SYS_mmap, window - page, 2 * page, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == -EINVAL);
	// Past the task's range a fixed placement fails as it does natively.
	report("window_fixed_beyond_task_enomem",
	       sys(SYS_mmap, 1UL << 47, page, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == -ENOMEM);
	// A hint that would put the mapping in the window, across its first
	// page or above the task's range is ignored, never an error.
	report("window_hint_inside_ignored", hint_ignored(window + page, page));
	report("window_hint_straddling_ignored",
	       hint_ignored(window - page, 4 * page));
	report("window_hint_beyond_task_ignored",
	       hint_ignored(1UL << 47, page) &&
		       hint_ignored(0x0000ffffffffe000UL, page) &&
		       hint_ignored(0xfffffffffffff000UL, page));
	// A hint that names free memory the task can map is still honoured.
	uint64_t free_hint = 1UL << 44;

	if (free_hint <= window && free_hint + page > window)
		free_hint = 1UL << 45;
	long r = sys(SYS_mmap, free_hint, page, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	report("window_hint_free_honored", r > 0 && (uint64_t)r == free_hint);
	if (r > 0)
		sys(SYS_munmap, r, page);
}

// ---- comparison with the oracle ----

const char *expected_value(const Row &native)
{
	if (!tbi_on)
		for (const Override &override : tbi_off_overrides)
			if (!strcmp(override.name, native.name))
				return override.value;
	for (const Override &deviation : deviations)
		if (!strcmp(deviation.name, native.name))
			return deviation.value;
	return native.value;
}

void compare()
{
	std::map<std::string, std::string> seen;
	size_t matches = 0;

	for (auto &row : rows)
		seen[row.first] = row.second;
	auto expect = [&](const Row &row, const char *value) {
		auto found = seen.find(row.name);

		if (found == seen.end()) {
			say("FAIL %s: not printed, expected %s\n", row.name, value);
			failures++;
		} else if (found->second != value) {
			say("FAIL %s: %s, expected %s\n", row.name,
			    found->second.c_str(), value);
			failures++;
		} else {
			matches++;
		}
		seen.erase(row.name);
	};

	for (const Row &row : native_rows) {
		if (!tbi_on && row.optional && !seen.count(row.name))
			continue;
		expect(row, expected_value(row));
	}
	if (!tbi_on)
		for (const Row &row : tbi_off_rows)
			expect(row, row.value);
	for (const Row &row : window_rows)
		expect(row, row.value);
	for (auto &leftover : seen) {
		say("FAIL %s=%s: no such row expected\n", leftover.first.c_str(),
		    leftover.second.c_str());
		failures++;
	}
	say("%zu rows match the native arm64 oracle%s\n", matches,
	    failures ? "" : ", and nothing else printed");
}

}  // namespace

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--exec-child"))
		return (int)(get_control() & 0xff);
	if (argc != 2 || (strcmp(argv[1], "--tbi=on") && strcmp(argv[1], "--tbi=off"))) {
		fprintf(stderr, "usage: %s --tbi=on|off\n", argv[0]);
		return 2;
	}
	tbi_on = !strcmp(argv[1], "--tbi=on");

	sysctl_test();
	report("get_initial", get_control());
	report("get_badarg",
	       prctl(PR_GET_TAGGED_ADDR_CTRL, 1, 0, 0, 0) < 0 ? -(long)errno : 0);
	report("set_badbit",
	       prctl(PR_SET_TAGGED_ADDR_CTRL, 2, 0, 0, 0) < 0 ? -(long)errno : 0);
	report("set_badarg",
	       prctl(PR_SET_TAGGED_ADDR_CTRL, 1, 1, 0, 0) < 0 ? -(long)errno : 0);
	tbi_deref_test();
	io_tests("off_");
	mm_tests("off_");
	if (tbi_on)
		fault_tests("off_");
	long enabled = set_control(PR_TAGGED_ADDR_ENABLE);

	report("set_enable", enabled);
	report("get_after_enable", get_control());
	if (enabled == 0) {
		io_tests("on_");
		mm_tests("on_");
		fault_tests("on_");
		process_tests();
		thread_test();
	} else {
		tracer_refusal_test();
	}
	report("disable", set_control(0));
	if (enabled == 0) {
		char *page = map_pages(4096);

		report("write_after_disable",
		       sys(SYS_write, 1, tagged(page), 0));
	}
	aarch32_test();
	window_tests();
	compare();
	return failures;
}
