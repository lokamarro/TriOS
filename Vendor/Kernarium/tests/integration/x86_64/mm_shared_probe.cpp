// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freestanding x86-64 guest probe for the "mm is shared" dispatch flag
 * (DARWIN_UML_DISPATCH_MM_SHARED). It prints its own code range and the
 * return addresses of the system calls that start and end its sharing, then
 * runs the mode its first argument names:
 *
 *   single        only system calls on its own;
 *   fork          fork() a child, which exits;
 *   vfork         vfork() a child, which exits;
 *   private-file  map a memfd MAP_PRIVATE and write it;
 *   thread        clone() a thread that shares the address space;
 *   shared-anon   map shared anonymous memory and write it;
 *   shared-ro     map a memfd MAP_SHARED, read-only, and read it;
 *   shm           attach a SysV shm segment and write it;
 *   uring-nommap  set up an io_uring whose rings live in the probe's own
 *                 memory (IORING_SETUP_NO_MMAP);
 *   fork-shared   map shared anonymous memory, then fork() a child, which
 *                 inherits the mapping and the mark;
 *   thread-fork   clone() a thread, then fork() a child, whose mm has one
 *                 task and no shared mapping;
 *
 * and the modes whose sharing ends again, each with the system call that
 * ends it:
 *
 *   thread-join   clone(CLONE_VM) a child task, which exits; wait4() reaps it;
 *   unmap         map shared anonymous memory, write it; munmap() it;
 *   shmdt         attach a SysV shm segment, write it; shmdt() it;
 *   split         map two pages shared, unmap the first, which leaves the
 *                 second mapped; munmap() the second;
 *   mremap        map a page shared, move it onto a reservation with
 *                 mremap(); munmap() it;
 *   remap-private map a page shared; map private memory over it MAP_FIXED;
 *   fork-dontcopy map shared anonymous memory MADV_DONTFORK, then fork() a
 *                 child, which gets no shared mapping; munmap() it;
 *   uring-close   set up the NO_MMAP io_uring and close() it; the kernel
 *                 unpins its rings later, from a work item.
 *
 * The first four leave the mm unshared; each of the others shares the
 * probe's mm, and fork-shared its child's too. A system call that starts or
 * ends the probe's sharing returns to share_return or unshare_return.
 */

namespace {

using u64 = unsigned long;

constexpr long nr_write = 1;
constexpr long nr_close = 3;
constexpr long nr_mmap = 9;
constexpr long nr_munmap = 11;
constexpr long nr_mremap = 25;
constexpr long nr_sched_yield = 24;
constexpr long nr_madvise = 28;
constexpr long nr_shmget = 29;
constexpr long nr_shmat = 30;
constexpr long nr_shmctl = 31;
constexpr long nr_nanosleep = 35;
constexpr long nr_getpid = 39;
constexpr long nr_fork = 57;
constexpr long nr_vfork = 58;
constexpr long nr_exit = 60;
constexpr long nr_wait4 = 61;
constexpr long nr_shmdt = 67;
constexpr long nr_ftruncate = 77;
constexpr long nr_memfd_create = 319;
constexpr long nr_io_uring_setup = 425;

constexpr long prot_none = 0;
constexpr long prot_read = 1;
constexpr long prot_read_write = 3;
constexpr long map_shared = 0x01;
constexpr long map_private = 0x02;
constexpr long map_fixed = 0x10;
constexpr long map_anonymous = 0x20;
constexpr long mremap_maymove = 1;
constexpr long mremap_fixed = 2;
constexpr long madv_dontfork = 10;
constexpr long page_size = 4096;

constexpr u64 clone_thread_flags = 0x00000100 | 0x00000200 | 0x00000400 |
	0x00000800 | 0x00010000 | 0x00040000; /* VM FS FILES SIGHAND THREAD SYSVSEM */
/* VM FS FILES, and SIGCHLD at its exit: a task on the probe's mm that wait4()
 * reaps once it has left the mm. */
constexpr u64 clone_task_flags = 0x00000100 | 0x00000200 | 0x00000400 | 17;

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0, long a3 = 0,
		long a4 = 0, long a5 = 0)
{
	long result;
	register long r10 __asm__("r10") = a3;
	register long r8 __asm__("r8") = a4;
	register long r9 __asm__("r9") = a5;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return result;
}

