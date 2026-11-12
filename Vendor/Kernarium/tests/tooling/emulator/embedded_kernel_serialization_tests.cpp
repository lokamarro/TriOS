// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "uml_elf64.h"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "kernel_resources.hpp"
#include "kernel_threads.hpp"
#include "context.hpp"
#include "testing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define UML_LOADER_TEST_WITH_SANITIZER 1
#endif
#endif

/* The ELF link address of the real Darwin UML kernel. The image runs at the
 * process's window, wherever that is. */
constexpr std::uint64_t linked_address = DARWIN_UML_NATIVE_LINK_ADDRESS;

/* These fixtures exercise process admission, so keep the managed-JIT run gate
 * from becoming an earlier independent serialization boundary. */
constexpr char admission_engine_argument[] = "--execution-engine=gadgets";

#if DARWIN_UML_NATIVE_HIGH_PLACEMENT
/* The process reserves its window once and the images run inside it. The
 * product's window is where macOS puts it; a Darwin sanitizer holds that
 * range itself (AddressSanitizer's heap, ThreadSanitizer's shadow), so a
 * sanitized build takes the start of ThreadSanitizer's middle application
 * range, which AddressSanitizer leaves to the application too. */
#if defined(UML_LOADER_TEST_WITH_SANITIZER)
constexpr std::uint64_t native_window_start = 0x550000000000ULL;
#else
constexpr std::uint64_t native_window_start =
	DARWIN_UML_MACOS_NATIVE_ADDRESS;
#endif
#else
/* Page-aligned, so the auxiliary allowance is this RAM. */
constexpr std::uint64_t default_guest_memory = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t default_auxiliary_memory =
	std::max(default_guest_memory,
		static_cast<std::uint64_t>(
			DARWIN_UML_NATIVE_AUXILIARY_MINIMUM)) +
	DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION +
	DARWIN_UML_NATIVE_STUB_PAGES * DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint64_t native_window_start =
	DARWIN_UML_NATIVE_ADDRESS_CEILING - default_guest_memory -
	default_auxiliary_memory;
#endif
constexpr std::uint64_t loader_runtime_address = linked_address +
	native_window_start - DARWIN_UML_NATIVE_LINK_ADDRESS;

struct Interleave final {
	std::mutex mutex;
	std::condition_variable changed;
	bool first_entered{};
	bool second_started{};
	bool second_handle_admitted{};
	bool second_at_wait_boundary{};
	bool second_entered{};
	bool second_finished{};
	bool release_first{};
} interleave;

int mmu_backing_fd = -1;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

int first_kernel(int, char *const[], char *const[])
{
	std::unique_lock lock{interleave.mutex};
	interleave.first_entered = true;
	interleave.changed.notify_all();
	if (!interleave.changed.wait_for(lock, 5s, [] {
		return interleave.release_first;
	}))
		return 91;
	return 17;
}

int second_kernel(int, char *const[], char *const[])
{
	std::lock_guard lock{interleave.mutex};
	interleave.second_entered = true;
	interleave.changed.notify_all();
	return 23;
}

int mapping_kernel(int, char *const[], char *const[])
{
	constexpr unsigned long generation = 991;
	constexpr unsigned long user_address = 0xa0'0000;
	const bool user_mapped = darwin_uml_static_vm_map_generation(
		generation, reinterpret_cast<void *>(user_address), mmu_backing_fd,
		0, DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0;
	const bool generation_zero_mapped = darwin_uml_static_vm_map_generation(
		generation, reinterpret_cast<void *>(native_window_start), mmu_backing_fd,
		0, DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0;
	return user_mapped && generation_zero_mapped ? 31 : 94;
}

int verify_fresh_mmu_kernel(int, char *const[], char *const[])
{
	constexpr unsigned long generation = 991;
	constexpr unsigned long user_address = 0xa0'0000;
	void *user = nullptr;
	void *generation_zero = nullptr;
	const bool user_fresh = darwin_uml_static_vm_translate(
		generation, user_address, DARWIN_UML_STATIC_READ, &user) ==
		DARWIN_UML_STATIC_UNMAPPED;
	const bool generation_zero_fresh = darwin_uml_static_vm_translate(
		generation, native_window_start, DARWIN_UML_STATIC_READ,
		&generation_zero) == DARWIN_UML_STATIC_UNMAPPED;
	return user_fresh && generation_zero_fresh ? 32 : 95;
}

/* Leaves process signal state behind the way the Darwin kernel does: a
 * SIGALRM handler inside its own image (here the image's entry trampoline), an
 * armed ITIMER_REAL, and a tick raised while every thread blocks SIGALRM,
 * which Darwin parks on the main thread. */
int signal_owning_kernel(int, char *const[], char *const[])
{
	struct sigaction action {};
	action.sa_sigaction = reinterpret_cast<void (*)(int, siginfo_t *, void *)>(
		static_cast<std::uintptr_t>(loader_runtime_address));
	action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGALRM, &action, nullptr) != 0)
		return 96;
	itimerval timer{};
	timer.it_value.tv_sec = 3600;
	timer.it_interval.tv_sec = 3600;
	if (setitimer(ITIMER_REAL, &timer, nullptr) != 0)
		return 97;
	sigset_t timer_signal;
	sigemptyset(&timer_signal);
	sigaddset(&timer_signal, SIGALRM);
	if (pthread_sigmask(SIG_BLOCK, &timer_signal, nullptr) != 0)
		return 98;
	return kill(getpid(), SIGALRM) == 0 ? 33 : 99;
}

