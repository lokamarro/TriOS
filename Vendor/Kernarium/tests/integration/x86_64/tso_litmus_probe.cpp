// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding x86-64 and i386 Linux guest probe: litmus tests of x86's
 * memory order between agents that run at once on different vCPUs.
 *
 *   tso-litmus-probe CASE ROUNDS
 *
 * CASE says what the agents are and how they share memory:
 *
 *   thread  threads of one process, sharing its globals;
 *   shm     processes, each attaching one SysV shm segment after fork();
 *   memfd   processes, each mapping one memfd MAP_SHARED after fork();
 *   anon    processes forked after their parent mapped shared anonymous
 *           memory, which they inherit.
 *
 * and the cycles, whose sharing ends and starts again, in two shared
 * phases with an unshared one between them and after them:
 *
 *   thread-cycle  each test's agents are tasks on the probe's address space
 *                 (clone(CLONE_VM) without CLONE_THREAD), joined with
 *                 wait4(), so the space is shared from the first clone()
 *                 of a test to the last wait4() and unshared between tests
 *                 and phases;
 *   shm-cycle     a phase attaches the shm segment, forks the agents, which
 *                 inherit it, and detaches it at its end;
 *   anon-cycle    the same with shared anonymous memory, mapped at the
 *                 start of a phase and unmapped at its end.
 *
 * Each case runs ROUNDS rounds of each test, in each phase of a cycle, and
 * prints one line per test:
 *
 *   mp       message passing: the writer stores x, then y; the reader sees
 *            y, then reads x. x == 0 after y == 1 is forbidden, by a store
 *            the writer reordered or by a load the reader did.
 *   mp-stack-load, mp-stack-store  (threads only) the same with x and y in
 *            the reader's or the writer's own stack frame, which that
 *            thread reaches through RSP and the other through a pointer.
 *   sb       store buffering: each of two agents stores its own variable
 *            and loads the other's. Both loads seeing 0 is allowed on x86;
 *            the count only shows that the agents raced.
 *   sb-mfence  the same with MFENCE between the store and the load, which
 *            forbids both loads seeing 0. A translator emits the fence
 *            whatever order it keeps otherwise, so only a broken MFENCE
 *            fails this one.
 *   iriw     independent reads of independent writes: two writers, two
 *            readers that see the variables become 1 in opposite orders.
 *            x86 stores have one global order, so that is forbidden. Each
 *            reader loads both variables on every pass of its wait, which
 *            gives an unordered second load the chance to run first.
 *
 * Each round uses its own pair of cache lines, x and y in one guest page so
 * that no software-TLB miss falls between two of the accesses, and primes the
 * line each reordering needs: the reader's copy of x, which a reordered load
 * reads stale, and the writer's copy of y, which a reordered store makes
 * visible before x. Exits 0 when no test saw a forbidden outcome and 1
 * otherwise; errors exit 2.
 *
 * First the probe prints its code range and the return addresses of the
 * system calls that start a cycle's sharing (the clone() of an agent task,
 * or the shmat() or mmap()) and end it (the wait4(), or the shmdt() or
 * munmap()), which the host's UML_X86_MM_SHARED_LOG names.
 */

namespace {
struct Job;
} // namespace

extern "C" {
extern const char __ehdr_start[];
extern const char etext[];
extern const char clone_return[];
extern const char share_return[];
extern const char unshare_return[];
long clone_agent(void *stack_top, void (*function)(Job *), Job *job, long flags,
	volatile int *tid);
/* System calls that return to share_return and unshare_return. */
long sys_share(long number, long a0, long a1 = 0, long a2 = 0, long a3 = 0, long a4 = 0);
long sys_unshare(long number, long a0, long a1 = 0, long a2 = 0, long a3 = 0, long a4 = 0);
long probe_main(long *stack);
}

