// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest probe: what a tracer sees and changes at a system call's stops.
//
// A tracer follows a child's raw system calls with PTRACE_SYSCALL and reads
// the registers at each stop. On native Linux the first argument is in its
// register at the entry stop, and what a tracer writes there is what the call
// uses; the exit stop shows the return value; and a call with no number
// behind it (syscall(-1)) returns ENOSYS. On arm64 the first argument and
// the return value share x0, so the entry stop shows -ENOSYS in x0 for
// syscall(-1) alone; x7 (r12 of an AArch32 task) holds 0 at an entry stop and
// 1 at an exit stop and has the task's value again afterwards; and
// NT_ARM_SYSTEM_CALL reads and sets the call's number, which a tracer sets to
// -1 to skip the call. x86-64 keeps the arguments elsewhere and -ENOSYS in
// rax at every entry stop. For each of
//
//   write(77, ...)      fd 77 is closed; the tracer rewrites the first argument
//                       to a pipe at the entry stop, so the data reaches it
//   close(4242)         untouched; EBADF
//   syscall(-1)         ENOSYS
//   syscall(9999)       ENOSYS; arm64 shows the argument at its entry stop
//   getpid()            arm64: the tracer skips it and sets EPERM
//   getppid()           arm64: the tracer changes it to getpid
//   getppid(pipe, ...)  arm64: the tracer changes it to a blocking read, a
//                       signal interrupts it, and the call starts again as
//                       getppid, the call the task made, whether a handler
//                       runs or the tracer suppresses the signal
//   write(78, ...)      the tracer replaces the arguments with
//                       PTRACE_SET_SYSCALL_INFO
//   read(pipe, ...)     a signal interrupts it, the handler returns and the
//                       call restarts with its own first argument, whose entry
//                       stop shows it again
//
// the probe checks the entry stop's registers, PTRACE_GET_SYSCALL_INFO, the
// exit stop, the data and the value the child itself sees. On arm64 the same
// goes for an AArch32 child (a program of about 250 bytes the probe writes as
// an ELF file), which also makes the ARM-private cacheflush and set_tls calls,
// whose arguments are the registers at the stop.
// Prints one line per check and exits with the number of failures.
//
// Build in the guest: c++ -std=c++20 -O2 ptrace_syscall_args_probe.cpp

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/audit.h>

#include <string>

#if defined(__aarch64__)
#include <asm/ptrace.h>
#elif defined(__x86_64__)
#include <sys/user.h>
#else
#error "an arm64 or x86-64 guest probe"
#endif

namespace {

constexpr int ptrace_set_syscall_info = 0x4212;
constexpr int ptrace_get_syscall_info = 0x420e;
constexpr int syscall_info_entry = 1;
constexpr int syscall_info_exit = 2;

// struct ptrace_syscall_info of the uapi, which glibc's <sys/ptrace.h> cannot
// share a translation unit with.
struct SyscallInfo {
	uint8_t op;
	uint8_t reserved;
	uint16_t flags;
	uint32_t arch;
	uint64_t instruction_pointer;
	uint64_t stack_pointer;
	union {
		struct {
			uint64_t nr;
			uint64_t args[6];
		} entry;
		struct {
			int64_t rval;
			uint8_t is_error;
		} exit;
		uint64_t padding[9];
	};
};

constexpr uint64_t unimplemented_number = 9999;
constexpr int closed_fd = 77;
constexpr int replaced_fd = 78;
constexpr int closed_fd_to_close = 4242;
constexpr char message[] = "a64: fd 77 became the pipe!\n";
constexpr size_t message_length = sizeof(message) - 1;
static_assert(message_length == 28);

int failures;

void check(const char *name, bool passed, const char *format = nullptr, ...)
{
	char detail[256] = "";

	if (!passed && format) {
		va_list arguments;

		va_start(arguments, format);
		vsnprintf(detail, sizeof(detail), format, arguments);
		va_end(arguments);
	}
	printf("%s %s%s%s\n", passed ? "PASS" : "FAIL", name,
	       *detail ? ": " : "", detail);
	fflush(stdout);
	if (!passed)
		failures++;
}

#if defined(__aarch64__)
// How many of the child's system calls changed x7, which a call leaves alone
// whatever a tracer saw in it.
volatile long x7_clobbered;
#endif

// One system call from the child with no libc between it and the kernel.
inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0)
{
#if defined(__aarch64__)
	register long x8 asm("x8") = number;
	register long x0 asm("x0") = a0;
	register long x1 asm("x1") = a1;
	register long x2 asm("x2") = a2;
	register long x7 asm("x7") = 0x77;

	asm volatile("svc #0"
		     : "+r"(x0), "+r"(x7)
		     : "r"(x8), "r"(x1), "r"(x2)
		     : "memory");
	if (x7 != 0x77)
		x7_clobbered = x7_clobbered + 1;
	return x0;
#else
	long result;

	asm volatile("syscall"
		     : "=a"(result)
		     : "a"(number), "D"(a0), "S"(a1), "d"(a2)
		     : "rcx", "r11", "memory");
	return result;
#endif
}