void reset_interleave()
{
	std::lock_guard lock{interleave.mutex};
	interleave.first_entered = false;
	interleave.second_started = false;
	interleave.second_handle_admitted = false;
	interleave.second_at_wait_boundary = false;
	interleave.second_entered = false;
	interleave.second_finished = false;
	interleave.release_first = false;
}

void after_handle_admission(void *) noexcept
{
	std::lock_guard lock{interleave.mutex};
	interleave.second_handle_admitted = true;
	interleave.changed.notify_all();
}

void at_admission_wait_boundary(void *) noexcept
{
	std::lock_guard lock{interleave.mutex};
	interleave.second_at_wait_boundary = true;
	interleave.changed.notify_all();
}

class AdmissionHooks final {
public:
	AdmissionHooks() noexcept
	{
		uml::emulator::testing::set_address_space_admission_hooks(
			after_handle_admission, at_admission_wait_boundary, nullptr,
			nullptr);
	}

	~AdmissionHooks()
	{
		uml::emulator::testing::set_address_space_admission_hooks(
			nullptr, nullptr, nullptr, nullptr);
	}

	AdmissionHooks(const AdmissionHooks &) = delete;
	AdmissionHooks &operator=(const AdmissionHooks &) = delete;
};

/* What the image's entry trampoline branches to, with the kernel's argc, argv
 * and envp. */
using KernelCallback = int (*)(int, char *const[], char *const[]);

std::vector<std::byte> embedded_image(KernelCallback entry,
	std::uint64_t link_offset = 0)
{
	constexpr std::size_t payload_offset = 256;
	constexpr std::uint32_t load_callback = 0x5800'0050U;
	constexpr std::uint32_t branch_callback = 0xd61f'0200U;
	std::vector<std::byte> image(payload_offset + 16);
	auto *header = reinterpret_cast<uml_elf64_ehdr *>(image.data());
	header->ident[0] = UML_ELFMAG0;
	header->ident[1] = UML_ELFMAG1;
	header->ident[2] = UML_ELFMAG2;
	header->ident[3] = UML_ELFMAG3;
	header->ident[4] = UML_ELFCLASS64;
	header->ident[5] = UML_ELFDATA2LSB;
	header->type = UML_ET_DYN;
	header->machine = UML_EM_AARCH64;
	header->version = 1;
	header->entry = linked_address + link_offset;
	header->phoff = sizeof(*header);
	header->ehsize = sizeof(*header);
	header->phentsize = sizeof(uml_elf64_phdr);
	header->phnum = 1;
	auto *program = reinterpret_cast<uml_elf64_phdr *>(
		image.data() + header->phoff);
	program->type = UML_PT_LOAD;
	program->flags = UML_PF_R | UML_PF_X;
	program->offset = payload_offset;
	program->vaddr = linked_address + link_offset;
	program->paddr = linked_address + link_offset;
	program->filesz = 16;
	program->memsz = 16;
	program->align = 0x4000;
	std::memcpy(image.data() + payload_offset, &load_callback,
		sizeof(load_callback));
	std::memcpy(image.data() + payload_offset + 4, &branch_callback,
		sizeof(branch_callback));
	const auto callback = reinterpret_cast<std::uintptr_t>(entry);
	std::memcpy(image.data() + payload_offset + 8, &callback,
		sizeof(callback));
	return image;
}