namespace {

using word = unsigned long;

#if defined(__x86_64__)
constexpr const char *abi_name = "x86-64";
constexpr long nr_write = 1;
constexpr long nr_mmap = 9;
constexpr long nr_munmap = 11;
constexpr long nr_getpid = 39;
constexpr long nr_shmdt = 67;
constexpr long nr_shmget = 29;
constexpr long nr_shmat = 30;
constexpr long nr_shmctl = 31;
constexpr long nr_fork = 57;
constexpr long nr_wait4 = 61;
constexpr long nr_ftruncate = 77;
constexpr long nr_futex = 202;
constexpr long nr_exit_group = 231;
constexpr long nr_memfd_create = 319;

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0, long a3 = 0, long a4 = 0)
{
	long result;
	register long r10 __asm__("r10") = a3;
	register long r8 __asm__("r8") = a4;
	register long r9 __asm__("r9") = 0;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory", "cc");
	return result;
}

/* mmap with offset 0. */
inline long map(long length, long protection, long flags, long descriptor)
{
	return sys(nr_mmap, 0, length, protection, flags, descriptor);
}

/* The same through sys_share(). */
inline long share_map(long length, long protection, long flags, long descriptor)
{
	return sys_share(nr_mmap, 0, length, protection, flags, descriptor);
}

__asm__(R"(
	.text
	.globl	clone_agent
	.type	clone_agent,@function
clone_agent:			# rdi stack top, rsi function, rdx argument, rcx flags,
				# r8 the thread ID that CLONE_*_*TID set and clear
	mov	%r8, %r10	# the child's copy
	mov	%rsi, %r9	# the function and argument survive the call
	mov	%rdx, %r8	# in r9 and r8 (TLS, unused without SETTLS)
	mov	%rdi, %rsi
	mov	%rcx, %rdi
	mov	%r10, %rdx	# the parent's copy
	mov	$56, %eax
	syscall
	.globl	clone_return
clone_return:
	test	%rax, %rax
	jnz	1f
	mov	%r8, %rdi	# the child, on its own stack
	call	*%r9
	mov	$60, %eax	# exit this thread only
	xor	%edi, %edi
	syscall
	hlt
1:	ret
	.size	clone_agent,.-clone_agent

	.globl	sys_share
	.type	sys_share,@function
sys_share:			# rdi number, rsi rdx rcx r8 r9 arguments
	mov	%rdi, %rax
	mov	%rsi, %rdi
	mov	%rdx, %rsi
	mov	%rcx, %rdx
	mov	%r8, %r10
	mov	%r9, %r8
	xor	%r9d, %r9d
	syscall
	.globl	share_return
share_return:
	ret
	.size	sys_share,.-sys_share

	.globl	sys_unshare
	.type	sys_unshare,@function
sys_unshare:
	mov	%rdi, %rax
	mov	%rsi, %rdi
	mov	%rdx, %rsi
	mov	%rcx, %rdx
	mov	%r8, %r10
	mov	%r9, %r8
	xor	%r9d, %r9d
	syscall
	.globl	unshare_return
unshare_return:
	ret
	.size	sys_unshare,.-sys_unshare

	.globl	_start
	.type	_start,@function
_start:
	xor	%ebp, %ebp
	mov	%rsp, %rdi
	and	$-16, %rsp
	call	probe_main
	mov	%eax, %edi
	mov	$231, %eax
	syscall
	hlt
	.size	_start,.-_start
)");
#elif defined(__i386__)
constexpr const char *abi_name = "i386";
constexpr long nr_fork = 2;
constexpr long nr_write = 4;
constexpr long nr_getpid = 20;
constexpr long nr_munmap = 91;
constexpr long nr_ftruncate = 93;
constexpr long nr_wait4 = 114;
constexpr long nr_mmap2 = 192;
constexpr long nr_futex = 240;
constexpr long nr_exit_group = 252;
constexpr long nr_memfd_create = 356;
constexpr long nr_shmget = 395;
constexpr long nr_shmctl = 396;
constexpr long nr_shmat = 397;
constexpr long nr_shmdt = 398;

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0, long a3 = 0, long a4 = 0)
{
	long result;
	__asm__ volatile("int $0x80"
			 : "=a"(result)
			 : "a"(number), "b"(a0), "c"(a1), "d"(a2), "S"(a3), "D"(a4)
			 : "memory", "cc");
	return result;
}

