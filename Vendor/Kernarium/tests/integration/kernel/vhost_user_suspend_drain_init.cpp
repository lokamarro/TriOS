// SPDX-License-Identifier: GPL-2.0-or-later
// Freestanding arm64/Linux /init of guest-vhost-user-suspend-drain.
//
// The host serves a single-port Virtio console that holds the receive
// buffers its driver posts and raises a size change whenever the transport
// resets it. The init waits for the console, arms an RTC wake alarm, and
// suspends the guest from CPU 0: the suspend disables the console's rings
// there, the first disable resets the device, and the guest takes the change
// that follows inside the reply wait of the second. The suspend must still
// end at the alarm. One "SUSPEND_DRAIN" line per step, then power off.

namespace {

constexpr long nr_mkdirat = 34;
constexpr long nr_mount = 40;
constexpr long nr_faccessat = 48;
constexpr long nr_openat = 56;
constexpr long nr_close = 57;
constexpr long nr_write = 64;
constexpr long nr_exit_group = 94;
constexpr long nr_nanosleep = 101;
constexpr long nr_sched_setaffinity = 122;
constexpr long nr_sync = 81;
constexpr long nr_reboot = 142;

constexpr long at_fdcwd = -100;
constexpr long o_wronly = 1;

/* The alarm that ends the suspend, in seconds from when it is armed. */
constexpr char wake_alarm[] = "+3";

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

bool exists(const char *path)
{
	return sys(nr_faccessat, at_fdcwd, ptr(path), 0) == 0;
}

/* Writes `text` to the sysfs file at `path`; the write's result. */
long write_file(const char *path, const char *text)
{
	const long fd = sys(nr_openat, at_fdcwd, ptr(path), o_wronly);
	if (fd < 0)
		return fd;
	const long result =
		sys(nr_write, fd, ptr(text), static_cast<long>(length(text)));
	sys(nr_close, fd);
	return result;
}

[[noreturn]] void power_off()
{
	sys(nr_sync);
	sys(nr_reboot, 0xfee1dead, 672274793, 0x4321fedc);
	for (;;)
		sys(nr_exit_group, 1);
}

} // namespace

extern "C" [[noreturn]] void _start()
{
	sys(nr_mkdirat, at_fdcwd, ptr("/dev"), 0755);
	sys(nr_mkdirat, at_fdcwd, ptr("/sys"), 0755);
	sys(nr_mount, ptr("devtmpfs"), ptr("/dev"), ptr("devtmpfs"), 0, 0);
	sys(nr_mount, ptr("sysfs"), ptr("/sys"), ptr("sysfs"), 0, 0);
	console = sys(nr_openat, at_fdcwd, ptr("/dev/console"), o_wronly);
	out("SUSPEND_DRAIN started\n");

	/* The port's receive buffers are posted once its device exists. */
	for (int attempt = 0; attempt < 100 && !exists("/dev/hvc0"); ++attempt)
		sleep_ms(100);
	if (!exists("/dev/hvc0")) {
		out("SUSPEND_DRAIN no console\n");
		power_off();
	}

	/* The suspend runs in this task, and CPU 0 is the one that would
	 * otherwise take the console's backend request as an interrupt. */
	const unsigned long mask = 1;
	sys(nr_sched_setaffinity, 0, sizeof(mask), ptr(&mask));
	const long armed =
		write_file("/sys/class/rtc/rtc0/wakealarm", wake_alarm);
	if (armed < 0) {
		out("SUSPEND_DRAIN no alarm ");
		out_num(armed);
		out("\n");
		power_off();
	}
	out("SUSPEND_DRAIN suspending\n");
	const long suspended = write_file("/sys/power/state", "mem");
	out("SUSPEND_DRAIN resumed result=");
	out_num(suspended);
	out("\n");

	out(exists("/sys/bus/virtio/drivers/virtio_console/virtio0") ?
		    "SUSPEND_DRAIN bound\n" : "SUSPEND_DRAIN unbound\n");
	out("SUSPEND_DRAIN done\n");
	power_off();
}
