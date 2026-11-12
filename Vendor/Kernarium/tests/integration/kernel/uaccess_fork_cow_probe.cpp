// SPDX-License-Identifier: GPL-2.0-or-later
/* A kernel copy to a valid private page never fails with EFAULT while sibling
 * threads fork.
 *
 * Writer threads make system calls that copy their result into a private page
 * only the kernel writes, and two sibling threads fork and reap children
 * without pause.  Every fork write-protects those pages, so each writer's next
 * copy breaks a copy-on-write inside the kernel's uaccess, and the next fork
 * often takes mmap_lock between that fault and the copy.  Native uaccess
 * retries such a fault until it succeeds; an EFAULT here is the spurious kind
 * that made a binder ioctl fail and libbinder abort.  The exit status is the
 * number of failed calls, at most 100.
 */
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int writers = 4;
constexpr int forkers = 2;
constexpr int run_seconds = 8;

std::atomic<bool> stop{false};
std::atomic<long> failures{0};
std::atomic<long> calls{0};
std::atomic<long> forks{0};
int zero_fd = -1;
long page_size = 0;
unsigned char *pages = nullptr;

void fail(const char *call, int error)
{
	if (failures.fetch_add(1) < 10)
		std::printf("FAIL %s: %s\n", call, std::strerror(error));
}

void *write_loop(void *argument)
{
	const long index = reinterpret_cast<long>(argument);
	unsigned char *page = pages + index * page_size;
	for (unsigned long round = 0; !stop.load(std::memory_order_relaxed);
	     ++round) {
		/* Userspace never stores to the page, so the kernel's copy is
		 * always the first write after a fork. */
		unsigned char *target = page + (round * 64) % (page_size - 512);
		switch (round % 4) {
		case 0:
			if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, target))
				fail("clock_gettime", errno);
			break;
		case 1:
			if (read(zero_fd, target, 256) != 256)
				fail("read", errno);
			break;
		case 2:
			if (syscall(SYS_getrusage, RUSAGE_THREAD, target))
				fail("getrusage", errno);
			break;
		default:
			if (syscall(SYS_uname, target))
				fail("uname", errno);
			break;
		}
		calls.fetch_add(1, std::memory_order_relaxed);
	}
	return nullptr;
}

void *fork_loop(void *)
{
	while (!stop.load(std::memory_order_relaxed)) {
		const pid_t child = fork();
		if (child == 0)
			_exit(0);
		if (child < 0) {
			fail("fork", errno);
			continue;
		}
		int status = 0;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
		forks.fetch_add(1, std::memory_order_relaxed);
	}
	return nullptr;
}

} // namespace

int main()
{
	page_size = sysconf(_SC_PAGESIZE);
	zero_fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);
	void *mapping = mmap(nullptr, writers * page_size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (zero_fd < 0 || mapping == MAP_FAILED) {
		std::perror("setup");
		return 100;
	}
	pages = static_cast<unsigned char *>(mapping);
	std::memset(pages, 1, writers * page_size);

	pthread_t threads[writers + forkers];
	for (long i = 0; i < writers; ++i)
		pthread_create(&threads[i], nullptr, write_loop,
			       reinterpret_cast<void *>(i));
	for (int i = 0; i < forkers; ++i)
		pthread_create(&threads[writers + i], nullptr, fork_loop, nullptr);
	for (int second = 0; second < run_seconds && failures.load() < 100;
	     ++second)
		sleep(1);
	stop.store(true);
	for (auto thread : threads)
		pthread_join(thread, nullptr);

	std::printf("uaccess-fork-cow: %ld calls, %ld forks, %ld failures\n",
		    calls.load(), forks.load(), failures.load());
	if (forks.load() < 100) {
		std::printf("FAIL only %ld forks ran\n", forks.load());
		failures.fetch_add(1);
	}
	return failures.load() > 100 ? 100 : static_cast<int>(failures.load());
}