/* mmap2 with offset 0, whose sixth argument goes in EBP. */
inline long map(long length, long protection, long flags, long descriptor)
{
	long result;
	__asm__ volatile("push %%ebp\n\t"
			 "xor %%ebp, %%ebp\n\t"
			 "int $0x80\n\t"
			 "pop %%ebp"
			 : "=a"(result)
			 : "a"(nr_mmap2), "b"(0), "c"(length), "d"(protection), "S"(flags), "D"(descriptor)
			 : "memory", "cc");
	return result;
}

/* The same through sys_share(), which zeroes EBP. */
inline long share_map(long length, long protection, long flags, long descriptor)
{
	return sys_share(nr_mmap2, 0, length, protection, flags, descriptor);
}

__asm__(R"(
	.text
	.globl	clone_agent
	.type	clone_agent,@function
clone_agent:			# (stack top, function, argument, flags, thread ID)
	push	%ebx
	push	%esi
	push	%edi
	mov	16(%esp), %ecx	# the child's stack: function, argument
	mov	20(%esp), %eax
	mov	24(%esp), %edx
	mov	28(%esp), %ebx
	sub	$20, %ecx	# 16-byte aligned past the return address
	mov	%eax, (%ecx)
	mov	%edx, 4(%ecx)
	mov	32(%esp), %edx	# the parent's and the child's copy
	mov	%edx, %edi
	xor	%esi, %esi
	mov	$120, %eax
	int	$0x80
	.globl	clone_return
clone_return:
	test	%eax, %eax
	jnz	1f
	pop	%eax		# the child: function
	call	*%eax		# with the argument at 4(%esp) on entry
	mov	$1, %eax	# exit this thread only
	xor	%ebx, %ebx
	int	$0x80
	hlt
1:	pop	%edi
	pop	%esi
	pop	%ebx
	ret
	.size	clone_agent,.-clone_agent

	.globl	sys_share
	.type	sys_share,@function
sys_share:			# (number, five arguments); EBP, the sixth, is 0
	push	%ebp
	push	%ebx
	push	%esi
	push	%edi
	mov	20(%esp), %eax
	mov	24(%esp), %ebx
	mov	28(%esp), %ecx
	mov	32(%esp), %edx
	mov	36(%esp), %esi
	mov	40(%esp), %edi
	xor	%ebp, %ebp
	int	$0x80
	.globl	share_return
share_return:
	pop	%edi
	pop	%esi
	pop	%ebx
	pop	%ebp
	ret
	.size	sys_share,.-sys_share

	.globl	sys_unshare
	.type	sys_unshare,@function
sys_unshare:
	push	%ebp
	push	%ebx
	push	%esi
	push	%edi
	mov	20(%esp), %eax
	mov	24(%esp), %ebx
	mov	28(%esp), %ecx
	mov	32(%esp), %edx
	mov	36(%esp), %esi
	mov	40(%esp), %edi
	xor	%ebp, %ebp
	int	$0x80
	.globl	unshare_return
unshare_return:
	pop	%edi
	pop	%esi
	pop	%ebx
	pop	%ebp
	ret
	.size	sys_unshare,.-sys_unshare

	.globl	_start
	.type	_start,@function
_start:
	xor	%ebp, %ebp
	mov	%esp, %eax
	and	$-16, %esp
	sub	$12, %esp
	push	%eax
	call	probe_main
	mov	%eax, %ebx
	mov	$252, %eax
	int	$0x80
	hlt
	.size	_start,.-_start
)");
#else
#error "an x86-64 or i386 target is required"
#endif

