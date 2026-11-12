// SPDX-License-Identifier: GPL-2.0-or-later
/* The AArch32 engine probe: a freestanding armhf program that runs as the
 * guest's /init (or on its own under qemu-user) and checks what an engine
 * that executes AArch32 must get right where it meets the system: system
 * calls and errno, A32 and T32 interworking, signals and their VFP state
 * through the sigpage and SA_RESTORER, every kuser helper, the TLS registers,
 * cacheflush with self-modifying code, SIGBUS and SIGSEGV faults, the
 * instructions that must raise SIGILL, breakpoints and single steps under
 * ptrace, exclusives across threads, and NEON and VFP state. Instruction
 * semantics are the corpus's job; this is the engine in a task.
 *
 * Each group runs in a forked process and reports PASS and FAIL lines, so a
 * group that dies is a failure and the next one still runs. The program
 * counts the SIGILLs it expects (instructions the profile makes UNDEFINED)
 * and reports any other as a failure; run_guest_a32_engine.py checks the
 * totals. It is C++ compiled for a soft-float ARM target, so the compiler
 * emits no VFP or NEON; the groups that need them use assembly in their own
 * functions.
 *
 * Environment of the init process (kernel command-line words):
 *   A32_PROBE_GROUPS  comma-separated group names to run; default every group
 */
#include "engine_probe.hpp"

extern "C" {
void *memcpy(void *destination, const void *source, unsigned long length)
{
	auto *to = static_cast<unsigned char *>(destination);
	auto *from = static_cast<const unsigned char *>(source);
	while (length--)
		*to++ = *from++;
	return destination;
}

void *memmove(void *destination, const void *source, unsigned long length)
{
	auto *to = static_cast<unsigned char *>(destination);
	auto *from = static_cast<const unsigned char *>(source);
	if (to < from) {
		while (length--)
			*to++ = *from++;
	} else {
		to += length;
		from += length;
		while (length--)
			*--to = *--from;
	}
	return destination;
}

void *memset(void *destination, int value, unsigned long length)
{
	auto *to = static_cast<unsigned char *>(destination);
	while (length--)
		*to++ = static_cast<unsigned char>(value);
	return destination;
}

int memcmp(const void *a, const void *b, unsigned long length)
{
	auto *left = static_cast<const unsigned char *>(a);
	auto *right = static_cast<const unsigned char *>(b);
	for (; length; --length, ++left, ++right)
		if (*left != *right)
			return *left < *right ? -1 : 1;
	return 0;
}
}

