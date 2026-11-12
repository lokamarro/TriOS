// SPDX-License-Identifier: GPL-2.0-or-later
// Freestanding arm64/Linux probe: a process whose address space holds many
// dirty shared VMAs exits in time that grows with their number, not with its
// square.
//
// A child maps N one-page MAP_SHARED|MAP_ANONYMOUS regions, each backed by
// its own shmem file and so its own VMA, and dirties every page. Tearing the
// mm down then forces an mmu_gather flush in every page table it zaps, since
// a dirty shared page needs its rmap removed after the TLB flush. A flush
// that walks all N VMAs on the host made the exit cost N * N walk steps
// while it held the page-table lock; the exit must cost the same
// per-VMA work as the mappings did.
//
// Usage: exit_shared_vmas [N [LIMIT_MS]]
// N defaults to 20000 and LIMIT_MS to 10000. The exit takes tens of
// milliseconds when linear and tens of seconds at N = 20000 when quadratic.
// Output: "RESULT exit-shared-vmas PASS|FAIL n=<N> exit_ms=<ms> limit_ms=<ms>".
// The exit status is 0 on PASS.

namespace {

using u32 = unsigned int;
using u64 = unsigned long;

constexpr long nr_read = 63;
constexpr long nr_write = 64;
constexpr long nr_pipe2 = 59;
constexpr long nr_exit_group = 94;
constexpr long nr_clock_gettime = 113;
constexpr long nr_clone = 220;
constexpr long nr_mmap = 222;
constexpr long nr_wait4 = 260;

constexpr long prot_read = 1;
constexpr long prot_write = 2;
constexpr long map_shared = 1;
constexpr long map_anonymous = 0x20;
constexpr long sigchld = 17;
constexpr long clock_monotonic = 1;

constexpr u64 default_count = 20000;
constexpr u64 default_limit_ms = 10000;
constexpr u64 page_size = 4096;

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0,
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
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}

template <typename T> long ptr(T *p) { return reinterpret_cast<long>(p); }

unsigned long length(const char *s)
{
	unsigned long n = 0;
	while (s[n])
		n++;
	return n;
}

void out(const char *s) { sys(nr_write, 1, ptr(s), static_cast<long>(length(s))); }

void out_num(u64 value)
{
	char buffer[24];
	int at = 23;
	buffer[at] = 0;
	do {
		buffer[--at] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value);
	out(buffer + at);
}

u64 parse(const char *s, u64 fallback)
{
	if (!s || !*s)
		return fallback;
	u64 value = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return fallback;
		value = value * 10 + static_cast<u64>(*s - '0');
	}
	return value;
}

u64 now_ms()
{
	struct {
		long seconds;
		long nanoseconds;
	} t{};
	sys(nr_clock_gettime, clock_monotonic, ptr(&t));
	return static_cast<u64>(t.seconds) * 1000 +
	       static_cast<u64>(t.nanoseconds) / 1000000;
}

bool read_exact(int fd, void *buffer, unsigned long count)
{
	auto *bytes = static_cast<unsigned char *>(buffer);
	while (count) {
		long got = sys(nr_read, fd, ptr(bytes), static_cast<long>(count));
		if (got <= 0)
			return false;
		bytes += got;
		count -= static_cast<unsigned long>(got);
	}
	return true;
}

bool write_exact(int fd, const void *buffer, unsigned long count)
{
	auto *bytes = static_cast<const unsigned char *>(buffer);
	while (count) {
		long put = sys(nr_write, fd, ptr(bytes), static_cast<long>(count));
		if (put <= 0)
			return false;
		bytes += put;
		count -= static_cast<unsigned long>(put);
	}
	return true;
}

// The child: map, dirty, report how many regions it holds, wait for the go
// byte and exit, which is what the parent times.
[[noreturn]] void child(u64 count, int up, int down)
{
	u64 mapped = 0;
	for (; mapped < count; mapped++) {
		long address = sys(nr_mmap, 0, static_cast<long>(page_size),
				   prot_read | prot_write,
				   map_shared | map_anonymous, -1, 0);
		if (address < 0 && address > -4096)
			break;
		*reinterpret_cast<volatile unsigned char *>(address) = 1;
	}
	char go = 0;
	if (write_exact(up, &mapped, sizeof(mapped)))
		read_exact(down, &go, 1);
	sys(nr_exit_group, mapped == count && go ? 0 : 1);
	__builtin_unreachable();
}

int run(long *sp)
{
	long argc = sp[0];
	char **argv = reinterpret_cast<char **>(sp + 1);
	u64 count = parse(argc > 1 ? argv[1] : nullptr, default_count);
	u64 limit_ms = parse(argc > 2 ? argv[2] : nullptr, default_limit_ms);

	int up[2], down[2];
	if (sys(nr_pipe2, ptr(up), 0) < 0 || sys(nr_pipe2, ptr(down), 0) < 0) {
		out("RESULT exit-shared-vmas FAIL pipe\n");
		return 1;
	}
	long pid = sys(nr_clone, sigchld, 0, 0, 0, 0);
	if (pid < 0) {
		out("RESULT exit-shared-vmas FAIL clone\n");
		return 1;
	}
	if (pid == 0)
		child(count, up[1], down[0]);

	u64 mapped = 0;
	bool ok = read_exact(up[0], &mapped, sizeof(mapped));
	u64 start = now_ms();
	char go = 1;
	ok = ok && write_exact(down[1], &go, 1);
	int status = 0;
	long reaped = sys(nr_wait4, pid, ptr(&status), 0, 0);
	u64 exit_ms = now_ms() - start;

	bool pass = ok && reaped == pid && status == 0 && mapped == count &&
		    exit_ms <= limit_ms;
	out("RESULT exit-shared-vmas ");
	out(pass ? "PASS" : "FAIL");
	out(" n=");
	out_num(count);
	out(" mapped=");
	out_num(mapped);
	out(" status=");
	out_num(static_cast<u64>(status));
	out(" exit_ms=");
	out_num(exit_ms);
	out(" limit_ms=");
	out_num(limit_ms);
	out("\n");
	return pass ? 0 : 1;
}

} // namespace

extern "C" __attribute__((noreturn, used)) void probe_main(long *sp)
{
	int status = run(sp);
	sys(nr_exit_group, status);
	__builtin_unreachable();
}

__asm__(".text\n"
	".global _start\n"
	"_start:\n"
	"	mov x0, sp\n"
	"	and x1, x0, #-16\n"
	"	mov sp, x1\n"
	"	bl probe_main\n");
