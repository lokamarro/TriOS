/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native Linux/x86-64 oracle for the x86 unit-corpus producer: it traces K
 * programs one instruction at a time and runs repository cases to their HLT.
 *
 * Build it freestanding for an Intel Linux host (tests/unit/x86_64/corpus/
 * README.md has the workflow):
 *
 *   clang++ --target=x86_64-unknown-linux-gnu -std=c++20 -O2 \
 *     -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
 *     -fno-builtin -fno-pie -mno-red-zone -nostdlib -static -fuse-ld=lld \
 *     -Wl,-e,_start -Wl,--build-id=none -Wall -Wextra -Werror \
 *     tools/x86-tests/native-k-trace.cpp -o native-k-trace
 */

namespace {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using usize = unsigned long;
using isize = long;

static_assert(sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8);

constexpr u64 code_address = 0x1'0000;
constexpr u64 stack_address = 0x7000'0000;
constexpr u64 stack_pointer = stack_address + 1024;
constexpr usize page_size = 4096;
constexpr usize request_capacity = 32 * 1024 * 1024;
constexpr usize xstate_capacity = 64 * 1024;

constexpr isize sys_read = 0;
constexpr isize sys_write = 1;
constexpr isize sys_mmap = 9;
constexpr isize sys_fork = 57;
constexpr isize sys_exit = 60;
constexpr isize sys_wait4 = 61;
constexpr isize sys_kill = 62;
constexpr isize sys_getpid = 39;
constexpr isize sys_alarm = 37;
constexpr isize sys_ptrace = 101;

constexpr isize prot_read = 1;
constexpr isize prot_write = 2;
constexpr isize prot_execute = 4;
constexpr isize map_private = 2;
constexpr isize map_fixed = 0x10;
constexpr isize map_anonymous = 0x20;
constexpr isize map_fixed_noreplace = 0x100000;

constexpr isize ptrace_traceme = 0;
constexpr isize ptrace_continue = 7;
constexpr isize ptrace_single_step = 9;
constexpr isize ptrace_get_regs = 12;
constexpr isize ptrace_set_regs = 13;
constexpr isize ptrace_get_regset = 0x4204;
constexpr isize ptrace_set_regset = 0x4205;
constexpr isize ptrace_set_options = 0x4200;
constexpr isize ptrace_option_exit_kill = 0x100000;
constexpr isize nt_x86_xstate = 0x202;

constexpr int signal_ill = 4;
constexpr int signal_trap = 5;
constexpr int signal_bus = 7;
constexpr int signal_fpe = 8;
constexpr int signal_kill = 9;
constexpr int signal_segv = 11;
constexpr int signal_alarm = 14;
constexpr int signal_stop = 19;
/* A repository case that has not reached its HLT by then never will. */
constexpr isize case_time_limit_seconds = 10;

alignas(page_size) u8 request_bytes[request_capacity];
alignas(64) u8 xstate[xstate_capacity];

struct Iovec final {
	void *base;
	usize length;
};

struct Registers final {
	u64 r15;
	u64 r14;
	u64 r13;
	u64 r12;
	u64 rbp;
	u64 rbx;
	u64 r11;
	u64 r10;
	u64 r9;
	u64 r8;
	u64 rax;
	u64 rcx;
	u64 rdx;
	u64 rsi;
	u64 rdi;
	u64 orig_rax;
	u64 rip;
	u64 cs;
	u64 rflags;
	u64 rsp;
	u64 ss;
	u64 fs_base;
	u64 gs_base;
	u64 ds;
	u64 es;
	u64 fs;
	u64 gs;
};

struct Cursor final {
	const u8 *bytes;
	usize size;
	usize offset;

	bool read_u32(u32 &value)
	{
		if (size - offset < 4)
			return false;
		value = static_cast<u32>(bytes[offset]) |
			(static_cast<u32>(bytes[offset + 1]) << 8) |
			(static_cast<u32>(bytes[offset + 2]) << 16) |
			(static_cast<u32>(bytes[offset + 3]) << 24);
		offset += 4;
		return true;
	}