std::vector<std::byte> malformed_dynamic_image(bool terminated)
{
	auto image = embedded_image(second_kernel);
	constexpr std::size_t payload_offset = 256;
	image.resize(payload_offset + 128);
	auto *header = reinterpret_cast<uml_elf64_ehdr *>(image.data());
	header->phnum = 2;
	auto *programs = reinterpret_cast<uml_elf64_phdr *>(
		image.data() + header->phoff);
	programs[0].filesz = 128;
	programs[0].memsz = 128;
	programs[1].type = UML_PT_DYNAMIC;
	programs[1].flags = UML_PF_R;
	programs[1].offset = payload_offset + 32;
	programs[1].vaddr = linked_address + 32;
	programs[1].paddr = linked_address + 32;
	programs[1].filesz = terminated ? 3 * sizeof(uml_elf64_dyn) :
		sizeof(uml_elf64_dyn);
	programs[1].memsz = programs[1].filesz;
	programs[1].align = alignof(uml_elf64_dyn);
	auto *dynamic = reinterpret_cast<uml_elf64_dyn *>(
		image.data() + programs[1].offset);
	dynamic[0].tag = UML_DT_RELA;
	dynamic[0].un.pointer = UINT64_MAX - 7;
	if (terminated) {
		dynamic[1].tag = UML_DT_RELASZ;
		dynamic[1].un.value = sizeof(uml_elf64_rela);
		dynamic[2].tag = UML_DT_NULL;
	}
	return image;
}

std::vector<std::byte> writable_entry_image()
{
	auto image = embedded_image(second_kernel);
	auto *header = reinterpret_cast<uml_elf64_ehdr *>(image.data());
	auto *program = reinterpret_cast<uml_elf64_phdr *>(
		image.data() + header->phoff);
	program->flags = UML_PF_R | UML_PF_W | UML_PF_X;
	return image;
}

std::vector<std::byte> unaligned_program_header_image()
{
	auto image = embedded_image(second_kernel);
	auto *header = reinterpret_cast<uml_elf64_ehdr *>(image.data());
	uml_elf64_phdr program{};
	std::memcpy(&program, image.data() + header->phoff, sizeof(program));
	++header->phoff;
	std::memcpy(image.data() + header->phoff, &program, sizeof(program));
	return image;
}

using Emulator = std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)>;

Emulator make_emulator(const std::vector<std::byte> &image)
{
	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/embedded-kernel-serialization.img",
		.kernel_image = image.data(),
		.kernel_image_size = image.size(),
	};
	uml_emulator *created = nullptr;
	require(uml_emulator_create(&config, &created) == UML_EMULATOR_OK,
		"embedded-kernel emulator creation failed");
	return {created, uml_emulator_destroy};
}

class TemporaryBacking final {
public:
	TemporaryBacking()
	{
		char path[] = "/tmp/uml-embedded-mmu-domain-XXXXXX";
		fd_ = mkstemp(path);
		require(fd_ >= 0, "could not create embedded MMU backing");
		require(unlink(path) == 0, "could not unlink embedded MMU backing");
		require(ftruncate(fd_, DARWIN_UML_GUEST_PAGE_SIZE) == 0,
			"could not size embedded MMU backing");
	}

	~TemporaryBacking()
	{
		if (fd_ >= 0)
			close(fd_);
	}

	TemporaryBacking(const TemporaryBacking &) = delete;
	TemporaryBacking &operator=(const TemporaryBacking &) = delete;

	[[nodiscard]] int fd() const noexcept { return fd_; }

private:
	int fd_{-1};
};

void release_blocked_kernel()
{
	{
		std::lock_guard lock{interleave.mutex};
		interleave.release_first = true;
	}
	interleave.changed.notify_all();
}

/* True when every page of [address, address + length) is mapped and
 * protectionless: the process's window as reserved, with no mapping of a
 * guest in it and no free address space either. */
[[nodiscard]] bool reserved_and_protectionless(std::uintptr_t address,
	std::size_t length)
{
	mach_vm_address_t cursor = address;
	const mach_vm_address_t end = address + length;
	while (cursor < end) {
		mach_vm_address_t region = cursor;
		mach_vm_size_t size = 0;
		vm_region_basic_info_data_64_t information{};
		mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		if (mach_vm_region(mach_task_self(), &region, &size,
			    VM_REGION_BASIC_INFO_64,
			    reinterpret_cast<vm_region_info_t>(&information),
			    &count, &object) != KERN_SUCCESS ||
		    region > cursor || information.protection != VM_PROT_NONE)
			return false;
		cursor = region + size;
	}
	return true;
}

/* The kernel image has gone back to the process's window: the runtime
 * address is part of the window's protectionless reservation, not a mapping
 * the kernel left and not free address space either. */
