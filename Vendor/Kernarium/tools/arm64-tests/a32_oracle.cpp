// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The AArch32 corpus oracle: a freestanding armhf program that runs the
 * cases of a request file on the machine it runs on and writes what each
 * one did. tools/arm64-tests/a32-oracle.sh builds it with the pinned clang
 * and lld, copies it to a private directory on an armhf-capable host or one
 * running qemu-arm, and runs it there; a32-corpus.py writes the request and
 * reads the response.
 *
 * A case is a program of A32 words or Thumb halfwords, an initial state
 * (r0-r14, APSR, FPSCR, TPIDRURO, TPIDRURW, D0-D31) and an initial data
 * region. The program runs at CODE_BASE plus the case's origin (0 unless it
 * tests instruction fetch near a line) in the instruction set the case
 * names, and ends by falling into or branching to an exit stub, which
 * records the final state; or it raises a signal, whose handler records the
 * state at the fault. Nothing else is observed: the corpus asks of a
 * program what the architecture defines, not what the host does around it.
 *
 * The memory map is the corpus's:
 *
 *   0x4f000000  16 KiB  code (program, exit stubs, launch), read-write-execute
 *   0x50000000  32 KiB  data, read-write, refilled before each case
 *   0x50008000  16 KiB  read-only data
 *   everything else unmapped
 *
 * Request (little endian): "A32ORQ1\0", u32 cases, u32 0, then per case
 *   u32 size, u8 flags (bit 0: Thumb entry), u8 data fill, u16 program bytes,
 *   u16 patches, u16 origin (bytes), State (336 bytes), the program padded to 4 bytes, and
 *   per patch u32 offset, u32 length, bytes padded to 4.
 * Response: "A32ORS1\0", u32 cases, u32 AT_HWCAP, u32 AT_HWCAP2, u32 0, then
 *   per case u32 size, u32 signal (0 if the program finished), u32 si_code,
 *   u32 si_addr, u32 pc at the fault, u32 exit stub (0 ARM, 1 Thumb),
 *   u32 digest of the initial data, u32 runs, State, and per run u32 offset,
 *   u32 length, bytes padded to 4 (the data bytes the case changed).
 */

namespace {

using u8 = __UINT8_TYPE__;
using u16 = __UINT16_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;

constexpr u32 kCodeBase = 0x4f000000;
constexpr u32 kCodeSize = 0x4000;
constexpr u32 kArmExit = kCodeBase + 0x800;
constexpr u32 kThumbExit = kCodeBase + 0x900;
/* The stubs jump through these, so the word after a stub's load is the same
 * for every oracle; the harness's own epilogues, which have no fixed address,
 * are behind them. A program cannot read them by a literal load. */
constexpr u32 kArmEpilogue = kCodeBase + 0x3f00;
constexpr u32 kThumbEpilogue = kCodeBase + 0x3f08;
constexpr u32 kLaunch = kCodeBase + 0x3f80;
constexpr u32 kDataBase = 0x50000000;
constexpr u32 kDataSize = 0x8000;
constexpr u32 kReadOnlyBase = 0x50008000;
constexpr u32 kReadOnlySize = 0x4000;
constexpr u32 kMaxProgram = 0x700;

struct State {
	u32 r[15];
	u32 apsr;
	u32 fpscr;
	u32 tpidruro;
	u32 tpidrurw;
	u32 reserved;
	u64 d[32];
};
static_assert(sizeof(State) == 336, "State layout is shared with a32_oracle_entry.S");

extern "C" {
void a32_run(const State *in);
void a32_exit_arm();
void a32_exit_thumb();
void a32_resume();
void a32_sigreturn();
u32 a32_read_tpidrurw();
extern State a32_save_area;
extern u32 a32_exit_kind;
extern u32 a32_saved_sp;
}

/* System calls --------------------------------------------------------------- */

inline long sys(long number, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0,
	long f = 0)
{
	register long r7 __asm__("r7") = number;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	register long r5 __asm__("r5") = f;
	__asm__ volatile("svc #0"
			 : "+r"(r0)
			 : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
			 : "memory");
	return r0;
}

constexpr long kSysRead = 3;
constexpr long kSysWrite = 4;
constexpr long kSysOpen = 5;
constexpr long kSysClose = 6;
constexpr long kSysAlarm = 27;
constexpr long kSysMunmap = 91;
constexpr long kSysSigaltstack = 186;
constexpr long kSysRtSigaction = 174;
constexpr long kSysMmap2 = 192;
constexpr long kSysCacheflush = 0xf0002;
constexpr long kSysSetTls = 0xf0005;

constexpr int kProtRead = 1;
constexpr int kProtWrite = 2;
constexpr int kProtExec = 4;
constexpr int kMapPrivate = 2;
constexpr int kMapFixed = 0x10;
constexpr int kMapAnonymous = 0x20;

constexpr u32 kSaSiginfo = 4;
constexpr u32 kSaOnstack = 0x08000000;
constexpr u32 kSaRestorer = 0x04000000;
constexpr u32 kSaNodefer = 0x40000000;

/* Library functions the compiler may call --------------------------------------- */

} // namespace

