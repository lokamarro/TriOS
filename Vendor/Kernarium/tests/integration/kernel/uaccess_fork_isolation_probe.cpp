// SPDX-License-Identifier: GPL-2.0-or-later
/* A kernel write into a parent's private page after fork() never reaches the
 * child.
 *
 * pinned-fork: a buffer registered with io_uring is pinned with FOLL_PIN, so
 * fork() gives the child a copy of it and the parent keeps the pinned page.
 * The child's copy must hold what the parent stored just before fork(), and
 * an IORING_OP_READ_FIXED the parent completes after fork() must show in the
 * parent's buffer and never in the child's.  A UML uaccess write pins its
 * page the same way, so this checks the copy fork() then makes.
 *
 * child-stable: writer threads make system calls that copy fresh values into
 * a private page only the kernel writes while two siblings fork.  Each child
 * snapshots those pages, waits, and compares: nothing in the child writes
 * them, so any change is a parent's kernel write that reached the child.
 * Without the pin, a write that found its page before fork() copied the PTE
 * could land in the page the child then shared.
 *
 * The exit status is the number of failures, at most 100.
 */
#include <linux/io_uring.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

long failures = 0;
long page_size = 0;

void fail(const char *what)
{
	std::printf("uaccess-fork-isolation: FAIL %s\n", what);
	++failures;
}

struct Ring {
	int fd = -1;
	unsigned *sq_tail = nullptr;
	unsigned *sq_mask = nullptr;
	unsigned *sq_array = nullptr;
	unsigned *cq_head = nullptr;
	unsigned *cq_tail = nullptr;
	unsigned *cq_mask = nullptr;
	io_uring_sqe *sqes = nullptr;
	io_uring_cqe *cqes = nullptr;
};

bool ring_setup(Ring &ring)
{
	io_uring_params params{};
	ring.fd = static_cast<int>(syscall(__NR_io_uring_setup, 4, &params));
	if (ring.fd < 0) {
		std::printf("uaccess-fork-isolation: io_uring_setup: %s\n",
			    std::strerror(errno));
		return false;
	}
	size_t sq_size = params.sq_off.array +
		params.sq_entries * sizeof(unsigned);
	size_t cq_size = params.cq_off.cqes +
		params.cq_entries * sizeof(io_uring_cqe);
	const bool single = params.features & IORING_FEAT_SINGLE_MMAP;
	if (single)
		sq_size = cq_size = sq_size > cq_size ? sq_size : cq_size;
	auto *sq = static_cast<unsigned char *>(mmap(nullptr, sq_size,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring.fd,
		IORING_OFF_SQ_RING));
	if (sq == MAP_FAILED)
		return false;
	auto *cq = sq;
	if (!single) {
		cq = static_cast<unsigned char *>(mmap(nullptr, cq_size,
			PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
			ring.fd, IORING_OFF_CQ_RING));
		if (cq == MAP_FAILED)
			return false;
	}
	void *sqes = mmap(nullptr, params.sq_entries * sizeof(io_uring_sqe),
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring.fd,
		IORING_OFF_SQES);
	if (sqes == MAP_FAILED)
		return false;
	ring.sq_tail = reinterpret_cast<unsigned *>(sq + params.sq_off.tail);
	ring.sq_mask = reinterpret_cast<unsigned *>(sq + params.sq_off.ring_mask);
	ring.sq_array = reinterpret_cast<unsigned *>(sq + params.sq_off.array);
	ring.cq_head = reinterpret_cast<unsigned *>(cq + params.cq_off.head);
	ring.cq_tail = reinterpret_cast<unsigned *>(cq + params.cq_off.tail);
	ring.cq_mask = reinterpret_cast<unsigned *>(cq + params.cq_off.ring_mask);
	ring.sqes = static_cast<io_uring_sqe *>(sqes);
	ring.cqes = reinterpret_cast<io_uring_cqe *>(cq + params.cq_off.cqes);
	return true;
}

/* One READ_FIXED of len bytes from fd offset 0 into registered buffer 0. */
int ring_read_fixed(Ring &ring, int fd, void *buffer, unsigned len)
{
	const unsigned tail = *ring.sq_tail;
	const unsigned index = tail & *ring.sq_mask;
	io_uring_sqe *sqe = &ring.sqes[index];
	std::memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_READ_FIXED;
	sqe->fd = fd;
	sqe->addr = reinterpret_cast<std::uintptr_t>(buffer);
	sqe->len = len;
	sqe->off = 0;
	sqe->buf_index = 0;
	ring.sq_array[index] = index;
	__atomic_store_n(ring.sq_tail, tail + 1, __ATOMIC_RELEASE);
	if (syscall(__NR_io_uring_enter, ring.fd, 1, 1,
		    IORING_ENTER_GETEVENTS, nullptr, 0) < 0)
		return -errno;
	const unsigned head = *ring.cq_head;
	if (head == __atomic_load_n(ring.cq_tail, __ATOMIC_ACQUIRE))
		return -EAGAIN;
	const int result = ring.cqes[head & *ring.cq_mask].res;
	__atomic_store_n(ring.cq_head, head + 1, __ATOMIC_RELEASE);
	return result;
}