constexpr long prot_read_write = 3;
constexpr long map_shared = 0x01;
constexpr long map_anonymous = 0x20;
constexpr long ipc_private = 0;
constexpr long ipc_create = 01000;
constexpr long ipc_remove = 0;

/* An agent thread: VM FS FILES SIGHAND THREAD SYSVSEM, with its ID in a
 * word of the probe's from before it runs (PARENT_SETTID) until it has left
 * its stack for good (CHILD_CLEARTID), as a pthread's. */
constexpr long clone_thread_flags = 0x50f00 | 0x100000 | 0x200000;
constexpr long futex_wait = 0;
/* An agent task of a thread-cycle: VM FS FILES, and SIGCHLD to its
 * parent, which reaps it with wait4() once it has left the address
 * space. */
constexpr long clone_task_flags = 0x700 | 17;

constexpr unsigned pairs = 512;
constexpr unsigned maximum_agents = 4;
constexpr unsigned long spin_limit = 1UL << 30;

struct alignas(128) Line {
	volatile word value;
	volatile word pad[128 / sizeof(word) - 1];
};

/* x and y in one 256-byte block, so in one guest page. */
struct alignas(256) Pair {
	Line x;
	Line y;
};

struct alignas(128) Slot {
	volatile word value;
};

struct Shared {
	Pair *volatile stack_pair;
	Slot go;
	Slot done;
	Slot finished;
	Slot result[maximum_agents];
	Pair pair[pairs];
};

constexpr long shared_size = (sizeof(Shared) + 4095) & ~4095L;

inline void store_fence(volatile word *slot, word value)
{
	__asm__ volatile("xchg %0, %1" : "+r"(value), "+m"(*slot) : : "memory");
}

inline void add_fence(volatile word *slot, word value)
{
	__asm__ volatile("lock xadd %0, %1" : "+r"(value), "+m"(*slot) : : "memory", "cc");
}

inline void mfence()
{
	__asm__ volatile("mfence" : : : "memory");
}

inline void relax()
{
	__asm__ volatile("pause" : : : "memory");
}

/* A delay of a few dozen cycles that varies from round to round, so that
 * the agents of the store-buffering tests meet at every offset. */
inline void stagger(word round, word scale)
{
	for (volatile word count = (round * scale) % 23; count != 0; count = count - 1) {
	}
}

void write_text(const char *text)
{
	long length = 0;
	while (text[length] != '\0')
		length++;
	sys(nr_write, 1, reinterpret_cast<long>(text), length);
}

void write_hex(unsigned long value)
{
	char text[2 + 2 * sizeof(value) + 1];
	text[0] = '0';
	text[1] = 'x';
	for (unsigned digit = 0; digit < 2 * sizeof(value); digit++)
		text[2 + digit] = "0123456789abcdef"[(value >> (4 * (2 * sizeof(value) - 1 - digit))) & 15];
	text[sizeof(text) - 1] = '\0';
	write_text(text);
}

void write_decimal(unsigned long value)
{
	char text[24];
	int position = 23;
	text[position] = '\0';
	do {
		text[--position] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value != 0);
	write_text(text + position);
}

bool same(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}
	return *left == *right;
}

[[noreturn]] void fail(const char *message)
{
	write_text("tso-litmus: error: ");
	write_text(message);
	write_text("\n");
	for (;;)
		sys(nr_exit_group, 2);
}

void wait_at_least(volatile word *slot, word value)
{
	for (unsigned long spins = 0; *slot < value; spins++) {
		if (spins == spin_limit)
			fail("an agent stopped answering");
		relax();
	}
}

enum class Test { mp, sb, sb_mfence, iriw, mp_stack_load, mp_stack_store };