void require_runtime_window_available()
{
	const auto page_size = static_cast<std::size_t>(getpagesize());
	require(reserved_and_protectionless(
		loader_runtime_address, page_size),
		"successful loader call retained the runtime window");
}

/* The imports the loader follows to give a restarted guest's resources back,
 * as a kernel image binds them: by name, through relocations of its own. */
constexpr std::array<const char *, 17> ledger_imports{
	"darwin_uml_open", "darwin_uml_close", "darwin_uml_fcntl", "socket",
	"_socketpair", "accept", "pipe", "kqueue", "dup", "_recvmsg", "malloc",
	"free", "_mmap", "_munmap", "sbrk", "pthread_key_create",
	/* Not followed: bound to the system's own. */
	"getpid",
};

constexpr std::size_t ledger_got_offset = 2048;

/* Descriptors and counts the kernel reports to the test. */
struct LedgerKernel final {
	int pipe_ends[2]{-1, -1};
	int socket_descriptor{-1};
	pthread_key_t key{};
	bool created{};
	std::size_t descriptors_held{};
	std::size_t keys_held{};
	std::size_t allocations_held{};
	bool every_import_bound{};
	bool descriptors_closed{};
	bool key_deleted{};
	bool ledger_empty{};
} ledger_kernel_state;

[[nodiscard]] void *const *ledger_got() noexcept
{
	return reinterpret_cast<void *const *>(
		static_cast<std::uintptr_t>(loader_runtime_address) +
		ledger_got_offset);
}

[[nodiscard]] std::size_t ledger_index(std::string_view name) noexcept
{
	for (std::size_t index = 0; index < ledger_imports.size(); ++index)
		if (name == ledger_imports[index])
			return index;
	return ledger_imports.size();
}

/* The first boot resolves every import through the image's own relocations
 * and creates resources through them; the boot after the hand-back finds them
 * given back. */
int ledger_kernel(int, char *const[], char *const[])
{
	auto &state = ledger_kernel_state;
	const auto *got = ledger_got();
	if (state.created) {
		state.descriptors_closed = true;
		for (const int descriptor : {state.pipe_ends[0],
			     state.pipe_ends[1], state.socket_descriptor})
			state.descriptors_closed = state.descriptors_closed &&
				fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
		/* A deleted key refuses a value. */
		state.key_deleted = pthread_setspecific(state.key, &state) ==
			EINVAL;
		const auto counts = uml::runtime::kernel_resource_counts();
		state.ledger_empty = counts.descriptors == 0 &&
			counts.keys == 0 && counts.allocations == 0 &&
			counts.mappings == 0;
		return 62;
	}
	bool bound = true;
	for (std::size_t index = 0; index < ledger_imports.size(); ++index) {
		const std::string_view name = ledger_imports[index];
		void *const expected = name == "getpid" ?
			dlsym(RTLD_DEFAULT, "getpid") :
			uml::runtime::kernel_resource_import(name.data());
		bound = bound && expected != nullptr && got[index] == expected;
	}
	/* getpid is not followed: it is the system's own. */
	bound = bound && uml::runtime::kernel_resource_import("getpid") == nullptr;
	state.every_import_bound = bound;
	const auto create_pipe = reinterpret_cast<int (*)(int[2])>(
		got[ledger_index("pipe")]);
	const auto create_socket = reinterpret_cast<int (*)(int, int, int)>(
		got[ledger_index("socket")]);
	const auto allocate = reinterpret_cast<void *(*)(std::size_t)>(
		got[ledger_index("malloc")]);
	const auto create_key = reinterpret_cast<int (*)(
		pthread_key_t *, void (*)(void *))>(
		got[ledger_index("pthread_key_create")]);
	if (!bound || create_pipe(state.pipe_ends) != 0)
		return 94;
	state.socket_descriptor = create_socket(AF_UNIX, SOCK_DGRAM, 0);
	if (state.socket_descriptor < 0 || allocate(1000) == nullptr ||
	    create_key(&state.key, nullptr) != 0)
		return 94;
	const auto held = uml::runtime::kernel_resource_counts();
	state.descriptors_held = held.descriptors;
	state.keys_held = held.keys;
	state.allocations_held = held.allocations;
	state.created = true;
	uml::emulator::testing::report_guest_shutdown(
		uml::runtime::GuestShutdownKind::restart, "ledger");
	return 61;
}

/* A kernel image that imports the creating functions by name, with the
 * dynamic table, symbols and relocations a linker would give it, and calls
 * `entry` from its entry stub. */