extern "C" {

void *memcpy(void *destination, const void *source, __SIZE_TYPE__ count)
{
	auto *to = static_cast<u8 *>(destination);
	const auto *from = static_cast<const u8 *>(source);
	while (count--)
		*to++ = *from++;
	return destination;
}

void *memmove(void *destination, const void *source, __SIZE_TYPE__ count)
{
	auto *to = static_cast<u8 *>(destination);
	const auto *from = static_cast<const u8 *>(source);
	if (to < from) {
		while (count--)
			*to++ = *from++;
	} else {
		to += count;
		from += count;
		while (count--)
			*--to = *--from;
	}
	return destination;
}

void *memset(void *destination, int value, __SIZE_TYPE__ count)
{
	auto *to = static_cast<u8 *>(destination);
	while (count--)
		*to++ = static_cast<u8>(value);
	return destination;
}

int memcmp(const void *left, const void *right, __SIZE_TYPE__ count)
{
	const auto *a = static_cast<const u8 *>(left);
	const auto *b = static_cast<const u8 *>(right);
	for (; count; --count, ++a, ++b)
		if (*a != *b)
			return *a < *b ? -1 : 1;
	return 0;
}

} // extern "C"

namespace {

/* Input and output buffers --------------------------------------------------- */

struct Buffer {
	u8 *data = nullptr;
	u32 size = 0;
	u32 capacity = 0;
};

void *map_anonymous(u32 bytes)
{
	const long result = sys(kSysMmap2, 0, bytes, kProtRead | kProtWrite,
		kMapPrivate | kMapAnonymous, -1, 0);
	return result < 0 && result > -4096 ? nullptr : reinterpret_cast<void *>(result);
}

bool map_fixed(u32 address, u32 bytes, int protection)
{
	const long result = sys(kSysMmap2, address, bytes, protection,
		kMapPrivate | kMapAnonymous | kMapFixed, -1, 0);
	return result == static_cast<long>(address);
}

bool reserve(Buffer &buffer, u32 more)
{
	if (buffer.size + more <= buffer.capacity)
		return true;
	u32 capacity = buffer.capacity ? buffer.capacity : (1U << 20);
	while (capacity < buffer.size + more)
		capacity *= 2;
	auto *grown = static_cast<u8 *>(map_anonymous(capacity));
	if (grown == nullptr)
		return false;
	memcpy(grown, buffer.data, buffer.size);
	if (buffer.data)
		sys(kSysMunmap, reinterpret_cast<long>(buffer.data), buffer.capacity);
	buffer.data = grown;
	buffer.capacity = capacity;
	return true;
}

bool put(Buffer &buffer, const void *source, u32 bytes)
{
	if (!reserve(buffer, bytes))
		return false;
	memcpy(buffer.data + buffer.size, source, bytes);
	buffer.size += bytes;
	return true;
}

bool put32(Buffer &buffer, u32 value)
{
	return put(buffer, &value, 4);
}

bool pad(Buffer &buffer)
{
	static const u8 zero[4] = {};
	return buffer.size % 4 == 0 || put(buffer, zero, 4 - buffer.size % 4);
}

bool read_file(const char *path, Buffer &buffer)
{
	const long fd = sys(kSysOpen, reinterpret_cast<long>(path), 0);
	if (fd < 0)
		return false;
	for (;;) {
		if (!reserve(buffer, 1U << 16))
			return false;
		const long count = sys(kSysRead, fd, reinterpret_cast<long>(buffer.data + buffer.size),
			buffer.capacity - buffer.size);
		if (count < 0)
			return false;
		if (count == 0)
			break;
		buffer.size += static_cast<u32>(count);
	}
	sys(kSysClose, fd);
	return true;
}

bool write_file(const char *path, const Buffer &buffer)
{
	const long fd = sys(kSysOpen, reinterpret_cast<long>(path), 0x241 /* O_WRONLY|O_CREAT|O_TRUNC */,
		0644);
	if (fd < 0)
		return false;
	u32 done = 0;
	while (done < buffer.size) {
		const long count = sys(kSysWrite, fd, reinterpret_cast<long>(buffer.data + done),
			buffer.size - done);
		if (count <= 0)
			return false;
		done += static_cast<u32>(count);
	}
	sys(kSysClose, fd);
	return true;
}

/* Data fills ------------------------------------------------------------------- */

/* Fill 0 is zero. Fill 1 is the byte recipe the corpus tooling calls hash32
 * (start 1, multiplier 0x9e3779b1): byte i is ((start + i) * multiplier) >> 24. */
void fill_data(u8 fill, u8 *data)
{
	if (fill == 0) {
		memset(data, 0, kDataSize);
		return;
	}
	for (u32 index = 0; index < kDataSize; ++index)
		data[index] = static_cast<u8>(((1U + index) * 0x9e3779b1U) >> 24);
}

void fill_read_only(u8 *data)
{
	for (u32 index = 0; index < kReadOnlySize; ++index)
		data[index] = static_cast<u8>(((0x5aU + index) * 0x9e3779b1U) >> 24);
}

u32 digest(const u8 *data, u32 bytes)
{
	u32 hash = 0x811c9dc5U;
	for (u32 index = 0; index < bytes; ++index)
		hash = (hash ^ data[index]) * 0x01000193U;
	return hash;
}

/* Faults ------------------------------------------------------------------------ */

struct Fault {
	bool raised = false;
	u32 signal = 0;
	u32 code = 0;
	u32 address = 0;
	u32 pc = 0;
	State state{};
};
Fault g_fault;

struct SigContext {
	u32 trap_no, error_code, oldmask;
	u32 r[11];
	u32 fp, ip, sp, lr, pc, cpsr, fault_address;
};

struct Ucontext {
	u32 flags;
	u32 link;
	u32 stack[3];
	SigContext context;
	u32 sigmask[2];
	u32 unused[30];
	alignas(8) u8 regspace[1024];
};

struct SigInfo {
	int number, error, code;
	u32 address;
};

constexpr u32 kVfpMagic = 0x56465001;

void handler(int number, SigInfo *info, void *raw)
{
	auto *context = static_cast<Ucontext *>(raw);
	const SigContext &saved = context->context;
	State &state = g_fault.state;
	for (int index = 0; index < 11; ++index)
		state.r[index] = saved.r[index];
	state.r[11] = saved.fp;
	state.r[12] = saved.ip;
	state.r[13] = saved.sp;
	state.r[14] = saved.lr;
	state.apsr = saved.cpsr & 0xf80f0000U;
	state.tpidrurw = a32_read_tpidrurw();
	const u8 *cursor = context->regspace;
	for (int guard = 0; guard < 16; ++guard) {
		u32 magic, size;
		memcpy(&magic, cursor, 4);
		memcpy(&size, cursor + 4, 4);
		if (magic == 0 || size < 8)
			break;
		if (magic == kVfpMagic) {
			memcpy(state.d, cursor + 8, 256);
			memcpy(&state.fpscr, cursor + 8 + 256, 4);
			break;
		}
		cursor += size;
	}
	g_fault.raised = true;
	g_fault.signal = static_cast<u32>(number);
	g_fault.code = static_cast<u32>(info->code);
	g_fault.address = info->address;
	g_fault.pc = saved.pc;
	/* Resume in the harness: the stack pointer a32_run saved, ARM state,
	 * user mode. */
	context->context.sp = a32_saved_sp;
	context->context.pc = reinterpret_cast<u32>(&a32_resume);
	context->context.cpsr = 0x10;
}

struct KernelSigaction {
	void (*handler)(int, SigInfo *, void *);
	u32 flags;
	void (*restorer)();
	u32 mask[2];
};

bool install_handlers()
{
	auto *stack = map_anonymous(0x10000);
	if (stack == nullptr)
		return false;
	u32 alt[3] = {reinterpret_cast<u32>(stack), 0, 0x10000};
	if (sys(kSysSigaltstack, reinterpret_cast<long>(alt), 0) != 0)
		return false;
	static const int signals[] = {4, 5, 7, 8, 11, 14};
	for (int number : signals) {
		KernelSigaction action{handler, kSaSiginfo | kSaOnstack | kSaNodefer | kSaRestorer,
			a32_sigreturn, {0, 0}};
		if (sys(kSysRtSigaction, number, reinterpret_cast<long>(&action), 0, 8) != 0)
			return false;
	}
	return true;
}

/* The code region ------------------------------------------------------------------ */

void put_word(u32 address, u32 value)
{
	memcpy(reinterpret_cast<void *>(address), &value, 4);
}

void put_half(u32 address, u16 value)
{
	memcpy(reinterpret_cast<void *>(address), &value, 2);
}

/* The exit stub of an instruction set at address: a load of the epilogue's
 * trampoline into the PC, which touches no register. Thumb needs a 4-byte
 * aligned address; the caller aligns it. */
void write_arm_stub(u32 address, u32 target)
{
	put_word(address, 0xe51ff004); /* ldr pc, [pc, #-4] */
	put_word(address + 4, target);
}

void write_thumb_stub(u32 address, u32 target)
{
	put_half(address, 0xf8df); /* ldr.w pc, [pc, #0] */
	put_half(address + 2, 0xf000);
	put_word(address + 4, target);
}

void prepare_code(const u8 *program, u32 bytes, u32 origin, bool thumb)
{
	auto *code = reinterpret_cast<u8 *>(kCodeBase);
	memset(code, 0, kCodeSize);
	memcpy(code + origin, program, bytes);
	const u32 entry = kCodeBase + origin;
	const u32 limit = kCodeBase + kCodeSize;
	u32 end = entry + bytes;
	if (thumb) {
		const u32 stub = (end + 3) & ~3U;
		if (stub + 8 <= limit) {
			if (end % 4)
				put_half(end, 0xbf00); /* nop, so the stub is word aligned */
			write_thumb_stub(stub, kThumbEpilogue);
		}
	} else if (end + 8 <= limit) {
		write_arm_stub(end, kArmEpilogue); /* a program at the end of the region falls off it */
	}
	write_arm_stub(kArmEpilogue, reinterpret_cast<u32>(&a32_exit_arm));
	write_arm_stub(kThumbEpilogue, reinterpret_cast<u32>(&a32_exit_thumb));
	write_arm_stub(kArmExit, kArmEpilogue);
	write_thumb_stub(kThumbExit, kThumbEpilogue);
	write_arm_stub(kLaunch, entry | (thumb ? 1U : 0U));
	sys(kSysCacheflush, kCodeBase, kCodeBase + kCodeSize, 0);
}

/* Cases --------------------------------------------------------------------------- */

struct CaseHeader {
	u32 size;
	u8 flags;
	u8 fill;
	u16 program_bytes;
	u16 patches;
	u16 origin; /* byte offset of the program in the code region */
};

bool run_case(const u8 *record, Buffer &response, u8 *pristine)
{
	CaseHeader header;
	memcpy(&header, record, sizeof header);
	alignas(8) State initial;
	memcpy(&initial, record + sizeof header, sizeof initial);
	const u8 *program = record + sizeof header + sizeof initial;
	const u8 *patches = program + ((header.program_bytes + 3U) & ~3U);

	auto *data = reinterpret_cast<u8 *>(kDataBase);
	fill_data(header.fill, data);
	for (u32 index = 0; index < header.patches; ++index) {
		u32 offset, length;
		memcpy(&offset, patches, 4);
		memcpy(&length, patches + 4, 4);
		if (offset > kDataSize || length > kDataSize - offset)
			return false;
		memcpy(data + offset, patches + 8, length);
		patches += 8 + ((length + 3U) & ~3U);
	}
	memcpy(pristine, data, kDataSize);
	const u32 data_digest = digest(data, kDataSize);

	const bool thumb = header.flags & 1;
	if (header.program_bytes > kMaxProgram)
		return false;
	if ((header.origin & (thumb ? 1U : 3U)) != 0 || header.origin + header.program_bytes > kCodeSize)
		return false;
	prepare_code(program, header.program_bytes, header.origin, thumb);
	sys(kSysSetTls, initial.tpidruro);

	g_fault = Fault{};
	a32_exit_kind = 0;
	memset(&a32_save_area, 0, sizeof a32_save_area);
	sys(kSysAlarm, 20);
	a32_run(&initial);
	sys(kSysAlarm, 0);

	State final;
	if (g_fault.raised) {
		final = g_fault.state;
	} else {
		memcpy(&final, &a32_save_area, sizeof final);
	}
	final.tpidruro = initial.tpidruro;

	Buffer runs;
	u32 run_count = 0;
	for (u32 index = 0; index < kDataSize;) {
		if (data[index] == pristine[index]) {
			++index;
			continue;
		}
		u32 end = index;
		while (end < kDataSize && data[end] != pristine[end])
			++end;
		const u32 length = end - index;
		if (!put32(runs, index) || !put32(runs, length) || !put(runs, data + index, length)
			|| !pad(runs))
			return false;
		++run_count;
		index = end;
	}

	const u32 size = 8 * 4 + sizeof(State) + runs.size;
	bool ok = put32(response, size) && put32(response, g_fault.raised ? g_fault.signal : 0)
		&& put32(response, g_fault.code) && put32(response, g_fault.address)
		&& put32(response, g_fault.pc) && put32(response, g_fault.raised ? 0 : a32_exit_kind)
		&& put32(response, data_digest) && put32(response, run_count)
		&& put(response, &final, sizeof final) && put(response, runs.data, runs.size);
	if (runs.data)
		sys(kSysMunmap, reinterpret_cast<long>(runs.data), runs.capacity);
	return ok;
}

} // namespace