constexpr const char *test_name(Test test)
{
	switch (test) {
	case Test::mp: return "mp";
	case Test::sb: return "sb";
	case Test::sb_mfence: return "sb-mfence";
	case Test::iriw: return "iriw";
	case Test::mp_stack_load: return "mp-stack-load";
	case Test::mp_stack_store: return "mp-stack-store";
	}
	return "?";
}

constexpr unsigned agents_of(Test test)
{
	return test == Test::iriw ? 4 : 2;
}

struct Job {
	Shared *shared;
	Test test;
	word rounds;
	unsigned agent;
};

/* One agent other than the leader: its part of every round, then a mark
 * that it is done. */
void run_agent(const Job &job)
{
	Shared &shared = *job.shared;
	Pair own;
	if (job.test == Test::mp_stack_store)
		shared.stack_pair = &own;
	for (word round = 1; round <= job.rounds; round++) {
		wait_at_least(&shared.go.value, round);
		Pair &pair = shared.pair[round % pairs];
		switch (job.test) {
		case Test::mp:
			/* The writer: own y's line, then store x and y. */
			pair.y.pad[3] = round;
			pair.x.value = 1;
			pair.y.value = 1;
			break;
		case Test::mp_stack_load: {
			Pair &remote = *shared.stack_pair;
			remote.y.pad[3] = round;
			remote.x.value = 1;
			remote.y.value = 1;
			break;
		}
		case Test::mp_stack_store:
			own.y.pad[3] = round;
			own.x.value = 1;
			own.y.value = 1;
			break;
		case Test::sb:
			stagger(round, 7);
			pair.y.value = 1;
			shared.result[1].value = pair.x.value;
			break;
		case Test::sb_mfence:
			stagger(round, 7);
			pair.y.value = 1;
			mfence();
			shared.result[1].value = pair.x.value;
			break;
		case Test::iriw:
			if (job.agent == 1) {
				/* The second reader: y, then x, until y is 1. */
				word y = 0;
				word x = 0;
				for (unsigned long spins = 0;; spins++) {
					y = pair.y.value;
					x = pair.x.value;
					if (y != 0)
						break;
					if (spins == spin_limit)
						fail("iriw: y never became 1");
				}
				shared.result[1].value = x;
			} else {
				stagger(round, job.agent == 2 ? 5 : 11);
				(job.agent == 2 ? pair.x : pair.y).value = 1;
			}
			break;
		}
		add_fence(&shared.done.value, 1);
	}
	add_fence(&shared.finished.value, 1);
	if (job.test == Test::mp_stack_store)
		wait_at_least(&shared.go.value, job.rounds + 1);
}

/* MP whose reader keeps x and y in its own frame (mp-stack-load), or whose
 * writer does (mp-stack-store, see run_agent). */
word run_stack_leader(const Job &job)
{
	Shared &shared = *job.shared;
	Pair own;
	if (job.test == Test::mp_stack_load)
		shared.stack_pair = &own;
	else
		while (shared.stack_pair == nullptr)
			relax();
	word counted = 0;
	for (word round = 1; round <= job.rounds; round++) {
		word x = 0;
		word y = 0;
		if (job.test == Test::mp_stack_load) {
			own.x.value = 0;
			own.y.value = 0;
			own.x.pad[5] = round;
			store_fence(&shared.go.value, round);
			for (unsigned long spins = 0;; spins++) {
				y = own.y.value;
				x = own.x.value;
				if (y != 0)
					break;
				if (spins == spin_limit)
					fail("mp-stack: y never became 1");
			}
		} else {
			Pair &remote = *shared.stack_pair;
			remote.x.value = 0;
			remote.y.value = 0;
			remote.x.pad[5] = round;
			store_fence(&shared.go.value, round);
			for (unsigned long spins = 0;; spins++) {
				y = remote.y.value;
				x = remote.x.value;
				if (y != 0)
					break;
				if (spins == spin_limit)
					fail("mp-stack: y never became 1");
			}
		}
		wait_at_least(&shared.done.value, round);
		mfence();
		counted += x == 0;
	}
	wait_at_least(&shared.finished.value, 1);
	shared.stack_pair = nullptr;
	store_fence(&shared.go.value, job.rounds + 1);
	return counted;
}