bool failed(long result)
{
	return result < 0 && result > -4096;
}

void write_text(const char *text)
{
	long length = 0;
	while (text[length] != '\0')
		length++;
	sys(nr_write, 1, reinterpret_cast<long>(text), length);
}

void write_hex(u64 value)
{
	char text[19] = "0x";
	for (int index = 0; index < 16; index++)
		text[2 + index] = "0123456789abcdef"[(value >> (60 - 4 * index)) & 15];
	text[18] = '\0';
	write_text(text);
}

bool same(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}
	return *left == *right;
}

volatile int thread_done;

alignas(16) unsigned char thread_stack[64 * 1024];

/* io_uring rings in the probe's own memory: the rings in one page, the
 * submission entries in another. */
alignas(4096) unsigned char uring_rings[page_size];
alignas(4096) unsigned char uring_entries[page_size];

/* struct io_uring_params, with only what the probe sets named. */
struct UringParams {
	unsigned sq_entries;
	unsigned cq_entries;
	unsigned flags;
	unsigned rest[7];
	unsigned sq_off[8];
	unsigned long sq_user_addr;
	unsigned cq_off[8];
	unsigned long cq_user_addr;
};
static_assert(sizeof(UringParams) == 120);
constexpr unsigned uring_setup_no_mmap = 1U << 14;

/* Maps a fresh memfd of one page and returns the mapping, or 0. */
long map_memfd(long protection, long flags)
{
	const long descriptor = sys(nr_memfd_create, reinterpret_cast<long>("mm-shared"), 0);
	if (failed(descriptor) || sys(nr_ftruncate, descriptor, page_size) != 0)
		return 0;
	const long address = sys(nr_mmap, 0, page_size, protection, flags, descriptor, 0);
	sys(nr_close, descriptor);
	return failed(address) ? 0 : address;
}

} // namespace

extern "C" {
extern const char __ehdr_start[];
extern const char etext[];
extern const char share_return[];
extern const char unshare_return[];
long fork_exit(long number);
long clone_thread(u64 flags, void *stack, int (*function)());
/* System calls that return to share_return and unshare_return. */
long sys_share(long number, long a0, long a1 = 0, long a2 = 0, long a3 = 0,
	long a4 = 0);
long sys_unshare(long number, long a0, long a1 = 0, long a2 = 0, long a3 = 0,
	long a4 = 0);
int thread_main();
[[noreturn]] void probe_main(long argc, char **argv);
}

int thread_main()
{
	for (int round = 0; round < 8; round++)
		sys(nr_getpid);
	thread_done = 1;
	return 0;
}

__asm__(R"(
	.text
	.globl fork_exit
fork_exit:			# fork or vfork; the child exits at once
	mov	%rdi, %rax
	syscall
	test	%rax, %rax
	jnz	1f
	mov	$60, %eax
	xor	%edi, %edi
	syscall
1:	ret

	.globl clone_thread
clone_thread:			# rdi flags, rsi stack, rdx function
	mov	%rdx, %r9
	xor	%edx, %edx
	xor	%r10d, %r10d
	xor	%r8d, %r8d
	mov	$56, %eax
	syscall
	test	%rax, %rax
	jnz	1f
	call	*%r9
	mov	%eax, %edi
	mov	$60, %eax
	syscall
1:	ret

	.globl sys_share
sys_share:			# rdi number, rsi rdx rcx r8 r9 arguments
	mov	%rdi, %rax
	mov	%rsi, %rdi
	mov	%rdx, %rsi
	mov	%rcx, %rdx
	mov	%r8, %r10
	mov	%r9, %r8
	xor	%r9d, %r9d
	syscall
	.globl share_return
share_return:
	ret

	.globl sys_unshare
sys_unshare:
	mov	%rdi, %rax
	mov	%rsi, %rdi
	mov	%rdx, %rsi
	mov	%rcx, %rdx
	mov	%r8, %r10
	mov	%r9, %r8
	xor	%r9d, %r9d
	syscall
	.globl unshare_return
unshare_return:
	ret

	.globl _start
_start:
	mov	(%rsp), %rdi
	lea	8(%rsp), %rsi
	and	$-16, %rsp
	call	probe_main
	hlt
)");