std::vector<std::byte> ledger_image(KernelCallback entry)
{
	constexpr std::size_t payload_offset = 256;
	constexpr std::size_t dynamic_offset = 64;
	constexpr std::size_t symbol_offset = 192;
	const std::size_t count = ledger_imports.size();
	const std::size_t string_offset = symbol_offset +
		(count + 1) * sizeof(uml_elf64_sym);
	std::vector<char> names(1, '\0');
	std::vector<std::uint32_t> name_offsets;
	for (const char *name : ledger_imports) {
		name_offsets.push_back(static_cast<std::uint32_t>(names.size()));
		names.insert(names.end(), name, name + std::strlen(name) + 1);
	}
	const std::size_t relocation_offset =
		(string_offset + names.size() + 7U) & ~std::size_t{7};
	const std::size_t total = ledger_got_offset + 8 * count + 8;
	if (relocation_offset + count * sizeof(uml_elf64_rela) > ledger_got_offset)
		throw std::runtime_error{"the ledger image overlaps its slots"};
	auto image = embedded_image(entry);
	image.resize(payload_offset + total);
	auto *header = reinterpret_cast<uml_elf64_ehdr *>(image.data());
	header->phnum = 2;
	auto *programs = reinterpret_cast<uml_elf64_phdr *>(
		image.data() + header->phoff);
	programs[0].filesz = total;
	programs[0].memsz = total;
	programs[1].type = UML_PT_DYNAMIC;
	programs[1].flags = UML_PF_R;
	programs[1].offset = payload_offset + dynamic_offset;
	programs[1].vaddr = linked_address + dynamic_offset;
	programs[1].paddr = linked_address + dynamic_offset;
	programs[1].filesz = 6 * sizeof(uml_elf64_dyn);
	programs[1].memsz = programs[1].filesz;
	programs[1].align = alignof(uml_elf64_dyn);
	auto *dynamic = reinterpret_cast<uml_elf64_dyn *>(
		image.data() + payload_offset + dynamic_offset);
	dynamic[0] = {.tag = UML_DT_SYMTAB,
		.un = {.pointer = linked_address + symbol_offset}};
	dynamic[1] = {.tag = UML_DT_STRTAB,
		.un = {.pointer = linked_address + string_offset}};
	dynamic[2] = {.tag = UML_DT_STRSZ, .un = {.value = names.size()}};
	dynamic[3] = {.tag = UML_DT_RELA,
		.un = {.pointer = linked_address + relocation_offset}};
	dynamic[4] = {.tag = UML_DT_RELASZ,
		.un = {.value = count * sizeof(uml_elf64_rela)}};
	dynamic[5] = {.tag = UML_DT_NULL, .un = {.value = 0}};
	auto *symbols = reinterpret_cast<uml_elf64_sym *>(
		image.data() + payload_offset + symbol_offset);
	auto *relocations = reinterpret_cast<uml_elf64_rela *>(
		image.data() + payload_offset + relocation_offset);
	for (std::size_t index = 0; index < count; ++index) {
		symbols[index + 1] = {.name = name_offsets[index],
			.info = 0x10, .other = 0, .shndx = UML_SHN_UNDEF,
			.value = 0, .size = 0};
		relocations[index] = {
			.offset = linked_address + ledger_got_offset + 8 * index,
			.info = (static_cast<std::uint64_t>(index + 1) << 32) |
				UML_R_AARCH64_GLOB_DAT,
			.addend = 0};
	}
	std::memcpy(image.data() + payload_offset + string_offset, names.data(),
		names.size());
	return image;
}

/* The loader binds a kernel's creating imports to the ledger, and a restarted
 * guest's hand-back gives back what the kernel created through them. */
void test_loader_binds_creating_imports_to_the_ledger()
{
	const auto image = ledger_image(ledger_kernel);
	auto emulator = make_emulator(image);
	const char *arguments[]{
		"embedded-ledger-imports", admission_engine_argument};
	require(uml_emulator_run(emulator.get(), 2, arguments, nullptr) == 61,
		"the ledger kernel did not boot");
	auto &state = ledger_kernel_state;
	require(state.every_import_bound,
		"the loader did not bind the creating imports to the ledger");
	require(state.descriptors_held == 3 && state.keys_held == 1 &&
		state.allocations_held == 1,
		"the ledger did not record what the kernel created through its "
		"imports");
	uml_emulator_run_end end{.struct_size = sizeof(end)};
	require(uml_emulator_get_run_end(emulator.get(), &end) ==
			UML_EMULATOR_OK &&
		end.kind == UML_EMULATOR_RUN_END_RESTART &&
		std::strcmp(end.restart_command, "ledger") == 0,
		"the ledger kernel's restart was not reported");

	require(uml_emulator_run(emulator.get(), 2, arguments, nullptr) == 62,
		"the restarted ledger kernel did not boot");
	require(state.descriptors_closed,
		"the hand-back left the kernel's descriptors open");
	require(state.key_deleted,
		"the hand-back left the kernel's pthread key");
	require(state.ledger_empty, "the hand-back left the ledger non-empty");
	require_runtime_window_available();
}