/* The leader: resets each round's pair, starts the round, plays its own
 * part, waits for the others and judges the outcome. Returns the count of
 * outcomes x86 forbids (or, for sb, of allowed reorderings seen). */
word run_leader(const Job &job)
{
	Shared &shared = *job.shared;
	const word others = agents_of(job.test) - 1;
	word counted = 0;
	for (word round = 1; round <= job.rounds; round++) {
		Pair &pair = shared.pair[round % pairs];
		pair.x.value = 0;
		pair.y.value = 0;
		shared.result[1].value = 2;
		if (job.test == Test::mp)
			pair.x.pad[5] = round;	/* the reader's copy of x */
		store_fence(&shared.go.value, round);
		switch (job.test) {
		case Test::mp: {
			word y = 0;
			word x = 0;
			for (unsigned long spins = 0;; spins++) {
				y = pair.y.value;
				x = pair.x.value;
				if (y != 0)
					break;
				if (spins == spin_limit)
					fail("mp: y never became 1");
			}
			wait_at_least(&shared.done.value, round * others);
			mfence();
			counted += x == 0;
			break;
		}
		case Test::sb:
		case Test::sb_mfence: {
			stagger(round, 3);
			pair.x.value = 1;
			if (job.test == Test::sb_mfence)
				mfence();
			const word own = pair.y.value;
			wait_at_least(&shared.done.value, round * others);
			mfence();
			counted += own == 0 && shared.result[1].value == 0;
			break;
		}
		case Test::iriw: {
			/* The first reader: x, then y, until x is 1. */
			word x = 0;
			word y = 0;
			for (unsigned long spins = 0;; spins++) {
				x = pair.x.value;
				y = pair.y.value;
				if (x != 0)
					break;
				if (spins == spin_limit)
					fail("iriw: x never became 1");
			}
			wait_at_least(&shared.done.value, round * others);
			mfence();
			counted += y == 0 && shared.result[1].value == 0;
			break;
		}
		default:
			break;
		}
	}
	return counted;
}

enum class Case { thread, shm, memfd, anon, thread_cycle, shm_cycle, anon_cycle };

alignas(4096) Shared thread_shared;
alignas(16) unsigned char thread_stacks[maximum_agents][64 * 1024];
Job thread_jobs[maximum_agents];
volatile int thread_ids[maximum_agents];

void thread_agent(Job *job)
{
	run_agent(*job);
}

void reset(Shared &shared)
{
	shared.go.value = 0;
	shared.done.value = 0;
	shared.finished.value = 0;
}

/* Maps the memory a process case shares, as each process does for itself:
 * an attach of the shm segment or a MAP_SHARED mapping of the memfd. */
Shared *attach(Case kind, long handle)
{
	long address = kind == Case::shm ?
		sys(nr_shmat, handle, 0, 0) :
		map(shared_size, prot_read_write, map_shared, handle);
	if (address < 0 && address > -4096)
		fail("cannot map the shared memory");
	return reinterpret_cast<Shared *>(address);
}