// The registers a tracer reads at a stop, whatever the child's ABI, and the
// ABI's ways to write them.
enum class Abi { native, aarch32 };

struct Stop {
	bool entry = false;
	uint64_t nr = 0;	// the call's number as the tracer reads it
	uint64_t args[3] = {};	// the registers that hold the first arguments
	uint64_t ret = 0;	// the register that holds the return value
	uint64_t marker = 0;	// x7, r12 of an AArch32 task
	SyscallInfo info{};
};

#if defined(__aarch64__)
using NativeRegs = user_pt_regs;
#else
using NativeRegs = user_regs_struct;
#endif
struct Aarch32Regs {
	uint32_t r[18];
};

bool get_native(pid_t pid, NativeRegs &regs)
{
	iovec vector = {&regs, sizeof(regs)};

	return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &vector) == 0;
}

bool set_native(pid_t pid, NativeRegs &regs)
{
	iovec vector = {&regs, sizeof(regs)};

	return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &vector) == 0;
}

bool get_aarch32(pid_t pid, Aarch32Regs &regs)
{
	iovec vector = {&regs, sizeof(regs)};

	return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &vector) == 0 &&
	       vector.iov_len == sizeof(regs);
}

bool set_aarch32(pid_t pid, Aarch32Regs &regs)
{
	iovec vector = {&regs, sizeof(regs)};

	return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &vector) == 0;
}

// The first argument's register, rewritten at a stop.
bool set_first_argument(pid_t pid, Abi abi, uint64_t value)
{
	if (abi == Abi::aarch32) {
		Aarch32Regs regs;

		if (!get_aarch32(pid, regs))
			return false;
		regs.r[0] = (uint32_t)value;
		return set_aarch32(pid, regs);
	}
	NativeRegs regs;

	if (!get_native(pid, regs))
		return false;
#if defined(__aarch64__)
	regs.regs[0] = value;
#else
	regs.rdi = value;
#endif
	return set_native(pid, regs);
}

bool read_stop(pid_t pid, Abi abi, Stop &stop)
{
	if (abi == Abi::aarch32) {
		Aarch32Regs regs;

		if (!get_aarch32(pid, regs))
			return false;
		// A 32-bit register is a signed int: -ENOSYS reads as one.
		stop.nr = (int64_t)(int32_t)regs.r[7];
		for (int i = 0; i < 3; i++)
			stop.args[i] = regs.r[i];
		stop.ret = regs.r[0];
		stop.marker = regs.r[12];
	} else {
		NativeRegs regs;

		if (!get_native(pid, regs))
			return false;
#if defined(__aarch64__)
		stop.nr = regs.regs[8];
		stop.args[0] = regs.regs[0];
		stop.args[1] = regs.regs[1];
		stop.args[2] = regs.regs[2];
		stop.ret = regs.regs[0];
		stop.marker = regs.regs[7];
#else
		stop.nr = regs.orig_rax;
		stop.args[0] = regs.rdi;
		stop.args[1] = regs.rsi;
		stop.args[2] = regs.rdx;
		stop.ret = regs.rax;
#endif
	}
	return true;
}

bool read_info(pid_t pid, SyscallInfo &info)
{
	memset(&info, 0, sizeof(info));
	return ptrace((__ptrace_request)ptrace_get_syscall_info, pid,
		      (void *)sizeof(info), &info) > 0;
}

#if defined(__aarch64__)
// How many system call stops the tracer looked at, and how many of them
// lacked their marker in x7 (r12 of an AArch32 task).
int markers_seen;
int markers_wrong;

constexpr int nt_arm_system_call = 0x404;

// NT_ARM_SYSTEM_CALL, the number of the call the task is in.
bool get_system_call(pid_t pid, int &number)
{
	iovec vector = {&number, sizeof(number)};

	return ptrace(PTRACE_GETREGSET, pid, (void *)(long)nt_arm_system_call,
		      &vector) == 0 && vector.iov_len == sizeof(number);
}

bool set_system_call(pid_t pid, int number)
{
	iovec vector = {&number, sizeof(number)};

	return ptrace(PTRACE_SETREGSET, pid, (void *)(long)nt_arm_system_call,
		      &vector) == 0;
}
#endif

// The next system call stop. A stop of any other kind comes back with
// *status filled in and false.
bool resume(pid_t pid, Abi abi, Stop &stop, int signal = 0, int *status = nullptr)
{
	int wait_status = 0;

	if (ptrace(PTRACE_SYSCALL, pid, nullptr, (void *)(long)signal) != 0)
		return false;
	if (waitpid(pid, &wait_status, __WALL) != pid) {
		wait_status = -1;
	}
	if (status)
		*status = wait_status;
	if (!WIFSTOPPED(wait_status) ||
	    WSTOPSIG(wait_status) != (SIGTRAP | 0x80))
		return false;
	stop = Stop();
	if (!read_info(pid, stop.info) || !read_stop(pid, abi, stop))
		return false;
	stop.entry = stop.info.op == syscall_info_entry;
#if defined(__aarch64__)
	markers_seen++;
	if (stop.marker != (stop.entry ? 0 : 1))
		markers_wrong++;
#endif
	return true;
}

