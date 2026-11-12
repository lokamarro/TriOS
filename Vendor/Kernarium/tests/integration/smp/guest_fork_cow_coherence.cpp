// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding arm64/Linux guest workload: threads of one process contend on a
 * futex mutex and count under it while a sibling forks without pause.
 *
 * Every fork write-protects the page that holds the mutex and the counter, the
 * next store breaks the copy-on-write, and a contended lock makes the kernel
 * read the mutex word through uaccess on the same page at the same time.  A
 * host which lets any of those leave a thread on the page Linux replaced loses
 * stores: increments made under the lock go missing, or an unlock does and
 * every thread sleeps on a mutex nobody owns.  Both are reported.
 */

namespace {

using u64 = unsigned long;

constexpr long nr_getppid = 173;
constexpr long nr_write = 64;
constexpr long nr_exit_group = 94;
constexpr long nr_futex = 98;
constexpr long nr_nanosleep = 101;
constexpr long nr_clock_gettime = 113;
constexpr long nr_clone = 220;
constexpr long nr_mmap = 222;
constexpr long nr_wait4 = 260;

constexpr long futex_wait = 0;
constexpr long futex_wake = 1;
constexpr u64 clone_thread_flags =
	0x00000100 | 0x00000200 | 0x00000400 | 0x00000800 | 0x00010000 |
	0x00040000; /* VM, FS, FILES, SIGHAND, THREAD, SYSVSEM */
constexpr long sigchld = 17;

constexpr int workers = 8;
constexpr long run_nanoseconds = 20L * 1000 * 1000 * 1000;
constexpr long stall_nanoseconds = 8L * 1000 * 1000 * 1000;
constexpr u64 stack_size = 256 * 1024;

inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
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
			 : "=r"(x0)
			 : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4),
			   "r"(x5)
			 : "memory", "cc");
	return x0;
}

struct Timespec {
	long seconds;
	long nanoseconds;
};

long now()
{
	Timespec time{};
	raw_syscall(nr_clock_gettime, 1, reinterpret_cast<long>(&time));
	return time.seconds * 1000000000L + time.nanoseconds;
}

void sleep_milliseconds(long milliseconds)
{
	Timespec time{milliseconds / 1000, (milliseconds % 1000) * 1000000L};
	raw_syscall(nr_nanosleep, reinterpret_cast<long>(&time), 0);
}

/* The mutex, the counter it guards and the overlap detector share one page,
 * so a copy-on-write of any of them moves all three. */
struct alignas(16384) Shared {
	int mutex;
	int inside;
	long counter;
};

struct alignas(64) Progress {
	long increments;
};

Shared shared;
Progress progress[workers];
volatile int stop;
volatile long forks;
volatile long overlaps;
volatile int finished;