/* The native display runs the kernel on a worker while the AppKit thread
 * blocks SIGALRM, and restores that thread's mask only after the loader has
 * released the image. A tick parked there must not reach a handler inside the
 * released image. */
void test_kernel_signal_state_is_revoked_before_release()
{
	sigset_t timer_signal;
	sigemptyset(&timer_signal);
	sigaddset(&timer_signal, SIGALRM);
	sigset_t lent;
	require(pthread_sigmask(SIG_BLOCK, &timer_signal, &lent) == 0,
		"could not block SIGALRM on the main thread");
	struct sigaction host {};
	require(sigaction(SIGALRM, nullptr, &host) == 0,
		"could not read the host SIGALRM disposition");
	const auto image = embedded_image(signal_owning_kernel);
	auto emulator = make_emulator(image);
	const char *arguments[]{
		"embedded-signal-revocation", admission_engine_argument};
	int status = -1;
	std::thread worker{[&] {
		status = uml_emulator_run(emulator.get(), 2, arguments, nullptr);
	}};
	worker.join();
	require(status == 33, "signal-owning kernel did not complete");
	struct sigaction after {};
	require(sigaction(SIGALRM, nullptr, &after) == 0,
		"could not read the SIGALRM disposition after the run");
	require(after.sa_handler == host.sa_handler &&
		after.sa_flags == host.sa_flags,
		"a SIGALRM handler inside the released image survived the run");
	itimerval timer{};
	require(getitimer(ITIMER_REAL, &timer) == 0 &&
		timer.it_value.tv_sec == 0 && timer.it_value.tv_usec == 0 &&
		timer.it_interval.tv_sec == 0 && timer.it_interval.tv_usec == 0,
		"the kernel's ITIMER_REAL survived the run");
	sigset_t pending;
	sigemptyset(&pending);
	require(sigpending(&pending) == 0 && !sigismember(&pending, SIGALRM),
		"the tick parked on the main thread survived the run");
	/* Before revocation this delivered the parked tick into the released
	 * image; now it must deliver nothing. */
	require(pthread_sigmask(SIG_SETMASK, &lent, nullptr) == 0,
		"could not restore the main thread's signal mask");
	require_runtime_window_available();
}

/* The window belongs to the process's guest, and nothing outside it does. An
 * image whose segments lie beyond the window is refused before it maps
 * anything, and the window stays as the process reserved it. */
void test_image_outside_the_window_is_refused()
{
	const auto page_size = static_cast<std::size_t>(getpagesize());
	constexpr std::uint64_t beyond_the_window = 1ULL << 40;
	const auto image = embedded_image(second_kernel, beyond_the_window);
	auto emulator = make_emulator(image);
	const char *arguments[]{"embedded-loader-outside-window"};
	require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 126,
		"loader accepted an image outside the reserved window");
	void *const outside = reinterpret_cast<void *>(
		static_cast<std::uintptr_t>(loader_runtime_address +
			beyond_the_window));
	void *const probe = mmap(outside, page_size, PROT_NONE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	const bool untouched = probe == outside;
	if (probe != MAP_FAILED)
		(void)munmap(probe, page_size);
	require(untouched, "loader mapped an image outside the reserved window");
	require_runtime_window_available();
}

void test_malformed_dynamic_metadata_is_guest_failure()
{
	const char *arguments[]{"embedded-loader-malformed"};
	for (const bool terminated : {false, true}) {
		const auto image = malformed_dynamic_image(terminated);
		auto emulator = make_emulator(image);
		require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 126,
			"malformed dynamic metadata did not return status 126");
		require_runtime_window_available();
	}
}

void test_non_executable_final_entry_page_is_guest_failure()
{
	const auto image = writable_entry_image();
	auto emulator = make_emulator(image);
	const char *arguments[]{"embedded-loader-writable-entry"};
	require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 126,
		"unsafe embedded entry page reached host execution");
	require_runtime_window_available();
}

void test_unaligned_loader_metadata_is_guest_failure()
{
	const auto image = unaligned_program_header_image();
	auto emulator = make_emulator(image);
	const char *arguments[]{"embedded-loader-unaligned-metadata"};
	require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 126,
		"unaligned embedded metadata reached typed loader access");
	require_runtime_window_available();
}