#if defined(__aarch64__)
constexpr bool first_argument_is_return = true;
#else
constexpr bool first_argument_is_return = false;
#endif
constexpr uint64_t minus_enosys = (uint64_t)-ENOSYS;
constexpr uint64_t minus_ebadf = (uint64_t)-EBADF;
// The kernel-internal ERESTARTSYS, which an interrupted call returns.
constexpr uint64_t minus_erestartsys = (uint64_t)-512;

std::string describe(const Stop &stop)
{
	char text[160];

	snprintf(text, sizeof(text),
		 "%s nr=%#llx a0=%#llx a1=%#llx a2=%#llx ret=%#llx op=%u",
		 stop.entry ? "entry" : "exit", (unsigned long long)stop.nr,
		 (unsigned long long)stop.args[0],
		 (unsigned long long)stop.args[1],
		 (unsigned long long)stop.args[2], (unsigned long long)stop.ret,
		 stop.info.op);
	return text;
}

// Whether the stop is the entry or exit of the call, else a failure that ends
// the scenario.
bool expect(const char *what, const Stop &stop, bool entry, uint64_t nr)
{
	bool matched = stop.entry == entry && stop.info.op ==
		(entry ? syscall_info_entry : syscall_info_exit) &&
		stop.nr == nr;

	if (!matched) {
		std::string seen = describe(stop);

		check(what, false, "%s, wanted nr %#llx", seen.c_str(),
		      (unsigned long long)nr);
	}
	return matched;
}

struct Shared {
	long write_result;
	long close_result;
	long nosys_result;
	long unimplemented_result;
	long skipped_result;
	long swapped_result;
	long restart_swapped_result[2];
	char swap_byte;
	long replaced_result;
	long read_result;
	long x7_clobbered;
	char read_byte;
};

Shared *shared_state;
int data_pipe[2];
int restart_pipe[2];
int swap_pipe[2];

void on_signal(int)
{
}

[[noreturn]] void native_child()
{
	struct sigaction action{};

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	action.sa_handler = on_signal;
	action.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &action, nullptr);
	close(data_pipe[0]);
	close(restart_pipe[1]);
	close(swap_pipe[1]);
	ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
	raise(SIGSTOP);

	shared_state->write_result =
		raw_syscall(SYS_write, closed_fd, (long)message, message_length);
	shared_state->close_result = raw_syscall(SYS_close, closed_fd_to_close);
	shared_state->nosys_result = raw_syscall(-1);
	shared_state->unimplemented_result =
		raw_syscall(unimplemented_number, 7);
	shared_state->skipped_result = raw_syscall(SYS_getpid, 7);
	shared_state->swapped_result = raw_syscall(SYS_getppid);
	for (long &result : shared_state->restart_swapped_result)
		result = raw_syscall(SYS_getppid, swap_pipe[0],
				     (long)&shared_state->swap_byte, 1);
	shared_state->replaced_result =
		raw_syscall(SYS_write, replaced_fd, (long)message, message_length);
	shared_state->read_result =
		raw_syscall(SYS_read, restart_pipe[0], (long)&shared_state->read_byte, 1);
#if defined(__aarch64__)
	shared_state->x7_clobbered = x7_clobbered;
#endif
	raw_syscall(SYS_exit_group, 0);
	for (;;)
		;
}

// The bytes the child wrote into the data pipe since the last call.
std::string drain_data_pipe()
{
	char buffer[256];
	ssize_t length;

	fcntl(data_pipe[0], F_SETFL, O_NONBLOCK);
	length = read(data_pipe[0], buffer, sizeof(buffer));
	return length > 0 ? std::string(buffer, length) : std::string();
}

void kill_child(pid_t pid)
{
	int status;

	kill(pid, SIGKILL);
	waitpid(pid, &status, __WALL);
}