namespace probe {

Stats stats;
const char *group_name = "";
word hwcap;
word hwcap2;
word page_size = 4096;
static char **environment_block;

/* ---- output ---- */

namespace {

char buffer[512];
unsigned buffered;

void flush()
{
	if (buffered)
		sys(nr_write, 1, addr(buffer), buffered);
	buffered = 0;
}

void emit(const char *text, word length)
{
	if (buffered + length > sizeof(buffer))
		flush();
	if (length > sizeof(buffer)) {
		sys(nr_write, 1, addr(text), length);
		return;
	}
	memcpy(buffer + buffered, text, length);
	buffered += length;
}

} // namespace

word strlen(const char *text)
{
	word length = 0;
	while (text[length])
		++length;
	return length;
}

bool streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

void put(const char *text)
{
	emit(text, strlen(text));
}

void put_dec(u32 value)
{
	char digits[12];
	unsigned count = 0;
	do {
		digits[count++] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value);
	char text[12];
	for (unsigned index = 0; index < count; ++index)
		text[index] = digits[count - 1 - index];
	emit(text, count);
}

void put_hex(u32 value)
{
	char text[10] = {'0', 'x'};
	for (unsigned index = 0; index < 8; ++index)
		text[2 + index] = "0123456789abcdef"[(value >> (28 - 4 * index)) & 15];
	emit(text, 10);
}

void line(const char *a, const char *b, const char *c)
{
	put(a);
	if (b)
		put(b);
	if (c)
		put(c);
	put("\n");
	flush();
}

static void report(bool passed, const char *name)
{
	++stats.checks;
	if (!passed)
		++stats.failures;
	put(passed ? "PASS " : "FAIL ");
	put(group_name);
	put("/");
	put(name);
}

void check(bool passed, const char *name)
{
	report(passed, name);
	put("\n");
	flush();
}

void check_eq(u32 got, u32 want, const char *name)
{
	report(got == want, name);
	if (got != want) {
		put(": got ");
		put_hex(got);
		put(" want ");
		put_hex(want);
	}
	put("\n");
	flush();
}

void check_eq64(u64 got, u64 want, const char *name)
{
	report(got == want, name);
	if (got != want) {
		put(": got ");
		put_hex(static_cast<u32>(got >> 32));
		put_hex(static_cast<u32>(got));
		put(" want ");
		put_hex(static_cast<u32>(want >> 32));
		put_hex(static_cast<u32>(want));
	}
	put("\n");
	flush();
}

void check_mem(const void *got, const void *want, u32 length, const char *name)
{
	const bool same = memcmp(got, want, length) == 0;
	report(same, name);
	if (!same) {
		const auto *a = static_cast<const u8 *>(got);
		const auto *b = static_cast<const u8 *>(want);
		u32 index = 0;
		while (a[index] == b[index])
			++index;
		put(": differs at byte ");
		put_dec(index);
		put(": got ");
		put_hex(a[index]);
		put(" want ");
		put_hex(b[index]);
	}
	put("\n");
	flush();
}

void quit(long status)
{
	flush();
	restore_swp();
	for (;;)
		sys(nr_exit_group, status);
}

/* ---- the environment ---- */

const char *environment(const char *name)
{
	const word length = strlen(name);
	for (char **entry = environment_block; entry && *entry; ++entry) {
		const char *text = *entry;
		word index = 0;
		while (index < length && text[index] == name[index])
			++index;
		if (index == length && text[length] == '=')
			return text + length + 1;
	}
	return nullptr;
}

bool find_mapping(const char *name, word *start, word *end)
{
	const long fd = sys(nr_openat, -100, addr("/proc/self/maps"), 0);
	if (fd < 0)
		return false;
	const word capacity = 1 << 17;
	char *text = ptr<char>(mmap_pages(capacity, prot_read | prot_write));
	long length = 0;
	for (;;) {
		const long got = sys(nr_read, fd, addr(text) + length, capacity - 1 - length);
		if (got <= 0)
			break;
		length += got;
	}
	sys(nr_close, fd);
	text[length] = 0;
	bool found = false;
	for (char *cursor = text; *cursor && !found;) {
		char *eol = cursor;
		while (*eol && *eol != '\n')
			++eol;
		const char saved = *eol;
		*eol = 0;
		const word line_length = static_cast<word>(eol - cursor);
		const word name_length = strlen(name);
		if (line_length >= name_length && streq(eol - name_length, name)) {
			word values[2] = {0, 0};
			char *p = cursor;
			for (int part = 0; part < 2; ++part) {
				while (*p && *p != '-' && *p != ' ') {
					const char c = *p++;
					values[part] = values[part] * 16 +
						(c <= '9' ? c - '0' : c - 'a' + 10);
				}
				if (*p == '-')
					++p;
			}
			*start = values[0];
			*end = values[1];
			found = true;
		}
		*eol = saved;
		cursor = *eol ? eol + 1 : eol;
	}
	sys(nr_munmap, addr(text), capacity);
	return found;
}

extern "C" long a32_clone(long (*function)(void *), void *stack_top, word flags,
			   void *argument, word tls, i32 *parent_tid, i32 *child_tid);

bool thread_start(Thread &thread, long (*function)(void *), void *argument, word tls)
{
	thread.stack_size = 1 << 16;
	thread.stack = mmap_pages(thread.stack_size, prot_read | prot_write);
	if (thread.stack > static_cast<word>(-4096))
		return false;
	thread.alive = 1;
	thread.tid = 0;
	const word flags = clone_vm | clone_fs | clone_files | clone_sighand | clone_thread |
		clone_sysvsem | clone_child_cleartid | clone_parent_settid | (tls ? clone_settls : 0);
	const long result = a32_clone(function, ptr<void>(thread.stack + thread.stack_size - 16), flags,
				      argument, tls, reinterpret_cast<i32 *>(&thread.tid),
				      const_cast<i32 *>(&thread.alive));
	if (result < 0) {
		sys(nr_munmap, thread.stack, thread.stack_size);
		return false;
	}
	return true;
}

bool thread_join(Thread &thread, u32 seconds)
{
	for (u32 waited = 0; thread.alive;) {
		const i32 seen = thread.alive;
		struct {
			long seconds;
			long nanoseconds;
		} timeout{1, 0};
		sys(nr_futex, addr(&thread.alive), 0 /* FUTEX_WAIT */, seen, addr(&timeout));
		if (thread.alive && ++waited >= seconds)
			return false;
	}
	sys(nr_munmap, thread.stack, thread.stack_size);
	return true;
}

bool under_qemu()
{
	return environment("A32_PROBE_QEMU") != nullptr;
}

/* ---- abi.swp ---- */

namespace {
/* The value abi.swp had before this process wrote it, and the process. */
long swp_saved = -1;
long swp_writer;

bool write_swp(int value)
{
	const long fd = sys(nr_openat, -100, addr("/proc/sys/abi/swp"), 1);
	if (fd < 0)
		return false;
	const char text[2] = {static_cast<char>('0' + value), '\n'};
	const long written = sys(nr_write, fd, addr(text), 2);
	sys(nr_close, fd);
	return written == 2;
}
} // namespace

extern "C" u32 sw_word(u32 *p, u32 value, u32 sentinel);

long swp_setting()
{
	const long fd = sys(nr_openat, -100, addr("/proc/sys/abi/swp"), 0);
	if (fd == -eacces || fd == -eperm) {
		/* A shell may not even read the file (Android's SELinux policy), so
		 * ask a child: it exits 0 if its SWP ran and dies of SIGILL if not. */
		static u32 word = 0;
		const long child = fork_child();
		if (child == 0)
			quit(sw_word(&word, 1, 2) == 0 ? 0 : 1);
		const long status = wait_child(child);
		if (exited_with(status, 0))
			return 1;
		return killed_by(status, sigill) ? 0 : -einval;
	}
	if (fd < 0)
		return fd;
	char text[4] = {};
	const long length = sys(nr_read, fd, addr(text), 3);
	sys(nr_close, fd);
	if (length < 1)
		return -einval;
	return (text[0] == '0' || text[0] == '1') ? text[0] - '0' : -einval;
}

bool swp_set(int value)
{
	const long before = swp_setting();
	if (before < 0 || !write_swp(value))
		return false;
	if (swp_saved < 0) {
		swp_saved = before;
		swp_writer = getpid();
	}
	return true;
}

void restore_swp()
{
	if (swp_saved < 0 || getpid() != swp_writer)
		return;
	const long wanted = swp_saved;
	swp_saved = -1;
	write_swp(static_cast<int>(wanted));
}

/* ---- helpers ---- */

word mmap_pages(word length, word protection, word flags)
{
	return static_cast<word>(sys(nr_mmap2, 0, length, protection, flags,
				     -1, 0));
}

long set_handler(int signal, void (*handler)(int, SigInfo *, UContext *),
		 word flags, word restorer)
{
	KernelSigaction action{};
	action.handler = addr(handler);
	action.flags = flags | (restorer ? sa_restorer : 0);
	action.restorer = restorer;
	return sys(nr_rt_sigaction, signal, addr(&action), 0, 8);
}

long reset_handler(int signal)
{
	KernelSigaction action{};
	return sys(nr_rt_sigaction, signal, addr(&action), 0, 8);
}

long block_signal(int signal, bool block)
{
	SigSet set{};
	set.bits[(signal - 1) / 32] = 1U << ((signal - 1) % 32);
	return sys(nr_rt_sigprocmask, block ? 0 : 1, addr(&set), 0, 8);
}

long fork_child()
{
	return sys(nr_clone, sigchld, 0, 0, 0, 0);
}

long wait_child(long pid)
{
	i32 status = 0;
	const long result = sys(nr_wait4, pid, addr(&status), 0, 0);
	return result < 0 ? result : status;
}

bool exited_with(long status, int code)
{
	return status >= 0 && (status & 0x7f) == 0 && ((status >> 8) & 0xff) == code;
}

bool killed_by(long status, int signal)
{
	return status >= 0 && (status & 0x7f) == signal;
}

long getpid()
{
	return sys(nr_getpid);
}

long gettid()
{
	return sys(nr_gettid);
}

long raise_signal(int signal)
{
	return sys(nr_tgkill, getpid(), gettid(), signal);
}

void yield()
{
	sys(nr_sched_yield);
}

void sleep_ms(u32 ms)
{
	struct {
		long seconds;
		long nanoseconds;
	} request{static_cast<long>(ms / 1000), static_cast<long>(ms % 1000) * 1000000};
	sys(nr_nanosleep, addr(&request), 0);
}

Caught caught;
static word catch_skip;

static void catch_handler(int signo, SigInfo *info, UContext *context)
{
	++caught.count;
	caught.signo = signo;
	caught.code = info->code;
	caught.address = info->address;
	caught.pc = context->mcontext.pc;
	caught.cpsr = context->mcontext.cpsr;
	caught.fault_address = context->mcontext.fault_address;
	if (signo == sigill)
		++stats.sigill_expected;
	if (catch_skip == return_to_lr) {
		const word target = context->mcontext.lr;
		context->mcontext.pc = target & ~1UL;
		context->mcontext.cpsr = (target & 1) ? (context->mcontext.cpsr | (1U << 5)) :
							(context->mcontext.cpsr & ~(1U << 5));
		return;
	}
	context->mcontext.pc += catch_skip;
}

void catch_signal(int signal, word skip)
{
	catch_skip = skip;
	caught = {};
	set_handler(signal, catch_handler);
}

void restore_sigill_handler();

void release_signal(int signal)
{
	if (signal == sigill)
		restore_sigill_handler();
	else
		reset_handler(signal);
}

} // namespace probe