extern "C" int oracle_main(long *stack)
{
	const long argc = stack[0];
	char **argv = reinterpret_cast<char **>(stack + 1);
	if (argc != 3)
		return 64;
	char **cursor = argv + argc + 1;
	while (*cursor)
		++cursor;
	u32 hwcap = 0, hwcap2 = 0;
	for (const u32 *aux = reinterpret_cast<const u32 *>(cursor + 1); aux[0]; aux += 2) {
		if (aux[0] == 16)
			hwcap = aux[1];
		else if (aux[0] == 26)
			hwcap2 = aux[1];
	}

	Buffer request;
	if (!read_file(argv[1], request) || request.size < 16)
		return 66;
	if (memcmp(request.data, "A32ORQ1\0", 8) != 0)
		return 65;
	u32 cases;
	memcpy(&cases, request.data + 8, 4);

	if (!map_fixed(kCodeBase, kCodeSize, kProtRead | kProtWrite | kProtExec)
		|| !map_fixed(kDataBase, kDataSize, kProtRead | kProtWrite)
		|| !map_fixed(kReadOnlyBase, kReadOnlySize, kProtRead | kProtWrite))
		return 70;
	fill_read_only(reinterpret_cast<u8 *>(kReadOnlyBase));
	if (sys(125 /* mprotect */, kReadOnlyBase, kReadOnlySize, kProtRead) != 0)
		return 70;
	if (!install_handlers())
		return 71;

	auto *pristine = static_cast<u8 *>(map_anonymous(kDataSize));
	Buffer response;
	if (pristine == nullptr || !put(response, "A32ORS1\0", 8) || !put32(response, cases)
		|| !put32(response, hwcap) || !put32(response, hwcap2) || !put32(response, 0))
		return 72;
	const u8 *record = request.data + 16;
	for (u32 index = 0; index < cases; ++index) {
		if (record + sizeof(CaseHeader) > request.data + request.size)
			return 67;
		u32 size;
		memcpy(&size, record, 4);
		if (size < sizeof(CaseHeader) + sizeof(State) || record + size > request.data + request.size)
			return 67;
		if (!run_case(record, response, pristine))
			return 68;
		record += size;
	}
	return write_file(argv[2], response) ? 0 : 69;
}
