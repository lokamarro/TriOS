// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/mmu/execution_gate.hpp"
#include "runtime/context.hpp"
#include "runtime/execution_state.hpp"
#include "guest_register_tail.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_host.h"
#include "uml_darwin_static_mmu.h"
#include "static_mmu_test_support.hpp"
#include "scratch_backing.hpp"
#include "require.hpp"
#include "runtime_test_route.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using uml::testing::require;

using uml::testing::ScratchBacking;

constexpr std::string_view backing_name = "uml-static-mmu.XXXXXX";

class PosixSharedMemoryBacking final {
public:
	PosixSharedMemoryBacking()
	{
		static std::atomic_uint sequence{};
		char name[64];
		const int count = std::snprintf(name, sizeof(name),
			"/uml-static-shm-%ld-%u", static_cast<long>(getpid()),
			sequence.fetch_add(1, std::memory_order_relaxed));
		require(count > 0 && static_cast<std::size_t>(count) < sizeof(name));
		fd_ = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
		require(fd_ >= 0);
		require(shm_unlink(name) == 0);
		require(ftruncate(fd_, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
		mapping_ = mmap(nullptr, DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
		require(mapping_ != MAP_FAILED);
	}

	PosixSharedMemoryBacking(const PosixSharedMemoryBacking &) = delete;
	PosixSharedMemoryBacking &operator=(const PosixSharedMemoryBacking &) = delete;

	~PosixSharedMemoryBacking()
	{
		if (mapping_ != MAP_FAILED)
			munmap(mapping_, DARWIN_UML_GUEST_PAGE_SIZE);
		if (fd_ >= 0)
			close(fd_);
	}

	[[nodiscard]] int fd() const noexcept { return fd_; }
	[[nodiscard]] void *data() const noexcept { return mapping_; }

private:
	int fd_{-1};
	void *mapping_{MAP_FAILED};
};

static void uml_static_entry_bridge()
{
	constexpr unsigned long generation = 77;
	constexpr std::uint64_t base = 0x1'0000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd280'00a0U);  // movz x0, #5
	backing.put32(4, 0x9100'1c01U);  // add x1, x0, #7
	backing.put32(8, 0xf900'07e1U);  // str x1, [sp, #8]
	backing.put32(12, 0xd400'0001U); // svc #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		1, 1) == 0);
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.sp = base + 0x800;
	unsigned long tls = 0x1234;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 5);
	require(registers.x[1] == 12);
	require(registers.pc == base + 16);
	void *stored = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, registers.sp + 8, DARWIN_UML_STATIC_READ,
		&stored) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t value = 0;
	std::memcpy(&value, stored, sizeof(value));
	require(value == 12);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

#if DARWIN_UML_X86_64_GUEST
/* From bridge ABI 20 the kernel hands every x86 task's selectors over, ES,
 * CS, SS and DS in fpmr and FS and GS in svcr, as x86 Linux's execve() sets
 * them, and the execution mode follows CS. */
constexpr std::uint64_t x86_long64_selectors =
	(std::uint64_t{0x2b} << 32U) | (std::uint64_t{0x33} << 16U);
constexpr std::uint64_t x86_compat32_selectors =
	(std::uint64_t{0x2b} << 48U) | (std::uint64_t{0x2b} << 32U) |
	(std::uint64_t{0x23} << 16U) | std::uint64_t{0x2b};

static void uml_static_x86_64_entry_bridge()
{
	constexpr unsigned long generation = 131;
	constexpr std::uint64_t base = 0x1'8000;
	constexpr std::array program{
		std::byte{0xb8}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x00},                         // mov eax, 1
		std::byte{0xbf}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x00},                         // mov edi, 1
		std::byte{0x48}, std::byte{0x8d}, std::byte{0x35}, std::byte{0x07},
		std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, // lea rsi, [rip+7]
		std::byte{0xba}, std::byte{0x05}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x00},                         // mov edx, 5
		std::byte{0x0f}, std::byte{0x05},         // syscall
		std::byte{'x'}, std::byte{'8'}, std::byte{'6'}, std::byte{'!'},
		std::byte{'\n'},
	};
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put(0, program);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);

	/* A CS that IRET could not load faults before anything runs, with
	 * the state as the kernel handed it over. */
	darwin_uml_guest_registers refused{};
	uml::testing::GuestFpTail refused_fp{};
	refused.padding = 2;
	refused.pc = base;
	refused.x[16] = 2;
	refused.fpmr = std::uint64_t{0x2b} << 32U;
	unsigned long tls = 0;
	require(darwin_uml_static_enter_guest(&refused, &refused_fp,
		sizeof(refused_fp), generation, &tls) == DARWIN_UML_EVENT_BREAKPOINT);
	require(refused.pc == base);
	require(refused.x[0] == 0);
	require(refused.fault.trap_number == 13);
	require(refused.fault.error_code == 0);
	require(refused.fpmr == std::uint64_t{0x2b} << 32U);

	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.padding = 2;
	registers.pc = base;
	registers.x[16] = 2;
	registers.fpmr = x86_long64_selectors;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 1); // rax
	require(registers.x[7] == 1); // rdi
	require(registers.x[6] == base + 24); // rsi
	require(registers.x[2] == 5); // rdx
	require(registers.pc == base + 24);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* Linux runs x86 userspace with IF set (RFLAGS 0x202 after execve), and no
 * user instruction can clear it. The kernel's register image of a fresh task
 * holds 0, and one restored from a signal frame holds what the frame said, so
 * the host must supply IF on entry, as it supplies the reserved bit. */