void pinned_fork()
{
	const size_t size = 4 * page_size;
	auto *buffer = static_cast<unsigned char *>(mmap(nullptr, size,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (buffer == MAP_FAILED) {
		fail("pinned-fork mmap");
		return;
	}
	std::memset(buffer, 0x11, size);
	Ring ring;
	if (!ring_setup(ring)) {
		fail("pinned-fork io_uring setup");
		return;
	}
	iovec vector{buffer, size};
	if (syscall(__NR_io_uring_register, ring.fd, IORING_REGISTER_BUFFERS,
		    &vector, 1) < 0) {
		std::printf("uaccess-fork-isolation: register: %s\n",
			    std::strerror(errno));
		fail("pinned-fork register");
		return;
	}
	const int source = memfd_create("secret", 0);
	unsigned char secret[4096];
	std::memset(secret, 0x5a, sizeof(secret));
	if (source < 0 || write(source, secret, sizeof(secret)) !=
			static_cast<ssize_t>(sizeof(secret))) {
		fail("pinned-fork memfd");
		return;
	}
	int go[2];
	if (pipe(go) != 0) {
		fail("pinned-fork pipe");
		return;
	}
	/* Stores the forking thread makes just before fork(). */
	for (size_t offset = 0; offset < size; offset += 64)
		buffer[offset] = static_cast<unsigned char>(0x20 + offset / 64);

	const pid_t child = fork();
	if (child == 0) {
		char byte;
		close(go[1]);
		if (read(go[0], &byte, 1) != 1)
			_exit(90);
		int bad = 0;
		for (size_t offset = 0; offset < size; ++offset) {
			const unsigned char want = offset % 64 == 0 ?
				static_cast<unsigned char>(0x20 + offset / 64) :
				0x11;
			if (buffer[offset] != want)
				++bad;
		}
		_exit(bad == 0 ? 0 : bad > 80 ? 80 : bad);
	}
	if (child < 0) {
		fail("pinned-fork fork");
		return;
	}
	close(go[0]);
	const int result = ring_read_fixed(ring, source, buffer, 4096);
	if (result != 4096) {
		std::printf("uaccess-fork-isolation: READ_FIXED returned %d\n", result);
		fail("pinned-fork READ_FIXED");
	} else if (std::memcmp(buffer, secret, sizeof(secret)) != 0) {
		fail("pinned-fork parent lost the post-fork kernel write");
	}
	if (write(go[1], "g", 1) != 1)
		fail("pinned-fork go");
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		std::printf("uaccess-fork-isolation: child status 0x%x\n", status);
		fail("pinned-fork child page differs from its fork-time copy");
	} else {
		std::printf("uaccess-fork-isolation: pinned-fork ok\n");
	}
	close(go[1]);
	close(source);
	close(ring.fd);
	munmap(buffer, size);
}

constexpr int writers = 4;
constexpr int forkers = 2;
constexpr int run_seconds = 8;
std::atomic<bool> stop{false};
std::atomic<long> forks{0};
std::atomic<long> changed{0};
unsigned char *stable_pages = nullptr;

void *writer(void *argument)
{
	const long index = reinterpret_cast<long>(argument);
	auto *slot = reinterpret_cast<timespec *>(stable_pages +
		index * page_size);
	while (!stop.load(std::memory_order_relaxed))
		syscall(SYS_clock_gettime, CLOCK_MONOTONIC, slot);
	return nullptr;
}

void *forker(void *)
{
	while (!stop.load(std::memory_order_relaxed)) {
		const pid_t child = fork();
		if (child == 0) {
			timespec first[writers], second[writers];
			for (int index = 0; index < writers; ++index)
				std::memcpy(&first[index], stable_pages +
					    index * page_size, sizeof(timespec));
			const timespec pause{0, 2000000};
			nanosleep(&pause, nullptr);
			for (int index = 0; index < writers; ++index)
				std::memcpy(&second[index], stable_pages +
					    index * page_size, sizeof(timespec));
			_exit(std::memcmp(first, second, sizeof(first)) ? 1 : 0);
		}
		if (child < 0) {
			sched_yield();
			continue;
		}
		int status = 0;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
		forks.fetch_add(1, std::memory_order_relaxed);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			changed.fetch_add(1, std::memory_order_relaxed);
	}
	return nullptr;
}

void child_stable()
{
	stable_pages = static_cast<unsigned char *>(mmap(nullptr,
		writers * page_size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (stable_pages == MAP_FAILED) {
		fail("child-stable mmap");
		return;
	}
	pthread_t threads[writers + forkers];
	for (long index = 0; index < writers; ++index)
		pthread_create(&threads[index], nullptr, writer,
			       reinterpret_cast<void *>(index));
	for (int index = 0; index < forkers; ++index)
		pthread_create(&threads[writers + index], nullptr, forker,
			       nullptr);
	const timespec run{run_seconds, 0};
	nanosleep(&run, nullptr);
	stop.store(true);
	for (auto &thread : threads)
		pthread_join(thread, nullptr);
	std::printf("uaccess-fork-isolation: child-stable %ld changed in %ld forks\n",
		    changed.load(), forks.load());
	if (changed.load() != 0)
		fail("child-stable: a parent's kernel write reached a child");
	if (forks.load() < 100)
		fail("child-stable: fewer than 100 forks ran");
}

} // namespace

int main()
{
	page_size = sysconf(_SC_PAGESIZE);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	pinned_fork();
	child_stable();
	std::printf("uaccess-fork-isolation: %ld failures\n", failures);
	return failures > 100 ? 100 : static_cast<int>(failures);
}