	bool take(usize count, const u8 *&value)
	{
		if (count > size - offset)
			return false;
		value = bytes + offset;
		offset += count;
		return true;
	}
};

struct Record final {
	u32 name_size;
	u32 code_size;
	u32 instruction_count;
	u32 state_count;
	const u8 *offset_bytes;
	const u8 *path_bytes;
	const u8 *name;
	const u8 *code;
};

/* A repository case: its code, loaded at code_address, and the regions its
 * configuration maps. */
struct CaseRecord final {
	u32 name_size;
	u32 code_size;
	u32 region_count;
	const u8 *region_bytes;
	const u8 *name;
	const u8 *code;
};

[[nodiscard]] inline isize syscall6(isize number, isize first = 0,
	isize second = 0, isize third = 0, isize fourth = 0, isize fifth = 0,
	isize sixth = 0)
{
	register isize r10 __asm__("r10") = fourth;
	register isize r8 __asm__("r8") = fifth;
	register isize r9 __asm__("r9") = sixth;
	isize result;
	__asm__ volatile("syscall"
		: "=a"(result)
		: "a"(number), "D"(first), "S"(second), "d"(third),
		  "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory");
	return result;
}

[[noreturn]] void terminate(int status)
{
	(void)syscall6(sys_exit, status);
	__builtin_unreachable();
}

[[nodiscard]] bool failed(isize result)
{
	return static_cast<u64>(result) >= static_cast<u64>(-4095LL);
}

[[nodiscard]] bool write_all(int descriptor, const void *data, usize size)
{
	const auto *bytes = static_cast<const u8 *>(data);
	while (size != 0) {
		const auto written = syscall6(sys_write, descriptor,
			reinterpret_cast<isize>(bytes), size);
		if (written <= 0)
			return false;
		bytes += written;
		size -= static_cast<usize>(written);
	}
	return true;
}

[[noreturn]] void fatal(const char *message, usize size)
{
	(void)write_all(2, message, size);
	terminate(2);
}

#define UML_NATIVE_FATAL(text) fatal(text "\n", sizeof(text))

[[nodiscard]] u32 load_u32(const u8 *bytes)
{
	return static_cast<u32>(bytes[0]) |
		(static_cast<u32>(bytes[1]) << 8) |
		(static_cast<u32>(bytes[2]) << 16) |
		(static_cast<u32>(bytes[3]) << 24);
}

[[nodiscard]] u64 load_u64(const u8 *bytes)
{
	return static_cast<u64>(load_u32(bytes)) |
		(static_cast<u64>(load_u32(bytes + 4)) << 32);
}

void store_u16(u8 *bytes, u16 value)
{
	bytes[0] = static_cast<u8>(value);
	bytes[1] = static_cast<u8>(value >> 8);
}

void store_u32(u8 *bytes, u32 value)
{
	for (unsigned index = 0; index < 4; ++index)
		bytes[index] = static_cast<u8>(value >> (index * 8));
}

[[nodiscard]] bool equal(const u8 *left, const char *right, usize size)
{
	for (usize index = 0; index < size; ++index) {
		if (left[index] != static_cast<u8>(right[index]))
			return false;
	}
	return true;
}

void copy(u8 *destination, const u8 *source, usize size)
{
	for (usize index = 0; index < size; ++index)
		destination[index] = source[index];
}

void zero(u8 *destination, usize size)
{
	for (usize index = 0; index < size; ++index)
		destination[index] = 0;
}

void cpuid(u32 leaf, u32 subleaf, u32 &eax, u32 &ebx, u32 &ecx, u32 &edx)
{
	__asm__ volatile("cpuid"
		: "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		: "a"(leaf), "c"(subleaf));
}

[[nodiscard]] u64 xgetbv()
{
	u32 low;
	u32 high;
	__asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
	return (static_cast<u64>(high) << 32) | low;
}

void require_profile(char vendor[12], u32 &signature)
{
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
	cpuid(0, 0, eax, ebx, ecx, edx);
	copy(reinterpret_cast<u8 *>(vendor), reinterpret_cast<u8 *>(&ebx), 4);
	copy(reinterpret_cast<u8 *>(vendor + 4), reinterpret_cast<u8 *>(&edx), 4);
	copy(reinterpret_cast<u8 *>(vendor + 8), reinterpret_cast<u8 *>(&ecx), 4);
	if (!equal(reinterpret_cast<u8 *>(vendor), "GenuineIntel", 12))
		UML_NATIVE_FATAL("native oracle is not an Intel CPU");
	cpuid(1, 0, eax, ebx, ecx, edx);
	signature = eax;
	constexpr u32 required_ecx = (1U << 0) | (1U << 1) | (1U << 9) |
		(1U << 12) | (1U << 19) | (1U << 20) | (1U << 22) | (1U << 23) |
		(1U << 25) | (1U << 26) | (1U << 27) | (1U << 28) | (1U << 29);
	if ((ecx & required_ecx) != required_ecx || (xgetbv() & 6) != 6)
		UML_NATIVE_FATAL("native oracle lacks the required leaf-1 ISA profile");
	cpuid(7, 0, eax, ebx, ecx, edx);
	constexpr u32 required_ebx = (1U << 0) | (1U << 3) | (1U << 5) |
		(1U << 8);
	if ((ebx & required_ebx) != required_ebx)
		UML_NATIVE_FATAL("native oracle lacks the required leaf-7 ISA profile");
}

[[nodiscard]] bool parse_record(Cursor &cursor, Record &record)
{
	if (!cursor.read_u32(record.name_size) ||
		!cursor.read_u32(record.code_size) ||
		!cursor.read_u32(record.instruction_count) ||
		!cursor.read_u32(record.state_count) ||
		record.name_size == 0 || record.code_size == 0 ||
		record.instruction_count == 0 || record.state_count < 2)
		return false;
	const auto offset_size =
		(static_cast<usize>(record.instruction_count) + 1) * 4;
	const auto path_size = static_cast<usize>(record.state_count) * 4;
	return cursor.take(offset_size, record.offset_bytes) &&
		cursor.take(path_size, record.path_bytes) &&
		cursor.take(record.name_size, record.name) &&
		cursor.take(record.code_size, record.code);
}

[[nodiscard]] u32 offset(const Record &record, u32 index)
{
	return load_u32(record.offset_bytes + static_cast<usize>(index) * 4);
}

[[nodiscard]] u32 path_index(const Record &record, u32 index)
{
	return load_u32(record.path_bytes + static_cast<usize>(index) * 4);
}

[[nodiscard]] bool get_xstate(isize child, Iovec &vector)
{
	vector = {xstate, xstate_capacity};
	return !failed(syscall6(sys_ptrace, ptrace_get_regset, child,
		nt_x86_xstate, reinterpret_cast<isize>(&vector)));
}

[[nodiscard]] int set_initial_state(isize child, u64 stack)
{
	Registers registers{};
	if (failed(syscall6(sys_ptrace, ptrace_get_regs, child, 0,
		reinterpret_cast<isize>(&registers))))
		return 1;
	const auto cs = registers.cs;
	const auto ss = registers.ss;
	zero(reinterpret_cast<u8 *>(&registers), sizeof(registers));
	registers.rbp = stack;
	registers.rsp = stack;
	registers.orig_rax = ~u64{};
	registers.rip = code_address;
	registers.cs = cs;
	registers.ss = ss;
	registers.rflags = 2;
	if (failed(syscall6(sys_ptrace, ptrace_set_regs, child, 0,
		reinterpret_cast<isize>(&registers))))
		return 2;
	Iovec vector{};
	if (!get_xstate(child, vector) || vector.length < 832)
		return 3;
	zero(xstate + 32, 384);
	zero(xstate + 576, 256);
	store_u16(xstate, 0x037f);
	store_u32(xstate + 24, 0x1f80);
	if (failed(syscall6(sys_ptrace, ptrace_set_regset, child,
		nt_x86_xstate, reinterpret_cast<isize>(&vector))))
		return 4;
	return 0;
}

[[nodiscard]] bool write_state(isize child, u32 event, u32 rip_index)
{
	Registers registers{};
	if (failed(syscall6(sys_ptrace, ptrace_get_regs, child, 0,
		reinterpret_cast<isize>(&registers))))
		return false;
	Iovec vector{};
	if (!get_xstate(child, vector) || vector.length < 832)
		return false;
	constexpr usize state_size = 4 + 4 + 16 * 8 + 16 * 32 + 8;
	u8 state[state_size];
	store_u32(state, event);
	store_u32(state + 4, rip_index);
	const u64 gprs[16]{registers.rax, registers.rbx, registers.rcx,
		registers.rdx, registers.rsi, registers.rdi, registers.rbp,
		registers.rsp, registers.r8, registers.r9, registers.r10,
		registers.r11, registers.r12, registers.r13, registers.r14,
		registers.r15};
	copy(state + 8, reinterpret_cast<const u8 *>(gprs), sizeof(gprs));
	auto *vectors = state + 8 + sizeof(gprs);
	for (unsigned index = 0; index < 16; ++index) {
		copy(vectors + index * 32, xstate + 160 + index * 16, 16);
		copy(vectors + index * 32 + 16, xstate + 576 + index * 16, 16);
	}
	copy(state + state_size - 8,
		reinterpret_cast<const u8 *>(&registers.rflags), 8);
	return write_all(1, state, sizeof(state));
}

[[noreturn]] void child_run(const Record &record)
{
	const auto rounded_code =
		(static_cast<usize>(record.code_size) + page_size - 1) & ~(page_size - 1);
	const auto mapped_code = syscall6(sys_mmap, code_address, rounded_code,
		prot_read | prot_write | prot_execute,
		map_private | map_fixed | map_anonymous, -1, 0);
	constexpr usize stack_size = 2 * 1024 * 1024;
	const auto mapped_stack = syscall6(sys_mmap,
		stack_address - stack_size / 2, stack_size, prot_read | prot_write,
		map_private | map_fixed | map_anonymous, -1, 0);
	if (failed(mapped_code) || static_cast<u64>(mapped_code) != code_address ||
		failed(mapped_stack))
		terminate(111);
	copy(reinterpret_cast<u8 *>(code_address), record.code, record.code_size);
	if (failed(syscall6(sys_ptrace, ptrace_traceme)))
		terminate(112);
	if (failed(syscall6(sys_kill, syscall6(sys_getpid), signal_stop)))
		terminate(113);
	terminate(114);
}

[[nodiscard]] int stopped_signal(int status)
{
	return (status >> 8) & 0xff;
}

void initialize_child(isize child, u64 stack)
{
	int status{};
	if (failed(syscall6(sys_wait4, child, reinterpret_cast<isize>(&status), 0, 0)) ||
		stopped_signal(status) != signal_stop)
		UML_NATIVE_FATAL("child did not reach its initial trace stop");
	switch (set_initial_state(child, stack)) {
	case 0:
		break;
	case 1:
		UML_NATIVE_FATAL("cannot read initial native GPR state");
	case 2:
		UML_NATIVE_FATAL("cannot write initial native GPR state");
	case 3:
		UML_NATIVE_FATAL("cannot read initial native XSAVE state");
	default:
		UML_NATIVE_FATAL("cannot write initial native XSAVE state");
	}
}

void trace_record(const Record &record)
{
	const auto child = syscall6(sys_fork);
	if (failed(child))
		UML_NATIVE_FATAL("fork failed");
	if (child == 0)
		child_run(record);
	int status{};
	initialize_child(child, stack_pointer);
	if (path_index(record, 0) != 0 || !write_state(child, 0, 0))
		UML_NATIVE_FATAL("cannot write initial native state");
	for (u32 state_index = 1; state_index < record.state_count; ++state_index) {
		const auto previous_index = path_index(record, state_index - 1);
		const auto next_index = path_index(record, state_index);
		if (previous_index >= record.instruction_count ||
			next_index > record.instruction_count)
			UML_NATIVE_FATAL("request has an invalid instruction path");
		if (failed(syscall6(sys_ptrace, ptrace_single_step, child)))
			UML_NATIVE_FATAL("native single-step request failed");
		if (failed(syscall6(sys_wait4, child,
			reinterpret_cast<isize>(&status), 0, 0)))
			UML_NATIVE_FATAL("native single-step wait failed");
		const auto signal = stopped_signal(status);
		Registers registers{};
		if (failed(syscall6(sys_ptrace, ptrace_get_regs, child, 0,
			reinterpret_cast<isize>(&registers))))
			UML_NATIVE_FATAL("cannot read native registers");
		u32 event{};
		if (signal == signal_fpe) {
			event = 1;
			if (registers.rip != code_address + offset(record, previous_index))
				UML_NATIVE_FATAL("native divide fault was imprecise");
			registers.rip = code_address + offset(record, next_index);
			if (failed(syscall6(sys_ptrace, ptrace_set_regs, child, 0,
				reinterpret_cast<isize>(&registers))))
				UML_NATIVE_FATAL("cannot resume after native divide fault");
		} else if (signal == signal_ill) {
			UML_NATIVE_FATAL("native oracle encountered an illegal instruction");
		} else if (signal == signal_segv) {
			UML_NATIVE_FATAL("native oracle encountered a memory fault");
		} else if (signal != signal_trap) {
			UML_NATIVE_FATAL("native oracle encountered an unexpected signal");
		} else if (registers.rip != code_address + offset(record, next_index)) {
			UML_NATIVE_FATAL("native execution diverged from the requested path");
		}
		if (!write_state(child, event, next_index))
			UML_NATIVE_FATAL("cannot write native state");
	}
	(void)syscall6(sys_kill, child, signal_kill);
	(void)syscall6(sys_wait4, child, reinterpret_cast<isize>(&status), 0, 0);
}

[[nodiscard]] bool parse_case_record(Cursor &cursor, CaseRecord &record)
{
	if (!cursor.read_u32(record.name_size) ||
		!cursor.read_u32(record.code_size) ||
		!cursor.read_u32(record.region_count) ||
		record.name_size == 0 || record.code_size == 0 ||
		record.region_count > 64)
		return false;
	return cursor.take(static_cast<usize>(record.region_count) * 16,
			record.region_bytes) &&
		cursor.take(record.name_size, record.name) &&
		cursor.take(record.code_size, record.code);
}

[[noreturn]] void case_fatal(const CaseRecord &record, const char *message,
	usize size)
{
	(void)write_all(2, record.name, record.name_size);
	(void)write_all(2, ": ", 2);
	fatal(message, size);
}

#define UML_CASE_FATAL(record, text) case_fatal(record, text "\n", sizeof(text))

/* The case's regions and code are mapped where the unit runner maps them.
 * No mapping may replace one the helper itself occupies. */
[[noreturn]] void case_child_run(const CaseRecord &record)
{
	(void)syscall6(sys_alarm, case_time_limit_seconds);
	for (u32 index = 0; index < record.region_count; ++index) {
		const auto address = load_u64(record.region_bytes + index * 16);
		const auto size = load_u64(record.region_bytes + index * 16 + 8);
		const auto mapped = syscall6(sys_mmap, static_cast<isize>(address),
			static_cast<isize>(size), prot_read | prot_write | prot_execute,
			map_private | map_fixed_noreplace | map_anonymous, -1, 0);
		if (failed(mapped) || static_cast<u64>(mapped) != address)
			terminate(121);
	}
	const auto rounded_code =
		(static_cast<usize>(record.code_size) + page_size - 1) & ~(page_size - 1);
	const auto mapped_code = syscall6(sys_mmap, code_address, rounded_code,
		prot_read | prot_write | prot_execute,
		map_private | map_fixed_noreplace | map_anonymous, -1, 0);
	if (failed(mapped_code) || static_cast<u64>(mapped_code) != code_address)
		terminate(122);
	copy(reinterpret_cast<u8 *>(code_address), record.code, record.code_size);
	if (failed(syscall6(sys_ptrace, ptrace_traceme)))
		terminate(123);
	if (failed(syscall6(sys_kill, syscall6(sys_getpid), signal_stop)))
		terminate(124);
	terminate(125);
}

/* Runs a case from zeroed registers to its HLT, which raises #GP at CPL 3,
 * without single-stepping: a trap flag the tracer set would otherwise show
 * in the flags image PUSHF stores. */
void run_case(const CaseRecord &record)
{
	const auto child = syscall6(sys_fork);
	if (failed(child))
		UML_NATIVE_FATAL("fork failed");
	if (child == 0)
		case_child_run(record);
	initialize_child(child, 0);
	if (failed(syscall6(sys_ptrace, ptrace_set_options, child, 0,
		ptrace_option_exit_kill)) ||
		failed(syscall6(sys_ptrace, ptrace_continue, child, 0, 0)))
		UML_CASE_FATAL(record, "cannot start the native case");
	int status{};
	if (failed(syscall6(sys_wait4, child, reinterpret_cast<isize>(&status), 0, 0)))
		UML_CASE_FATAL(record, "native case wait failed");
	if ((status & 0xff) != 0x7f)
		UML_CASE_FATAL(record, "native case exited before its HLT");
	const auto signal = stopped_signal(status);
	Registers registers{};
	if (failed(syscall6(sys_ptrace, ptrace_get_regs, child, 0,
		reinterpret_cast<isize>(&registers))))
		UML_CASE_FATAL(record, "cannot read native registers");
	const auto offset = registers.rip - code_address;
	if (signal == signal_ill)
		UML_CASE_FATAL(record, "native case encountered an illegal instruction");
	if (signal == signal_alarm)
		UML_CASE_FATAL(record, "native case did not reach its HLT in time");
	if (signal == signal_bus)
		UML_CASE_FATAL(record, "native case raised an alignment or bus fault");
	if (signal != signal_segv)
		UML_CASE_FATAL(record, "native case stopped on an unexpected signal");
	if (registers.rip < code_address || offset >= record.code_size ||
		record.code[offset] != 0xf4)
		UML_CASE_FATAL(record, "native case raised a memory fault");
	if (!write_state(child, 0, static_cast<u32>(offset)))
		UML_CASE_FATAL(record, "cannot write native case state");
	(void)syscall6(sys_kill, child, signal_kill);
	(void)syscall6(sys_wait4, child, reinterpret_cast<isize>(&status), 0, 0);
}

int run()
{
	usize size{};
	while (size != request_capacity) {
		const auto count = syscall6(sys_read, 0,
			reinterpret_cast<isize>(request_bytes + size), request_capacity - size);
		if (count < 0)
			UML_NATIVE_FATAL("cannot read native trace request");
		if (count == 0)
			break;
		size += static_cast<usize>(count);
	}
	if (size == request_capacity)
		UML_NATIVE_FATAL("native trace request exceeds its size limit");
	Cursor cursor{request_bytes, size, 0};
	const u8 *magic{};
	u32 version{};
	u32 record_count{};
	const u8 *request_identity{};
	if (!cursor.take(8, magic) || (!equal(magic, "UMLKREQ\0", 8) &&
		!equal(magic, "UMLCREQ\0", 8)) ||
		!cursor.read_u32(version) || version != 1 ||
		!cursor.read_u32(record_count) || record_count == 0 ||
		!cursor.take(32, request_identity))
		UML_NATIVE_FATAL("invalid native trace request header");
	const bool cases = magic[3] == 'C';
	char vendor[12];
	u32 signature{};
	require_profile(vendor, signature);
	const char response_magic[8]{'U', 'M', 'L', cases ? 'C' : 'K', 'R', 'S',
		'P', 0};
	if (!write_all(1, response_magic, sizeof(response_magic)) ||
		!write_all(1, &version, sizeof(version)) ||
		!write_all(1, &record_count, sizeof(record_count)) ||
		!write_all(1, vendor, sizeof(vendor)) ||
		!write_all(1, &signature, sizeof(signature)) ||
		!write_all(1, request_identity, 32))
		UML_NATIVE_FATAL("cannot write native response header");
	for (u32 index = 0; cases && index < record_count; ++index) {
		CaseRecord record{};
		if (!parse_case_record(cursor, record))
			UML_NATIVE_FATAL("invalid native case request record");
		if (!write_all(1, &record.name_size, sizeof(record.name_size)) ||
			!write_all(1, record.name, record.name_size))
			UML_NATIVE_FATAL("cannot write native response record");
		run_case(record);
	}
	for (u32 index = 0; !cases && index < record_count; ++index) {
		Record record{};
		if (!parse_record(cursor, record))
			UML_NATIVE_FATAL("invalid native trace request record");
		const u32 header[2]{record.name_size, record.state_count};
		if (!write_all(1, header, sizeof(header)) ||
			!write_all(1, record.name, record.name_size))
			UML_NATIVE_FATAL("cannot write native response record");
		trace_record(record);
	}
	if (cursor.offset != cursor.size)
		UML_NATIVE_FATAL("native trace request has trailing bytes");
	return 0;
}

} // namespace

extern "C" int uml_native_trace_main()
{
	return run();
}

extern "C" [[noreturn, gnu::naked]] void _start()
{
	__asm__ volatile(
		"andq $-16, %rsp\n"
		"callq uml_native_trace_main\n"
		"movl %eax, %edi\n"
		"movl $60, %eax\n"
		"syscall\n"
		"ud2\n");
}