static void uml_static_x86_user_interrupt_flag()
{
	constexpr std::uint64_t base = 0x2'c000;
	constexpr std::uint64_t stack = base + 0x800;
	constexpr std::uint64_t interrupt_flag = std::uint64_t{1} << 9;
	constexpr std::uint64_t compat32_execution = std::uint64_t{1} << 33;
	struct Entry final {
		unsigned long generation;
		std::array<std::byte, 4> program;
		std::uint64_t kernel_flags;
		std::uint64_t mode;
		std::uint64_t pushed;
	};
	const std::array entries{
		/* pushfq; pop rax; syscall -- a task fresh from execve. */
		Entry{137, {std::byte{0x9c}, std::byte{0x58}, std::byte{0x0f},
			std::byte{0x05}}, 0, 0, 0x202},
		/* pushfd; pop eax; int 80h -- flags a signal frame restored
		 * without IF. */
		Entry{138, {std::byte{0x9c}, std::byte{0x58}, std::byte{0xcd},
			std::byte{0x80}}, 0x46, compat32_execution, 0x246},
	};
	for (const auto &entry : entries) {
		ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
		backing.put(0, entry.program);
		require(darwin_uml_static_vm_activate(entry.generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
			1, 1) == 0);
		darwin_uml_guest_registers registers{};
		uml::testing::GuestFpTail fp{};
		registers.padding = 2;
		registers.pc = base;
		registers.x[4] = stack; // rsp
		registers.x[16] = entry.kernel_flags;
		registers.svcr = entry.mode;
		registers.fpmr = entry.mode == compat32_execution ?
			x86_compat32_selectors : x86_long64_selectors;
		unsigned long tls = 0;
		const int event = darwin_uml_static_enter_guest(
			&registers, &fp, sizeof(fp), entry.generation, &tls);
		require(event == DARWIN_UML_EVENT_SYSCALL);
		require(registers.x[0] == entry.pushed); // rax
		require((registers.x[16] & interrupt_flag) != 0);
		require(darwin_uml_static_vm_destroy(entry.generation) == 0);
	}
}

static void uml_static_x86_compat32_entry_bridge()
{
	constexpr unsigned long generation = 134;
	constexpr std::uint64_t base = 0x2'4000;
	constexpr std::uint64_t stack = base + 0x800;
	constexpr std::uint64_t compat32_execution = std::uint64_t{1} << 33;
	constexpr std::uint64_t compatibility_syscall = std::uint64_t{1} << 32;
	constexpr std::array program{
		std::byte{0x40},             // inc eax, not a REX prefix
		std::byte{0x8b}, std::byte{0x15},
		std::byte{0x00}, std::byte{0x41}, std::byte{0x02}, std::byte{0x00},
									 // mov edx, dword ptr [0x24100]
		std::byte{0x53},             // push ebx
		std::byte{0x59},             // pop ecx
		std::byte{0xe8}, std::byte{0x02}, std::byte{0x00},
		std::byte{0x00}, std::byte{0x00}, // call the trailing function
		std::byte{0xcd}, std::byte{0x80}, // int 80h
		std::byte{0x40},             // inc eax
		std::byte{0xc3},             // ret
	};
	constexpr std::size_t syscall_return_offset = 16;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put(0, program);
	backing.put32(0x100, 0x5566'7788);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		1, 1) == 0);

	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.padding = 2;
	registers.pc = base;
	registers.x[0] = 41;
	registers.x[3] = 0x1122'3344;
	registers.x[4] = stack;
	registers.x[16] = 2;
	registers.svcr = compat32_execution;
	registers.fpmr = x86_compat32_selectors;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 43);
	require(registers.x[1] == 0x1122'3344);
	require(registers.x[2] == 0x5566'7788);
	require(registers.x[4] == stack);
	require(registers.pc == base + syscall_return_offset);
	require(((registers.fpmr >> 16U) & 0xffffU) == 0x23);
	require(((registers.fpmr >> 32U) & 0xffffU) == 0x2b);
	require((registers.svcr & compat32_execution) != 0);
	require((registers.svcr & compatibility_syscall) != 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_x86_exception_bridge()
{
	constexpr std::uint64_t base = 0x2'8000;
	constexpr std::uint64_t compat32_execution = std::uint64_t{1} << 33;
	{
		constexpr unsigned long generation = 135;
		constexpr std::array program{
			std::byte{0x48}, std::byte{0xf7}, std::byte{0xf1}, // div rcx
		};
		ScratchBacking backing{backing_name,
			DARWIN_UML_GUEST_PAGE_SIZE};
		backing.put(0, program);
		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC,
			1, 0) == 0);
		darwin_uml_guest_registers registers{};
		uml::testing::GuestFpTail fp{};
		registers.padding = 2;
		registers.pc = base;
		registers.x[0] = 42;
		registers.x[1] = 0;
		registers.x[2] = 0;
		registers.x[16] = 2;
		registers.fpmr = x86_long64_selectors;
		unsigned long tls = 0;
		const int event = darwin_uml_static_enter_guest(
			&registers, &fp, sizeof(fp), generation, &tls);
		require(event == DARWIN_UML_EVENT_FLOATING_POINT_TRAP);
		require(registers.pc == base);
		require(registers.fault.address == base);
		require(registers.fault.error_code == 0);
		require(registers.fault.trap_number == 0);
		require(darwin_uml_static_vm_destroy(generation) == 0);
	}
	{
		constexpr unsigned long generation = 136;
		constexpr std::array program{std::byte{0xce}}; // into
		ScratchBacking backing{backing_name,
			DARWIN_UML_GUEST_PAGE_SIZE};
		backing.put(0, program);
		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC,
			1, 0) == 0);
		darwin_uml_guest_registers registers{};
		uml::testing::GuestFpTail fp{};
		registers.padding = 2;
		registers.pc = base;
		registers.x[16] = 2 | (std::uint64_t{1} << 11);
		registers.svcr = compat32_execution;
		registers.fpmr = x86_compat32_selectors;
		unsigned long tls = 0;
		const int event = darwin_uml_static_enter_guest(
			&registers, &fp, sizeof(fp), generation, &tls);
		require(event == DARWIN_UML_EVENT_BREAKPOINT);
		require(registers.pc == base + 1);
		require(registers.fault.address == base + 1);
		require(registers.fault.error_code == 0);
		require(registers.fault.trap_number == 4);
		require(darwin_uml_static_vm_destroy(generation) == 0);
	}
}