void lock()
{
	int observed = 0;
	if (__atomic_compare_exchange_n(&shared.mutex, &observed, 1, false,
					__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
		return;
	if (observed != 2)
		observed = __atomic_exchange_n(&shared.mutex, 2,
					       __ATOMIC_SEQ_CST);
	while (observed != 0) {
		raw_syscall(nr_futex, reinterpret_cast<long>(&shared.mutex),
			    futex_wait, 2, 0);
		observed = __atomic_exchange_n(&shared.mutex, 2,
					       __ATOMIC_SEQ_CST);
	}
}

void unlock()
{
	if (__atomic_fetch_sub(&shared.mutex, 1, __ATOMIC_SEQ_CST) != 1) {
		__atomic_store_n(&shared.mutex, 0, __ATOMIC_SEQ_CST);
		raw_syscall(nr_futex, reinterpret_cast<long>(&shared.mutex),
			    futex_wake, 1);
	}
}

void worker(void *argument)
{
	auto &mine = progress[reinterpret_cast<u64>(argument)];
	while (!stop) {
		lock();
		if (shared.inside++ != 0)
			__atomic_fetch_add(&overlaps, 1, __ATOMIC_SEQ_CST);
		shared.counter = shared.counter + 1;
		--shared.inside;
		unlock();
		if ((++mine.increments & 15) == 0)
			raw_syscall(nr_getppid);
	}
	__atomic_fetch_add(&finished, 1, __ATOMIC_SEQ_CST);
}

void forker(void *)
{
	while (!stop) {
		const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
		if (child == 0)
			raw_syscall(nr_exit_group, 0);
		if (child < 0)
			break;
		int status = 0;
		raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0,
			    0);
		forks = forks + 1;
	}
	__atomic_fetch_add(&finished, 1, __ATOMIC_SEQ_CST);
}

long total_increments()
{
	long total = 0;
	for (const auto &entry : progress)
		total += entry.increments;
	return total;
}

void print(const char *text)
{
	u64 length = 0;
	while (text[length] != 0)
		++length;
	raw_syscall(nr_write, 1, reinterpret_cast<long>(text),
		    static_cast<long>(length));
}

void print(long value)
{
	char digits[24];
	int position = sizeof(digits);
	const bool negative = value < 0;
	u64 magnitude = negative ? 0 - static_cast<u64>(value) :
				   static_cast<u64>(value);
	digits[--position] = 0;
	do {
		digits[--position] = static_cast<char>('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude != 0);
	if (negative)
		digits[--position] = '-';
	print(digits + position);
}

[[noreturn]] void report(const char *verdict, int status)
{
	print(verdict);
	print(" forks=");
	print(forks);
	print(" increments=");
	print(total_increments());
	print(" counter=");
	print(shared.counter);
	print(" overlaps=");
	print(overlaps);
	print(" mutex=");
	print(shared.mutex);
	print("\n");
	raw_syscall(nr_exit_group, status);
	__builtin_trap();
}

} // namespace

extern "C" {

long guest_spawn_thread(u64 flags, void *stack_top, void (*entry)(void *),
			void *argument);

/* clone() returns twice on two stacks, which no compiled frame survives. */
__asm__(".text\n"
	".global guest_spawn_thread\n"
	".type guest_spawn_thread,%function\n"
	"guest_spawn_thread:\n"
	"	mov x9, x2\n"
	"	mov x10, x3\n"
	"	mov x2, #0\n"
	"	mov x3, #0\n"
	"	mov x4, #0\n"
	"	mov x8, #220\n"
	"	svc #0\n"
	"	cbnz x0, 1f\n"
	"	mov x29, #0\n"
	"	mov x30, #0\n"
	"	mov x0, x10\n"
	"	blr x9\n"
	"	mov x0, #0\n"
	"	mov x8, #93\n"
	"	svc #0\n"
	"1:	ret\n"
	".size guest_spawn_thread,.-guest_spawn_thread\n"
	".global _start\n"
	".type _start,%function\n"
	"_start:\n"
	"	mov x29, #0\n"
	"	mov x30, #0\n"
	"	bl guest_main\n"
	"	mov x8, #94\n"
	"	svc #0\n"
	".size _start,.-_start\n");

void *memset(void *destination, int value, u64 length)
{
	auto *bytes = static_cast<unsigned char *>(destination);
	for (u64 index = 0; index < length; ++index)
		bytes[index] = static_cast<unsigned char>(value);
	return destination;
}

int guest_main()
{
	for (u64 index = 0; index <= workers; ++index) {
		const long stack = raw_syscall(nr_mmap, 0, stack_size, 3,
					       0x02 | 0x20, -1, 0);
		if (stack < 0 && stack > -4096)
			report("FORK-COW SETUP-FAILED", 4);
		const auto entry = index < workers ? worker : forker;
		if (guest_spawn_thread(clone_thread_flags,
				       reinterpret_cast<void *>(stack + stack_size),
				       entry, reinterpret_cast<void *>(index)) < 0)
			report("FORK-COW SETUP-FAILED", 4);
	}

	const long started = now();
	long last_total = -1;
	long last_change = started;
	for (;;) {
		sleep_milliseconds(250);
		const long current = now();
		const long total = total_increments();
		if (total != last_total) {
			last_total = total;
			last_change = current;
		} else if (current - last_change >= stall_nanoseconds) {
			report("FORK-COW STUCK", 3);
		}
		if (current - started >= run_nanoseconds)
			break;
	}
	stop = 1;
	const long stopping = now();
	while (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) != workers + 1) {
		if (now() - stopping >= stall_nanoseconds)
			report("FORK-COW STUCK", 3);
		sleep_milliseconds(50);
	}
	if (overlaps != 0 || total_increments() != shared.counter ||
	    shared.mutex != 0)
		report("FORK-COW FAIL", 2);
	report("FORK-COW OK", 0);
}

} // extern "C"