#if defined(__aarch64__)
// A call a tracer changed to a blocking read(pipe) is interrupted by SIGUSR1
// and starts again.  Natively the register that holds the number still has
// what the task wrote, so the restart shows the call the task made at its
// entry stop and runs it, not the call the tracer put in its place.  deliver
// has the child's handler run; otherwise the tracer suppresses the signal.
bool check_swapped_restart(const char *prefix, pid_t pid, Abi abi, int original,
			   int read_number, uint64_t pipe_fd, bool deliver)
{
	char name[128];
	Stop stop;
	int wait_status = 0;
	int signal = deliver ? SIGUSR1 : 0;
	bool restarted = false;
	const uint64_t restart_code = abi == Abi::aarch32 ?
		(uint64_t)(uint32_t)-512 : minus_erestartsys;

	auto label = [&](const char *text) {
		snprintf(name, sizeof(name), "%s %s", prefix, text);
		return name;
	};

	if (!resume(pid, abi, stop) ||
	    !expect(label("swapped call entry"), stop, true, original))
		return false;
	check(label("a tracer changes the call to a blocking read"),
	      set_system_call(pid, read_number), "%s", strerror(errno));
	if (abi == Abi::aarch32)
		check(label("a tracer gives the read its descriptor"),
		      set_first_argument(pid, abi, pipe_fd), "%s", strerror(errno));
	// The signal is pending when the read starts, so it returns
	// -ERESTARTSYS at once.
	kill(pid, SIGUSR1);
	if (!resume(pid, abi, stop) ||
	    !expect(label("interrupted swapped call exit"), stop, false, original))
		return false;
	check(label("the interrupted call's exit stop shows -ERESTARTSYS"),
	      stop.ret == restart_code, "%s", describe(stop).c_str());
	ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
	waitpid(pid, &wait_status, __WALL);
	if (!WIFSTOPPED(wait_status) || WSTOPSIG(wait_status) != SIGUSR1) {
		check(label("the signal stops the child"), false, "status %#x",
		      wait_status);
		return false;
	}
	for (int step = 0; step < 8 && !restarted; step++) {
		if (!resume(pid, abi, stop, signal))
			break;
		signal = 0;
		restarted = stop.entry &&
			    (stop.nr == (uint64_t)original || stop.nr == (uint64_t)read_number);
	}
	check(label("the interrupted call starts again"), restarted, "%s",
	      describe(stop).c_str());
	if (!restarted)
		return false;
	check(label("the restarted entry stop shows the call the task made"),
	      stop.nr == (uint64_t)original, "%s", describe(stop).c_str());
	if (abi == Abi::native)
		check(label("the restarted entry stop shows the descriptor"),
		      stop.args[0] == pipe_fd, "%s", describe(stop).c_str());
	if (!resume(pid, abi, stop) ||
	    !expect(label("restarted call exit"), stop, false, original))
		return false;
	check(label("the restarted call ran as the call the task made"),
	      (abi == Abi::aarch32 ? (uint32_t)stop.ret : stop.ret) ==
	      (abi == Abi::aarch32 ? (uint32_t)getpid() : (uint64_t)getpid()),
	      "%s", describe(stop).c_str());
	return true;
}
#endif

void check_native()
{
	const char *abi = "A64";
#if defined(__x86_64__)
	abi = "x86-64";
#endif
	char name[96];
	Stop stop;
	int status = 0;
	pid_t pid;
	uint64_t pipe_write;

	auto label = [&](const char *text) {
		snprintf(name, sizeof(name), "%s %s", abi, text);
		return name;
	};

	shared_state = (Shared *)mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
				      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared_state == MAP_FAILED || pipe(data_pipe) || pipe(restart_pipe) ||
	    pipe(swap_pipe)) {
		check(label("set up"), false, "%s", strerror(errno));
		return;
	}
	memset(shared_state, 0, sizeof(*shared_state));
	pipe_write = data_pipe[1];
	pid = fork();
	if (pid == 0)
		native_child();
	waitpid(pid, &status, __WALL);
	if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
		check(label("child stops itself"), false, "status %#x", status);
		return;
	}
	ptrace(PTRACE_SETOPTIONS, pid, nullptr,
	       (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));

	// write(77, ...): the tracer sends it to the pipe.
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("write entry"), stop, true, SYS_write))
		return kill_child(pid);
	check(label("write entry stop shows the first argument"),
	      stop.args[0] == closed_fd, "%s", describe(stop).c_str());
	check(label("write entry stop shows the other arguments"),
	      stop.args[1] == (uint64_t)message && stop.args[2] == message_length,
	      "%s", describe(stop).c_str());
	if (!first_argument_is_return)
		check(label("write entry stop shows -ENOSYS as the result"),
		      stop.ret == minus_enosys, "%s", describe(stop).c_str());
	check(label("write entry: PTRACE_GET_SYSCALL_INFO names the argument"),
	      stop.info.entry.args[0] == closed_fd, "%s",
	      describe(stop).c_str());
	// The child inherited the pipe's write end at the same number.
	check(label("write entry: a tracer rewrites the first argument"),
	      set_first_argument(pid, Abi::native, pipe_write), "%s",
	      strerror(errno));
	Stop rewritten;
	check(label("write entry: PTRACE_GET_SYSCALL_INFO after the rewrite"),
	      read_info(pid, rewritten.info) &&
	      rewritten.info.entry.args[0] ==
	      (first_argument_is_return ? (uint64_t)closed_fd : pipe_write),
	      "%#llx", (unsigned long long)rewritten.info.entry.args[0]);
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("write exit"), stop, false, SYS_write))
		return kill_child(pid);
	check(label("write exit stop shows the return value"),
	      stop.ret == message_length && stop.info.exit.rval == (int64_t)message_length &&
	      !stop.info.exit.is_error, "%s", describe(stop).c_str());
	check(label("the rewritten argument reached the pipe"),
	      drain_data_pipe() == message);

	// close(4242): untouched.
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("close entry"), stop, true, SYS_close))
		return kill_child(pid);
	check(label("close entry stop shows the first argument"),
	      stop.args[0] == closed_fd_to_close, "%s", describe(stop).c_str());
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("close exit"), stop, false, SYS_close))
		return kill_child(pid);
	check(label("close exit stop shows -EBADF"),
	      stop.ret == minus_ebadf && stop.info.exit.is_error &&
	      stop.info.exit.rval == -EBADF, "%s", describe(stop).c_str());

	// syscall(-1): there is no such call.
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("syscall(-1) entry"), stop, true, (uint64_t)-1))
		return kill_child(pid);
	check(label("syscall(-1) entry stop shows -ENOSYS"),
	      stop.ret == minus_enosys, "%s", describe(stop).c_str());
	check(label("syscall(-1): PTRACE_GET_SYSCALL_INFO names -1"),
	      stop.info.entry.nr == (uint64_t)-1, "%#llx",
	      (unsigned long long)stop.info.entry.nr);
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("syscall(-1) exit"), stop, false, (uint64_t)-1))
		return kill_child(pid);
	check(label("syscall(-1) exit stop shows -ENOSYS"),
	      stop.ret == minus_enosys && stop.info.exit.is_error, "%s",
	      describe(stop).c_str());

	// A number past the table.
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("unimplemented entry"), stop, true, unimplemented_number))
		return kill_child(pid);
	check(label("unimplemented entry stop shows the first argument or -ENOSYS"),
	      stop.ret == (first_argument_is_return ? 7 : minus_enosys), "%s",
	      describe(stop).c_str());
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("unimplemented exit"), stop, false, unimplemented_number))
		return kill_child(pid);
	check(label("unimplemented exit stop shows -ENOSYS"),
	      stop.ret == minus_enosys, "%s", describe(stop).c_str());

	// getpid(7): on arm64 the tracer skips it and sets its result.
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("getpid entry"), stop, true, SYS_getpid))
		return kill_child(pid);