namespace {

/* Starts a thread on the probe's mm and waits until it is done. */
bool run_thread()
{
	if (clone_thread(clone_thread_flags, thread_stack + sizeof(thread_stack), thread_main) <= 0)
		return false;
	while (!thread_done)
		sys(nr_sched_yield);
	return true;
}

/* fork() or vfork() a child that exits at once, and reap it. */
bool fork_child(long number)
{
	const long pid = fork_exit(number);
	int wait_status = 0;
	return pid > 0 && sys(nr_wait4, pid, reinterpret_cast<long>(&wait_status)) == pid &&
	       wait_status == 0;
}

/* Starts a task on the probe's mm, then reaps it with the wait4() that ends
 * the sharing: the task has left the mm before wait4() returns. */
bool join_task()
{
	const long pid = clone_thread(clone_task_flags, thread_stack + sizeof(thread_stack),
				      thread_main);
	int wait_status = 0;
	return pid > 0 && sys_unshare(nr_wait4, pid, reinterpret_cast<long>(&wait_status)) == pid &&
	       wait_status == 0;
}

/* Maps `pages` of shared anonymous memory with the mmap() that starts the
 * sharing, and writes it; returns the mapping or 0. */
long share_anon(long pages)
{
	const long address = sys_share(nr_mmap, 0, pages * page_size, prot_read_write,
				       map_shared | map_anonymous, -1);
	if (failed(address))
		return 0;
	for (long page = 0; page < pages; page++)
		reinterpret_cast<volatile long *>(address + page * page_size)[0] = 1;
	return address;
}

/* Some system calls, each a dispatch the host logs at the probe's RIP. */
void settle()
{
	for (int round = 0; round < 8; round++)
		sys(nr_getpid);
}

/* io_uring rings in the probe's own memory, set up with the system call
 * that starts the sharing; returns the ring's descriptor. */
long setup_uring()
{
	UringParams params{};
	params.flags = uring_setup_no_mmap;
	params.sq_user_addr = reinterpret_cast<unsigned long>(uring_entries);
	params.cq_user_addr = reinterpret_cast<unsigned long>(uring_rings);
	return sys_share(nr_io_uring_setup, 1, reinterpret_cast<long>(&params));
}

/* Ends the sharing of each mode that ends it; returns the exit status. */
long run_unshare_mode(const char *mode)
{
	if (same(mode, "thread-join"))
		return !join_task();
	if (same(mode, "unmap")) {
		const long address = share_anon(1);
		settle();
		return address == 0 || sys_unshare(nr_munmap, address, page_size) != 0;
	}
	if (same(mode, "shmdt")) {
		const long segment = sys(nr_shmget, 0, page_size, 01000 | 0600);
		const long address = failed(segment) ? segment : sys_share(nr_shmat, segment, 0, 0);
		long status = failed(address);
		if (status == 0) {
			*reinterpret_cast<volatile long *>(address) = 1;
			settle();
			status = sys_unshare(nr_shmdt, address) != 0;
		}
		if (!failed(segment))
			sys(nr_shmctl, segment, 0, 0);
		return status;
	}
	if (same(mode, "split")) {
		const long address = share_anon(2);
		if (address == 0 || sys(nr_munmap, address, page_size) != 0)
			return 1;
		settle();
		return sys_unshare(nr_munmap, address + page_size, page_size) != 0;
	}
	if (same(mode, "mremap")) {
		const long address = share_anon(1);
		const long target = sys(nr_mmap, 0, page_size, prot_none,
					map_private | map_anonymous, -1);
		if (address == 0 || failed(target))
			return 1;
		/* Moving the vma unmaps the old range after the copy is in. */
		const long moved = sys(nr_mremap, address, page_size, page_size,
				       mremap_maymove | mremap_fixed, target);
		if (moved != target || *reinterpret_cast<volatile long *>(moved) != 1)
			return 1;
		settle();
		return sys_unshare(nr_munmap, moved, page_size) != 0;
	}
	if (same(mode, "remap-private")) {
		const long address = share_anon(1);
		if (address == 0)
			return 1;
		settle();
		const long remapped = sys_unshare(nr_mmap, address, page_size, prot_read_write,
						  map_private | map_anonymous | map_fixed, -1);
		if (remapped != address)
			return 1;
		*reinterpret_cast<volatile long *>(address) = 2;
		return 0;
	}
	if (same(mode, "fork-dontcopy")) {
		const long address = share_anon(1);
		if (address == 0 || sys(nr_madvise, address, page_size, madv_dontfork) != 0 ||
		    !fork_child(nr_fork))
			return 1;
		settle();
		return sys_unshare(nr_munmap, address, page_size) != 0;
	}
	if (same(mode, "uring-close")) {
		const long ring = setup_uring();
		if (failed(ring))
			return 1;
		settle();
		sys(nr_close, ring);
		/* The rings are unpinned once a work item frees them: give it
		 * two seconds, each sleep a dispatch the host logs. */
		struct {
			long seconds, nanoseconds;
		} delay = {0, 20 * 1000 * 1000};
		for (int round = 0; round < 100; round++)
			sys(nr_nanosleep, reinterpret_cast<long>(&delay), 0);
		return 0;
	}
	return 2;
}

} // namespace