word run_test(Case kind, Test test, word rounds, Shared *anon, long handle)
{
	const unsigned agents = agents_of(test);
	if (kind == Case::thread || kind == Case::thread_cycle) {
		const bool tasks = kind == Case::thread_cycle;
		Shared &shared = thread_shared;
		long pids[maximum_agents]{};
		reset(shared);
		for (unsigned agent = 1; agent < agents; agent++) {
			thread_jobs[agent] = {&shared, test, rounds, agent};
			pids[agent] = clone_agent(thread_stacks[agent] + sizeof(thread_stacks[agent]),
				thread_agent, &thread_jobs[agent],
				tasks ? clone_task_flags : clone_thread_flags, &thread_ids[agent]);
			if (pids[agent] <= 0)
				fail("clone failed");
		}
		const bool stack = test == Test::mp_stack_load || test == Test::mp_stack_store;
		const word counted = stack ? run_stack_leader({&shared, test, rounds, 0}) :
			run_leader({&shared, test, rounds, 0});
		wait_at_least(&shared.finished.value, agents - 1);
		/* Join every agent before the next test reuses its stack. */
		for (unsigned agent = 1; agent < agents; agent++) {
			if (tasks) {
				int status = -1;
				if (sys_unshare(nr_wait4, pids[agent], reinterpret_cast<long>(&status)) !=
					    pids[agent] ||
				    status != 0)
					fail("an agent task failed");
				continue;
			}
			for (int id; (id = thread_ids[agent]) != 0;)
				sys(nr_futex, reinterpret_cast<long>(&thread_ids[agent]), futex_wait, id, 0);
		}
		return counted;
	}
	long children[maximum_agents]{};
	Shared *shared = anon;
	if (shared != nullptr)
		reset(*shared);
	/* A process case's memory starts zeroed, and only the leader resets
	 * it, after every agent has mapped it. */
	for (unsigned agent = 1; agent < agents; agent++) {
		const long pid = sys(nr_fork);
		if (pid < 0)
			fail("fork failed");
		if (pid == 0) {
			Shared *mapped = shared != nullptr ? shared : attach(kind, handle);
			run_agent({mapped, test, rounds, agent});
			for (;;)
				sys(nr_exit_group, 0);
		}
		children[agent] = pid;
	}
	if (shared == nullptr)
		shared = attach(kind, handle);
	const word counted = run_leader({shared, test, rounds, 0});
	wait_at_least(&shared->finished.value, agents - 1);
	reset(*shared);
	for (unsigned agent = 1; agent < agents; agent++) {
		int status = -1;
		if (sys(nr_wait4, children[agent], reinterpret_cast<long>(&status)) != children[agent] || status != 0)
			fail("an agent process failed");
	}
	return counted;
}

word parse(const char *text)
{
	word value = 0;
	if (*text == '\0')
		fail("bad round count");
	for (; *text != '\0'; text++) {
		if (*text < '0' || *text > '9')
			fail("bad round count");
		value = value * 10 + static_cast<word>(*text - '0');
	}
	return value;
}

/* System calls on the probe's own, each a dispatch the host logs: an
 * unshared phase of a cycle. */
void settle()
{
	for (int round = 0; round < 64; round++)
		sys(nr_getpid);
}

/* Starts a phase of shm-cycle or anon-cycle: maps the memory the agents
 * will inherit, with the system call that starts the sharing. */
Shared *share(Case kind, long handle)
{
	const long address = kind == Case::shm_cycle ?
		sys_share(nr_shmat, handle, 0, 0) :
		share_map(shared_size, prot_read_write, map_shared | map_anonymous, -1);
	if (address < 0 && address > -4096)
		fail("cannot map the shared memory");
	return reinterpret_cast<Shared *>(address);
}

/* Ends it with the system call that ends the sharing. */
void unshare(Case kind, Shared *shared)
{
	const long address = reinterpret_cast<long>(shared);
	if ((kind == Case::shm_cycle ? sys_unshare(nr_shmdt, address) :
				       sys_unshare(nr_munmap, address, shared_size)) != 0)
		fail("cannot unmap the shared memory");
}

} // namespace