#if defined(__aarch64__)
	{
		int number = 0;

		check(label("NT_ARM_SYSTEM_CALL reads the number of the call"),
		      get_system_call(pid, number) && number == SYS_getpid, "%d",
		      number);
		check(label("a tracer sets the number of the call to -1"),
		      set_system_call(pid, -1), "%s", strerror(errno));
		check(label("NT_ARM_SYSTEM_CALL reads back -1"),
		      get_system_call(pid, number) && number == -1, "%d", number);
		Stop after;
		check(label("PTRACE_GET_SYSCALL_INFO names -1 after the change"),
		      read_info(pid, after.info) &&
		      after.info.entry.nr == (uint64_t)-1, "%#llx",
		      (unsigned long long)after.info.entry.nr);
		check(label("a tracer sets the result of the skipped call"),
		      set_first_argument(pid, Abi::native, (uint64_t)-EPERM), "%s",
		      strerror(errno));
	}
#endif
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("getpid exit"), stop, false, SYS_getpid))
		return kill_child(pid);
#if defined(__aarch64__)
	check(label("a skipped call's exit stop shows what the tracer wrote"),
	      stop.ret == (uint64_t)-EPERM, "%s", describe(stop).c_str());
#endif

	// getppid(): on arm64 the tracer changes it to getpid.
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("getppid entry"), stop, true, SYS_getppid))
		return kill_child(pid);
#if defined(__aarch64__)
	check(label("a tracer sets the number of the call to another call"),
	      set_system_call(pid, SYS_getpid), "%s", strerror(errno));
#endif
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("getppid exit"), stop, false, SYS_getppid))
		return kill_child(pid);
#if defined(__aarch64__)
	check(label("the other call ran: the exit stop shows the child's pid"),
	      stop.ret == (uint64_t)pid, "%s pid %d", describe(stop).c_str(),
	      (int)pid);
	// getppid(pipe, ...) changed to a blocking read and interrupted: once
	// with the child's handler running, once with the signal suppressed.
	if (!check_swapped_restart("A64 handler", pid, Abi::native, SYS_getppid,
				   SYS_read, swap_pipe[0], true) ||
	    !check_swapped_restart("A64 suppressed", pid, Abi::native,
				   SYS_getppid, SYS_read, swap_pipe[0], false))
		return kill_child(pid);
#else
	// The same calls with nothing changed: getppid twice.
	for (int call = 0; call < 2; call++) {
		if (!resume(pid, Abi::native, stop) ||
		    !expect(label("getppid entry"), stop, true, SYS_getppid) ||
		    !resume(pid, Abi::native, stop) ||
		    !expect(label("getppid exit"), stop, false, SYS_getppid))
			return kill_child(pid);
	}
