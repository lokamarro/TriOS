// SPDX-License-Identifier: GPL-2.0-or-later
/* The kernel's own access to a user range reaches userfaultfd as native Linux
 * delivers it: not at all for a descriptor made with UFFD_USER_MODE_ONLY, and
 * as a fault for one made without.
 *
 * A range registered for missing pages is read and written by system calls
 * (write and pread from and into it, and /dev/zero's clear_user) while a
 * helper thread serves the descriptor.  With UFFD_USER_MODE_ONLY the kernel's
 * access gets VM_FAULT_SIGBUS and the call fails with EFAULT, the helper sees
 * no message, and the range stays unpopulated; a store from user mode is
 * delivered, resolved, and then readable by the kernel.  Without the flag
 * the same calls wait for the helper and complete.  UML's uaccess resolves
 * a miss by calling the page-fault handler itself, so a handler told that the
 * fault came from user mode hands Android's userfaultfd test the success it
 * expects to be refused.  The exit status is the number of failed checks.
 */
#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

namespace {

constexpr int pages = 4;

long page_size = 0;
int failures = 0;

void check(bool ok, const char *what, const char *detail = "")
{
	std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, *detail ? ": " : "",
		    detail);
	if (!ok)
		++failures;
}

__attribute__((format(printf, 3, 4)))
void checkf(bool ok, const char *what, const char *format, ...)
{
	char detail[96];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(detail, sizeof detail, format, arguments);
	va_end(arguments);
	check(ok, what, detail);
}

/* One userfaultfd served by a thread: it resolves every missing-page fault
 * with a zero page and counts them. */
struct Uffd {
	int fd = -1;
	char *base = nullptr;
	std::atomic<long> served{0};
	std::atomic<bool> stop{false};
	pthread_t thread{};

	static void *serve(void *argument)
	{
		auto *self = static_cast<Uffd *>(argument);
		while (!self->stop.load()) {
			pollfd waiting{self->fd, POLLIN, 0};
			if (poll(&waiting, 1, 50) <= 0)
				continue;
			uffd_msg message;
			if (read(self->fd, &message, sizeof message) !=
			    static_cast<ssize_t>(sizeof message))
				continue;
			if (message.event != UFFD_EVENT_PAGEFAULT)
				continue;
			uffdio_zeropage zero{};
			zero.range.start = message.arg.pagefault.address &
					   ~static_cast<unsigned long long>(page_size - 1);
			zero.range.len = page_size;
			if (ioctl(self->fd, UFFDIO_ZEROPAGE, &zero) == 0 ||
			    errno == EEXIST)
				self->served.fetch_add(1);
		}
		return nullptr;
	}

	/* Returns 0, or the errno of the step that failed. */
	int open(unsigned flags)
	{
		fd = static_cast<int>(syscall(SYS_userfaultfd,
					      O_CLOEXEC | O_NONBLOCK | flags));
		if (fd < 0)
			return errno;
		uffdio_api api{};
		api.api = UFFD_API;
		if (ioctl(fd, UFFDIO_API, &api))
			return errno;
		void *mapping = mmap(nullptr, pages * page_size,
				     PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping == MAP_FAILED)
			return errno;
		base = static_cast<char *>(mapping);
		uffdio_register reg{};
		reg.range.start = reinterpret_cast<unsigned long>(base);
		reg.range.len = pages * page_size;
		reg.mode = UFFDIO_REGISTER_MODE_MISSING;
		if (ioctl(fd, UFFDIO_REGISTER, &reg))
			return errno;
		if (pthread_create(&thread, nullptr, serve, this))
			return errno;
		return 0;
	}

	void close_all()
	{
		stop.store(true);
		if (thread)
			pthread_join(thread, nullptr);
		if (base)
			munmap(base, pages * page_size);
		if (fd >= 0)
			::close(fd);
	}
};

/* A kernel read of the range: write it to a memfd. */
ssize_t kernel_read(int memfd, const char *from, size_t length, int &error)
{
	errno = 0;
	const ssize_t done = write(memfd, from, length);
	error = errno;
	return done;
}

