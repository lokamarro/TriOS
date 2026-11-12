// SPDX-License-Identifier: GPL-2.0-or-later
// Freestanding arm64/Linux probes of kernel/user shared memory coherence.
//
// Each probe shares a buffer between a kernel alias and a user mapping and
// checks both directions after the user mapping is already established, so
// that a fresh mapping's first-touch publication cannot hide the defect.
// Every kernel alias outside the linear map is a host-private copy of its
// frames unless the kernel maps it as whole aligned groups of four pages, so
// a probe that finds the two views apart names a kernel memory that stopped
// being shared.
//
// Usage: shared_memory_probe TEST...   TEST is one of
//   array        BPF_F_MMAPABLE array: syscall update vs mmap, both ways
//   ringbuf      BPF ring buffer, kernel producer, 4 KiB to 64 KiB data
//                with 24-byte records, so that records straddle the end
//   user-ringbuf BPF user ring buffer, user producer, the same sizes
//   perf         perf_event mmap ring (software dummy event, mmap records)
//   packet       PACKET_MMAP TPACKET_V2 ring, page path and vzalloc path
//   iouring      io_uring rings and a provided-buffer ring that the kernel
//                allocates, and the same in the user's memory
//   all          every test above
// Output lines are "RESULT <test> PASS|FAIL <detail>", then "SUMMARY".
// The exit status is the number of failures.

