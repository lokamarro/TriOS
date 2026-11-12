// SPDX-License-Identifier: GPL-2.0-or-later
/* Forks from a busy multi-threaded x86 process, as glibc's fork() does.
 *
 * Worker threads count under a futex mutex in a loop of their own code
 * while a sibling forks. Each fork clears the parent's host mappings, so
 * the workers' next code fetches and stores fault and must find the pages
 * again, each store landing in the parent's copy. Around each fork the
 * forker holds three lock words in separate mappings, as
 * __malloc_fork_lock_parent holds the malloc arenas: after it the child
 * clears them with plain stores and the parent releases them with
 * "mov %reg, %eax; xchg %eax, (lock)", the sequence of
 * __malloc_fork_unlock_parent, whose exchange faults on the page the fork
 * write-protected and must run again with the moved value. A lock the
 * parent then still sees held, or the kernel does, is a lost release.
 *
 * Prints one line, "fork-cow: abi=... verdict=ok" when nothing was lost. */
namespace {

#if defined(__x86_64__)
using word = unsigned long;
constexpr long nr_write = 1, nr_mmap = 9, nr_clone = 56, nr_wait4 = 61,
	 nr_getppid = 110, nr_futex = 202, nr_nanosleep = 35,
	nr_clock_gettime = 228, nr_exit_group = 231;
constexpr const char *abi = "x86-64";
long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
	long a3 = 0, long a4 = 0, long a5 = 0)
{
	register long r10 __asm__("r10") = a3;
	register long r8 __asm__("r8") = a4;
	register long r9 __asm__("r9") = a5;
	long result;
	__asm__ volatile("syscall"
		: "=a"(result)
		: "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory", "cc");
	return result;
}
#else
using word = unsigned int;
constexpr long nr_write = 4, nr_mmap = 192, nr_clone = 120, nr_wait4 = 114,
	 nr_getppid = 64, nr_futex = 240, nr_nanosleep = 162,
	nr_clock_gettime = 265, nr_exit_group = 252;
constexpr const char *abi = "i386";
long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
	long a3 = 0, long a4 = 0, long a5 = 0)
{
	long result;
	__asm__ volatile("push %%ebp\n\tmov %7, %%ebp\n\tint $0x80\n\tpop %%ebp"
		: "=a"(result)
		: "a"(number), "b"(a0), "c"(a1), "d"(a2), "S"(a3), "D"(a4), "m"(a5)
		: "memory", "cc");
	return result;
}
#endif

constexpr long futex_wait_private = 128, futex_wake_private = 129;
constexpr long etimedout = 110;
constexpr long sigchld = 17;
/* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
 * CLONE_SYSVSEM */
constexpr word thread_flags = 0x00000100 | 0x00000200 | 0x00000400 |
	0x00000800 | 0x00010000 | 0x00040000;
constexpr int workers = 3;
constexpr long long run_nanoseconds = 5LL * 1000 * 1000 * 1000;
constexpr long long stall_nanoseconds = 10LL * 1000 * 1000 * 1000;
constexpr word stack_size = 64 * 1024;
constexpr word arena_size = 1 << 20;

struct Timespec final {
	long seconds;
	long nanoseconds;
};

long long now()
{
	Timespec time{};
	raw_syscall(nr_clock_gettime, 1, reinterpret_cast<long>(&time));
	return time.seconds * 1000000000LL + time.nanoseconds;
}

void sleep_milliseconds(long milliseconds)
{
	Timespec time{milliseconds / 1000, (milliseconds % 1000) * 1000000L};
	raw_syscall(nr_nanosleep, reinterpret_cast<long>(&time), 0);
}

struct alignas(4096) Shared final {
	int mutex;
	int inside;
	long counter;
};
struct alignas(64) Progress final {
	long increments;
};
/* A malloc arena's head: the lock word sits past heap_info, as there. */
struct Arena final {
	word header[6];
	word lock;
};

Shared shared;
Progress progress[workers];
Arena *arenas[3];
volatile int stop;
volatile int finished;
volatile long forks;
volatile long overlaps;
volatile long lost;