#endif

	// PTRACE_SET_SYSCALL_INFO replaces write(78, ...) by a write to the pipe.
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("replaced write entry"), stop, true, SYS_write))
		return kill_child(pid);
	{
		SyscallInfo info = stop.info;
		long result;

		info.entry.args[0] = pipe_write;
		result = ptrace((__ptrace_request)ptrace_set_syscall_info, pid,
				(void *)sizeof(info), &info);
		check(label("PTRACE_SET_SYSCALL_INFO replaces the entry arguments"),
		      result == 0, "%s", strerror(errno));
		Stop after;
		check(label("the entry stop shows the replaced arguments"),
		      read_stop(pid, Abi::native, after) &&
		      after.args[0] == pipe_write && read_info(pid, after.info) &&
		      after.info.entry.args[0] == pipe_write, "%#llx %#llx",
		      (unsigned long long)after.args[0],
		      (unsigned long long)after.info.entry.args[0]);
	}
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("replaced write exit"), stop, false, SYS_write))
		return kill_child(pid);
	check(label("the replaced arguments reached the pipe"),
	      stop.ret == message_length && drain_data_pipe() == message,
	      "%s", describe(stop).c_str());

	// read(): interrupted, restarted by a SA_RESTART handler.
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("read entry"), stop, true, SYS_read))
		return kill_child(pid);
	check(label("read entry stop shows the descriptor"),
	      stop.args[0] == (uint64_t)restart_pipe[0], "%s", describe(stop).c_str());
	// The signal is pending when the call starts, so the call returns
	// -ERESTARTSYS at once.
	kill(pid, SIGUSR1);
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("interrupted read exit"), stop, false, SYS_read))
		return kill_child(pid);
	check(label("the interrupted read's exit stop shows -ERESTARTSYS"),
	      stop.ret == minus_erestartsys, "%s", describe(stop).c_str());
	{
		int wait_status = 0;
		int signal = SIGUSR1;
		bool restarted = false;

		// The signal stop follows the exit stop.
		ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr);
		waitpid(pid, &wait_status, __WALL);
		if (!WIFSTOPPED(wait_status) || WSTOPSIG(wait_status) != SIGUSR1) {
			check(label("the signal stops the child"), false,
			      "status %#x", wait_status);
			return kill_child(pid);
		}
		check(label("the signal stops the child"), true);
		// Delivered, the handler runs and returns through
		// rt_sigreturn, a call of its own, and the interrupted call
		// starts again.
		for (int step = 0; step < 6 && !restarted; step++) {
			if (!resume(pid, Abi::native, stop, signal))
				break;
			signal = 0;
			restarted = stop.entry && stop.nr == SYS_read;
		}
		check(label("the interrupted read starts again"), restarted, "%s",
		      describe(stop).c_str());
		if (!restarted)
			return kill_child(pid);
	}
	check(label("the restarted read's entry stop shows the descriptor"),
	      stop.args[0] == (uint64_t)restart_pipe[0], "%s", describe(stop).c_str());
	{
		char byte = 'R';

		// The data is there before the call restarts, so it returns.
		if (write(restart_pipe[1], &byte, 1) != 1)
			check(label("feed the restarted read"), false);
	}
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("restarted read exit"), stop, false, SYS_read))
		return kill_child(pid);
	check(label("the restarted read exit stop shows its result"),
	      stop.ret == 1, "%s", describe(stop).c_str());

	// exit_group; then the child is gone.
	if (!resume(pid, Abi::native, stop) ||
	    !expect(label("exit_group entry"), stop, true, SYS_exit_group))
		return kill_child(pid);
	ptrace(PTRACE_CONT, pid, nullptr, nullptr);
	waitpid(pid, &status, __WALL);
	check(label("the child exits"), WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "status %#x", status);
	check(label("write returned the length to the child"),
	      shared_state->write_result == (long)message_length, "%ld",
	      shared_state->write_result);
	check(label("close returned -EBADF to the child"),
	      shared_state->close_result == -EBADF, "%ld",
	      shared_state->close_result);
	check(label("syscall(-1) returned -ENOSYS to the child"),
	      shared_state->nosys_result == -ENOSYS, "%ld",
	      shared_state->nosys_result);
	check(label("an unimplemented call returned -ENOSYS to the child"),
	      shared_state->unimplemented_result == -ENOSYS, "%ld",
	      shared_state->unimplemented_result);
#if defined(__aarch64__)
	check(label("the skipped call returned the tracer's result to the child"),
	      shared_state->skipped_result == -EPERM, "%ld",
	      shared_state->skipped_result);
	check(label("the other call returned the child's pid to the child"),
	      shared_state->swapped_result == (long)pid, "%ld, pid %d",
	      shared_state->swapped_result, (int)pid);
	for (long result : shared_state->restart_swapped_result)
		check(label("the restarted call returned the parent's pid to the child"),
		      result == (long)getpid(), "%ld, parent %d", result,
		      (int)getpid());
	check(label("the child's x7 survives every stop"),
	      shared_state->x7_clobbered == 0, "%ld changes",
	      shared_state->x7_clobbered);
#endif
	check(label("the replaced write returned the length to the child"),
	      shared_state->replaced_result == (long)message_length, "%ld",
	      shared_state->replaced_result);
	check(label("the restarted read returned the byte to the child"),
	      shared_state->read_result == 1 && shared_state->read_byte == 'R',
	      "%ld %d", shared_state->read_result, shared_state->read_byte);
}

#if defined(__aarch64__)

