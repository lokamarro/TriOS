// SPDX-License-Identifier: GPL-2.0-or-later
// Freestanding arm64/Linux /init of guest-video-error-answer.
//
// The device answers the capture buffer this init queues as a failed one,
// naming the timestamp 3.141592 s, as a device does for a picture it could not
// deliver. The buffer must come back from VIDIOC_DQBUF flagged
// V4L2_BUF_FLAG_ERROR with no payload and that timestamp, not the one the
// buffer carried when it was queued. It prints one "VIDEO_INIT" line per step
// on the console and then powers the guest off.

namespace {

constexpr long nr_mkdirat = 34;
constexpr long nr_mount = 40;
constexpr long nr_faccessat = 48;
constexpr long nr_openat = 56;
constexpr long nr_close = 57;
constexpr long nr_write = 64;
constexpr long nr_ioctl = 29;
constexpr long nr_exit = 93;
constexpr long nr_nanosleep = 101;
constexpr long nr_sync = 81;
constexpr long nr_reboot = 142;

constexpr long at_fdcwd = -100;
constexpr long o_wronly = 1;
constexpr long o_rdwr = 2;
constexpr long o_nonblock = 04000;
constexpr long enoent = 2;
constexpr long eagain = 11;

/* The V4L2 UAPI this test needs, as the arm64 kernel lays it out. */
constexpr unsigned buf_type_capture = 9;  /* CAPTURE_MPLANE */
constexpr unsigned buf_type_output = 10;  /* OUTPUT_MPLANE */
constexpr unsigned memory_mmap = 1;
constexpr unsigned buf_flag_error = 0x40;
constexpr unsigned pix_fmt_h264 = 'H' | '2' << 8 | '6' << 16 | 4u << 24;

struct plane_pix_format {
	unsigned sizeimage;
	unsigned bytesperline;
	unsigned short reserved[6];
} __attribute__((packed));

struct pix_format_mplane {
	unsigned width;
	unsigned height;
	unsigned pixelformat;
	unsigned field;
	unsigned colorspace;
	plane_pix_format plane_fmt[8];
	unsigned char num_planes;
	unsigned char flags;
	unsigned char ycbcr_enc;
	unsigned char quantization;
	unsigned char xfer_func;
	unsigned char reserved[7];
} __attribute__((packed));

struct format {
	unsigned type;
	union {
		pix_format_mplane pix_mp;
		unsigned char raw_data[200];
		unsigned long align;
	} fmt;
};

struct requestbuffers {
	unsigned count;
	unsigned type;
	unsigned memory;
	unsigned capabilities;
	unsigned char flags;
	unsigned char reserved[3];
};

struct plane {
	unsigned bytesused;
	unsigned length;
	unsigned long mem_offset;
	unsigned data_offset;
	unsigned reserved[11];
};

struct buffer {
	unsigned index;
	unsigned type;
	unsigned bytesused;
	unsigned flags;
	unsigned field;
	long tv_sec;
	long tv_usec;
	unsigned char timecode[16];
	unsigned sequence;
	unsigned memory;
	plane *planes;
	unsigned length;
	unsigned reserved2;
	int request_fd;
};

static_assert(sizeof(format) == 208);
static_assert(sizeof(requestbuffers) == 20);
static_assert(sizeof(plane) == 64);
static_assert(sizeof(buffer) == 88);

constexpr unsigned long ioc(unsigned direction, unsigned number,
			    unsigned long size)
{
	return static_cast<unsigned long>(direction) << 30 | size << 16 |
	       static_cast<unsigned long>('V') << 8 | number;
}
constexpr unsigned long vidioc_g_fmt = ioc(3, 4, sizeof(format));
constexpr unsigned long vidioc_s_fmt = ioc(3, 5, sizeof(format));
constexpr unsigned long vidioc_reqbufs = ioc(3, 8, sizeof(requestbuffers));
constexpr unsigned long vidioc_qbuf = ioc(3, 15, sizeof(buffer));
constexpr unsigned long vidioc_dqbuf = ioc(3, 17, sizeof(buffer));
constexpr unsigned long vidioc_streamon = ioc(1, 18, sizeof(int));

/* What the device names on the failed buffer: 3.141592 s. */
constexpr long failed_sec = 3;
constexpr long failed_usec = 141592;

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

/* Zeroes a structure without a call into a library this init lacks. */
template <typename T> void zero(T &object)
{
	volatile unsigned char *bytes =
		reinterpret_cast<volatile unsigned char *>(&object);
	for (unsigned long at = 0; at < sizeof(T); ++at)
		bytes[at] = 0;
}

/* Reports one step's outcome, "VIDEO_INIT <step> ok" or its errno, and
 * stops the guest when it failed. */
void must(const char *name, long result)
{
	out("VIDEO_INIT ");
	out(name);
	if (result >= 0) {
		out(" ok\n");
		return;
	}
	out(" errno=");
	out_num(-result);
	out("\nVIDEO_INIT unexpected\n");
	power_off();
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

	/* The decoder registers during boot; give a slow host a moment. */
	for (int attempt = 0; attempt < 100; ++attempt) {
		if (sys(nr_faccessat, at_fdcwd, ptr("/dev/video0"), 0) != -enoent)
			break;
		sleep_ms(100);
	}
	const long fd = sys(nr_openat, at_fdcwd, ptr("/dev/video0"),
			    o_rdwr | o_nonblock);
	must("open", fd);

	/* The coded format and the raw format the driver offers for it. */
	format coded;
	zero(coded);
	coded.type = buf_type_output;
	coded.fmt.pix_mp.width = 128;
	coded.fmt.pix_mp.height = 96;
	coded.fmt.pix_mp.pixelformat = pix_fmt_h264;
	coded.fmt.pix_mp.num_planes = 1;
	coded.fmt.pix_mp.plane_fmt[0].sizeimage = 1 << 16;
	must("S_FMT coded", sys(nr_ioctl, fd, vidioc_s_fmt, ptr(&coded)));
	format raw;
	zero(raw);
	raw.type = buf_type_capture;
	must("G_FMT raw", sys(nr_ioctl, fd, vidioc_g_fmt, ptr(&raw)));
	must("S_FMT raw", sys(nr_ioctl, fd, vidioc_s_fmt, ptr(&raw)));

	requestbuffers request;
	zero(request);
	request.count = 1;
	request.type = buf_type_output;
	request.memory = memory_mmap;
	must("REQBUFS coded", sys(nr_ioctl, fd, vidioc_reqbufs, ptr(&request)));
	zero(request);
	request.count = 1;
	request.type = buf_type_capture;
	request.memory = memory_mmap;
	must("REQBUFS raw", sys(nr_ioctl, fd, vidioc_reqbufs, ptr(&request)));

	/* Queue every buffer, since the driver starts the queue only once the
	 * minimum it needs is queued. Each carries a timestamp other than the
	 * one the device will name, so that a buffer returned without the
	 * device's own is told from one that carries it. */
	for (unsigned index = 0; index < request.count && index < 16; ++index) {
		plane planes[8];
		zero(planes);
		buffer queued;
		zero(queued);
		queued.type = buf_type_capture;
		queued.memory = memory_mmap;
		queued.index = index;
		queued.length = raw.fmt.pix_mp.num_planes;
		queued.planes = planes;
		queued.tv_sec = 7;
		queued.tv_usec = 7;
		must("QBUF raw", sys(nr_ioctl, fd, vidioc_qbuf, ptr(&queued)));
	}
	unsigned type = buf_type_output;
	must("STREAMON coded", sys(nr_ioctl, fd, vidioc_streamon, ptr(&type)));
	type = buf_type_capture;
	must("STREAMON raw", sys(nr_ioctl, fd, vidioc_streamon, ptr(&type)));

	/* The device fails each buffer as soon as it is queued. */
	long result = -eagain;
	buffer done;
	plane done_planes[8];
	for (int attempt = 0; attempt < 200 && result == -eagain; ++attempt) {
		zero(done);
		zero(done_planes);
		done.type = buf_type_capture;
		done.memory = memory_mmap;
		done.length = raw.fmt.pix_mp.num_planes;
		done.planes = done_planes;
		result = sys(nr_ioctl, fd, vidioc_dqbuf, ptr(&done));
		if (result == -eagain)
			sleep_ms(50);
	}
	must("DQBUF raw", result);

	const bool failed = (done.flags & buf_flag_error) != 0;
	const bool empty = done_planes[0].bytesused == 0;
	const bool named = done.tv_sec == failed_sec && done.tv_usec == failed_usec;
	out("VIDEO_INIT failed buffer error=");
	out_num(failed);
	out(" empty=");
	out_num(empty);
	out(" timestamp=");
	out_num(done.tv_sec);
	out(".");
	out_num(done.tv_usec);
	out(named ? " named\n" : " unnamed\n");
	sys(nr_close, fd);
	out(failed && empty && named ? "VIDEO_INIT done\n"
				     : "VIDEO_INIT unexpected\n");
	power_off();
}