void probe_main(long argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "single";
	write_text("mm-shared-probe: mode=");
	write_text(mode);
	write_text(" range=");
	write_hex(reinterpret_cast<u64>(__ehdr_start));
	write_text("-");
	write_hex(reinterpret_cast<u64>(etext));
	write_text(" share=");
	write_hex(reinterpret_cast<u64>(share_return));
	write_text(" unshare=");
	write_hex(reinterpret_cast<u64>(unshare_return));
	write_text("\n");
	long status = 0;
	if (same(mode, "fork") || same(mode, "vfork")) {
		status = !fork_child(same(mode, "fork") ? nr_fork : nr_vfork);
	} else if (same(mode, "thread")) {
		status = !run_thread();
	} else if (same(mode, "private-file") || same(mode, "shared-ro")) {
		const bool shared = same(mode, "shared-ro");
		const long address = map_memfd(shared ? prot_read : prot_read_write,
					       shared ? map_shared : map_private);
		if (address == 0)
			status = 1;
		else if (shared)
			status = *reinterpret_cast<volatile long *>(address) != 0;
		else
			*reinterpret_cast<volatile long *>(address) = 1;
	} else if (same(mode, "shared-anon") || same(mode, "fork-shared")) {
		const long address = sys(nr_mmap, 0, page_size, prot_read_write,
					 map_shared | map_anonymous, -1, 0);
		if (failed(address))
			status = 1;
		else
			*reinterpret_cast<volatile long *>(address) = 1;
		if (status == 0 && same(mode, "fork-shared"))
			status = !fork_child(nr_fork);
	} else if (same(mode, "shm")) {
		const long segment = sys(nr_shmget, 0, page_size, 01000 | 0600);
		const long address = failed(segment) ? segment : sys(nr_shmat, segment, 0, 0);
		if (failed(address))
			status = 1;
		else
			*reinterpret_cast<volatile long *>(address) = 1;
		if (!failed(segment))
			sys(nr_shmctl, segment, 0, 0);
	} else if (same(mode, "uring-nommap")) {
		const long ring = setup_uring();
		if (failed(ring))
			status = 1;
		else
			sys(nr_close, ring);
	} else if (same(mode, "thread-fork")) {
		status = !run_thread() || !fork_child(nr_fork);
	} else if (!same(mode, "single")) {
		status = run_unshare_mode(mode);
	}
	settle();
	for (;;)
		sys(nr_exit, status);
}