// The AArch32 child: a static ARM EABI program with one segment, the
// assembly below, whose exit status says which of its checks held. Its
// message is the 28 bytes at the end.
//
//	mov r8, #0 ; movw r5, #0x5678 ; movt r5, #0x1234 ; mov r12, #0x77
//	write(77, msg, 28)                  ; r8 |= 1 if it returned 28
//	                                    ; r8 |= 64 if r12 is still 0x77
//	close(4242)                         ; r8 |= 2 if it returned -EBADF
//	syscall(-1) with r0 = 7             ; r8 |= 4 if it returned -ENOSYS
//	syscall(9999) with r0 = 7           ; r8 |= 8 if it returned -ENOSYS
//	getpid() with r0 = 7                ; r8 |= 128 if it returned -EPERM
//	getppid()
//	cacheflush(0x10000, 0x10100, 0)     ; r8 |= 16 if it returned 0
//	set_tls(0x12345678), mrc TPIDRURO   ; r8 |= 32 if it reads the value
//	exit_group(r8)
constexpr uint32_t a32_code[] = {
	0xe3a08000, 0xe3055678, 0xe3415234, 0xe3a0c077, 0xe3a0004d,
	0xe28f10c8, 0xe3a0201c, 0xe3a07004, 0xef000000, 0xe350001c,
	0x03888001, 0xe35c0077, 0x03888040, 0xe3010092, 0xe3a07006,
	0xef000000, 0xe3700009, 0x03888002, 0xe3a00007, 0xe3e07000,
	0xef000000, 0xe3700026, 0x03888004, 0xe3a00007, 0xe302770f,
	0xef000000, 0xe3700026, 0x03888008, 0xe3a00007, 0xe3a07014,
	0xef000000, 0xe3700001, 0x03888080, 0xe3a07040, 0xef000000,
	0xe3a02001, 0xe3a07040, 0xef000000, 0xe3000000, 0xe3400001,
	0xe2801c01, 0xe3a02000, 0xe3007002, 0xe340700f, 0xef000000,
	0xe3500000, 0x03888010, 0xe1a00005, 0xe3007005, 0xe340700f,
	0xef000000, 0xee1d1f70, 0xe1510005, 0x03888020, 0xe1a00008,
	0xe3a070f8, 0xef000000,
};
constexpr char a32_message[] = "a32: fd 77 became the pipe!\n";
constexpr uint32_t a32_checks = 255;
constexpr uint32_t a32_load_address = 0x10000;
constexpr uint32_t a32_code_offset = sizeof(Elf32_Ehdr) + 2 * sizeof(Elf32_Phdr);
constexpr uint32_t a32_message_address =
	a32_load_address + a32_code_offset + sizeof(a32_code);
constexpr uint32_t a32_size = a32_code_offset + sizeof(a32_code) + sizeof(a32_message) - 1;

bool write_a32_program(const char *path)
{
	unsigned char image[a32_size] = {};
	Elf32_Ehdr header{};
	Elf32_Phdr load{}, stack{};
	FILE *file;

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
	memcpy(image + a32_code_offset + sizeof(a32_code), a32_message,
	       sizeof(a32_message) - 1);
	file = fopen(path, "wb");
	if (!file)
		return false;
	bool written = fwrite(image, 1, sizeof(image), file) == sizeof(image);

	return fclose(file) == 0 && written && chmod(path, 0755) == 0;
}

