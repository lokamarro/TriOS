// SPDX-License-Identifier: GPL-2.0-or-later
/* A mapping that permits writes permits reads, as on native arm64 and x86
 * Linux.
 *
 * Neither architecture can map a page writable but not readable, so a load
 * from a PROT_WRITE mapping succeeds there, for a task and for the kernel's
 * own access through a system call.  An execute-only mapping is readable too,
 * as on arm64 without EPAN and on x86.  A PROT_NONE mapping and a store to a
 * read-only one still fault with SEGV_ACCERR.  Each row runs in a child, so a
 * row that dies of SIGSEGV is reported as a failure of that row.  The exit
 * status is the number of failed checks.
 */
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int pages = 2;
constexpr unsigned char pattern = 0xa5;

/* Child exit codes beyond a row's own 0 (pass) and 1 (wrong value). */
constexpr int segv_access_error = 42;
constexpr int segv_other = 43;

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

void on_segv(int, siginfo_t *info, void *)
{
	_exit(info->si_code == SEGV_ACCERR ? segv_access_error : segv_other);
}

/* Run the body in a child and return its exit code, or 128 plus the signal
 * that killed it. */
template <typename Body>
int in_child(Body body)
{
	std::fflush(stdout);
	const pid_t child = fork();
	if (child < 0)
		return -1;
	if (child == 0) {
		struct sigaction action {};
		action.sa_sigaction = on_segv;
		action.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &action, nullptr);
		_exit(body());
	}
	int status = 0;
	if (waitpid(child, &status, 0) != child)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void *map(int prot, int flags, int fd = -1)
{
	void *address = mmap(nullptr, page_size * pages, prot, flags, fd, 0);
	return address == MAP_FAILED ? nullptr : address;
}

/* A file of two pages of `pattern` on the guest's tmpfs. */
int pattern_file()
{
	char path[] = "/tmp/write-only-XXXXXX";
	const int fd = mkstemp(path);
	if (fd < 0)
		return -1;
	unlink(path);
	unsigned char block[4096];
	std::memset(block, pattern, sizeof block);
	for (long done = 0; done < page_size * pages; done += sizeof block)
		if (write(fd, block, sizeof block) !=
		    static_cast<ssize_t>(sizeof block)) {
			close(fd);
			return -1;
		}
	return fd;
}

/* The row's own result: 0 when the loads read what the mapping holds. */
int loads(const void *address, unsigned char expected)
{
	const volatile unsigned char *bytes =
		static_cast<const volatile unsigned char *>(address);
	return bytes[0] == expected && bytes[page_size] == expected ? 0 : 1;
}

void expect_pass(const char *what, int result)
{
	checkf(result == 0, what, "child result %d", result);
}

}  // namespace

int main()
{
	page_size = sysconf(_SC_PAGESIZE);

	/* The load that crashed Piano Tiles 2: the first touch of a mapping
	 * that permits writes only. */
	const int anonymous = MAP_PRIVATE | MAP_ANONYMOUS;
	void *write_only = map(PROT_WRITE, anonymous);
	check(write_only != nullptr, "map write-only anonymous");
	if (write_only)
		expect_pass("load from untouched write-only private anonymous",
			    in_child([&] { return loads(write_only, 0); }));

	write_only = map(PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS);
	check(write_only != nullptr, "map write-only shared anonymous");
	if (write_only)
		expect_pass("load from untouched write-only shared anonymous",
			    in_child([&] { return loads(write_only, 0); }));

	const int file = pattern_file();
	check(file >= 0, "create the pattern file");
	if (file >= 0) {
		void *shared = map(PROT_WRITE, MAP_SHARED, file);
		check(shared != nullptr, "map write-only shared file");
		if (shared)
			expect_pass("load from write-only shared file",
				    in_child([&] { return loads(shared, pattern); }));

		void *priv = map(PROT_WRITE, MAP_PRIVATE, file);
		check(priv != nullptr, "map write-only private file");
		if (priv) {
			expect_pass("load from write-only private file",
				    in_child([&] { return loads(priv, pattern); }));
			/* The store after the load breaks copy-on-write
			 * without touching the file. */
			expect_pass("store after the load stays private",
				    in_child([&] {
					    volatile unsigned char *bytes =
						    static_cast<volatile unsigned char *>(priv);
					    bytes[0] = 0x11;
					    return bytes[0] == 0x11 ? 0 : 1;
				    }));
			unsigned char first = 0;
			check(pread(file, &first, 1, 0) == 1 && first == pattern,
			      "a private store leaves the file alone");
		}
	}

	/* Stores keep working, and a load after one reads it back. */
	write_only = map(PROT_WRITE, anonymous);
	if (write_only)
		expect_pass("store then load on write-only anonymous",
			    in_child([&] {
				    volatile unsigned char *bytes =
					    static_cast<volatile unsigned char *>(write_only);
				    bytes[page_size] = 0x5a;
				    return bytes[page_size] == 0x5a ? 0 : 1;
			    }));

	/* A load from a page that was readable and became write-only. */
	void *changed = map(PROT_READ | PROT_WRITE, anonymous);
	if (changed) {
		const int result = in_child([&] {
			volatile unsigned char *bytes =
				static_cast<volatile unsigned char *>(changed);
			bytes[0] = 0x7e;
			if (mprotect(changed, page_size * pages, PROT_WRITE))
				return 2;
			return bytes[0] == 0x7e ? 0 : 1;
		});
		expect_pass("load after mprotect to write-only", result);
	}

	/* The kernel's own load from the mapping, as write() does it. */
	write_only = map(PROT_WRITE, anonymous);
	int channel[2];
	if (write_only && pipe(channel) == 0) {
		const ssize_t sent = write(channel[1], write_only, 16);
		checkf(sent == 16, "write() from write-only memory",
		       "returned %zd, errno %d", sent, sent == 16 ? 0 : errno);
		close(channel[0]);
		close(channel[1]);
	}

	/* Execute-only follows arm64 without EPAN and x86: it reads. */
	void *execute_only = map(PROT_EXEC, anonymous);
	check(execute_only != nullptr, "map execute-only anonymous");
	if (execute_only)
		expect_pass("load from execute-only anonymous",
			    in_child([&] { return loads(execute_only, 0); }));

	/* What stays refused. */
	void *none = map(PROT_NONE, anonymous);
	if (none) {
		const int result = in_child([&] { return loads(none, 0); });
		checkf(result == segv_access_error, "load from PROT_NONE",
		       "child result %d, want SEGV_ACCERR", result);
	}
	void *read_only = map(PROT_READ, anonymous);
	if (read_only) {
		const int result = in_child([&] {
			static_cast<volatile unsigned char *>(read_only)[0] = 1;
			return 0;
		});
		checkf(result == segv_access_error, "store to read-only",
		       "child result %d, want SEGV_ACCERR", result);
	}

	std::printf("%d failure(s)\n", failures);
	return failures;
}