namespace {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long;

constexpr long nr_epoll_create1 = 20;
constexpr long nr_epoll_ctl = 21;
constexpr long nr_epoll_pwait = 22;
constexpr long nr_close = 57;
constexpr long nr_pipe2 = 59;
constexpr long nr_write = 64;
constexpr long nr_exit_group = 94;
constexpr long nr_socket = 198;
constexpr long nr_bind = 200;
constexpr long nr_sendto = 206;
constexpr long nr_setsockopt = 208;
constexpr long nr_munmap = 215;
constexpr long nr_mmap = 222;
constexpr long nr_perf_event_open = 241;
constexpr long nr_bpf = 280;
constexpr long nr_io_uring_setup = 425;
constexpr long nr_io_uring_enter = 426;
constexpr long nr_io_uring_register = 427;

constexpr long einval = 22;

constexpr long prot_read = 1;
constexpr long prot_write = 2;
constexpr long map_shared = 1;
constexpr long map_private = 2;
constexpr long map_anonymous = 0x20;
constexpr long map_populate = 0x8000;

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

void out_num(long value)
{
	char buffer[32];
	int at = 31;
	buffer[at] = 0;
	bool negative = value < 0;
	unsigned long v = negative ? -static_cast<unsigned long>(value) : value;
	do {
		buffer[--at] = static_cast<char>('0' + v % 10);
		v /= 10;
	} while (v);
	if (negative)
		buffer[--at] = '-';
	out(buffer + at);
}

void kv(const char *name, long value)
{
	out("  ");
	out(name);
	out("=");
	out_num(value);
	out("\n");
}

int failures;
int passes;

void result(const char *test, bool pass, const char *detail)
{
	out("RESULT ");
	out(test);
	out(pass ? " PASS " : " FAIL ");
	out(detail);
	out("\n");
	if (pass)
		passes++;
	else
		failures++;
}

void zero(void *p, unsigned long n)
{
	auto *b = static_cast<volatile u8 *>(p);
	for (unsigned long i = 0; i < n; i++)
		b[i] = 0;
}

bool same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

long page_size;

void *map_or_null(long len, long protection, long flags, long fd, long offset)
{
	long r = sys(nr_mmap, 0, len, protection, flags, fd, offset);
	if (r < 0 && r > -4096) {
		out("  mmap error ");
		out_num(r);
		out("\n");
		return nullptr;
	}
	return reinterpret_cast<void *>(r);
}

template <typename T> T load_acquire(volatile T *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

template <typename T> void store_release(volatile T *p, T v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ BPF */

constexpr int bpf_map_create = 0;
constexpr int bpf_map_lookup_elem = 1;
constexpr int bpf_map_update_elem = 2;
constexpr int bpf_prog_load = 5;
constexpr int bpf_prog_test_run = 10;
constexpr u32 map_type_array = 2;
constexpr u32 map_type_ringbuf = 27;
constexpr u32 map_type_user_ringbuf = 31;
constexpr u32 bpf_f_mmapable = 1u << 10;
constexpr u32 ringbuf_busy_bit = 1u << 31;
constexpr u32 ringbuf_discard_bit = 1u << 30;

struct BpfAttr {
	u64 words[18];
};

long bpf(int command, BpfAttr &attr)
{
	return sys(nr_bpf, command, ptr(&attr), sizeof(attr));
}

int map_create(u32 type, u32 key_size, u32 value_size, u32 max_entries, u32 flags)
{
	BpfAttr attr{};
	auto *w = reinterpret_cast<u32 *>(&attr);
	w[0] = type;
	w[1] = key_size;
	w[2] = value_size;
	w[3] = max_entries;
	w[4] = flags;
	return static_cast<int>(bpf(bpf_map_create, attr));
}

long map_update(int fd, u32 key, const void *value)
{
	BpfAttr attr{};
	auto *w = reinterpret_cast<u32 *>(&attr);
	w[0] = static_cast<u32>(fd);
	attr.words[1] = reinterpret_cast<u64>(&key);
	attr.words[2] = reinterpret_cast<u64>(value);
	attr.words[3] = 0;
	return bpf(bpf_map_update_elem, attr);
}

long map_lookup(int fd, u32 key, void *value)
{
	BpfAttr attr{};
	auto *w = reinterpret_cast<u32 *>(&attr);
	w[0] = static_cast<u32>(fd);
	attr.words[1] = reinterpret_cast<u64>(&key);
	attr.words[2] = reinterpret_cast<u64>(value);
	return bpf(bpf_map_lookup_elem, attr);
}

struct Insn {
	u8 code;
	u8 regs;
	short off;
	int imm;
};

constexpr Insn insn(u8 code, int dst, int src, short off, int imm)
{
	return Insn{code, static_cast<u8>((src << 4) | dst), off, imm};
}

char verifier_log[65536];

struct FuncInfo {
	u32 instruction_offset;
	u32 type_id;
};

template <unsigned long N>
int load_socket_filter(const Insn (&program)[N], int btf_fd = -1,
		       const FuncInfo *functions = nullptr,
		       u32 function_count = 0)
{
	static const char license[] = "GPL";
	BpfAttr attr{};
	auto *w = reinterpret_cast<u32 *>(&attr);
	w[0] = 1; /* BPF_PROG_TYPE_SOCKET_FILTER */
	w[1] = N;
	attr.words[1] = reinterpret_cast<u64>(program);
	attr.words[2] = reinterpret_cast<u64>(license);
	w[6] = 1; /* log_level */
	w[7] = sizeof(verifier_log);
	attr.words[4] = reinterpret_cast<u64>(verifier_log);
	if (btf_fd >= 0) {
		w[18] = static_cast<u32>(btf_fd);          /* prog_btf_fd */
		w[19] = sizeof(FuncInfo);                  /* func_info_rec_size */
		attr.words[10] = reinterpret_cast<u64>(functions);
		w[22] = function_count;                    /* func_info_cnt */
	}
	int fd = static_cast<int>(bpf(bpf_prog_load, attr));
	if (fd < 0) {
		out("  prog load error ");
		out_num(fd);
		out("\n");
		out(verifier_log);
		out("\n");
	}
	return fd;
}

/* The verifier requires BTF function information for a program with a
 * subprogram. This is the smallest valid blob: int, a proto that returns int,
 * and one function per subprogram (main_prog at id 3, cb at id 4). */
int load_two_function_btf()
{
	static const char strings[] = "\0int\0main_prog\0cb";
	alignas(8) static u8 raw[256];
	/* Header: magic, version, flags, header length, type offset, type
	 * length, string offset, string length. */
	const u32 types[] = {
		1, 1u << 24, 4, (1u << 24) | 32,   /* id 1: signed 32-bit int */
		0, 13u << 24, 1,                   /* id 2: func_proto, returns 1 */
		5, (12u << 24) | 1, 2,             /* id 3: global main_prog */
		15, 12u << 24, 2,                  /* id 4: static cb */
	};
	const u32 header_length = 24;
	const u32 type_length = sizeof(types);
	u16 *magic = reinterpret_cast<u16 *>(raw);
	*magic = 0xeb9f;
	raw[2] = 1;
	raw[3] = 0;
	u32 *fields = reinterpret_cast<u32 *>(raw + 4);
	fields[0] = header_length;
	fields[1] = 0;
	fields[2] = type_length;
	fields[3] = type_length;
	fields[4] = sizeof(strings);
	for (unsigned i = 0; i < type_length; ++i)
		raw[header_length + i] = reinterpret_cast<const u8 *>(types)[i];
	for (unsigned i = 0; i < sizeof(strings); ++i)
		raw[header_length + type_length + i] = static_cast<u8>(strings[i]);
	static char btf_log[4096];
	BpfAttr attr{};
	auto *w = reinterpret_cast<u32 *>(&attr);
	attr.words[0] = reinterpret_cast<u64>(raw);
	attr.words[1] = reinterpret_cast<u64>(btf_log);
	w[4] = header_length + type_length + sizeof(strings);
	w[5] = sizeof(btf_log);
	w[6] = 1;
	int fd = static_cast<int>(bpf(18 /* BPF_BTF_LOAD */, attr));
	if (fd < 0) {
		out("  btf load error ");
		out_num(fd);
		out("\n");
		out(btf_log);
		out("\n");
	}
	return fd;
}

/* A socket filter that emits one 16-byte sample {12345, 777} with
 * bpf_ringbuf_output(); the 8-byte header makes each record 24 bytes, which
 * does not divide the ring, so records straddle its end. */
int load_ringbuf_prog(int map_fd)
{
	Insn program[] = {
		insn(0x7a, 10, 0, -16, 12345),  /* *(u64 *)(r10 - 16) = 12345 */
		insn(0x7a, 10, 0, -8, 777),     /* *(u64 *)(r10 - 8) = 777 */
		insn(0x18, 1, 1, 0, map_fd),    /* r1 = map (pseudo map fd) */
		insn(0x00, 0, 0, 0, 0),
		insn(0xbf, 2, 10, 0, 0),        /* r2 = r10 */
		insn(0x07, 2, 0, 0, -16),       /* r2 += -16 */
		insn(0xb7, 3, 0, 0, 16),        /* r3 = 16 */
		insn(0xb7, 4, 0, 0, 0),         /* r4 = 0 */
		insn(0x85, 0, 0, 0, 130),       /* call bpf_ringbuf_output */
		insn(0xb7, 0, 0, 0, 0),         /* r0 = 0 */
		insn(0x95, 0, 0, 0, 0),         /* exit */
	};
	return load_socket_filter(program);
}

/* A socket filter that drains a user ring buffer. Its callback reads the
 * 16 bytes of each sample and adds them to a one-element array map of
 * {sum of first words, sample count, last second word}. */
int load_user_ringbuf_prog(int ring_fd, int array_fd)
{
	const int btf = load_two_function_btf();
	if (btf < 0)
		return btf;
	const FuncInfo functions[] = {{0, 3}, {9, 4}};
	Insn program[] = {
		/* main */
		insn(0x18, 1, 1, 0, ring_fd),   /*  0: r1 = user ring buffer */
		insn(0x00, 0, 0, 0, 0),
		insn(0x18, 2, 4, 0, 6),         /*  2: r2 = callback (+6 = insn 9) */
		insn(0x00, 0, 0, 0, 0),
		insn(0xb7, 3, 0, 0, 0),         /*  4: r3 = 0 */
		insn(0xb7, 4, 0, 0, 0),         /*  5: r4 = 0 */
		insn(0x85, 0, 0, 0, 209),       /*  6: bpf_user_ringbuf_drain() */
		insn(0xb7, 0, 0, 0, 0),         /*  7: r0 = 0 */
		insn(0x95, 0, 0, 0, 0),         /*  8: exit */
		/* callback(struct bpf_dynptr *dynptr, void *context) */
		insn(0xbf, 3, 1, 0, 0),         /*  9: r3 = dynptr */
		insn(0xbf, 1, 10, 0, 0),        /* 10: r1 = r10 */
		insn(0x07, 1, 0, 0, -16),       /* 11: r1 += -16 */
		insn(0xb7, 2, 0, 0, 16),        /* 12: r2 = 16 */
		insn(0xb7, 4, 0, 0, 0),         /* 13: r4 = 0 */
		insn(0xb7, 5, 0, 0, 0),         /* 14: r5 = 0 */
		insn(0x85, 0, 0, 0, 201),       /* 15: bpf_dynptr_read() */
		insn(0x62, 10, 0, -20, 0),      /* 16: *(u32 *)(r10 - 20) = 0 */
		insn(0x18, 1, 1, 0, array_fd),  /* 17: r1 = array */
		insn(0x00, 0, 0, 0, 0),
		insn(0xbf, 2, 10, 0, 0),        /* 19: r2 = r10 */
		insn(0x07, 2, 0, 0, -20),       /* 20: r2 += -20 */
		insn(0x85, 0, 0, 0, 1),         /* 21: bpf_map_lookup_elem() */
		insn(0x15, 0, 0, 9, 0),         /* 22: if r0 == 0 goto 32 */
		insn(0x79, 3, 10, -16, 0),      /* 23: r3 = first word */
		insn(0x79, 4, 0, 0, 0),         /* 24: r4 = sum */
		insn(0x0f, 4, 3, 0, 0),         /* 25: r4 += r3 */
		insn(0x7b, 0, 4, 0, 0),         /* 26: sum = r4 */
		insn(0x79, 4, 0, 8, 0),         /* 27: r4 = count */
		insn(0x07, 4, 0, 0, 1),         /* 28: r4 += 1 */
		insn(0x7b, 0, 4, 8, 0),         /* 29: count = r4 */
		insn(0x79, 3, 10, -8, 0),       /* 30: r3 = second word */
		insn(0x7b, 0, 3, 16, 0),        /* 31: last = r3 */
		insn(0xb7, 0, 0, 0, 0),         /* 32: r0 = 0 (keep draining) */
		insn(0x95, 0, 0, 0, 0),         /* 33: exit */
	};
	const int fd = load_socket_filter(program, btf, functions, 2);
	sys(nr_close, btf);
	return fd;
}

long run_prog(int prog)
{
	static char packet[64];
	BpfAttr attr{};
	auto *w = reinterpret_cast<u32 *>(&attr);
	w[0] = static_cast<u32>(prog);
	w[2] = sizeof(packet);
	attr.words[2] = reinterpret_cast<u64>(packet);
	w[8] = 1; /* repeat */
	return bpf(bpf_prog_test_run, attr);
}

/* ---------------------------------------------------------------- epoll */

struct EpollEvent {
	u32 events;
	u32 pad;
	u64 data;
} __attribute__((packed));

int epoll_for(int fd)
{
	int epfd = static_cast<int>(sys(nr_epoll_create1, 0));
	EpollEvent add{};
	add.events = 1;
	add.data = 1;
	sys(nr_epoll_ctl, epfd, 1, fd, ptr(&add));
	return epfd;
}

long ready(int epfd)
{
	EpollEvent event{};
	return sys(nr_epoll_pwait, epfd, ptr(&event), 1, 0, 0, 8);
}

/* ---------------------------------------------------------------- array */

void test_array()
{
	constexpr u32 entries = 4096; /* 32 KiB of values: several 4 KiB pages */
	int fd = map_create(map_type_array, 4, 8, entries, bpf_f_mmapable);
	if (fd < 0) {
		kv("map_create", fd);
		result("array", false, "map_create failed");
		return;
	}
	long len = (entries * 8 + page_size - 1) & ~(page_size - 1);
	auto *values = static_cast<volatile u64 *>(
		map_or_null(len, prot_read | prot_write, map_shared, fd, 0));
	if (!values) {
		result("array", false, "mmap failed");
		return;
	}
	/* Establish every page of the user mapping before the kernel writes. */
	u64 sum = 0;
	for (u32 i = 0; i < entries; i += 256)
		sum += values[i];
	kv("initial_sum", static_cast<long>(sum));

	static const u32 keys[] = {0, 511, 512, 1023, 1024, 2047, 2048, 4095};
	int k2u_bad = 0;
	for (u32 key : keys) {
		u64 v = 0xa5a5000000000000ul | key;
		if (map_update(fd, key, &v) != 0)
			k2u_bad++;
	}
	for (u32 key : keys)
		if (values[key] != (0xa5a5000000000000ul | key))
			k2u_bad++;
	kv("kernel_to_user_mismatches", k2u_bad);

	static const u32 ukeys[] = {1, 600, 1500, 3000, 4000};
	int u2k_bad = 0;
	for (u32 key : ukeys)
		values[key] = 0x5a5a000000000000ul | key;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (u32 key : ukeys) {
		u64 v = 0;
		if (map_lookup(fd, key, &v) != 0 || v != (0x5a5a000000000000ul | key))
			u2k_bad++;
	}
	kv("user_to_kernel_mismatches", u2k_bad);

	auto *fresh = static_cast<volatile u64 *>(
		map_or_null(len, prot_read, map_shared, fd, 0));
	if (fresh) {
		int fresh_bad = 0;
		for (u32 key : keys)
			if (fresh[key] != (0xa5a5000000000000ul | key))
				fresh_bad++;
		kv("fresh_mapping_kernel_mismatches", fresh_bad);
	}
	result("array", k2u_bad == 0 && u2k_bad == 0,
	       k2u_bad || u2k_bad ? "user mapping and kernel alias disagree"
				  : "coherent");
	sys(nr_close, fd);
}

/* -------------------------------------------------------------- ringbuf */

void test_ringbuf_size(u32 data_size, const char *name)
{
	int map = map_create(map_type_ringbuf, 0, 0, data_size, 0);
	if (map < 0) {
		kv("map_create", map);
		result(name, false, "map_create failed");
		return;
	}
	int prog = load_ringbuf_prog(map);
	if (prog < 0) {
		result(name, false, "prog load failed");
		return;
	}
	auto *consumer = static_cast<volatile u64 *>(
		map_or_null(page_size, prot_read | prot_write, map_shared, map, 0));
	auto *producer_map = static_cast<char *>(
		map_or_null(page_size + 2L * data_size, prot_read, map_shared, map,
			    page_size));
	if (!consumer || !producer_map) {
		result(name, false, "mmap failed");
		return;
	}
	auto *producer = reinterpret_cast<volatile u64 *>(producer_map);
	char *data = producer_map + page_size;
	u64 mask = data_size - 1;
	/* Establish the user mapping of every page before the kernel writes. */
	u64 touch = *consumer + *producer;
	for (u64 off = 0; off < 2ul * data_size; off += page_size)
		touch += *reinterpret_cast<volatile u8 *>(data + off);
	(void)touch;
	int epfd = epoll_for(map);

	long records = 0, wrong = 0, stalls = 0, stuck_ready = 0, run_errors = 0;
	/* Three laps of the ring, consuming after every batch of four. */
	long total = 3 * data_size / 24 + 8;
	for (long produced = 0; produced < total;) {
		for (int i = 0; i < 4; i++, produced++)
			if (run_prog(prog) != 0)
				run_errors++;
		u64 cons = *consumer;
		u64 prod = load_acquire(producer);
		if (prod == cons)
			stalls++;
		while (cons < prod) {
			u32 header = load_acquire(
				reinterpret_cast<volatile u32 *>(data + (cons & mask)));
			if (header & ringbuf_busy_bit)
				break;
			u32 len = header & 0x3fffffff;
			if (!(header & ringbuf_discard_bit)) {
				u64 a = *reinterpret_cast<volatile u64 *>(data + ((cons + 8) & mask));
				u64 b = *reinterpret_cast<volatile u64 *>(data + (cons & mask) + 16);
				records++;
				if (a != 12345 || b != 777)
					wrong++;
			}
			cons += (len + 8 + 7) & ~7ul;
			store_release(consumer, cons);
		}
		if (ready(epfd) > 0)
			stuck_ready++;
	}
	kv("data_size", data_size);
	kv("produced", total);
	kv("consumed_records", records);
	kv("wrong_values", wrong);
	kv("batches_with_stale_producer", stalls);
	kv("batches_epoll_still_ready_after_consume", stuck_ready);
	kv("run_errors", run_errors);
	kv("final_producer_pos", static_cast<long>(*producer));
	kv("final_consumer_pos", static_cast<long>(*consumer));
	bool pass = records == total && wrong == 0 && stuck_ready == 0;
	result(name, pass, pass ? "coherent" : "user and kernel views disagree");
	sys(nr_close, epfd);
	sys(nr_close, prog);
	sys(nr_close, map);
}

/* A ring must be at least one guest page. Rings smaller than a 16 KiB group
 * exercise the synchronization of a wrapped record; larger rings exercise
 * congruent double mapping. */
void test_ringbuf()
{
	test_ringbuf_size(4096, "ringbuf-4k");
	test_ringbuf_size(8192, "ringbuf-8k");
	test_ringbuf_size(16384, "ringbuf-16k");
	test_ringbuf_size(65536, "ringbuf-64k");
}

/* --------------------------------------------------------- user ringbuf */

void test_user_ringbuf_size(u32 data_size, const char *name)
{
	int ring = map_create(map_type_user_ringbuf, 0, 0, data_size, 0);
	if (ring < 0) {
		kv("map_create", ring);
		result(name, false, "map_create failed");
		return;
	}
	int sums = map_create(map_type_array, 4, 24, 1, 0);
	if (sums < 0) {
		kv("array map_create", sums);
		result(name, false, "array map_create failed");
		return;
	}
	int prog = load_user_ringbuf_prog(ring, sums);
	if (prog < 0) {
		result(name, false, "prog load failed");
		return;
	}
	/* The kernel owns consumer_pos, the user producer_pos and the data. */
	auto *consumer = static_cast<volatile u64 *>(
		map_or_null(page_size, prot_read, map_shared, ring, 0));
	auto *producer_map = static_cast<char *>(
		map_or_null(page_size + 2L * data_size, prot_read | prot_write,
			    map_shared, ring, page_size));
	if (!consumer || !producer_map) {
		result(name, false, "mmap failed");
		return;
	}
	auto *producer = reinterpret_cast<volatile u64 *>(producer_map);
	char *data = producer_map + page_size;
	const u64 mask = data_size - 1;
	/* Establish the user mapping of every page before the kernel reads. */
	u64 touch = *consumer + *producer;
	for (u64 off = 0; off < 2ul * data_size; off += page_size)
		touch += *reinterpret_cast<volatile u8 *>(data + off);
	(void)touch;

	/* Three laps of the ring in 24-byte records of two words, drained by
	 * the kernel after every batch of four. */
	const long total = 3 * data_size / 24 + 8;
	u64 prod = *producer;
	long produced = 0, drained_batches = 0, full = 0, run_errors = 0;
	u64 expected_sum = 0;
	while (produced < total) {
		for (int i = 0; i < 4 && produced < total; i++) {
			u64 cons = load_acquire(consumer);
			if (prod + 24 - cons > data_size) {
				full++;
				break;
			}
			auto *header = reinterpret_cast<volatile u32 *>(data + (prod & mask));
			header[0] = 16 | ringbuf_busy_bit;
			header[1] = 0;
			prod += 24;
			store_release(producer, prod);
			/* Past the end of the data the sample continues in the
			 * second mapping of the ring. */
			auto *words = reinterpret_cast<volatile u64 *>(
				data + ((prod - 24 + 8) & mask));
			words[0] = 1000 + static_cast<u64>(produced);
			words[1] = 0x4d4b000000000000ul + static_cast<u64>(produced);
			expected_sum += 1000 + static_cast<u64>(produced);
			store_release(header, 16u);
			produced++;
		}
		if (run_prog(prog) != 0)
			run_errors++;
		drained_batches++;
	}
	u64 value[3] = {};
	long lookup = map_lookup(sums, 0, value);
	kv("data_size", data_size);
	kv("produced", produced);
	kv("drained_samples", static_cast<long>(value[1]));
	kv("batches", drained_batches);
	kv("ring_full_batches", full);
	kv("run_errors", run_errors);
	kv("sum_mismatch", static_cast<long>(value[0] - expected_sum));
	kv("final_producer_pos", static_cast<long>(prod));
	kv("final_consumer_pos", static_cast<long>(load_acquire(consumer)));
	bool pass = lookup == 0 && run_errors == 0 && value[1] == static_cast<u64>(produced) &&
		    value[0] == expected_sum && load_acquire(consumer) == prod;
	result(name, pass, pass ? "coherent" : "user and kernel views disagree");
	sys(nr_close, prog);
	sys(nr_close, sums);
	sys(nr_close, ring);
}

void test_user_ringbuf()
{
	test_user_ringbuf_size(4096, "user-ringbuf-4k");
	test_user_ringbuf_size(8192, "user-ringbuf-8k");
	test_user_ringbuf_size(16384, "user-ringbuf-16k");
	test_user_ringbuf_size(65536, "user-ringbuf-64k");
}

/* ----------------------------------------------------------------- perf */

void test_perf()
{
	struct {
		u32 type, size;
		u64 config, sample_period, sample_type, read_format, flags;
		u32 wakeup_events, bp_type;
		u64 rest[11];
	} attr{};
	attr.type = 1;  /* PERF_TYPE_SOFTWARE */
	attr.size = 136;
	attr.config = 9; /* PERF_COUNT_SW_DUMMY */
	attr.flags = (1ul << 8) | (1ul << 17) | (1ul << 5) | (1ul << 6);
	int fd = static_cast<int>(sys(nr_perf_event_open, ptr(&attr), 0, -1, -1, 8));
	if (fd < 0) {
		kv("perf_event_open", fd);
		result("perf", false, "perf_event_open failed");
		return;
	}
	long len = 9 * page_size;
	auto *base = static_cast<char *>(
		map_or_null(len, prot_read | prot_write, map_shared, fd, 0));
	if (!base) {
		result("perf", false, "mmap failed");
		return;
	}
	auto *head = reinterpret_cast<volatile u64 *>(base + 1024);
	auto *data_offset = reinterpret_cast<volatile u64 *>(base + 1040);
	u64 touch = 0;
	for (long off = 0; off < len; off += page_size)
		touch += *reinterpret_cast<volatile u8 *>(base + off);
	(void)touch;
	kv("data_offset", static_cast<long>(*data_offset));
	u64 before = load_acquire(head);
	for (int i = 0; i < 8; i++) {
		void *p = map_or_null(page_size, prot_read, map_private | map_anonymous, -1, 0);
		if (p)
			sys(nr_munmap, ptr(p), page_size);
	}
	u64 after = load_acquire(head);
	kv("data_head_before", static_cast<long>(before));
	kv("data_head_after_8_mmaps", static_cast<long>(after));
	auto *fresh = static_cast<char *>(map_or_null(len, prot_read | prot_write, map_shared, fd, 0));
	if (fresh)
		kv("fresh_mapping_data_head",
		   static_cast<long>(*reinterpret_cast<volatile u64 *>(fresh + 1024)));
	bool pass = after > before;
	result("perf", pass, pass ? "data_head advanced" : "data_head stale in the user mapping");
	sys(nr_close, fd);
}

/* --------------------------------------------------------------- packet */

void test_packet_block(u32 block_size, u32 frame_size, const char *name)
{
	int fd = static_cast<int>(sys(nr_socket, 17, 3, 0x0300));
	if (fd < 0) {
		kv("socket", fd);
		result(name, false, "socket failed");
		return;
	}
	int version = 1; /* TPACKET_V2 */
	long r = sys(nr_setsockopt, fd, 263, 10, ptr(&version), 4);
	if (r != 0) {
		kv("PACKET_VERSION", r);
		result(name, false, "setsockopt failed");
		return;
	}
	struct {
		u32 block_size, block_nr, frame_size, frame_nr;
	} req{block_size, 1, frame_size, block_size / frame_size};
	r = sys(nr_setsockopt, fd, 263, 5, ptr(&req), sizeof(req));
	if (r != 0) {
		kv("PACKET_RX_RING", r);
		result(name, false, "ring setup failed");
		return;
	}
	auto *ring = static_cast<char *>(
		map_or_null(block_size, prot_read | prot_write, map_shared, fd, 0));
	if (!ring) {
		result(name, false, "mmap failed");
		return;
	}
	u32 frames = block_size / frame_size;
	u32 checked = frames < 64 ? frames : 64;
	u32 touch = 0;
	for (u32 i = 0; i < checked; i++)
		touch += *reinterpret_cast<volatile u32 *>(ring + i * frame_size);
	kv("initial_status_sum", touch);
	struct {
		u16 family, protocol;
		int ifindex;
		u16 hatype;
		u8 pkttype, halen;
		u8 addr[8];
	} ll{17, 0x0300, 1, 0, 0, 0, {}};
	r = sys(nr_bind, fd, ptr(&ll), sizeof(ll));
	if (r != 0) {
		kv("bind", r);
		result(name, false, "bind failed");
		return;
	}
	int udp = static_cast<int>(sys(nr_socket, 2, 2, 0));
	struct {
		u16 family, port;
		u32 addr;
		u8 zero[8];
	} to{2, 0x0900, 0x0100007f, {}};
	static const char payload[] = "shared-memory-packet-probe";
	for (int i = 0; i < 8; i++)
		sys(nr_sendto, udp, ptr(payload), sizeof(payload), 0, ptr(&to), sizeof(to));
	u32 user_frames = 0;
	for (int spin = 0; spin < 2000000 && user_frames == 0; spin++)
		for (u32 i = 0; i < checked; i++)
			if (load_acquire(reinterpret_cast<volatile u32 *>(ring + i * frame_size)) & 1)
				user_frames++;
	user_frames = 0;
	for (u32 i = 0; i < checked; i++)
		if (load_acquire(reinterpret_cast<volatile u32 *>(ring + i * frame_size)) & 1)
			user_frames++;
	kv("block_size", block_size);
	kv("frames_marked_user", user_frames);
	auto *fresh = static_cast<char *>(map_or_null(block_size, prot_read, map_shared, fd, 0));
	if (fresh) {
		u32 fresh_frames = 0;
		for (u32 i = 0; i < checked; i++)
			if (*reinterpret_cast<volatile u32 *>(fresh + i * frame_size) & 1)
				fresh_frames++;
		kv("fresh_mapping_frames_marked_user", fresh_frames);
		sys(nr_munmap, ptr(fresh), block_size);
	}
	bool pass = user_frames > 0;
	result(name, pass, pass ? "frames visible" : "kernel frame status invisible to the user mapping");
	sys(nr_munmap, ptr(ring), block_size);
	sys(nr_close, udp);
	sys(nr_close, fd);
}

void test_packet()
{
	/* A block the page allocator serves directly (linear-map alias). */
	test_packet_block(64 * 1024, 2048, "packet-pages");
	/* A block above MAX_PAGE_ORDER: alloc_one_pg_vec_page()
	 * falls back to vzalloc(). */
	test_packet_block(32u * 1024 * 1024, 2u * 1024 * 1024, "packet-vzalloc");
}

/* -------------------------------------------------------------- io_uring */

struct UringParams {
	u32 sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle,
		features, wq_fd, resv[3];
	struct {
		u32 head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
		u64 user_addr;
	} sq_off;
	struct {
		u32 head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
		u64 user_addr;
	} cq_off;
};

constexpr u32 setup_no_mmap = 1u << 14;
constexpr u32 setup_no_sqarray = 1u << 16;
constexpr u32 enter_getevents = 1u;
constexpr long uring_off_sqes = 0x10000000L;
constexpr long uring_off_pbuf_ring = 0x80000000L;
constexpr long uring_register_pbuf_ring = 22;
constexpr u8 uring_op_nop = 0;
constexpr u8 uring_op_read = 22;
constexpr u8 uring_sqe_buffer_select = 1u << 5;

/* Submit @rounds NOPs and reap them, the rings being @rings and @sqes. */
bool uring_nop(int fd, char *rings, char *sqes, const UringParams &p, const char *name)
{
	auto *sq_tail = reinterpret_cast<volatile u32 *>(rings + p.sq_off.tail);
	auto *sq_mask = reinterpret_cast<volatile u32 *>(rings + p.sq_off.ring_mask);
	auto *cq_head = reinterpret_cast<volatile u32 *>(rings + p.cq_off.head);
	auto *cq_tail = reinterpret_cast<volatile u32 *>(rings + p.cq_off.tail);
	auto *cq_mask = reinterpret_cast<volatile u32 *>(rings + p.cq_off.ring_mask);
	int submitted_total = 0, completed = 0, wrong = 0;
	for (int round = 0; round < 4; round++) {
		u32 tail = *sq_tail;
		char *sqe = sqes + 64 * (tail & *sq_mask);
		zero(sqe, 64);
		sqe[0] = static_cast<char>(uring_op_nop);
		*reinterpret_cast<volatile u64 *>(sqe + 32) = 0x4d344b00ul + round;
		store_release(sq_tail, tail + 1);
		long r = sys(nr_io_uring_enter, fd, 1, 0, 0, 0, 0);
		if (r > 0)
			submitted_total += static_cast<int>(r);
		u32 head = *cq_head;
		u32 ctail = load_acquire(cq_tail);
		while (head != ctail) {
			char *cqe = rings + p.cq_off.cqes + 16 * (head & *cq_mask);
			if (*reinterpret_cast<volatile u64 *>(cqe) != 0x4d344b00ul + round)
				wrong++;
			completed++;
			head++;
		}
		store_release(cq_head, head);
	}
	out("  ");
	out(name);
	out(": submitted=");
	out_num(submitted_total);
	out(" completed=");
	out_num(completed);
	out(" wrong=");
	out_num(wrong);
	out("\n");
	return submitted_total == 4 && completed == 4 && wrong == 0;
}

/* One ring with its own memory, mapped by the application. */
void test_uring_kernel_rings(u32 entries)
{
	UringParams p{};
	p.flags = setup_no_sqarray;
	int fd = static_cast<int>(sys(nr_io_uring_setup, entries, ptr(&p)));
	if (fd < 0) {
		kv("io_uring_setup", fd);
		result("iouring-kernel-rings", false, "setup failed");
		return;
	}
	long ring_len = p.cq_off.cqes + p.cq_entries * 16L;
	ring_len = (ring_len + page_size - 1) & ~(page_size - 1);
	auto *rings = static_cast<char *>(map_or_null(
		ring_len, prot_read | prot_write, map_shared | map_populate, fd, 0));
	auto *sqes = static_cast<char *>(map_or_null(
		p.sq_entries * 64L, prot_read | prot_write, map_shared | map_populate,
		fd, uring_off_sqes));
	bool ok = rings && sqes && uring_nop(fd, rings, sqes, p, "kernel rings");
	result("iouring-kernel-rings", ok, ok ? "coherent" : "rings disagree");
	sys(nr_close, fd);
}

struct PbufReg {
	u64 ring_addr;
	u32 ring_entries;
	u16 bgid;
	u16 flags;
	u32 min_left;
	u32 resv[5];
};

constexpr u16 pbuf_ring_mmap = 1;

/* Read a pipe into a buffer that the kernel takes from a provided-buffer
 * ring: the application writes the ring's tail and the kernel must see it. */
bool pbuf_read_through(char *ring_memory, u32 entries, bool mmap_ring, const char *name)
{
	UringParams p{};
	int fd = static_cast<int>(sys(nr_io_uring_setup, 8, ptr(&p)));
	if (fd < 0) {
		kv("io_uring_setup", fd);
		return false;
	}
	long ring_len = p.cq_off.cqes + p.cq_entries * 16L;
	ring_len = (ring_len + page_size - 1) & ~(page_size - 1);
	auto *rings = static_cast<char *>(map_or_null(
		ring_len, prot_read | prot_write, map_shared | map_populate, fd, 0));
	auto *sqes = static_cast<char *>(map_or_null(
		p.sq_entries * 64L, prot_read | prot_write, map_shared | map_populate,
		fd, uring_off_sqes));
	if (!rings || !sqes)
		return false;
	PbufReg reg{};
	reg.ring_entries = entries;
	reg.bgid = 7;
	reg.flags = mmap_ring ? pbuf_ring_mmap : 0;
	reg.ring_addr = mmap_ring ? 0 : reinterpret_cast<u64>(ring_memory);
	long r = sys(nr_io_uring_register, fd, uring_register_pbuf_ring, ptr(&reg), 1);
	if (r != 0) {
		out("  ");
		out(name);
		out(": register error ");
		out_num(r);
		out("\n");
		return false;
	}
	char *ring = ring_memory;
	if (mmap_ring) {
		ring = static_cast<char *>(map_or_null(
			(entries * 16L + page_size - 1) & ~(page_size - 1),
			prot_read | prot_write, map_shared | map_populate, fd,
			uring_off_pbuf_ring | (7L << 16)));
		if (!ring)
			return false;
	}
	/* Four buffers of 64 bytes, in memory the ring names. */
	static char buffers[4][64];
	auto *entries_at = reinterpret_cast<volatile char *>(ring);
	for (u32 i = 0; i < 4; i++) {
		auto *entry = entries_at + 16 * i;
		*reinterpret_cast<volatile u64 *>(entry) = reinterpret_cast<u64>(buffers[i]);
		*reinterpret_cast<volatile u32 *>(entry + 8) = 64;
		*reinterpret_cast<volatile u16 *>(entry + 12) = static_cast<u16>(i);
	}
	store_release(reinterpret_cast<volatile u16 *>(ring + 14), static_cast<u16>(4));

	long pipe_fds[2] = {};
	int pipe_pair[2] = {};
	if (sys(nr_pipe2, ptr(pipe_pair), 0) != 0)
		return false;
	pipe_fds[0] = pipe_pair[0];
	pipe_fds[1] = pipe_pair[1];
	static const char message[] = "m4k-provided";
	sys(nr_write, pipe_fds[1], ptr(message), sizeof(message) - 1);

	auto *sq_tail = reinterpret_cast<volatile u32 *>(rings + p.sq_off.tail);
	auto *sq_mask = reinterpret_cast<volatile u32 *>(rings + p.sq_off.ring_mask);
	auto *sq_array = reinterpret_cast<volatile u32 *>(rings + p.sq_off.array);
	auto *cq_head = reinterpret_cast<volatile u32 *>(rings + p.cq_off.head);
	auto *cq_tail = reinterpret_cast<volatile u32 *>(rings + p.cq_off.tail);
	auto *cq_mask = reinterpret_cast<volatile u32 *>(rings + p.cq_off.ring_mask);
	u32 tail = *sq_tail;
	u32 index = tail & *sq_mask;
	char *sqe = sqes + 64 * index;
	zero(sqe, 64);
	sqe[0] = static_cast<char>(uring_op_read);
	sqe[1] = static_cast<char>(uring_sqe_buffer_select);
	*reinterpret_cast<volatile int *>(sqe + 4) = static_cast<int>(pipe_fds[0]);
	*reinterpret_cast<volatile u32 *>(sqe + 24) = 64;
	*reinterpret_cast<volatile u64 *>(sqe + 32) = 0x4d344b50ul;
	*reinterpret_cast<volatile u16 *>(sqe + 40) = 7;
	sq_array[index] = index;
	store_release(sq_tail, tail + 1);
	long entered = sys(nr_io_uring_enter, fd, 1, 1, enter_getevents, 0, 0);
	u32 head = *cq_head;
	u32 ctail = load_acquire(cq_tail);
	bool ok = entered == 1 && head != ctail;
	long res = -1;
	u32 flags = 0;
	if (ok) {
		char *cqe = rings + p.cq_off.cqes + 16 * (head & *cq_mask);
		res = *reinterpret_cast<volatile int *>(cqe + 8);
		flags = *reinterpret_cast<volatile u32 *>(cqe + 12);
		store_release(cq_head, head + 1);
	}
	u32 bid = flags >> 16;
	bool data_ok = ok && (flags & 1) && bid < 4 && res == sizeof(message) - 1;
	for (long i = 0; data_ok && i < res; i++)
		if (buffers[bid][i] != message[i])
			data_ok = false;
	out("  ");
	out(name);
	out(": entered=");
	out_num(entered);
	out(" res=");
	out_num(res);
	out(" buffer_id=");
	out_num(bid);
	out("\n");
	sys(nr_close, pipe_fds[0]);
	sys(nr_close, pipe_fds[1]);
	sys(nr_close, fd);
	return data_ok;
}

/* The kernel's memory for the ring buffer, mapped by the application. */
void test_uring_pbuf_mmap()
{
	bool ok = pbuf_read_through(nullptr, 8, true, "pbuf ring (kernel memory)");
	result("iouring-pbuf-ring-mmap", ok, ok ? "coherent" : "buffer ring tail not seen");
}

/* One page of the application's own memory is one folio that the kernel
 * reaches through the linear map, so it works on every profile. */
void test_uring_pbuf_user_page()
{
	auto *page = static_cast<char *>(map_or_null(
		page_size, prot_read | prot_write, map_private | map_anonymous | map_populate, -1, 0));
	if (!page) {
		result("iouring-pbuf-ring-user-page", false, "mmap failed");
		return;
	}
	bool ok = pbuf_read_through(page, 8, false, "pbuf ring (one user page)");
	result("iouring-pbuf-ring-user-page", ok, ok ? "coherent" : "buffer ring tail not seen");
}

/* Pages of a mapping that are sure not to make groups: two mappings that are
 * touched alternately take alternate frames from the page allocator. */
bool scattered_pages(long len, char *&first, char *&second)
{
	first = static_cast<char *>(map_or_null(len, prot_read | prot_write,
						map_private | map_anonymous, -1, 0));
	second = static_cast<char *>(map_or_null(len, prot_read | prot_write,
						 map_private | map_anonymous, -1, 0));
	if (!first || !second)
		return false;
	for (long off = 0; off < len; off += page_size) {
		first[off] = 1;
		second[off] = 1;
	}
	return true;
}

/* A ring in the application's own scattered pages. The guest cannot map it
 * so that the kernel and the application share it, and refuses it, as Linux
 * refuses where the two mappings must be coloured alike. */
void test_uring_user_memory()
{
	constexpr u32 entries = 1024;
	constexpr long len = 512 * 1024;
	char *rings, *sqes;
	if (!scattered_pages(len, rings, sqes)) {
		result("iouring-user-memory", false, "mmap failed");
		return;
	}
	UringParams p{};
	p.flags = setup_no_mmap | setup_no_sqarray;
	p.sq_off.user_addr = reinterpret_cast<u64>(sqes);
	p.cq_off.user_addr = reinterpret_cast<u64>(rings);
	int fd = static_cast<int>(sys(nr_io_uring_setup, entries, ptr(&p)));
	kv("io_uring_setup_no_mmap", fd);
	result("iouring-user-memory", fd == -einval,
	       fd == -einval ? "refused: the guest cannot share scattered pages"
			     : "scattered pages were accepted without being shared");
	if (fd >= 0)
		sys(nr_close, fd);
}

void test_iouring()
{
	test_uring_kernel_rings(1024);
	test_uring_pbuf_mmap();
	test_uring_pbuf_user_page();
	test_uring_user_memory();
}

long page_size_from_auxv(long *sp)
{
	long argc = *sp;
	long *p = sp + 1 + argc + 1;
	while (*p)
		p++;
	p++;
	for (; p[0]; p += 2)
		if (p[0] == 6)
			return p[1];
	return 4096;
}

int run(long *sp)
{
	long argc = *sp;
	char **argv = reinterpret_cast<char **>(sp + 1);
	page_size = page_size_from_auxv(sp);
	kv("page_size", page_size);
	for (long i = 1; i < argc; i++) {
		const char *t = argv[i];
		bool all = same(t, "all");
		if (all || same(t, "array"))
			test_array();
		if (all || same(t, "ringbuf"))
			test_ringbuf();
		if (all || same(t, "user-ringbuf"))
			test_user_ringbuf();
		if (all || same(t, "perf"))
			test_perf();
		if (all || same(t, "packet"))
			test_packet();
		if (all || same(t, "iouring"))
			test_iouring();
	}
	out("SUMMARY pass=");
	out_num(passes);
	out(" fail=");
	out_num(failures);
	out("\n");
	return failures ? 1 : 0;
}

} // namespace

extern "C" void *memset(void *d, int c, unsigned long n)
{
	auto *b = static_cast<unsigned char *>(d);
	for (unsigned long i = 0; i < n; i++)
		b[i] = static_cast<unsigned char>(c);
	return d;
}

extern "C" void *memcpy(void *d, const void *s, unsigned long n)
{
	auto *a = static_cast<unsigned char *>(d);
	auto *b = static_cast<const unsigned char *>(s);
	for (unsigned long i = 0; i < n; i++)
		a[i] = b[i];
	return d;
}

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
