// SPDX-License-Identifier: GPL-2.0-or-later
// Freestanding arm64/Linux /init of guest-video-late-answer.
//
// The device answers the first stream-parameter query 6.5 s late and never
// answers the second, so the first two opens of /dev/video0 must fail with
// ETIMEDOUT after the driver's 5 s wait, and the third must succeed. Between
// them the init waits for the late answer to land and has SLUB check every
// object of the driver's command cache, which reports a freed object the
// answer wrote into. The unanswered command must stay allocated until the
// driver's unbind takes it back from the device, and the unbind must then
// destroy the empty cache. It prints one "VIDEO_INIT" line per step on the
// console and then powers the guest off.

namespace {

constexpr long nr_mkdirat = 34;
constexpr long nr_faccessat = 48;
constexpr long nr_mount = 40;
constexpr long nr_openat = 56;
constexpr long nr_close = 57;
constexpr long nr_read = 63;
constexpr long nr_write = 64;
constexpr long nr_exit = 93;
constexpr long nr_nanosleep = 101;
constexpr long nr_sync = 81;
constexpr long nr_reboot = 142;

constexpr long at_fdcwd = -100;
constexpr long o_wronly = 1;
constexpr long o_rdwr = 2;
constexpr long enoent = 2;
constexpr long etimedout = 110;

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

[[noreturn]] void power_off()
{
	sys(nr_sync);
	sys(nr_reboot, 0xfee1dead, 672274793, 0x4321fedc);
	for (;;)
		sys(nr_exit, 1);
}

/* Opens /dev/video0 and reports how it went: "ok" or the errno. */
long open_decoder(const char *step)
{
	const long fd = sys(nr_openat, at_fdcwd, ptr("/dev/video0"), o_rdwr);
	out("VIDEO_INIT ");
	out(step);
	if (fd >= 0) {
		out(" ok\n");
		sys(nr_close, fd);
		return 0;
	}
	out(" errno=");
	out_num(-fd);
	out("\n");
	return -fd;
}

/* Writes `value` to the sysfs attribute `path`. */
bool write_attribute(const char *path, const char *value)
{
	const long fd = sys(nr_openat, at_fdcwd, ptr(path), o_wronly);
	if (fd < 0)
		return false;
	const long size = static_cast<long>(length(value));
	const bool written = sys(nr_write, fd, ptr(value), size) == size;
	sys(nr_close, fd);
	return written;
}


/* The command cache's allocated objects, or -1. The driver names each
 * device's cache for it, and the decoder is the guest's virtio0. */
long cache_objects()
{
	const long fd = sys(nr_openat, at_fdcwd,
		ptr("/sys/kernel/slab/virtio-video-virtio0-vbufs/objects"), 0);
	if (fd < 0)
		return -1;
	char text[64] = {};
	const long got = sys(nr_read, fd, ptr(text), sizeof(text) - 1);
	sys(nr_close, fd);
	if (got <= 0 || text[0] < '0' || text[0] > '9')
		return -1;
	long objects = 0;
	for (int at = 0; text[at] >= '0' && text[at] <= '9'; ++at)
		objects = objects * 10 + (text[at] - '0');
	return objects;
}

/* Has SLUB check every object of the command cache, free ones included,
 * and reports how many are allocated. */
void validate(const char *step)
{
	const bool validated = write_attribute(
		"/sys/kernel/slab/virtio-video-virtio0-vbufs/validate", "1");
	out("VIDEO_INIT ");
	out(step);
	out(validated ? " validated objects=" : " validate failed objects=");
	out_num(cache_objects());
	out("\n");
}

} // namespace

extern "C" [[noreturn]] void _start()
{
	sys(nr_mkdirat, at_fdcwd, ptr("/dev"), 0755);
	sys(nr_mkdirat, at_fdcwd, ptr("/sys"), 0755);
	sys(nr_mount, ptr("devtmpfs"), ptr("/dev"), ptr("devtmpfs"), 0, 0);
	sys(nr_mount, ptr("sysfs"), ptr("/sys"), ptr("sysfs"), 0, 0);
	console = sys(nr_openat, at_fdcwd, ptr("/dev/console"), o_wronly);
	out("VIDEO_INIT started\n");

	/* The decoder registers during boot; give a slow host a moment. An
	 * open would already ask for parameters, so only look for the node. */
	for (int attempt = 0; attempt < 100; ++attempt) {
		if (sys(nr_faccessat, at_fdcwd, ptr("/dev/video0"), 0) != -enoent)
			break;
		sleep_ms(100);
	}

	validate("booted");
	const bool first = open_decoder("open1") == etimedout;
	validate("timed-out");
	sleep_ms(3000);
	validate("late-answer");
	const bool second = open_decoder("open2") == etimedout;
	validate("abandoned");
	const bool third = open_decoder("open3") == 0;
	validate("final");

	/* Once the answers to the last commands are reaped, which their
	 * interrupt does a moment after the kick, only the command the device
	 * never answered is still allocated. */
	sleep_ms(1000);
	out("VIDEO_INIT held objects=");
	out_num(cache_objects());
	out("\n");
	out(write_attribute("/sys/bus/virtio/drivers/virtio-video/unbind",
		"virtio0") ? "VIDEO_INIT unbound\n" : "VIDEO_INIT unbind failed\n");
	out(sys(nr_faccessat, at_fdcwd,
		ptr("/sys/kernel/slab/virtio-video-virtio0-vbufs"), 0) == -enoent ?
		"VIDEO_INIT cache destroyed\n" : "VIDEO_INIT cache kept\n");
	out(first && second && third ? "VIDEO_INIT done\n" : "VIDEO_INIT unexpected\n");
	power_off();
}