/* The loader maps an image's executable pages as native code, aliased from
 * the embedded section on a product that carries one. Only an AArch64 image
 * may be, so an x86-64 image is refused before it maps anything: the x86-64
 * guests are userspace, translated by the cpu engines, never a kernel image. */
void test_non_aarch64_image_is_refused()
{
	constexpr std::uint16_t machine_x86_64 = 62;
	for (const std::uint16_t machine : {machine_x86_64, std::uint16_t{0}}) {
		auto image = embedded_image(second_kernel);
		reinterpret_cast<uml_elf64_ehdr *>(image.data())->machine = machine;
		auto emulator = make_emulator(image);
		const char *arguments[]{"embedded-loader-foreign-machine"};
		require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 126,
			"the loader accepted an image that is not AArch64");
		require_runtime_window_available();
	}
}

void test_embedded_image_is_copied_at_creation()
{
	auto image = embedded_image(second_kernel);
	auto emulator = make_emulator(image);
	std::fill(image.begin(), image.end(), std::byte{0});
	const char *arguments[]{"embedded-loader-owned-image"};
	require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 23,
		"embedded run retained the caller-owned kernel-image buffer");
	require_runtime_window_available();
}

void test_embedded_runs_start_with_fresh_mmu_domain()
{
	TemporaryBacking backing;
	mmu_backing_fd = backing.fd();
	const auto mapping_image = embedded_image(mapping_kernel);
	const auto fresh_image = embedded_image(verify_fresh_mmu_kernel);
	auto first = make_emulator(mapping_image);
	auto second = make_emulator(fresh_image);
	const char *arguments[]{"embedded-mmu-domain"};
	require(uml_emulator_run(first.get(), 1, arguments, nullptr) == 31,
		"first embedded run could not establish MMU mappings");
	require(uml_emulator_run(second.get(), 1, arguments, nullptr) == 32,
		"later embedded run inherited user or generation-zero mappings");
	mmu_backing_fd = -1;
	require_runtime_window_available();
}

void test_same_handle_run_fails_before_global_admission()
{
	reset_interleave();
	const auto image = embedded_image(first_kernel);
	auto emulator = make_emulator(image);
	const char *arguments[]{"embedded-same-handle"};
	int first_status = -1;
	int duplicate_status = -1;
	std::thread first{[&] {
		first_status = uml_emulator_run(
			emulator.get(), 1, arguments, nullptr);
	}};
	bool entered = false;
	{
		std::unique_lock lock{interleave.mutex};
		entered = interleave.changed.wait_for(lock, 5s, [] {
			return interleave.first_entered;
		});
	}
	std::thread duplicate;
	if (entered) {
		duplicate = std::thread{[&] {
			duplicate_status = uml_emulator_run(
				emulator.get(), 1, arguments, nullptr);
			{
				std::lock_guard lock{interleave.mutex};
				interleave.second_finished = true;
			}
			interleave.changed.notify_all();
		}};
	}
	bool failed_fast = false;
	{
		std::unique_lock lock{interleave.mutex};
		failed_fast = entered && interleave.changed.wait_for(lock, 500ms, [] {
			return interleave.second_finished;
		});
	}
	release_blocked_kernel();
	first.join();
	if (duplicate.joinable())
		duplicate.join();
	require(entered, "same-handle first run did not enter");
	require(failed_fast &&
		duplicate_status == -UML_EMULATOR_RUNTIME_ERROR,
		"same-handle second run did not fail before global admission");
	require(first_status == 17, "same-handle first run did not complete");
	require_runtime_window_available();
}

