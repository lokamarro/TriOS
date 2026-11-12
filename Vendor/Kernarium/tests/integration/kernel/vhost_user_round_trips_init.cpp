// SPDX-License-Identifier: GPL-2.0-or-later
// Freestanding arm64/Linux /init of guest-vhost-user-round-trips.
//
// The host serves a single-port Virtio console that keeps changing its size
// and answers each configuration read late. Every change sends the guest's
// console driver to read the new size, a GET_CONFIG round trip on the
// device's vhost-user socket, while a writer pinned to each vCPU keeps
// kicking the console's output queue, an in-band kick round trip on the same
// socket. The init lets them run for a while, then reports whether the
// console is still bound to its driver, one "ROUND_TRIPS" line per step, and
// powers the guest off.

namespace {

constexpr long nr_mkdirat = 34;
constexpr long nr_mount = 40;
constexpr long nr_faccessat = 48;
constexpr long nr_openat = 56;
constexpr long nr_write = 64;
constexpr long nr_exit_group = 94;
constexpr long nr_nanosleep = 101;
constexpr long nr_clock_gettime = 113;
constexpr long nr_sched_setaffinity = 122;
constexpr long nr_sync = 81;
constexpr long nr_reboot = 142;
constexpr long nr_clone = 220;
constexpr long nr_wait4 = 260;

constexpr long at_fdcwd = -100;
constexpr long o_wronly = 1;
constexpr long o_noctty = 0400;
constexpr long o_nonblock = 04000;
constexpr long sigchld = 17;
constexpr long wnohang = 1;
constexpr long clock_monotonic = 1;
constexpr long eagain = 11;

/* The host boots this many vCPUs; one writer runs on each. */
constexpr int writers = 4;
constexpr long run_seconds = 6;
/* How long the writers may outlast their deadline before the init stops
 * waiting for them: a wedged transport must still end in a report. */
constexpr long grace_seconds = 20;

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0,
		long a3 = 0, long a4 = 0)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
			 : "memory", "cc");
	return x0;
}

template <typename T> long ptr(T *p) { return reinterpret_cast<long>(p); }

long console = -1;

unsigned long length(const char *s)
{
	unsigned long n = 0;
	while (s[n])
		n++;
	return n;
}

void out(const char *s)
{
	sys(nr_write, console, ptr(s), static_cast<long>(length(s)));
}

void out_num(long value)
{
	char buffer[24];
	int at = 23;
	buffer[at] = 0;
	unsigned long v = value < 0 ? -static_cast<unsigned long>(value) : value;
	do {
		buffer[--at] = static_cast<char>('0' + v % 10);
		v /= 10;
	} while (v);
	if (value < 0)
		buffer[--at] = '-';
	out(buffer + at);
}

void sleep_ms(long milliseconds)
{
	const long request[2] = {milliseconds / 1000,
				 (milliseconds % 1000) * 1000000};
	sys(nr_nanosleep, ptr(request), 0);
}

long now_seconds()
{
	long time[2] = {};
	sys(nr_clock_gettime, clock_monotonic, ptr(time));
	return time[0];
}

bool exists(const char *path)
{
	return sys(nr_faccessat, at_fdcwd, ptr(path), 0) == 0;
}

[[noreturn]] void power_off()
{
	sys(nr_sync);
	sys(nr_reboot, 0xfee1dead, 672274793, 0x4321fedc);
	for (;;)
		sys(nr_exit_group, 1);
}

/* Keeps the console's output queue busy from one vCPU until the deadline.
 * The descriptor does not block, so a console that stops taking output
 * cannot hold a writer past it. Exits 0 once it wrote anything. */
[[noreturn]] void write_until(int cpu, long deadline)
{
	const unsigned long mask = 1UL << cpu;
	sys(nr_sched_setaffinity, 0, sizeof(mask), ptr(&mask));
	const long fd = sys(nr_openat, at_fdcwd, ptr("/dev/hvc0"),
			    o_wronly | o_noctty | o_nonblock);
	if (fd < 0)
		sys(nr_exit_group, 2);
	static const char line[] =
		"vhost-user round trips: an in-band kick per write\n";
	long written = 0;
	while (now_seconds() < deadline) {
		const long result = sys(nr_write, fd, ptr(line),
					static_cast<long>(sizeof(line) - 1));
		if (result > 0)
			written += result;
		else if (result != -eagain)
			sys(nr_exit_group, 3);
	}
	sys(nr_exit_group, written > 0 ? 0 : 4);
	for (;;)
		;
}

} // namespace

extern "C" [[noreturn]] void _start()
{
	sys(nr_mkdirat, at_fdcwd, ptr("/dev"), 0755);
	sys(nr_mkdirat, at_fdcwd, ptr("/sys"), 0755);
	sys(nr_mount, ptr("devtmpfs"), ptr("/dev"), ptr("devtmpfs"), 0, 0);
	sys(nr_mount, ptr("sysfs"), ptr("/sys"), ptr("sysfs"), 0, 0);
	console = sys(nr_openat, at_fdcwd, ptr("/dev/console"), o_wronly);
	out("ROUND_TRIPS started\n");

	for (int attempt = 0; attempt < 100 && !exists("/dev/hvc0"); ++attempt)
		sleep_ms(100);
	if (!exists("/dev/hvc0")) {
		out("ROUND_TRIPS no console\n");
		power_off();
	}

	const long deadline = now_seconds() + run_seconds;
	long children[writers] = {};
	for (int cpu = 0; cpu < writers; ++cpu) {
		children[cpu] = sys(nr_clone, sigchld, 0, 0, 0, 0);
		if (children[cpu] == 0)
			write_until(cpu, deadline);
	}
	out("ROUND_TRIPS writers started\n");

	int finished = 0;
	int succeeded = 0;
	while (finished < writers && now_seconds() < deadline + grace_seconds) {
		for (int cpu = 0; cpu < writers; ++cpu) {
			if (children[cpu] <= 0)
				continue;
			int status = 0;
			if (sys(nr_wait4, children[cpu], ptr(&status), wnohang,
				0) != children[cpu])
				continue;
			children[cpu] = 0;
			++finished;
			if (status == 0)
				++succeeded;
		}
		sleep_ms(100);
	}
	out("ROUND_TRIPS writers finished=");
	out_num(finished);
	out(" succeeded=");
	out_num(succeeded);
	out("\n");

	out(exists("/sys/bus/virtio/drivers/virtio_console/virtio0") ?
		    "ROUND_TRIPS bound\n" : "ROUND_TRIPS unbound\n");
	out("ROUND_TRIPS done\n");
	power_off();
}