static void uml_static_x86_64_signal_return_bridge()
{
	constexpr unsigned long generation = 132;
	constexpr std::uint64_t base = 0x1'c000;
	constexpr std::uint64_t stack = base + 0x800;
	constexpr std::array program{std::byte{0xc3}}; // ret
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put(0, program);
	backing.put64(stack - base, DARWIN_UML_SIGRETURN_TRAMPOLINE);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		1, 1) == 0);

	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.padding = 2;
	registers.pc = base;
	registers.x[16] = 2;
	registers.x[4] = stack; // rsp
	registers.svcr = std::uint64_t{1} << 32;
	registers.fpmr = x86_long64_selectors;
	fp.reserved[1] = 0x5838'3634U;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 15);
	require(registers.x[4] == stack + sizeof(std::uint64_t));
	require(registers.pc == DARWIN_UML_SIGRETURN_TRAMPOLINE);
	require((registers.svcr & (std::uint64_t{1} << 32)) == 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_x86_64_x87_task_state()
{
	constexpr unsigned long generation = 133;
	constexpr std::uint64_t base = 0x2'0000;
	constexpr std::uint64_t result_address = base + 0x200;
	constexpr std::array first_task{
		std::byte{0xd9}, std::byte{0xe8},             // fld1
		std::byte{0x0f}, std::byte{0x05},             // syscall
		std::byte{0xdd}, std::byte{0x1d},             // fstp qword [rip+0x1f6]
		std::byte{0xf6}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x0f}, std::byte{0x05},             // syscall
	};
	constexpr std::array second_task{
		std::byte{0xdb}, std::byte{0xe3},             // fninit
		std::byte{0x0f}, std::byte{0x05},             // syscall
	};
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put(0, first_task);
	backing.put(0x100, second_task);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		1, 1) == 0);

	darwin_uml_guest_registers first_registers{};
	uml::testing::GuestFpTail first_fp{};
	first_registers.padding = 2;
	first_registers.pc = base;
	first_registers.x[16] = 2;
	first_registers.fpmr = x86_long64_selectors;
	darwin_uml_guest_registers second_registers{};
	uml::testing::GuestFpTail second_fp{};
	second_registers.padding = 2;
	second_registers.pc = base + 0x100;
	second_registers.x[16] = 2;
	second_registers.fpmr = x86_long64_selectors;
	unsigned long tls = 0;
	require(darwin_uml_static_enter_guest(&first_registers, &first_fp,
		sizeof(first_fp), generation, &tls) == DARWIN_UML_EVENT_SYSCALL);
	require(first_registers.pc == base + 4);
	require(darwin_uml_static_enter_guest(&second_registers, &second_fp,
		sizeof(second_fp), generation, &tls) == DARWIN_UML_EVENT_SYSCALL);
	require(darwin_uml_static_enter_guest(&first_registers, &first_fp,
		sizeof(first_fp), generation, &tls) == DARWIN_UML_EVENT_SYSCALL);
	void *stored = nullptr;
	require(darwin_uml_static_vm_translate(generation, result_address,
		DARWIN_UML_STATIC_READ, &stored) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t value{};
	std::memcpy(&value, stored, sizeof(value));
	require(value == 0x3ff0'0000'0000'0000ULL);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* The kernel marks every dispatch of a task whose mm another agent may
 * observe, for as long as one may. The runtime keeps the mark of the
 * current x86 dispatch for the x86 JIT, and UML_X86_MM_SHARED_LOG records
 * each change of an address space's mark. */
static void uml_static_x86_mm_shared_dispatch()
{
	constexpr std::uint64_t base = 0x3'0000;
	constexpr std::array program{
		std::byte{0x0f}, std::byte{0x05}, // syscall
	};
	const char *temporary = std::getenv("TMPDIR");
	std::string path = temporary != nullptr && temporary[0] != '\0' ?
		temporary : "/tmp";
	path += "/uml-mm-shared-log.XXXXXX";
	const int descriptor = mkstemp(path.data());
	require(descriptor >= 0);
	close(descriptor);
	require(setenv("UML_X86_MM_SHARED_LOG", path.c_str(), 1) == 0);
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put(0, program);
	auto *context = uml::runtime::current_context();
	require(context != nullptr);
	for (const unsigned long generation : {139UL, 140UL}) {
		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);
		for (const std::uint32_t flags : {0U,
			     unsigned{DARWIN_UML_DISPATCH_MM_SHARED},
			     unsigned{DARWIN_UML_DISPATCH_MM_SHARED}, 0U, 0U,
			     unsigned{DARWIN_UML_DISPATCH_MM_SHARED}}) {
			darwin_uml_guest_registers registers{};
			uml::testing::GuestFpTail fp{};
			registers.padding = 2;
			registers.pc = base;
			registers.dispatch_flags = flags;
			registers.fpmr = x86_long64_selectors;
			unsigned long tls = 0;
			require(darwin_uml_static_enter_guest(&registers, &fp,
				sizeof(fp), generation, &tls) ==
				DARWIN_UML_EVENT_SYSCALL);
			require(registers.dispatch_flags == flags);
			require(context->thread_execution_state()->x86_mm_shared ==
				(flags != 0));
		}
		require(darwin_uml_static_vm_destroy(generation) == 0);
	}
	require(unsetenv("UML_X86_MM_SHARED_LOG") == 0);
	std::ifstream log{path};
	std::vector<std::string> lines;
	for (std::string line; std::getline(log, line);)
		lines.push_back(line);
	require(lines == std::vector<std::string>{
		"uml-x86 mm-shared generation=139 shared=1 rip=0x30000",
		"uml-x86 mm-shared generation=139 shared=0 rip=0x30000",
		"uml-x86 mm-shared generation=139 shared=1 rip=0x30000",
		"uml-x86 mm-shared generation=140 shared=1 rip=0x30000",
		"uml-x86 mm-shared generation=140 shared=0 rip=0x30000",
		"uml-x86 mm-shared generation=140 shared=1 rip=0x30000",
	});
	require(std::remove(path.c_str()) == 0);
}
#endif

static void uml_static_descriptor_reuse_invalidation()
{
	constexpr unsigned long first_generation = 107;
	constexpr unsigned long second_generation = 108;
	constexpr std::uint64_t base = 0x4'0000;
	constexpr std::uint64_t first_value = 0x1111'2222'3333'4444ULL;
	constexpr std::uint64_t second_value = 0xaaaa'bbbb'cccc'ddddULL;
	auto first_path =
		uml::testing::scratch_template("uml-static-fd-first-XXXXXX");
	int descriptor = mkstemp(first_path.data());
	require(descriptor >= 0);
	unlink(first_path.c_str());
	require(ftruncate(descriptor, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(pwrite(descriptor, &first_value, sizeof(first_value), 0) ==
		static_cast<ssize_t>(sizeof(first_value)));
	require(darwin_uml_static_vm_activate(first_generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), descriptor, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	const int reused_descriptor = descriptor;
	require(darwin_uml_close(descriptor) == 0);

	auto second_path =
		uml::testing::scratch_template("uml-static-fd-second-XXXXXX");
	descriptor = mkstemp(second_path.data());
	require(descriptor >= 0);
	unlink(second_path.c_str());
	if (descriptor != reused_descriptor) {
		require(dup2(descriptor, reused_descriptor) == reused_descriptor);
		require(darwin_uml_close(descriptor) == 0);
		descriptor = reused_descriptor;
	}
	require(ftruncate(descriptor, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(pwrite(descriptor, &second_value, sizeof(second_value), 0) ==
		static_cast<ssize_t>(sizeof(second_value)));
	require(darwin_uml_static_vm_activate(second_generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), descriptor, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(
		second_generation, base, DARWIN_UML_STATIC_READ,
		&translated) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t actual = 0;
	std::memcpy(&actual, translated, sizeof(actual));
	require(actual == second_value);
	require(darwin_uml_close(descriptor) == 0);
	require(darwin_uml_static_vm_destroy(first_generation) == 0);
	require(darwin_uml_static_vm_destroy(second_generation) == 0);
}

static void uml_static_inode_less_backings_remain_distinct()
{
	constexpr unsigned long generation = 110;
	constexpr std::uint64_t base = 0x5'0000;
	constexpr std::uint64_t first_value = 0x1122'3344'5566'7788ULL;
	constexpr std::uint64_t second_value = 0x8877'6655'4433'2211ULL;
	constexpr std::uint64_t first_replacement = 0x1234'5678'9abc'def0ULL;
	constexpr std::uint64_t second_replacement = 0x0fed'cba9'8765'4321ULL;
	PosixSharedMemoryBacking first;
	PosixSharedMemoryBacking second;
	std::memcpy(first.data(), &first_value, sizeof(first_value));
	std::memcpy(second.data(), &second_value, sizeof(second_value));

	/* Distinct Darwin shm objects currently share a zero stat identity; their
	 * contents must remain independent. */
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
		first.fd(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(
		base + DARWIN_UML_GUEST_PAGE_SIZE), second.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *first_host = nullptr;
	void *second_host = nullptr;
	require(darwin_uml_static_vm_translate(generation, base,
		DARWIN_UML_STATIC_READ, &first_host) == DARWIN_UML_STATIC_TRANSLATED);
	require(darwin_uml_static_vm_translate(generation,
		base + DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_STATIC_READ,
		&second_host) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t actual_first = 0;
	std::uint64_t actual_second = 0;
	std::memcpy(&actual_first, first_host, sizeof(actual_first));
	std::memcpy(&actual_second, second_host, sizeof(actual_second));
	require(actual_first == first_value);
	require(actual_second == second_value);

	std::memcpy(first_host, &first_replacement, sizeof(first_replacement));
	std::memcpy(second_host, &second_replacement, sizeof(second_replacement));
	std::memcpy(&actual_first, first.data(), sizeof(actual_first));
	std::memcpy(&actual_second, second.data(), sizeof(actual_second));
	require(actual_first == first_replacement);
	require(actual_second == second_replacement);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_host_interrupt_sequence()
{
	using namespace std::chrono_literals;
	constexpr unsigned long generation = 106;
	constexpr std::uint64_t base = 0x3'c000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x1400'0000U); // b .
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);

	unsigned int sequence = 0;
	require(darwin_uml_static_guest_interrupt_sequence(
		&sequence, sequence) == 0);
	std::thread interrupt{[&] {
		std::this_thread::sleep_for(10ms);
		__atomic_fetch_add(&sequence, 1U, __ATOMIC_RELEASE);
	}};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	interrupt.join();
	require(darwin_uml_static_guest_interrupt_sequence(nullptr, 0) == 0);
	require(event == DARWIN_UML_EVENT_INTERRUPT);
	require(registers.pc == base);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_signal_return_bridge()
{
	constexpr std::uint64_t base = 0x1'4000;
	for (const auto [generation, fallback] :
	     std::array<std::pair<unsigned long, bool>, 2>{{{78, false}, {79, true}}}) {
		ScratchBacking backing{backing_name,
			DARWIN_UML_GUEST_PAGE_SIZE};
		if (fallback) {
			backing.put32(0, 0x8b01'0000U); // add x0, x0, x1
			backing.put32(4, 0xd65f'03c0U); // ret
		} else {
			backing.put32(0, 0xd65f'03c0U); // ret
		}

		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_EXEC, 1, 0) == 0);
		darwin_uml_guest_registers registers{};
		uml::testing::GuestFpTail fp{};
		registers.pc = base;
		registers.lr = DARWIN_UML_SIGRETURN_TRAMPOLINE;
		registers.x[0] = 4;
		registers.x[1] = 5;
		unsigned long tls = 0;
		const int event = darwin_uml_static_enter_guest(
			&registers, &fp, sizeof(fp), generation, &tls);
		require(event == DARWIN_UML_EVENT_SYSCALL);
		require(registers.x[8] == DARWIN_UML_NR_RT_SIGRETURN);
		require(registers.pc == DARWIN_UML_SIGRETURN_TRAMPOLINE + 4);
		if (fallback)
			require(registers.x[0] == 9);
		require(darwin_uml_static_vm_destroy(generation) == 0);
	}
}

static void uml_static_permission_fault()
{
	constexpr unsigned long generation = 91;
	constexpr std::uint64_t base = 0x2'0000;
	ScratchBacking backing{backing_name, 2 * DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd280'0aa0U);  // movz x0, #0x55
	backing.put32(4, 0xf900'0020U);  // str x0, [x1]
	backing.put32(8, 0xd400'0001U);  // svc #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base + DARWIN_UML_GUEST_PAGE_SIZE),
		backing.descriptor(), DARWIN_UML_GUEST_PAGE_SIZE,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ, 0, 1) == 0);
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.x[1] = base + DARWIN_UML_GUEST_PAGE_SIZE;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_FAULT);
	require(registers.pc == base + 4);
	require(registers.fault.address == base + DARWIN_UML_GUEST_PAGE_SIZE);
	require(registers.fault.error_code == DARWIN_UML_FAULT_WRITE);
	require(registers.fault.trap_number == DARWIN_UML_FAULT_PAGE);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_illegal_instruction()
{
	constexpr unsigned long generation = 109;
	constexpr std::uint64_t base = 0x2'4000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x0000'0000U); // udf #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION);
	require(registers.pc == base);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* From bridge ABI 22 an AArch32 task is one whose pstate has nRW set. Both
 * engines run it, so an entry that names one executes A32 code and leaves at
 * its system call with the task still in AArch32 state. */
static void uml_static_aarch32_entry_runs()
{
	constexpr unsigned long generation = 133;
	constexpr std::uint64_t base = 0x2'4000;
	constexpr std::uint32_t pstate_aarch32_user = 0x10U;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xe3a0'0001U); // mov r0, #1
	backing.put32(4, 0xef00'0000U); // svc #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.pstate = pstate_aarch32_user;
	registers.x[0] = 0x1'2345'6789ULL;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	/* The MOV ran, the SVC retired, and the task is still an AArch32 one. */
	require(registers.pc == base + 8);
	require(registers.x[0] == 1);
	require((registers.pstate & pstate_aarch32_user) == pstate_aarch32_user);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_mapping_change_ranges()
{
	constexpr unsigned long generation = 97;
	constexpr std::uint64_t base = 0x2'4000;
	ScratchBacking backing{backing_name, 2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_activate(generation) == 0);
	const auto initial = darwin_uml_static_vm_changes(generation);
	const auto initial_sequences =
		darwin_uml_static_vm_sequences(generation);
	require(initial_sequences.mapping_changes == initial);
	require(initial_sequences.code_changes ==
		darwin_uml_static_vm_code_changes(generation));
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	std::array<darwin_uml_static_mapping_change, 4> ranges{};
	std::uint64_t current = 0;
	require(darwin_uml_static_vm_mapping_changes(
		generation, initial, ranges.data(), ranges.size(), &current) == 1);
	require(ranges[0].address == base);
	require(ranges[0].length == 2 * DARWIN_UML_GUEST_PAGE_SIZE);
	require(current == darwin_uml_static_vm_changes(generation));
	const auto mapped_sequences =
		darwin_uml_static_vm_sequences(generation);
	require(mapped_sequences.mapping_changes == current);
	require(mapped_sequences.code_changes == initial_sequences.code_changes);

	/* Replaying an unchanged Linux PTE batch must not invalidate the TLB. */
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(darwin_uml_static_vm_changes(generation) == current);

	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base + DARWIN_UML_GUEST_PAGE_SIZE),
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ) == 0);
	const auto before_protect = current;
	require(darwin_uml_static_vm_mapping_changes(
		generation, before_protect, ranges.data(), ranges.size(), &current) == 1);
	require(ranges[0].address == base + DARWIN_UML_GUEST_PAGE_SIZE);
	require(ranges[0].length == DARWIN_UML_GUEST_PAGE_SIZE);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_partial_uaccess_copy_preserves_sibling_writes()
{
	constexpr unsigned long generation = 130;
	constexpr std::uint64_t base = 0x2'8000;
	constexpr std::size_t write_offset = 32;
	constexpr std::size_t concurrent_offset = 512;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> stale_shadow{};
	stale_shadow.fill(std::byte{0x11});
	backing.put(0, stale_shadow);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(generation, base,
		DARWIN_UML_STATIC_WRITE, &translated) == DARWIN_UML_STATIC_TRANSLATED);
	auto *bytes = static_cast<std::byte *>(translated);
	bytes[concurrent_offset] = std::byte{0x7a};
	const std::array replacement{
		std::byte{0xa0}, std::byte{0xa1}, std::byte{0xa2}, std::byte{0xa3}};
	std::copy(replacement.begin(), replacement.end(),
		stale_shadow.begin() + write_offset);
	require(darwin_uml_static_vm_copy_from_buffer(
		base + write_offset, stale_shadow.data() + write_offset,
		replacement.size()) == 0);
	require(std::equal(replacement.begin(), replacement.end(),
		bytes + write_offset));
	require(bytes[concurrent_offset] == std::byte{0x7a});
	require(darwin_uml_static_vm_copy_from_buffer(base, nullptr, 0) == 0);
	require(darwin_uml_static_vm_copy_from_buffer(
		base + DARWIN_UML_GUEST_PAGE_SIZE - 1, stale_shadow.data(), 2) == -1);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_code_page_stamps_follow_physical_aliases()
{
	constexpr unsigned long generation = 131;
	constexpr std::uint64_t first = 0x2'c000;
	constexpr std::uint64_t second = first + DARWIN_UML_GUEST_PAGE_SIZE;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_activate(generation) == 0);
	for (const auto address : {first, second})
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(address),
			backing.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_WRITE | PROT_EXEC, 1, 1) == 0);

	void *first_handle = nullptr;
	void *second_handle = nullptr;
	darwin_uml_static_code_page_stamp first_stamp{};
	darwin_uml_static_code_page_stamp second_stamp{};
	require(darwin_uml_static_vm_code_page(
		generation, first, &first_handle, &first_stamp) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(darwin_uml_static_vm_code_page(
		generation, second, &second_handle, &second_stamp) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(first_handle == second_handle);
	require(first_stamp.identity == second_stamp.identity);
	require(first_stamp.version == second_stamp.version);

	darwin_uml_static_vm_invalidate_code_page(first_handle);
	require(darwin_uml_static_vm_code_page_stamp(
		second_handle, &second_stamp) == 0);
	require(second_stamp.identity == first_stamp.identity);
	require(second_stamp.version == first_stamp.version + 1);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* Aliases share a record; different backing pages must not. The index that
 * answers the repeated ask without a lock is two levels deep, so a page in the
 * second leaf and a page in the first have to keep records of their own, and
 * invalidating either must leave the other's version alone.
 */
static void uml_static_code_pages_are_distinct_across_the_index()
{
	constexpr unsigned long generation = 132;
	constexpr std::uint64_t base = 0x3'c000;
	/* One page past the first index leaf, which covers 512 backing pages. */
	constexpr std::uint64_t distant_page = 512;
	constexpr std::size_t pages = distant_page + 1U;
	ScratchBacking backing{backing_name,
		pages * DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_activate(generation) == 0);
	const std::uint64_t offsets[] = {
		0, DARWIN_UML_GUEST_PAGE_SIZE,
		distant_page * DARWIN_UML_GUEST_PAGE_SIZE};
	void *handles[3] = {};
	darwin_uml_static_code_page_stamp stamps[3]{};
	for (std::size_t index = 0; index < 3; ++index) {
		const auto address = base + index * DARWIN_UML_GUEST_PAGE_SIZE;
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(address), backing.descriptor(),
			offsets[index], DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_WRITE | PROT_EXEC, 1, 1) == 0);
		require(darwin_uml_static_vm_code_page(generation, address,
			&handles[index], &stamps[index]) ==
			DARWIN_UML_STATIC_TRANSLATED);
	}
	require(handles[0] != handles[1] && handles[1] != handles[2] &&
		handles[0] != handles[2]);
	require(stamps[0].identity != stamps[1].identity &&
		stamps[1].identity != stamps[2].identity &&
		stamps[0].identity != stamps[2].identity);

	darwin_uml_static_vm_invalidate_code_page(handles[2]);
	for (std::size_t index = 0; index < 3; ++index) {
		darwin_uml_static_code_page_stamp observed{};
		/* The second ask is the one the index answers. */
		void *again = nullptr;
		require(darwin_uml_static_vm_code_page(generation,
			base + index * DARWIN_UML_GUEST_PAGE_SIZE, &again,
			&observed) == DARWIN_UML_STATIC_TRANSLATED);
		require(again == handles[index]);
		require(observed.identity == stamps[index].identity);
		require(observed.version == stamps[index].version +
			(index == 2 ? 1U : 0U));
	}
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_thread_local_generations()
{
	constexpr unsigned long first_generation = 101;
	constexpr unsigned long second_generation = 102;
	constexpr std::uint64_t base = 0x3'0000;
	constexpr std::uint64_t first_value = 0x1111'2222'3333'4444ULL;
	constexpr std::uint64_t second_value = 0xaaaa'bbbb'cccc'ddddULL;
	ScratchBacking backing{backing_name, 2 * DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put64(0, first_value);
	backing.put64(DARWIN_UML_GUEST_PAGE_SIZE, second_value);
	std::barrier first_activated{2};
	std::barrier second_activated{2};
	std::thread first{[&] {
		require(darwin_uml_static_vm_activate(first_generation) == 0);
		first_activated.arrive_and_wait();
		second_activated.arrive_and_wait();
		require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
			backing.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_WRITE, 0, 1) == 0);
	}};
	std::thread second{[&] {
		first_activated.arrive_and_wait();
		require(darwin_uml_static_vm_activate(second_generation) == 0);
		second_activated.arrive_and_wait();
		require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
			backing.descriptor(), DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	}};
	first.join();
	second.join();
	void *first_host = nullptr;
	void *second_host = nullptr;
	require(darwin_uml_static_vm_translate(first_generation, base,
		DARWIN_UML_STATIC_READ, &first_host) == DARWIN_UML_STATIC_TRANSLATED);
	require(darwin_uml_static_vm_translate(second_generation, base,
		DARWIN_UML_STATIC_READ, &second_host) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t first_actual = 0;
	std::uint64_t second_actual = 0;
	std::memcpy(&first_actual, first_host, sizeof(first_actual));
	std::memcpy(&second_actual, second_host, sizeof(second_actual));
	require(first_actual == first_value);
	require(second_actual == second_value);
	const auto first_mapping_changes =
		darwin_uml_static_vm_changes(first_generation);
	const auto second_mapping_changes =
		darwin_uml_static_vm_changes(second_generation);
	const auto first_code_changes =
		darwin_uml_static_vm_code_changes(first_generation);
	const auto second_code_changes =
		darwin_uml_static_vm_code_changes(second_generation);
	const auto first_sequences =
		darwin_uml_static_vm_sequences(first_generation);
	const auto second_sequences =
		darwin_uml_static_vm_sequences(second_generation);
	require(first_sequences.mapping_changes == first_mapping_changes);
	require(first_sequences.code_changes == first_code_changes);
	require(second_sequences.mapping_changes == second_mapping_changes);
	require(second_sequences.code_changes == second_code_changes);
	require(darwin_uml_static_vm_activate(second_generation) == 0);
	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base), DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ) == 0);
	require(darwin_uml_static_vm_changes(first_generation) ==
		first_mapping_changes);
	require(darwin_uml_static_vm_changes(second_generation) >
		second_mapping_changes);
	require(darwin_uml_static_vm_code_changes(first_generation) ==
		first_code_changes);
	require(darwin_uml_static_vm_code_changes(second_generation) ==
		second_code_changes);
	darwin_uml_static_vm_invalidate_code(base);
	require(darwin_uml_static_vm_code_changes(first_generation) ==
		first_code_changes);
	require(darwin_uml_static_vm_code_changes(second_generation) >
		second_code_changes);
	require(darwin_uml_static_vm_destroy(first_generation) == 0);
	require(darwin_uml_static_vm_destroy(second_generation) == 0);
}

static void uml_static_concurrent_translation_readers()
{
	constexpr std::size_t reader_count = 16;
	constexpr std::size_t translation_count = 20'000;
	constexpr std::size_t protection_count = 4'000;
	constexpr unsigned long first_generation = 106;
	constexpr unsigned long writer_generation =
		first_generation + reader_count;
	constexpr std::uint64_t base = 0x3'0000;
	ScratchBacking backing{backing_name, (reader_count + 1) *
		DARWIN_UML_GUEST_PAGE_SIZE};

	for (std::size_t index = 0; index <= reader_count; ++index) {
		const std::uint64_t value = 0x5a5a'0000'0000'0000ULL + index;
		backing.put64(index * DARWIN_UML_GUEST_PAGE_SIZE, value);
		const auto generation = first_generation + index;
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(base), backing.descriptor(),
			index * DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
			0, 1) == 0);
	}

	std::barrier start{reader_count + 1};
	std::vector<std::thread> readers;
	readers.reserve(reader_count);
	for (std::size_t index = 0; index < reader_count; ++index) {
		readers.emplace_back([&, index] {
			const auto generation = first_generation + index;
			const std::uint64_t expected =
				0x5a5a'0000'0000'0000ULL + index;
			std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
			std::memcpy(page.data(), &expected, sizeof(expected));
			require(darwin_uml_static_vm_activate(generation) == 0);
			start.arrive_and_wait();
			for (std::size_t iteration = 0;
			     iteration < translation_count; ++iteration) {
				if (iteration % 1'000 == 0)
					require(darwin_uml_static_vm_copy_from_buffer(
						base, page.data(), page.size()) == 0);
				void *host = nullptr;
				require(darwin_uml_static_vm_translate(generation, base,
					DARWIN_UML_STATIC_READ, &host) ==
					DARWIN_UML_STATIC_TRANSLATED);
				std::uint64_t actual = 0;
				std::memcpy(&actual, host, sizeof(actual));
				require(actual == expected);
			}
		});
	}
	std::thread writer{[&] {
		require(darwin_uml_static_vm_activate(writer_generation) == 0);
		start.arrive_and_wait();
		for (std::size_t iteration = 0; iteration < protection_count;
		     ++iteration) {
			const int protection = iteration % 2 == 0 ? PROT_READ :
				PROT_READ | PROT_WRITE;
			require(darwin_uml_static_vm_protect(
				reinterpret_cast<void *>(base),
				DARWIN_UML_GUEST_PAGE_SIZE, protection) == 0);
		}
	}};
	for (auto &reader : readers)
		reader.join();
	writer.join();
	for (std::size_t index = 0; index <= reader_count; ++index)
		require(darwin_uml_static_vm_destroy(first_generation + index) == 0);
}

static void uml_static_tlb_shootdown_quiesces_execution()
{
	using namespace std::chrono_literals;
	constexpr unsigned long generation = 103;
	constexpr std::uint64_t base = 0x3'4000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
		backing.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);

	std::barrier execution_entered{2};
	std::barrier invalidation_started{2};
	std::atomic_bool release_execution{};
	std::atomic_bool invalidation_complete{};
	std::atomic<void *> context{};
	std::thread execution{[&] {
		void *execution_context =
			darwin_uml_static_vm_execution_context_create(generation);
		require(execution_context != nullptr);
		require(darwin_uml_static_vm_execution_context_begin(
			execution_context, base) == 0);
		context.store(execution_context, std::memory_order_release);
		execution_entered.arrive_and_wait();
		release_execution.wait(false, std::memory_order_acquire);
		require(darwin_uml_static_vm_execution_context_end(execution_context) == 0);
		darwin_uml_static_vm_execution_context_destroy(execution_context);
	}};
	execution_entered.arrive_and_wait();
	void *execution_context = context.load(std::memory_order_acquire);
	require(execution_context != nullptr);
	std::thread invalidation{[&] {
		invalidation_started.arrive_and_wait();
		require(darwin_uml_static_vm_unmap_generation(
			generation, reinterpret_cast<void *>(base),
			DARWIN_UML_GUEST_PAGE_SIZE) == 0);
		invalidation_complete.store(true, std::memory_order_release);
		invalidation_complete.notify_all();
	}};
	invalidation_started.arrive_and_wait();
	std::this_thread::sleep_for(20ms);
	require(!invalidation_complete.load(std::memory_order_acquire));
	const auto interrupted =
		darwin_uml_static_vm_execution_context_sequences(execution_context);
	require(interrupted.execution_interrupt != 0);
	release_execution.store(true, std::memory_order_release);
	release_execution.notify_all();
	execution.join();
	invalidation.join();
	require(invalidation_complete.load(std::memory_order_acquire));
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(generation, base,
		DARWIN_UML_STATIC_READ, &translated) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_execution_context_code_publication()
{
	constexpr unsigned long generation = 107;
	constexpr std::uint64_t base = 0x3'6000;
	require(darwin_uml_static_vm_activate(generation) == 0);
	void *context =
		darwin_uml_static_vm_execution_context_create(generation);
	require(context != nullptr);
	require(darwin_uml_static_vm_execution_context_begin(context, base) == 0);
	const auto before =
		darwin_uml_static_vm_execution_context_sequences(context);
	require(uml::memory::execution_context_publish_code_change(
		context, base));
	const auto after =
		darwin_uml_static_vm_execution_context_sequences(context);
	require(after.mapping_changes == before.mapping_changes);
	require(after.code_changes == before.code_changes + 1);
	require(after.execution_interrupt == before.execution_interrupt);
	/* A reader still holding the earlier sequence is told which page moved,
	 * so it can retire the translations covering it rather than all of them.
	 */
	struct darwin_uml_static_code_change changed[4] = {};
	std::uint64_t current = 0;
	require(darwin_uml_static_vm_execution_context_code_change_ranges(
		context, before.code_changes, changed, 4, &current) == 1);
	require(current == after.code_changes);
	require(changed[0].address ==
		(base & ~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U}));
	require(changed[0].length == DARWIN_UML_GUEST_PAGE_SIZE);
	require(darwin_uml_static_vm_execution_context_code_change_ranges(
		context, after.code_changes, changed, 4, &current) == 0);
	require(darwin_uml_static_vm_execution_context_end(context) == 0);
	/* A stale token must fall back to the routed invalidation path instead of
	 * mutating a generation after its read-side execution lease has ended.
	 */
	require(!uml::memory::execution_context_publish_code_change(
		context, base));
	const auto ended =
		darwin_uml_static_vm_execution_context_sequences(context);
	require(ended.code_changes == after.code_changes);
	darwin_uml_static_vm_execution_context_destroy(context);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_batch_targets_explicit_generation()
{
	constexpr unsigned long active_generation = 104;
	constexpr unsigned long target_generation = 105;
	constexpr std::uint64_t base = 0x3'8000;
	constexpr std::uint64_t value = 0x1234'5678'9abc'def0ULL;
	constexpr std::uint64_t replacement = 0x0fed'cba9'8765'4321ULL;
	ScratchBacking backing{backing_name, 2 * DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put64(0, value);
	backing.put64(DARWIN_UML_GUEST_PAGE_SIZE, replacement);
	require(darwin_uml_static_vm_activate(active_generation) == 0);

	/* Page-table synchronization may run on behalf of an mm other than the
	 * one active on this Darwin thread.  The batch and every operation inside
	 * it must target that explicit generation, and a mismatched nested batch
	 * must fail without disturbing the outer writer gate.
	 */
	require(darwin_uml_static_vm_batch_begin(target_generation) == 0);
	require(darwin_uml_static_vm_batch_begin(active_generation) == -1);
	require(darwin_uml_static_vm_map_generation(target_generation,
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *target_host = nullptr;
	require(darwin_uml_static_vm_translate(target_generation, base,
		DARWIN_UML_STATIC_READ, &target_host) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_batch_end() == 0);

	void *active_host = nullptr;
	require(darwin_uml_static_vm_translate(active_generation, base,
		DARWIN_UML_STATIC_READ, &active_host) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_translate(target_generation, base,
		DARWIN_UML_STATIC_READ, &target_host) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t actual = 0;
	std::memcpy(&actual, target_host, sizeof(actual));
	require(actual == value);

	/* An invalidation which wins while a page-table snapshot is buffered
	 * makes that snapshot stale.  The commit must be rejected instead of
	 * resurrecting its old physical mapping.
	 */
	require(darwin_uml_static_vm_batch_begin(target_generation) == 0);
	require(darwin_uml_static_vm_map_generation(target_generation,
		reinterpret_cast<void *>(base), backing.descriptor(),
		DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	std::thread invalidation{[&] {
		require(darwin_uml_static_vm_unmap_generation(
			target_generation, reinterpret_cast<void *>(base),
			DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	}};
	invalidation.join();
	require(darwin_uml_static_vm_batch_end() == -EAGAIN);
	target_host = nullptr;
	require(darwin_uml_static_vm_translate(target_generation, base,
		DARWIN_UML_STATIC_READ, &target_host) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_destroy(active_generation) == 0);
	require(darwin_uml_static_vm_destroy(target_generation) == 0);
}

/* A walker binds its own thread to a generation for the length of a walk.  The
 * binding is per thread: a sibling walks the same generation meanwhile, and an
 * exclusive binding would leave this join waiting for the unlock below it. */
static void uml_static_walker_binding_is_per_thread()
{
	constexpr unsigned long generation = 106;

	require(darwin_uml_static_vm_walker_lock(generation) == 0);
	require(darwin_uml_static_vm_walker_lock(generation) == -1);
	require(darwin_uml_static_vm_walker_unlock(generation + 1) == -1);
	std::thread sibling{[&] {
		require(darwin_uml_static_vm_walker_lock(generation) == 0);
		require(darwin_uml_static_vm_walker_unlock(generation) == 0);
	}};
	sibling.join();
	require(darwin_uml_static_vm_walker_unlock(generation) == 0);
	require(darwin_uml_static_vm_walker_unlock(generation) == -1);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* An exception return ends the local exclusive monitor, and so the engines'
 * software record of it: a store-exclusive after the guest comes back from
 * the kernel fails. The kernel's remap of the page between the pair empties
 * the target's TLB entry, so the store-exclusive misses and, were the record
 * live, would complete as a compare-and-swap of the recorded value at the
 * same address. */
static void uml_static_exception_return_ends_the_exclusive_record()
{
	constexpr unsigned long generation = 140;
	constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t base = 0x1'0000;
	constexpr std::uint64_t target = 0x4000'0000;
	constexpr std::uint64_t initial = 0x1122'3344'5566'7788ULL;
	constexpr std::uint64_t replacement = 0xa1a2'a3a4'a5a6'a7a8ULL;

	ScratchBacking backing{backing_name, 2 * page};
	const std::array<std::uint32_t, 4> program{
		0xc85f'7c41U,  // ldxr x1, [x2]
		0xd400'0001U,  // svc #0
		0xc807'7c49U,  // stxr w7, x9, [x2]
		0xd400'0001U,  // svc #0
	};
	for (std::size_t index = 0; index < program.size(); ++index)
		backing.put32(4 * index, program[index]);
	backing.put(page, std::as_bytes(std::span{&initial, 1}));

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.descriptor(), 0, page,
		PROT_READ | PROT_EXEC, 1, 0) == 0);
	const auto map_target = [&]() {
		return darwin_uml_static_vm_map(
			reinterpret_cast<void *>(target), backing.descriptor(),
			page, page, PROT_READ | PROT_WRITE, 0, 1);
	};
	require(map_target() == 0);

	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	unsigned long tls = 0;
	registers.pc = base;
	registers.x[2] = target;
	registers.x[9] = replacement;
	require(darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 8);
	require(registers.x[1] == initial);

	// The kernel replaces the mapping: the thread's next entry drops the
	// target's TLB entry, the host address stays the same.
	require(darwin_uml_static_vm_unmap(
		reinterpret_cast<void *>(target), page) == 0);
	require(map_target() == 0);

	require(darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 16);
	require(registers.x[7] == 1);
	std::uint64_t value = 0;
	backing.get(page, std::as_writable_bytes(std::span{&value, 1}));
	require(value == initial);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	uml_static_entry_bridge();
#if DARWIN_UML_X86_64_GUEST
	/* First: the runtime reads UML_X86_MM_SHARED_LOG at the process's
	 * first x86 dispatch. */
	uml_static_x86_mm_shared_dispatch();
	uml_static_x86_64_entry_bridge();
	uml_static_x86_user_interrupt_flag();
	uml_static_x86_compat32_entry_bridge();
	uml_static_x86_exception_bridge();
	uml_static_x86_64_signal_return_bridge();
	uml_static_x86_64_x87_task_state();
#endif
	uml_static_descriptor_reuse_invalidation();
	uml_static_inode_less_backings_remain_distinct();
	uml_static_host_interrupt_sequence();
	uml_static_signal_return_bridge();
	uml_static_permission_fault();
	uml_static_illegal_instruction();
	uml_static_aarch32_entry_runs();
	uml_static_mapping_change_ranges();
	uml_static_partial_uaccess_copy_preserves_sibling_writes();
	uml_static_code_page_stamps_follow_physical_aliases();
	uml_static_code_pages_are_distinct_across_the_index();
	uml_static_thread_local_generations();
	uml_static_concurrent_translation_readers();
	uml_static_tlb_shootdown_quiesces_execution();
	uml_static_execution_context_code_publication();
	uml_static_batch_targets_explicit_generation();
	uml_static_walker_binding_is_per_thread();
	uml_static_exception_return_ends_the_exclusive_record();
	return 0;
}