long probe_main(long *stack)
{
	const long argc = stack[0];
	auto **argv = reinterpret_cast<char **>(stack + 1);
	if (argc != 3)
		fail("usage: tso-litmus-probe thread|shm|memfd|anon|thread-cycle|shm-cycle|anon-cycle ROUNDS");
	struct {
		const char *name;
		Case kind;
	} constexpr names[] = {
		{"thread", Case::thread},
		{"shm", Case::shm},
		{"memfd", Case::memfd},
		{"anon", Case::anon},
		{"thread-cycle", Case::thread_cycle},
		{"shm-cycle", Case::shm_cycle},
		{"anon-cycle", Case::anon_cycle},
	};
	const Case *found = nullptr;
	for (const auto &name : names)
		if (same(argv[1], name.name))
			found = &name.kind;
	if (found == nullptr)
		fail("unknown case");
	const Case kind = *found;
	const word rounds = parse(argv[2]);
	const bool cycle = kind == Case::thread_cycle || kind == Case::shm_cycle ||
		kind == Case::anon_cycle;

	write_text("tso-litmus: abi=");
	write_text(abi_name);
	write_text(" range=");
	write_hex(reinterpret_cast<unsigned long>(__ehdr_start));
	write_text("-");
	write_hex(reinterpret_cast<unsigned long>(etext));
	write_text(" clone=");
	write_hex(reinterpret_cast<unsigned long>(clone_return));
	write_text(" share=");
	write_hex(reinterpret_cast<unsigned long>(share_return));
	write_text(" unshare=");
	write_hex(reinterpret_cast<unsigned long>(unshare_return));
	write_text("\n");

	/* Every process maps the shm segment or memfd itself, after fork();
	 * the anonymous memory is mapped before, and inherited. A cycle's
	 * memory is mapped at the start of each phase, and inherited. */
	long handle = -1;
	Shared *anon = nullptr;
	if (kind == Case::shm || kind == Case::shm_cycle) {
		handle = sys(nr_shmget, ipc_private, shared_size, ipc_create | 0600);
		if (handle < 0)
			fail("shmget failed");
	} else if (kind == Case::memfd) {
		handle = sys(nr_memfd_create, reinterpret_cast<long>("tso-litmus"), 0);
		if (handle < 0 || sys(nr_ftruncate, handle, shared_size) != 0)
			fail("memfd_create failed");
	} else if (kind == Case::anon) {
		const long address = map(shared_size, prot_read_write, map_shared | map_anonymous, -1);
		if (address < 0 && address > -4096)
			fail("cannot map shared anonymous memory");
		anon = reinterpret_cast<Shared *>(address);
	}

	long status = 0;
	constexpr Test thread_tests[] = {Test::mp, Test::mp_stack_load, Test::mp_stack_store,
		Test::sb, Test::sb_mfence, Test::iriw};
	constexpr Test process_tests[] = {Test::mp, Test::sb, Test::sb_mfence, Test::iriw};
	const bool threads = kind == Case::thread || kind == Case::thread_cycle;
	const Test *tests = threads ? thread_tests : process_tests;
	const unsigned count = threads ? sizeof(thread_tests) / sizeof(Test) :
		sizeof(process_tests) / sizeof(Test);
	const unsigned phases = cycle ? 2 : 1;
	for (unsigned phase = 1; phase <= phases; phase++) {
		const bool mapped = kind == Case::shm_cycle || kind == Case::anon_cycle;
		if (mapped)
			anon = share(kind, handle);
		for (unsigned index = 0; index < count; index++) {
			const Test test = tests[index];
			const word counted = run_test(kind, test, rounds, anon, handle);
			write_text("tso-litmus: abi=");
			write_text(abi_name);
			write_text(" case=");
			write_text(argv[1]);
			if (cycle) {
				write_text(" phase=");
				write_decimal(phase);
			}
			write_text(" test=");
			write_text(test_name(test));
			write_text(" rounds=");
			write_decimal(rounds);
			write_text(test == Test::sb ? " allowed=" : " forbidden=");
			write_decimal(counted);
			write_text("\n");
			if (test != Test::sb && counted != 0)
				status = 1;
		}
		if (mapped)
			unshare(kind, anon);
		if (cycle)
			settle();
	}
	if (handle >= 0 && (kind == Case::shm || kind == Case::shm_cycle))
		sys(nr_shmctl, handle, ipc_remove, 0);
	return status;
}