void check_aarch32()
{
	const char *program = "/tmp/ptrace-syscall-args-a32";
	char name[96];
	Stop stop;
	int status = 0;
	pid_t pid;

	auto label = [&](const char *text) {
		snprintf(name, sizeof(name), "A32 %s", text);
		return name;
	};

	if (!write_a32_program(program) || pipe(data_pipe)) {
		check(label("set up"), false, "%s", strerror(errno));
		return;
	}
	pid = fork();
	if (pid == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		close(data_pipe[0]);
		ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
		execl(program, program, (char *)nullptr);
		_exit(127);
	}
	waitpid(pid, &status, __WALL);
	if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
		check(label("exec stops the child"), false, "status %#x", status);
		return;
	}
	ptrace(PTRACE_SETOPTIONS, pid, nullptr,
	       (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));
	const uint64_t pipe_write = data_pipe[1];

	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("write entry"), stop, true, 4))
		return kill_child(pid);
	check(label("PTRACE_GET_SYSCALL_INFO names AUDIT_ARCH_ARM"),
	      stop.info.arch == AUDIT_ARCH_ARM, "%#x", stop.info.arch);
	check(label("write entry stop shows the first argument"),
	      stop.args[0] == closed_fd, "%s", describe(stop).c_str());
	check(label("write entry stop shows the other arguments"),
	      stop.args[1] == a32_message_address && stop.args[2] == message_length,
	      "%s", describe(stop).c_str());
	check(label("write entry: PTRACE_GET_SYSCALL_INFO names the argument"),
	      stop.info.entry.args[0] == closed_fd, "%s", describe(stop).c_str());
	check(label("write entry: a tracer rewrites r0"),
	      set_first_argument(pid, Abi::aarch32, pipe_write), "%s",
	      strerror(errno));
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("write exit"), stop, false, 4))
		return kill_child(pid);
	check(label("write exit stop shows the return value"),
	      stop.ret == message_length && stop.info.exit.rval == (int64_t)message_length,
	      "%s", describe(stop).c_str());
	check(label("the rewritten argument reached the pipe"),
	      drain_data_pipe() == a32_message);

	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("close entry"), stop, true, 6))
		return kill_child(pid);
	check(label("close entry stop shows the first argument"),
	      stop.args[0] == closed_fd_to_close, "%s", describe(stop).c_str());
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("close exit"), stop, false, 6))
		return kill_child(pid);
	check(label("close exit stop shows -EBADF"),
	      (uint32_t)stop.ret == (uint32_t)-EBADF && stop.info.exit.is_error,
	      "%s", describe(stop).c_str());

	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("syscall(-1) entry"), stop, true, (uint64_t)-1))
		return kill_child(pid);
	check(label("syscall(-1) entry stop shows -ENOSYS"),
	      (uint32_t)stop.ret == (uint32_t)-ENOSYS, "%s", describe(stop).c_str());
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("syscall(-1) exit"), stop, false, (uint64_t)-1))
		return kill_child(pid);
	check(label("syscall(-1) exit stop shows -ENOSYS"),
	      (uint32_t)stop.ret == (uint32_t)-ENOSYS, "%s", describe(stop).c_str());

	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("unimplemented entry"), stop, true, unimplemented_number))
		return kill_child(pid);
	check(label("unimplemented entry stop shows the first argument"),
	      stop.ret == 7, "%s", describe(stop).c_str());
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("unimplemented exit"), stop, false, unimplemented_number))
		return kill_child(pid);
	check(label("unimplemented exit stop shows -ENOSYS"),
	      (uint32_t)stop.ret == (uint32_t)-ENOSYS, "%s", describe(stop).c_str());

	// getpid(7): the tracer skips it and sets its result.
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("getpid entry"), stop, true, 20))
		return kill_child(pid);
	{
		int number = 0;

		check(label("NT_ARM_SYSTEM_CALL reads the number of the call"),
		      get_system_call(pid, number) && number == 20, "%d", number);
		check(label("a tracer sets the number of the call to -1"),
		      set_system_call(pid, -1), "%s", strerror(errno));
		check(label("NT_ARM_SYSTEM_CALL reads back -1"),
		      get_system_call(pid, number) && number == -1, "%d", number);
		Stop after;
		check(label("PTRACE_GET_SYSCALL_INFO names -1 after the change"),
		      read_info(pid, after.info) &&
		      after.info.entry.nr == (uint64_t)-1, "%#llx",
		      (unsigned long long)after.info.entry.nr);
		check(label("a tracer sets the result of the skipped call"),
		      set_first_argument(pid, Abi::aarch32, (uint32_t)-EPERM), "%s",
		      strerror(errno));
	}
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("getpid exit"), stop, false, 20))
		return kill_child(pid);
	check(label("a skipped call's exit stop shows what the tracer wrote"),
	      (uint32_t)stop.ret == (uint32_t)-EPERM, "%s", describe(stop).c_str());

	// getppid(): the tracer changes it to getpid.
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("getppid entry"), stop, true, 64))
		return kill_child(pid);
	check(label("a tracer sets the number of the call to another call"),
	      set_system_call(pid, 20), "%s", strerror(errno));
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("getppid exit"), stop, false, 64))
		return kill_child(pid);
	check(label("the other call ran: the exit stop shows the child's pid"),
	      stop.ret == (uint32_t)pid, "%s pid %d", describe(stop).c_str(),
	      (int)pid);
	// getppid(...) changed to a blocking read, interrupted, signal suppressed.
	if (!check_swapped_restart("A32 suppressed", pid, Abi::aarch32, 64, 3,
				   swap_pipe[0], false))
		return kill_child(pid);

	// ARM's private calls take r0-r2 as they stand at the stop.
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("cacheflush entry"), stop, true, 0xf0002))
		return kill_child(pid);
	check(label("cacheflush entry stop shows its range"),
	      stop.args[0] == a32_load_address &&
	      stop.args[1] == a32_load_address + 0x100 && stop.args[2] == 0,
	      "%s", describe(stop).c_str());
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("cacheflush exit"), stop, false, 0xf0002))
		return kill_child(pid);
	check(label("cacheflush exit stop shows 0"), stop.ret == 0, "%s",
	      describe(stop).c_str());
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("set_tls entry"), stop, true, 0xf0005))
		return kill_child(pid);
	check(label("set_tls entry stop shows the value"),
	      stop.args[0] == 0x12345678, "%s", describe(stop).c_str());
	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("set_tls exit"), stop, false, 0xf0005))
		return kill_child(pid);

	if (!resume(pid, Abi::aarch32, stop) ||
	    !expect(label("exit_group entry"), stop, true, 248))
		return kill_child(pid);
	check(label("exit_group entry stop shows the child's checks"),
	      stop.args[0] == a32_checks, "%s", describe(stop).c_str());
	ptrace(PTRACE_CONT, pid, nullptr, nullptr);
	waitpid(pid, &status, __WALL);
	check(label("the child saw every result"),
	      WIFEXITED(status) && WEXITSTATUS(status) == a32_checks,
	      "status %#x", status);
	unlink(program);
}

#endif

}  // namespace

int main()
{
	alarm(200);
	check_native();
#if defined(__aarch64__)
	check_aarch32();
	check("x7 (r12) is 0 at an entry stop and 1 at an exit stop",
	      markers_seen > 0 && markers_wrong == 0, "%d of %d stops",
	      markers_wrong, markers_seen);
#endif
	printf("%d failure(s)\n", failures);
	return failures;
}