namespace probe {
namespace {

/* ---- the process: setup, groups, the summary ---- */

struct Shared {
	Stats stats[64];
};
Shared *shared;
unsigned current_group;
constexpr u32 group_timeout_seconds = 300;

void unexpected_sigill(int, SigInfo *info, UContext *context)
{
	(void)info;
	++stats.sigill_unexpected;
	++stats.failures;
	++stats.checks;
	put("FAIL ");
	put(group_name);
	put("/an unexpected SIGILL at pc ");
	put_hex(context->mcontext.pc);
	put(" cpsr ");
	put_hex(context->mcontext.cpsr);
	put("\n");
	flush();
	/* The group cannot go on, but the totals it reached still count. */
	shared->stats[current_group] = stats;
	quit(stats.failures > 90 ? 90 : static_cast<long>(stats.failures));
}

void run_group(const Group &group, unsigned index)
{
	flush();
	const long pid = fork_child();
	if (pid < 0) {
		put("FAIL ");
		put(group.name);
		put("/fork\n");
		++stats.failures;
		++stats.checks;
		return;
	}
	if (pid == 0) {
		stats = {};
		group_name = group.name;
		current_group = index;
		set_handler(sigill, unexpected_sigill);
		group.run();
		flush();
		shared->stats[index] = stats;
		quit(stats.failures > 90 ? 90 : static_cast<long>(stats.failures));
	}
	/* A group that hangs is a failure, not the end of the run. */
	long status = 0;
	for (u32 polls = 0;; ++polls) {
		i32 raw = 0;
		const long done = sys(nr_wait4, pid, addr(&raw), 1 /* WNOHANG */, 0);
		if (done == pid) {
			status = raw;
			break;
		}
		if (done < 0 && done != 0) {
			status = done;
			break;
		}
		if (polls >= group_timeout_seconds * 20) {
			sys(nr_kill, pid, 9);
			wait_child(pid);
			++stats.checks;
			++stats.failures;
			put("FAIL ");
			put(group.name);
			put("/the group did not finish in ");
			put_dec(group_timeout_seconds);
			put(" seconds\n");
			flush();
			return;
		}
		sleep_ms(50);
	}
	if (status < 0 || (status & 0x7f)) {
		const int signal = status < 0 ? 0 : (status & 0x7f);
		++stats.checks;
		++stats.failures;
		put("FAIL ");
		put(group.name);
		put("/the group died of signal ");
		put_dec(static_cast<u32>(signal));
		put("\n");
		if (signal == sigill)
			++stats.sigill_unexpected;
		flush();
		return;
	}
	const Stats &child = shared->stats[index];
	stats.checks += child.checks;
	stats.failures += child.failures;
	stats.sigill_expected += child.sigill_expected;
	stats.sigill_unexpected += child.sigill_unexpected;
	if (!exited_with(status, child.failures > 90 ? 90 : static_cast<int>(child.failures))) {
		++stats.checks;
		++stats.failures;
		put("FAIL ");
		put(group.name);
		put("/the group's exit status is not its failure count\n");
		flush();
	}
}

bool wanted(const char *list, const char *name)
{
	if (!list)
		return true;
	const word length = strlen(name);
	for (const char *cursor = list; *cursor;) {
		word index = 0;
		while (index < length && cursor[index] == name[index])
			++index;
		if (index == length && (cursor[length] == ',' || cursor[length] == 0))
			return true;
		while (*cursor && *cursor != ',')
			++cursor;
		if (*cursor == ',')
			++cursor;
	}
	return false;
}

void prepare_init()
{
	/* Only the guest's /init has to bring up its own console and /proc. */
	if (getpid() != 1)
		return;
	sys(nr_mount, addr("devtmpfs"), addr("/dev"), addr("devtmpfs"), 0, 0);
	const long console = sys(nr_openat, -100, addr("/dev/console"), 2);
	if (console >= 0) {
		sys(nr_dup2, console, 1);
		sys(nr_dup2, console, 2);
		sys(nr_dup2, console, 0);
		if (console > 2)
			sys(nr_close, console);
	}
	sys(nr_mount, addr("proc"), addr("/proc"), addr("proc"), 0, 0);
}

[[noreturn]] void finish()
{
	flush();
	if (getpid() == 1) {
		sys(nr_reboot, 0xfee1dead, 672274793, 0x4321fedc /* RB_POWER_OFF */);
		for (;;)
			sys(nr_sched_yield);
	}
	quit(stats.failures > 90 ? 90 : static_cast<long>(stats.failures));
}

} // namespace

void restore_sigill_handler()
{
	set_handler(sigill, unexpected_sigill);
}

void end_group()
{
	shared->stats[current_group] = stats;
	quit(stats.failures > 90 ? 90 : static_cast<long>(stats.failures));
}

extern "C" [[noreturn, gnu::used]] void probe_main(word *stack)
{
	const word argc = stack[0];
	char **argv = reinterpret_cast<char **>(stack + 1);
	environment_block = argv + argc + 1;
	char **entry = environment_block;
	while (*entry)
		++entry;
	for (const word *vector = reinterpret_cast<word *>(entry + 1); vector[0];
	     vector += 2) {
		if (vector[0] == 6)
			page_size = vector[1];
		else if (vector[0] == 16)
			hwcap = vector[1];
		else if (vector[0] == 26)
			hwcap2 = vector[1];
	}
	prepare_init();
	group_name = "probe";

	shared = ptr<Shared>(mmap_pages(sizeof(Shared), prot_read | prot_write,
					map_shared | map_anonymous));
	if (addr(shared) > static_cast<word>(-4096)) {
		line("FAIL probe/shared memory");
		++stats.failures;
		finish();
	}

	const char *list = environment("A32_PROBE_GROUPS");
	for (unsigned index = 0; index < group_count && index < 64; ++index) {
		const Group &group = groups[index];
		if (!wanted(list, group.name))
			continue;
		run_group(group, index);
	}
	put("a32-engine-probe: ");
	put_dec(stats.failures);
	put(" failure(s) in ");
	put_dec(stats.checks);
	put(" check(s), sigill expected ");
	put_dec(stats.sigill_expected);
	put(" unexpected ");
	put_dec(stats.sigill_unexpected);
	put("\n");
	finish();
}

} // namespace probe