void test_queued_handle_stop_cancels_admission()
{
	reset_interleave();
	const auto image_a = embedded_image(first_kernel);
	const auto image_b = embedded_image(second_kernel);
	auto emulator_a = make_emulator(image_a);
	auto emulator_b = make_emulator(image_b);
	const char *arguments[]{
		"embedded-queued-stop", admission_engine_argument};
	int status_a = -1;
	int status_b = -1;
	std::thread first{[&] {
		status_a = uml_emulator_run(emulator_a.get(), 2, arguments, nullptr);
	}};
	bool first_entered = false;
	{
		std::unique_lock lock{interleave.mutex};
		first_entered = interleave.changed.wait_for(lock, 5s, [] {
			return interleave.first_entered;
		});
	}
	std::thread second;
	bool reached_wait_boundary = false;
	bool cancelled_before_release = false;
	AdmissionHooks hooks;
	if (first_entered) {
		second = std::thread{[&] {
			{
				std::lock_guard lock{interleave.mutex};
				interleave.second_started = true;
			}
			interleave.changed.notify_all();
			status_b = uml_emulator_run(
				emulator_b.get(), 2, arguments, nullptr);
			{
				std::lock_guard lock{interleave.mutex};
				interleave.second_finished = true;
			}
			interleave.changed.notify_all();
		}};
	}
	{
		std::unique_lock lock{interleave.mutex};
		reached_wait_boundary = first_entered &&
			interleave.changed.wait_for(lock, 5s, [] {
				return interleave.second_started &&
					interleave.second_handle_admitted &&
					interleave.second_at_wait_boundary;
			});
	}
	if (reached_wait_boundary)
		uml_emulator_request_stop(emulator_b.get());
	{
		std::unique_lock lock{interleave.mutex};
		cancelled_before_release = reached_wait_boundary &&
			interleave.changed.wait_for(lock, 1s, [] {
				return interleave.second_finished;
			});
	}
	release_blocked_kernel();
	first.join();
	if (second.joinable())
		second.join();
	require(first_entered && reached_wait_boundary,
		"queued-stop runs did not reach deterministic admission points");
	require(cancelled_before_release &&
		status_b == -UML_EMULATOR_RUNTIME_ERROR,
		"stop did not cancel queued embedded-kernel admission");
	require(!interleave.second_entered,
		"cancelled queued kernel entered the runtime window");
	require(status_a == 17, "admitted kernel failed during queued stop");
	require_runtime_window_available();
}

void test_embedded_handles_are_process_serialized()
{
	reset_interleave();
	const auto image_a = embedded_image(first_kernel);
	const auto image_b = embedded_image(second_kernel);
	auto emulator_a = make_emulator(image_a);
	auto emulator_b = make_emulator(image_b);
	const char *arguments[]{
		"embedded-kernel-serialization", admission_engine_argument};
	int status_a = -1;
	int status_b = -1;
	std::thread first{[&] {
		status_a = uml_emulator_run(
			emulator_a.get(), 2, arguments, nullptr);
	}};
	bool first_entered = false;
	{
		std::unique_lock lock{interleave.mutex};
		first_entered = interleave.changed.wait_for(lock, 5s, [] {
			return interleave.first_entered;
		});
	}
	std::thread second;
	if (first_entered) {
		second = std::thread{[&] {
			{
				std::lock_guard lock{interleave.mutex};
				interleave.second_started = true;
			}
			interleave.changed.notify_all();
			status_b = uml_emulator_run(
				emulator_b.get(), 2, arguments, nullptr);
		}};
	}
	bool overlapped = false;
	{
		std::unique_lock lock{interleave.mutex};
		if (first_entered && interleave.changed.wait_for(lock, 5s, [] {
			return interleave.second_started;
		}))
			overlapped = interleave.changed.wait_for(lock, 250ms, [] {
				return interleave.second_entered;
			});
		interleave.release_first = true;
	}
	interleave.changed.notify_all();
	first.join();
	if (second.joinable())
		second.join();
	if (!first_entered || overlapped || status_a != 17 || status_b != 23 ||
	    !interleave.second_entered)
		std::fprintf(stderr,
			"serialization diagnostics first-entered=%d second-started=%d "
			"second-entered=%d overlapped=%d status-a=%d status-b=%d\n",
			first_entered, interleave.second_started,
			interleave.second_entered, overlapped, status_a, status_b);
	require(first_entered, "first embedded kernel did not enter");
	require(!overlapped,
		"two embedded kernels occupied the runtime window concurrently");
	require(status_a == 17 && status_b == 23 && interleave.second_entered,
		"serialized embedded kernels did not both complete");
	require_runtime_window_available();
}

} // namespace

int main()
{
	try {
#if DARWIN_UML_NATIVE_HIGH_PLACEMENT
		uml::emulator::testing::place_native_kernel_window(
			native_window_start);
#endif
		test_kernel_signal_state_is_revoked_before_release();
		test_image_outside_the_window_is_refused();
		test_malformed_dynamic_metadata_is_guest_failure();
		test_non_executable_final_entry_page_is_guest_failure();
		test_unaligned_loader_metadata_is_guest_failure();
		test_non_aarch64_image_is_refused();
		test_embedded_image_is_copied_at_creation();
		test_embedded_runs_start_with_fresh_mmu_domain();
		test_loader_binds_creating_imports_to_the_ledger();
		test_same_handle_run_fails_before_global_admission();
		test_queued_handle_stop_cancels_admission();
		test_embedded_handles_are_process_serialized();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "embedded-kernel serialization test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
}