void lock()
{
	int observed = 0;
	if (__atomic_compare_exchange_n(&shared.mutex, &observed, 1, false,
		    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
		return;
	if (observed != 2)
		observed = __atomic_exchange_n(&shared.mutex, 2, __ATOMIC_SEQ_CST);
	while (observed != 0) {
		raw_syscall(nr_futex, reinterpret_cast<long>(&shared.mutex),
			futex_wait_private, 2, 0);
		observed = __atomic_exchange_n(&shared.mutex, 2, __ATOMIC_SEQ_CST);
	}
}

void unlock()
{
	if (__atomic_exchange_n(&shared.mutex, 0, __ATOMIC_SEQ_CST) == 2)
		raw_syscall(nr_futex, reinterpret_cast<long>(&shared.mutex),
			futex_wake_private, 1);
}

void worker(void *argument)
{
	auto &mine = progress[reinterpret_cast<word>(argument)];
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

/* glibc's release: the moved zero, then an exchange that may fault. */
word release(word *lock_word)
{
	word old;
#if defined(__x86_64__)
	__asm__ volatile("mov %2, %%rax\n\txchg %%rax, (%1)"
		: "=&a"(old) : "r"(lock_word), "r"(word{0}) : "memory");
#else
	__asm__ volatile("mov %2, %%eax\n\txchg %%eax, (%1)"
		: "=&a"(old) : "r"(lock_word), "r"(word{0}) : "memory");
#endif
	return old;
}

void forker(void *)
{
	while (!stop) {
		for (auto *arena : arenas) {
			word expected = 0;
			(void)__atomic_compare_exchange_n(&arena->lock, &expected, 1,
				false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
		}
		const long child = raw_syscall(nr_clone, sigchld, 0, 0, 0, 0);
		if (child == 0) {
			for (auto *arena : arenas)
				arena->lock = 0;
			raw_syscall(nr_exit_group, 0);
		}
		if (child < 0)
			break;
		for (auto *arena : arenas)
			(void)release(&arena->lock);
		for (auto *arena : arenas) {
			/* A zero timeout returns at once: ETIMEDOUT when the kernel
			 * still reads the lock as held. */
			Timespec zero{};
			const bool kernel_held = raw_syscall(nr_futex,
				reinterpret_cast<long>(&arena->lock), futex_wait_private, 1,
				reinterpret_cast<long>(&zero)) == -etimedout;
			if (kernel_held ||
			    __atomic_load_n(&arena->lock, __ATOMIC_SEQ_CST) != 0) {
				lost = lost + 1;
				arena->lock = 0;
			}
		}
		int status = 0;
		raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
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
	long length = 0;
	while (text[length] != 0)
		++length;
	raw_syscall(nr_write, 1, reinterpret_cast<long>(text), length);
}

void print(long value)
{
	char digits[24];
	int position = sizeof(digits);
	digits[--position] = 0;
	unsigned long magnitude = value < 0 ? 0UL - static_cast<unsigned long>(value) :
		static_cast<unsigned long>(value);
	do {
		digits[--position] = static_cast<char>('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude != 0);
	if (value < 0)
		digits[--position] = '-';
	print(digits + position);
}

[[noreturn]] void report(const char *verdict, int status)
{
	print("fork-cow: abi="); print(abi);
	print(" forks="); print(forks);
	print(" increments="); print(total_increments());
	print(" counter="); print(shared.counter);
	print(" overlaps="); print(overlaps);
	print(" lost="); print(lost);
	print(" verdict="); print(verdict); print("\n");
	raw_syscall(nr_exit_group, status);
	__builtin_trap();
}

} // namespace

extern "C" {
long spawn_thread(word flags, void *stack_top, void (*entry)(void *),
	void *argument);
#if defined(__x86_64__)
__asm__(".text\n"
	".global spawn_thread\n"
	"spawn_thread:\n"
	"	mov %rdx, %r8\n"
	"	mov %rcx, %r9\n"
	"	xor %edx, %edx\n"
	"	xor %r10d, %r10d\n"
	"	mov $56, %eax\n"
	"	syscall\n"
	"	test %rax, %rax\n"
	"	jnz 1f\n"
	"	xor %ebp, %ebp\n"
	"	mov %r9, %rdi\n"
	"	and $-16, %rsp\n"
	"	call *%r8\n"
	"	xor %edi, %edi\n"
	"	mov $60, %eax\n"
	"	syscall\n"
	"1:	ret\n"
	".global _start\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	and $-16, %rsp\n"
	"	call probe_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n");
#else
__asm__(".text\n"
	".global spawn_thread\n"
	"spawn_thread:\n"
	"	push %ebx\n"
	"	push %esi\n"
	"	push %edi\n"
	"	mov 16(%esp), %ebx\n"
	"	mov 20(%esp), %ecx\n"
	"	mov 24(%esp), %esi\n"
	"	mov 28(%esp), %edi\n"
	"	sub $16, %ecx\n"
	"	mov %esi, (%ecx)\n"
	"	mov %edi, 4(%ecx)\n"
	"	xor %edx, %edx\n"
	"	xor %esi, %esi\n"
	"	xor %edi, %edi\n"
	"	mov $120, %eax\n"
	"	int $0x80\n"
	"	test %eax, %eax\n"
	"	jnz 1f\n"
	"	xor %ebp, %ebp\n"
	"	mov (%esp), %eax\n"
	"	mov 4(%esp), %edx\n"
	"	sub $12, %esp\n"
	"	push %edx\n"
	"	call *%eax\n"
	"	xor %ebx, %ebx\n"
	"	mov $1, %eax\n"
	"	int $0x80\n"
	"1:	pop %edi\n"
	"	pop %esi\n"
	"	pop %ebx\n"
	"	ret\n"
	".global _start\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	and $-16, %esp\n"
	"	call probe_main\n"
	"	mov %eax, %ebx\n"
	"	mov $252, %eax\n"
	"	int $0x80\n");
#endif

void *memset(void *destination, int value, word length)
{
	auto *bytes = static_cast<unsigned char *>(destination);
	for (word index = 0; index < length; ++index)
		bytes[index] = static_cast<unsigned char>(value);
	return destination;
}

int probe_main()
{
	for (auto *&arena : arenas) {
		const long mapping = raw_syscall(nr_mmap, 0, arena_size, 3,
			0x02 | 0x20, -1, 0);
		if (mapping < 0 && mapping > -4096)
			report("setup-failed", 4);
		arena = reinterpret_cast<Arena *>(mapping);
	}
	for (word index = 0; index <= workers; ++index) {
		const long stack = raw_syscall(nr_mmap, 0, stack_size, 3,
			0x02 | 0x20, -1, 0);
		if (stack < 0 && stack > -4096)
			report("setup-failed", 4);
		const auto entry = index < static_cast<word>(workers) ? worker : forker;
		if (spawn_thread(thread_flags,
			    reinterpret_cast<void *>(stack + stack_size), entry,
			    reinterpret_cast<void *>(index)) < 0)
			report("setup-failed", 4);
	}
	const long long started = now();
	long last_progress = -1;
	long long last_change = started;
	for (;;) {
		sleep_milliseconds(100);
		const long long current = now();
		const long progressed = total_increments() + forks;
		if (progressed != last_progress) {
			last_progress = progressed;
			last_change = current;
		} else if (current - last_change >= stall_nanoseconds) {
			report("stuck", 3);
		}
		if (current - started >= run_nanoseconds)
			break;
	}
	stop = 1;
	const long long stopping = now();
	while (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) != workers + 1) {
		if (now() - stopping >= stall_nanoseconds)
			report("stuck", 3);
		sleep_milliseconds(10);
	}
	if (forks == 0 || overlaps != 0 || lost != 0 ||
	    total_increments() != shared.counter || shared.mutex != 0)
		report("fail", 2);
	report("ok", 0);
}
}