void user_mode_only()
{
	Uffd uffd;
	const int opened = uffd.open(UFFD_USER_MODE_ONLY);
	checkf(opened == 0, "UFFD_USER_MODE_ONLY userfaultfd is registered",
	       "errno %d", opened);
	if (opened) {
		uffd.close_all();
		return;
	}
	const int memfd = static_cast<int>(syscall(SYS_memfd_create, "uffd", 0));
	const int zero = open("/dev/zero", O_RDONLY | O_CLOEXEC);
	check(memfd >= 0 && zero >= 0, "descriptors for the kernel's access");

	int error = 0;
	ssize_t done = kernel_read(memfd, uffd.base, page_size, error);
	checkf(done == -1 && error == EFAULT,
	       "write() from a registered page fails with EFAULT",
	       "returned %zd, errno %d", done, error);

	/* The second page, entered through a copy that starts mid-page. */
	done = kernel_read(memfd, uffd.base + page_size + 100, 3 * page_size / 2,
			   error);
	checkf(done == -1 && error == EFAULT,
	       "write() from the middle of a registered range fails with EFAULT",
	       "returned %zd, errno %d", done, error);

	/* An empty memfd reads zero bytes without touching the buffer, so give
	 * it contents that make the copy happen. */
	char seed[64];
	std::memset(seed, 7, sizeof seed);
	check(write(memfd, seed, sizeof seed) == static_cast<ssize_t>(sizeof seed),
	      "memfd is seeded");
	errno = 0;
	done = pread(memfd, uffd.base + 2 * page_size, 16, 0);
	error = errno;
	checkf(done == -1 && error == EFAULT,
	       "pread() into a registered page fails with EFAULT",
	       "returned %zd, errno %d", done, error);

	errno = 0;
	done = read(zero, uffd.base + 3 * page_size, 64);
	error = errno;
	checkf(done == -1 && error == EFAULT,
	       "read() of /dev/zero into a registered page fails with EFAULT",
	       "returned %zd, errno %d", done, error);

	const long kernel_messages = uffd.served.load();
	checkf(kernel_messages == 0,
	       "the kernel's failed accesses reached no userfaultfd message",
	       "%ld served", kernel_messages);

	/* A page outside every registration is demand-paged as usual. */
	void *plain = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	done = kernel_read(memfd, static_cast<char *>(plain), page_size, error);
	checkf(done == page_size, "write() from an unregistered page completes",
	       "returned %zd", done);
	munmap(plain, page_size);

	/* A store from user mode is delivered and resolved. */
	*reinterpret_cast<volatile char *>(uffd.base) = 1;
	checkf(uffd.served.load() == kernel_messages + 1,
	       "a user-mode store is delivered to userfaultfd", "%ld served",
	       uffd.served.load());
	done = kernel_read(memfd, uffd.base, page_size, error);
	checkf(done == page_size, "write() from the resolved page completes",
	       "returned %zd, errno %d", done, error);
	checkf(uffd.served.load() == kernel_messages + 1,
	       "the resolved page needed no further message", "%ld served",
	       uffd.served.load());

	/* A user-mode load of another page is delivered too. */
	const char loaded =
		*reinterpret_cast<volatile char *>(uffd.base + page_size);
	checkf(loaded == 0 && uffd.served.load() == kernel_messages + 2,
	       "a user-mode load is delivered to userfaultfd",
	       "%ld served, byte %d", uffd.served.load(), loaded);

	close(memfd);
	close(zero);
	uffd.close_all();
}

void kernel_visible()
{
	Uffd uffd;
	const int opened = uffd.open(0);
	checkf(opened == 0, "plain userfaultfd is registered", "errno %d", opened);
	if (opened) {
		uffd.close_all();
		return;
	}
	const int memfd = static_cast<int>(syscall(SYS_memfd_create, "uffd", 0));
	int error = 0;
	const ssize_t done = kernel_read(memfd, uffd.base, 2 * page_size, error);
	checkf(done == 2 * page_size,
	       "write() from a registered page waits for the handler and completes",
	       "returned %zd, errno %d", done, error);
	checkf(uffd.served.load() == 2,
	       "the kernel's accesses are delivered as faults", "%ld served",
	       uffd.served.load());
	close(memfd);
	uffd.close_all();
}

} // namespace

int main()
{
	page_size = sysconf(_SC_PAGESIZE);
	/* A fault the kernel wrongly waits on would hang the run. */
	alarm(60);
	user_mode_only();
	kernel_visible();
	std::printf("uaccess-userfaultfd: %d failures\n", failures);
	return failures > 100 ? 100 : failures;
}
