// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A process runs one guest, and that guest may restart itself. A kernel
 * claims guest RAM early in its boot, and the host hands guest RAM out once
 * per process. After a run has claimed it, every later run in the process, on
 * the same handle or a fresh one, returns -UML_EMULATOR_UNSUPPORTED without
 * entering a kernel, and the process goes on, unless the run ended with a
 * restart: the next run on that handle hands the guest back to the process and
 * boots again. A run whose kernel returns before claiming guest RAM leaves the
 * guest for a later run.
 *
 * Each mode runs in its own process because the claim outlives every handle.
 * The restart modes boot a test kernel that does what a real one leaves
 * behind (descriptors, pthread keys, allocations and mappings through the
 * imports the loader follows, changed signal state, a guest RAM root over the
 * process's window, kernel threads, a guest file) and check, at the moment each
 * hand-back completes, that nothing of it is left and that the process is as it
 * was before the first run.
 */
#include "uml/emulator.h"
#include "context.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/gadget_cache_budget.hpp"
#include "device/sockets/socket_paths.hpp"
#include "emulator_test_support.hpp"
#include "guest_files.hpp"
#include "guest_hibernation.hpp"
#include "kernel_resources.hpp"
#include "kernel_threads.hpp"
#include "mach_vm_region_test_support.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "memory/mmu/domain_registry_testing.hpp"
#include "memory/shared_backing.hpp"
#include "native_kernel_layout.hpp"
#include "require.hpp"
#include "testing.hpp"
#include "uml_darwin_host.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#include <sanitizer/allocator_interface.h>
#define UML_TEST_SANITIZER_ALLOCATOR 1
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <malloc/malloc.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kernel_booted = 41;
constexpr int kernel_without_memory = 42;
constexpr int kernel_left_memory = 43;

/* Kernel entries in this process, and whether the next one claims RAM. */
int kernel_entries;
bool claim_guest_ram = true;

using ::uml::runtime::GuestShutdownKind;
using uml::emulator::testing::GuestHandBackReport;
using uml::testing::make_emulator;
using uml::testing::require;

/* Stands in for the start of a UML boot: setup_physmem() opens guest RAM. */
int kernel(int, char *const[], char *const[])
{
	++kernel_entries;
	if (!claim_guest_ram)
		return kernel_left_memory;
	const int memory = darwin_uml_open("uml:anonymous-physical-memory",
		O_RDWR, 0);
	if (memory < 0)
		return kernel_without_memory;
	(void)::close(memory);
	return kernel_booted;
}

int run(uml_emulator *emulator)
{
	const char *arguments[]{"uml-sequential-runs"};
	return uml_emulator_run(emulator, 1, arguments, nullptr);
}

void require_first_guest(uml_emulator *emulator)
{
	const int entries = kernel_entries;
	require(run(emulator) == kernel_booted,
		"the first guest in the process could not open guest RAM");
	require(kernel_entries == entries + 1,
		"the first guest did not enter its kernel once");
}

void require_refused(uml_emulator *emulator)
{
	const int entries = kernel_entries;
	require(run(emulator) == -UML_EMULATOR_UNSUPPORTED,
		"a run after the process's guest did not fail as unsupported");
	require(kernel_entries == entries,
		"a run after the process's guest entered its kernel");
}

void second_run_on_the_same_handle()
{
	auto emulator = make_emulator(kernel);
	require_first_guest(emulator.get());
	require_refused(emulator.get());
	require_refused(emulator.get());
}

void second_run_on_a_fresh_handle()
{
	{
		auto first = make_emulator(kernel);
		require_first_guest(first.get());
	}
	auto second = make_emulator(kernel);
	require_refused(second.get());
}

void run_that_left_guest_ram()
{
	auto emulator = make_emulator(kernel);
	claim_guest_ram = false;
	require(run(emulator.get()) == kernel_left_memory,
		"a kernel that left guest RAM did not return its status");
	claim_guest_ram = true;
	require_first_guest(emulator.get());
	require_refused(make_emulator(kernel).get());
}

/* ---- A guest that restarts itself ---- */

constexpr std::size_t ram_pages = 64;
constexpr std::size_t image_pages = 4;
/* One past the signals Darwin names that a test can query. */
constexpr int signal_count = 32;

[[nodiscard]] std::size_t host_page() noexcept
{
	return static_cast<std::size_t>(getpagesize());
}

/* The kernel's imports as the loader binds them. */
struct Imports final {
	int (*open)(const char *, int, int);
	int (*close)(int);
	int (*fcntl)(int, int, long);
	int (*socket)(int, int, int);
	int (*socketpair)(int, int, int, int[2]);
	int (*pipe)(int[2]);
	int (*kqueue)();
	int (*dup)(int);
	ssize_t (*recvmsg)(int, msghdr *, int);
	void *(*malloc)(std::size_t);
	void (*free)(void *);
	void *(*mmap)(void *, std::size_t, int, int, int, off_t);
	int (*munmap)(void *, std::size_t);
	void *(*sbrk)(int);
	int (*key_create)(pthread_key_t *, void (*)(void *));
};

template<typename Function>
[[nodiscard]] Function *import(const char *name)
{
	void *const wrapper = uml::runtime::kernel_resource_import(name);
	require(wrapper != nullptr, "the loader does not follow an import");
	return reinterpret_cast<Function *>(wrapper);
}

[[nodiscard]] Imports bind_imports()
{
	return {
		.open = import<int(const char *, int, int)>("darwin_uml_open"),
		.close = import<int(int)>("darwin_uml_close"),
		.fcntl = import<int(int, int, long)>("darwin_uml_fcntl"),
		.socket = import<int(int, int, int)>("socket"),
		.socketpair = import<int(int, int, int, int[2])>("_socketpair"),
		.pipe = import<int(int[2])>("pipe"),
		.kqueue = import<int()>("kqueue"),
		.dup = import<int(int)>("dup"),
		.recvmsg = import<ssize_t(int, msghdr *, int)>("_recvmsg"),
		.malloc = import<void *(std::size_t)>("malloc"),
		.free = import<void(void *)>("free"),
		.mmap = import<void *(void *, std::size_t, int, int, int, off_t)>(
			"_mmap"),
		.munmap = import<int(void *, std::size_t)>("_munmap"),
		.sbrk = import<void *(int)>("sbrk"),
		.key_create = import<int(pthread_key_t *, void (*)(void *))>(
			"pthread_key_create"),
	};
}

/* What the process looks like from outside the guest: the state a kernel
 * changes and the library must give back. */
struct ProcessState final {
	std::array<struct sigaction, signal_count> actions{};
	std::array<bool, signal_count> readable{};
	itimerval timer{};
	sigset_t mask{};
	stack_t signal_stack{};
	std::array<int, 3> standard_flags{};
	std::size_t software_domains{};
	std::size_t native_domains{};
	std::size_t gadget_accounts{};
	std::size_t gadget_bytes{};
};

constexpr int settable_status_flags =
	O_NONBLOCK | O_APPEND | O_ASYNC | O_FSYNC | O_DSYNC;

[[nodiscard]] ProcessState capture_process_state()
{
	ProcessState state;
	for (int number = 1; number < signal_count; ++number)
		state.readable[number] =
			sigaction(number, nullptr, &state.actions[number]) == 0;
	require(getitimer(ITIMER_REAL, &state.timer) == 0,
		"could not read ITIMER_REAL");
	require(pthread_sigmask(SIG_BLOCK, nullptr, &state.mask) == 0,
		"could not read the signal mask");
	require(sigaltstack(nullptr, &state.signal_stack) == 0,
		"could not read the signal stack");
	for (int descriptor = 0; descriptor < 3; ++descriptor)
		state.standard_flags[descriptor] =
			fcntl(descriptor, F_GETFL) & settable_status_flags;
	state.software_domains = uml::memory::testing::software_domain_count();
	state.native_domains = uml::memory::testing::native_domain_count();
	const auto gadgets = uml::cpu::gadget_cache::ledger_state();
	state.gadget_accounts = gadgets.accounts;
	state.gadget_bytes = gadgets.total;
	return state;
}

[[nodiscard]] bool same_mask(const sigset_t &first, const sigset_t &second)
{
	return std::memcmp(&first, &second, sizeof(first)) == 0;
}

void require_same_process(const ProcessState &before)
{
	const auto now = capture_process_state();
	for (int number = 1; number < signal_count; ++number) {
		require(before.readable[number] == now.readable[number],
			"a signal became unreadable");
		if (!before.readable[number])
			continue;
		const auto &was = before.actions[number];
		const auto &is = now.actions[number];
		require(was.sa_handler == is.sa_handler &&
			was.sa_flags == is.sa_flags &&
			same_mask(was.sa_mask, is.sa_mask),
			"a signal disposition was not restored");
	}
	require(now.timer.it_value.tv_sec == before.timer.it_value.tv_sec &&
		now.timer.it_value.tv_usec == before.timer.it_value.tv_usec &&
		now.timer.it_interval.tv_sec == before.timer.it_interval.tv_sec,
		"ITIMER_REAL was left armed");
	require(same_mask(before.mask, now.mask),
		"the run thread's signal mask was not restored");
	/* A disabled stack has no address worth comparing. */
	const bool disabled = (before.signal_stack.ss_flags & SS_DISABLE) != 0;
	require((now.signal_stack.ss_flags & SS_DISABLE) ==
		(before.signal_stack.ss_flags & SS_DISABLE) &&
		(disabled || (before.signal_stack.ss_sp == now.signal_stack.ss_sp &&
		before.signal_stack.ss_size == now.signal_stack.ss_size)),
		"the run thread's signal stack was not restored");
	require(before.standard_flags == now.standard_flags,
		"the status flags of descriptors 0-2 were not restored");
	require(now.software_domains == before.software_domains &&
		now.native_domains == before.native_domains,
		"an address space or generation outlived its run");
	require(now.gadget_accounts == before.gadget_accounts &&
		now.gadget_bytes == before.gadget_bytes,
		"a gadget cache account outlived its run");
	const auto monitor =
		uml::cpu::arm64::jit::testing::exclusive_monitor_state_for_testing();
	require(monitor.active_reservations == 0 && monitor.reservations == 0 &&
		monitor.store_participants == 0 && monitor.gate_quiescent,
		"the JIT exclusive monitor is not empty");
}

/* How the kernel of a boot ends. */
enum class Ending { restart, power_off, halt, stopped, failed };

struct Restarts final {
	uml_emulator *handle{};
	/* Boots 1 .. restarting end with a restart; the next ends as `last`. */
	int restarting{};
	Ending last{Ending::power_off};
	/* The first boot leaves a kernel thread running. */
	bool leave_thread{};
	/* The process before the first run. */
	ProcessState before;
	/* What the last boot created, for the hook to check. */
	GuestHandBackReport expected;
	std::vector<int> descriptors;
	int token{-1};
	int control{-1};
	void *outside_mapping{};
	/* Hand-backs seen, and the heap's live bytes at each, and the bytes the
	 * memory layer's heap region has committed. */
	int hand_backs{};
	std::vector<std::size_t> heap_in_use;
	std::vector<std::size_t> region_committed;
	std::size_t zone_allocated{};
	pthread_t held_thread{};
	std::atomic<bool> release_thread{false};
	bool thread_started{};
} world;

void key_destructor(void *) {}

void signal_tick(int) {}

void *held_kernel_thread(void *) noexcept
{
	while (!world.release_thread.load(std::memory_order_acquire))
		usleep(1000);
	return nullptr;
}

void *short_kernel_thread(void *) noexcept
{
	return nullptr;
}

/* The state a kernel changes that is not its own: the library gives it back
 * when the kernel returns. */
void perturb_process(const Imports &kernel)
{
	struct sigaction ignore {};
	ignore.sa_handler = SIG_IGN;
	sigemptyset(&ignore.sa_mask);
	for (const int number : {SIGPIPE, SIGIO, SIGWINCH})
		require(sigaction(number, &ignore, nullptr) == 0,
			"could not ignore a signal");
	struct sigaction handler {};
	handler.sa_handler = signal_tick;
	handler.sa_flags = SA_RESTART | SA_ONSTACK;
	sigemptyset(&handler.sa_mask);
	for (const int number : {SIGALRM, SIGUSR2})
		require(sigaction(number, &handler, nullptr) == 0,
			"could not install a handler");
	itimerval timer{};
	timer.it_value.tv_sec = 3600;
	timer.it_interval.tv_sec = 3600;
	require(setitimer(ITIMER_REAL, &timer, nullptr) == 0,
		"could not arm ITIMER_REAL");
	sigset_t blocked;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGALRM);
	sigaddset(&blocked, SIGUSR1);
	require(pthread_sigmask(SIG_BLOCK, &blocked, nullptr) == 0,
		"could not block signals");
	constexpr std::size_t stack_bytes = 128U * 1024U;
	void *const stack = kernel.malloc(stack_bytes);
	require(stack != nullptr, "could not allocate a signal stack");
	stack_t alternate{};
	alternate.ss_sp = stack;
	alternate.ss_size = stack_bytes;
	require(sigaltstack(&alternate, nullptr) == 0,
		"could not set a signal stack");
	const int flags = fcntl(STDIN_FILENO, F_GETFL);
	if (flags >= 0)
		require(fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0,
			"could not make stdin non-blocking");
}

void remember(int descriptor)
{
	require(descriptor >= 0, "a kernel import returned no descriptor");
	world.descriptors.push_back(descriptor);
}

/* The rights a kernel receives by recvmsg: a descriptor sent over a socket
 * pair, arriving as a new one. */
int receive_rights(const Imports &kernel, int sender, int receiver,
	int shared)
{
	char byte = 'r';
	iovec vector{.iov_base = &byte, .iov_len = 1};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
	msghdr message{};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	auto *header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	std::memcpy(CMSG_DATA(header), &shared, sizeof(shared));
	require(sendmsg(sender, &message, 0) == 1, "could not send rights");
	char received = 0;
	iovec incoming{.iov_base = &received, .iov_len = 1};
	alignas(cmsghdr) char incoming_control[CMSG_SPACE(sizeof(int))]{};
	msghdr reply{};
	reply.msg_iov = &incoming;
	reply.msg_iovlen = 1;
	reply.msg_control = incoming_control;
	reply.msg_controllen = sizeof(incoming_control);
	require(kernel.recvmsg(receiver, &reply, 0) == 1,
		"could not receive rights");
	int descriptor = -1;
	std::memcpy(&descriptor, CMSG_DATA(CMSG_FIRSTHDR(&reply)),
		sizeof(descriptor));
	return descriptor;
}

[[nodiscard]] bool nothing_present(const void *address, std::size_t length)
{
	std::vector<char> residency(length / host_page());
	require(mincore(const_cast<void *>(address), length,
		residency.data()) == 0, "mincore failed");
	for (const char page : residency)
		if ((page & (MINCORE_INCORE | MINCORE_PAGED_OUT)) != 0)
			return false;
	return true;
}

/* The guest RAM root a kernel maps over the process's window: an image
 * prefix, then the suffix the root's mapping covers. */
void claim_ram(int token, int boot)
{
	const auto window = uml::runtime::process_native_window();
	const std::size_t page = host_page();
	require(window.end - window.begin >= ram_pages * page,
		"the process's window is too small for the test RAM");
	require(ftruncate(token, static_cast<off_t>(ram_pages * page)) == 0,
		"could not size the RAM token");
	auto *const root = reinterpret_cast<std::uint8_t *>(window.begin);
	auto *const suffix = root + image_pages * page;
	const std::size_t suffix_bytes = (ram_pages - image_pages) * page;
	/* The window holds none of the previous guest's RAM. */
	require(nothing_present(root, ram_pages * page),
		"a page of the window is present before the guest maps its RAM");
	require(uml::memory::guest_ram_commitment() == nullptr,
		"a commitment outlived the previous guest");
	void *const mapped = uml::memory::map_shared_backing(suffix, suffix_bytes,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, token,
		image_pages * page);
	require(mapped == suffix, "the guest RAM was not mapped in place");
	require(uml::memory::guest_ram_commitment() != nullptr,
		"the guest RAM published no commitment");
	require(nothing_present(suffix, suffix_bytes),
		"the new commitment has a present page before the kernel runs");
	for (std::size_t offset = 0; offset < suffix_bytes; offset += page)
		require(suffix[offset] == 0,
			"the new guest RAM holds a byte of the previous guest");
	for (std::size_t offset = 0; offset < suffix_bytes; offset += page)
		suffix[offset] = static_cast<std::uint8_t>(0xa0 + boot);
}

/* A kernel as it leaves its process: what it created through its imports and
 * does not close, what it changed, the RAM it claimed, the threads it
 * started, and how it says it ended. */
int boot(int, char *const[], char *const[])
{
	const int index = ++kernel_entries;
	const Imports kernel = bind_imports();
	world.descriptors.clear();
	perturb_process(kernel);

	const int token = kernel.open("uml:anonymous-physical-memory", O_RDWR, 0);
	if (token < 0)
		return kernel_without_memory;
	remember(token);
	world.token = token;
	const std::string path = "/tmp/uml-restart-" + std::to_string(getpid()) +
		"-" + std::to_string(index);
	const int file = kernel.open(path.c_str(), O_RDWR | O_CREAT | O_EXCL,
		0600);
	remember(file);
	(void)unlink(path.c_str());
	uml::platform::record_guest_file(file, O_RDWR);

	int pair[2];
	require(kernel.socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
		"socketpair failed");
	remember(pair[0]);
	remember(pair[1]);
	int pipes[2];
	require(kernel.pipe(pipes) == 0, "pipe failed");
	remember(pipes[0]);
	remember(pipes[1]);
	remember(kernel.kqueue());
	const int duplicate = kernel.dup(pipes[0]);
	remember(duplicate);
	remember(kernel.socket(AF_UNIX, SOCK_DGRAM, 0));
	remember(kernel.fcntl(pipes[1], F_DUPFD, 100));
	remember(receive_rights(kernel, pair[0], pair[1], pipes[0]));
	/* A descriptor the kernel closes itself is not the library's to close. */
	require(kernel.close(duplicate) == 0, "close failed");
	world.descriptors.erase(std::find(world.descriptors.begin(),
		world.descriptors.end(), duplicate));

	/* The signal stack perturb_process() allocated, and 100 bytes more; one
	 * more allocation is freed by the kernel itself. */
	std::size_t allocation_bytes = 128U * 1024U + 100U;
	void *const scratch = kernel.malloc(100);
	void *const freed = kernel.malloc(64);
	require(scratch != nullptr && freed != nullptr, "malloc failed");
	kernel.free(freed);
	const std::size_t page = host_page();
	void *const outside = kernel.mmap(nullptr, 4 * page,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	void *const unmapped = kernel.mmap(nullptr, page,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	require(outside != MAP_FAILED && unmapped != MAP_FAILED,
		"mmap failed");
	require(kernel.munmap(unmapped, page) == 0, "munmap failed");
	world.outside_mapping = outside;
	(void)kernel.sbrk(0);
	pthread_key_t first_key;
	pthread_key_t second_key;
	require(kernel.key_create(&first_key, key_destructor) == 0 &&
		kernel.key_create(&second_key, nullptr) == 0,
		"pthread_key_create failed");

	claim_ram(token, index);

	std::array<pthread_t, 5> threads{};
	for (auto &thread : threads)
		require(uml::runtime::kernel_pthread_create(&thread, nullptr,
			short_kernel_thread, nullptr) == 0,
			"could not start a kernel thread");
	for (auto &thread : threads)
		require(pthread_join(thread, nullptr) == 0,
			"could not join a kernel thread");
	if (world.leave_thread && index == 1) {
		require(uml::runtime::kernel_pthread_create(&world.held_thread,
			nullptr, held_kernel_thread, nullptr) == 0,
			"could not start the held kernel thread");
		world.thread_started = true;
	}

	world.expected = {
		.descriptors = world.descriptors.size(),
		.keys = 2,
		.allocations = 2,
		.allocation_bytes = allocation_bytes,
		.mappings = 1,
		.mapping_bytes = 4 * page,
		.sbrk_calls = 1,
		.guest_files = 1,
	};

	const Ending ending = index <= world.restarting ? Ending::restart :
		world.last;
	switch (ending) {
	case Ending::stopped:
		uml_emulator_request_stop(world.handle);
		uml::emulator::testing::report_guest_shutdown(
			GuestShutdownKind::restart, "stopped");
		break;
	case Ending::restart:
		uml::emulator::testing::report_guest_shutdown(
			GuestShutdownKind::restart, "reboot");
		break;
	case Ending::power_off:
		uml::emulator::testing::report_guest_shutdown(
			GuestShutdownKind::power_off, nullptr);
		break;
	case Ending::halt:
		uml::emulator::testing::report_guest_shutdown(
			GuestShutdownKind::halt, nullptr);
		break;
	case Ending::failed:
		break;
	}
	return kernel_booted;
}

/* The cleanup tables for the socket paths and directories a run binds: a slot
 * that a run kept would show as a table that cannot hold a full set. */
void require_socket_path_tables_empty()
{
	namespace fs = std::filesystem;
	const fs::path directory =
		"/tmp/uml-restart-paths-" + std::to_string(getpid());
	fs::create_directory(directory);
	{
		std::vector<std::unique_ptr<uml::device::SocketPath>> paths;
		std::vector<int> sockets;
		for (int index = 0; index < 16; ++index) {
			const fs::path path = directory / ("s" + std::to_string(index));
			sockaddr_un address{};
			address.sun_family = AF_UNIX;
			std::strncpy(address.sun_path, path.c_str(),
				sizeof(address.sun_path) - 1);
			const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
			require(descriptor >= 0 &&
				bind(descriptor, reinterpret_cast<sockaddr *>(&address),
					sizeof(address)) == 0, "could not bind a socket");
			sockets.push_back(descriptor);
			paths.push_back(std::make_unique<uml::device::SocketPath>(path));
			paths.back()->adopt();
		}
		std::vector<std::unique_ptr<uml::device::DirectoryPath>> directories;
		for (int index = 0; index < 4; ++index) {
			const fs::path path = directory / ("d" + std::to_string(index));
			fs::create_directory(path);
			directories.push_back(
				std::make_unique<uml::device::DirectoryPath>(path));
			directories.back()->adopt("entry");
		}
		paths.clear();
		directories.clear();
		for (const int descriptor : sockets)
			(void)close(descriptor);
	}
	std::error_code ignored;
	fs::remove_all(directory, ignored);
}

void require_window_reserved_and_empty()
{
	const auto window = uml::runtime::process_native_window();
	require(window.end > window.begin, "the process holds no window");
	for (std::uintptr_t cursor = window.begin; cursor < window.end;) {
		const auto region = uml::testing::region_containing(cursor);
		require(region.has_value() &&
			region->protection == VM_PROT_NONE,
			"the window holds a mapping or a gap after the hand-back");
		cursor = region->start + region->size;
	}
	require(nothing_present(reinterpret_cast<void *>(window.begin),
		window.end - window.begin),
		"a page of the window is present after the hand-back");
}

/* The bytes the process's live allocations hold. A sanitizer replaces the
 * allocator, and the malloc zone statistics it reports then measure something
 * else: AddressSanitizer's count every byte ever allocated, freed or not, and
 * ThreadSanitizer's do not move at all. Its own count of live bytes answers
 * instead. */
std::size_t heap_in_use() noexcept
{
#if defined(UML_TEST_SANITIZER_ALLOCATOR)
	return __sanitizer_get_current_allocated_bytes();
#else
	malloc_statistics_t statistics{};
	malloc_zone_statistics(malloc_default_zone(), &statistics);
	return statistics.size_in_use;
#endif
}

/* Called inside the run that boots again, when the hand-back has completed. */
void check_hand_back(void *, const GuestHandBackReport &report) noexcept
{
	++world.hand_backs;
	const auto &expected = world.expected;
	require(report.descriptors == expected.descriptors &&
		report.keys == expected.keys &&
		report.allocations == expected.allocations &&
		report.allocation_bytes == expected.allocation_bytes &&
		report.mappings == expected.mappings &&
		report.mapping_bytes == expected.mapping_bytes &&
		report.sbrk_calls == expected.sbrk_calls &&
		report.guest_files == expected.guest_files,
		"the hand-back did not give back what the kernel created");

	const auto counts = uml::runtime::kernel_resource_counts();
	require(counts.descriptors == 0 && counts.keys == 0 &&
		counts.allocations == 0 && counts.allocation_bytes == 0 &&
		counts.mappings == 0 && counts.mapping_bytes == 0 &&
		counts.sbrk_calls == 0, "the kernel-resource ledger is not empty");
	/* No engine heap is leased once the guest is handed back, and what the
	 * memory layer's heap region holds then is recorded for the comparison
	 * at the end of the storm. */
	require(report.heap_leases == 0,
		"an engine heap lease survived the hand-back");
	world.region_committed.push_back(report.heap_bytes);
	for (const int descriptor : world.descriptors)
		require(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF,
			"a descriptor the kernel left open survived the hand-back");
	require(fcntl(world.control, F_GETFD) != -1,
		"the hand-back closed a descriptor the kernel did not create");
	require(!uml::testing::region_containing(world.outside_mapping)
			.has_value(),
		"a mapping the kernel made outside the window survived");
	require(uml::runtime::reclaim_ended_kernel_threads() == 0,
		"a kernel-thread slot survived the hand-back");
	uml::runtime::KernelThreadSnapshot snapshot;
	uml::runtime::snapshot_kernel_threads(snapshot);
	require(!snapshot.overflowed && snapshot.creator_count == 0,
		"the kernel-thread table overflowed");

	require(uml::runtime::current_context() == nullptr,
		"a run route survived the hand-back");
	require(uml::platform::forget_guest_files() == 0,
		"a guest-file record survived the hand-back");
	require_socket_path_tables_empty();

	require(uml::memory::guest_ram_commitment() == nullptr,
		"the guest RAM commitment was not retired");
	require(!uml::memory::anonymous_shared_backing_registered() &&
		!uml::runtime::process_guest_spent(),
		"the guest RAM token was not unregistered");
	require_window_reserved_and_empty();
	require_same_process(world.before);

	world.heap_in_use.push_back(heap_in_use());
	malloc_statistics_t zone{};
	malloc_zone_statistics(malloc_default_zone(), &zone);
	world.zone_allocated = zone.size_allocated;
}

void require_hand_backs(int count)
{
	require(world.hand_backs == count,
		"the hand-back ran a different number of times");
}

struct HandBackHooks final {
	HandBackHooks()
	{
		uml::emulator::testing::set_guest_hand_back_hook(
			check_hand_back, nullptr);
	}
	~HandBackHooks()
	{
		uml::emulator::testing::set_guest_hand_back_hook(nullptr, nullptr);
	}
	HandBackHooks(const HandBackHooks &) = delete;
	HandBackHooks &operator=(const HandBackHooks &) = delete;
};

void begin_restarts(uml_emulator *handle, int restarting, Ending last,
	bool leave_thread = false)
{
	world.handle = handle;
	world.restarting = restarting;
	world.last = last;
	world.leave_thread = leave_thread;
	world.control = open("/dev/null", O_RDONLY);
	require(world.control >= 0, "could not open the control descriptor");
	world.before = capture_process_state();
}

[[nodiscard]] uml_emulator_run_end_kind run_end_kind(
	const uml_emulator *handle)
{
	uml_emulator_run_end end{.struct_size = sizeof(end)};
	require(uml_emulator_get_run_end(handle, &end) == UML_EMULATOR_OK,
		"the run end could not be read");
	return end.kind;
}

void restart_storm()
{
	constexpr int boots = 64;
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), boots - 1, Ending::power_off);
	for (int index = 1; index <= boots; ++index) {
		require(run(emulator.get()) == kernel_booted,
			"a boot of the restarting guest failed");
		require(kernel_entries == index,
			"a run did not enter its kernel once");
		require(run_end_kind(emulator.get()) == (index < boots ?
			UML_EMULATOR_RUN_END_RESTART :
			UML_EMULATOR_RUN_END_POWER_OFF),
			"a boot did not report how it ended");
		require_hand_backs(index - 1);
	}
	require_same_process(world.before);
	/* The last guest powered off: it stays spent. */
	require_refused(emulator.get());
	require(run_end_kind(emulator.get()) == UML_EMULATOR_RUN_END_NONE,
		"a refused run repeated the power-off");
	require_refused(make_emulator(boot).get());
	require_hand_backs(boots - 1);

	/* The allocator keeps what a boot freed (xzone malloc retains empty
	 * chunks); the bytes the library or the kernel still holds stay flat
	 * across hand-backs. */
	constexpr std::size_t slack = 1U << 20;
	const std::size_t baseline = world.heap_in_use[2];
	for (std::size_t index = 3; index < world.heap_in_use.size(); ++index) {
		if (world.heap_in_use[index] <= baseline + slack)
			continue;
		std::fprintf(stderr,
			"heap: %zu bytes in use after the 3rd hand-back, %zu after the "
			"%zu-th\n",
			baseline, world.heap_in_use[index], index + 1);
		require(false, "the heap's in-use bytes grew across hand-backs");
	}
	std::fprintf(stderr,
		"heap: %zu bytes in use after the 3rd hand-back, %zu after the "
		"%zu-th; the default zone holds %zu bytes\n",
		baseline, world.heap_in_use.back(), world.heap_in_use.size(),
		world.zone_allocated);
	/* The memory layer's heap region holds the same bytes at every hand-back:
	 * a boot's heaps are given back whole, not kept until the system takes
	 * them. */
	for (const auto bytes : world.region_committed)
		require(bytes == world.region_committed.front(),
			"the heap region's committed bytes changed across hand-backs");
	(void)close(world.control);
}

void a_spent_end_stays_spent(Ending ending,
	uml_emulator_run_end_kind expected)
{
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), 0, ending);
	require(run(emulator.get()) == kernel_booted,
		"the guest did not boot");
	require(run_end_kind(emulator.get()) == expected,
		"the run did not report how it ended");
	if (ending == Ending::stopped || ending == Ending::failed)
		require(uml_emulator_release_guest(emulator.get()) ==
			-UML_EMULATOR_UNSUPPORTED,
			"an unclean guest could be released");
	require_refused(emulator.get());
	require_refused(make_emulator(boot).get());
	require_hand_backs(0);
	(void)close(world.control);
}

void clean_power_off_releases_the_process(bool fresh_handle, Ending ending)
{
	const HandBackHooks hooks;
	auto first = make_emulator(boot);
	begin_restarts(first.get(), 0, ending);
	require(uml_emulator_release_guest(nullptr) == -UML_EMULATOR_INVALID_ARGUMENT,
		"a null handle could release a guest");
	require(uml_emulator_release_guest(first.get()) == -UML_EMULATOR_UNSUPPORTED,
		"a fresh handle could release a guest");
	require(run(first.get()) == kernel_booted, "the first guest did not boot");
	auto second = make_emulator(boot);
	require(uml_emulator_release_guest(second.get()) == -UML_EMULATOR_UNSUPPORTED,
		"another handle could release the first guest");
	require(uml_emulator_release_guest(first.get()) == 0,
		"a clean power-off could not release its guest");
	require_hand_backs(1);
	require(run_end_kind(first.get()) == UML_EMULATOR_RUN_END_POWER_OFF,
		"releasing a guest changed its run end");
	world.handle = fresh_handle ? second.get() : first.get();
	require(run(world.handle) == kernel_booted, "the next guest did not boot");
	if (fresh_handle)
		require(uml_emulator_release_guest(first.get()) == -UML_EMULATOR_UNSUPPORTED,
			"an old handle could release a newer guest");
	require(uml_emulator_release_guest(world.handle) == 0,
		"the second clean shutdown could not release its guest");
	require_hand_backs(2);
	(void)close(world.control);
}

void a_clean_release_waits_for_resources(bool leave_thread)
{
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), 0, Ending::power_off, leave_thread);
	require(run(emulator.get()) == kernel_booted, "the clean guest did not boot");
	uml::memory::SharedBackingView view;
	if (!leave_thread)
		view = uml::memory::acquire_shared_backing_view(world.token,
			image_pages * host_page(), host_page(), PROT_READ | PROT_WRITE);
	const auto before = uml::runtime::kernel_resource_counts();
	for (int attempt = 0; attempt < 2; ++attempt)
		require(uml_emulator_release_guest(emulator.get()) == -UML_EMULATOR_RUNTIME_ERROR,
			"guest RAM was released while a service or kernel thread held it");
	require_hand_backs(0);
	require(uml::runtime::kernel_resource_counts().descriptors == before.descriptors,
		"a refused release closed kernel resources");
	if (leave_thread) {
		world.release_thread.store(true, std::memory_order_release);
		require(pthread_join(world.held_thread, nullptr) == 0, "could not join the kernel thread");
	} else {
		view = {};
	}
	require(uml_emulator_release_guest(emulator.get()) == 0,
		"the clean guest was not released after its resources stopped");
	require_hand_backs(1);
	(void)close(world.control);
}

void a_restart_belongs_to_its_handle()
{
	const HandBackHooks hooks;
	auto first = make_emulator(boot);
	begin_restarts(first.get(), 1, Ending::power_off);
	require(run(first.get()) == kernel_booted &&
		run_end_kind(first.get()) == UML_EMULATOR_RUN_END_RESTART,
		"the first boot did not restart");
	/* The process's guest waits for its own handle. */
	auto other = make_emulator(boot);
	require_refused(other.get());
	require_refused(other.get());
	require_hand_backs(0);
	require(run(first.get()) == kernel_booted &&
		run_end_kind(first.get()) == UML_EMULATOR_RUN_END_POWER_OFF,
		"the restarted guest did not boot on its own handle");
	require_hand_backs(1);
	require_refused(other.get());
	require_refused(first.get());
	(void)close(world.control);
}

void a_borrowed_view_stops_the_restart()
{
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), 1, Ending::power_off);
	require(run(emulator.get()) == kernel_booted,
		"the first boot failed");
	const std::size_t page = host_page();
	auto view = uml::memory::acquire_shared_backing_view(world.token,
		image_pages * page, page, PROT_READ | PROT_WRITE);
	require(static_cast<bool>(view), "could not borrow guest RAM");
	const auto *ram = static_cast<const std::uint8_t *>(view.data());
	const int entries = kernel_entries;
	const auto before = uml::runtime::kernel_resource_counts();
	require(before.descriptors != 0, "the kernel left no descriptors");
	for (int attempt = 0; attempt < 2; ++attempt) {
		require(run(emulator.get()) == -UML_EMULATOR_RUNTIME_ERROR,
			"a restart over a borrowed view did not fail");
		require(kernel_entries == entries,
			"a refused restart entered its kernel");
		require(run_end_kind(emulator.get()) == UML_EMULATOR_RUN_END_NONE,
			"a refused restart reported an end");
		/* Nothing was handed back. */
		const auto now = uml::runtime::kernel_resource_counts();
		require(now.descriptors == before.descriptors &&
			now.keys == before.keys &&
			now.allocations == before.allocations &&
			now.mappings == before.mappings,
			"a refused restart gave resources back");
		require(uml::memory::guest_ram_commitment() != nullptr &&
			uml::memory::anonymous_shared_backing_registered() &&
			ram[0] == 0xa1, "a refused restart took the guest's RAM");
	}
	require_hand_backs(0);
	{
		auto released = std::move(view);
	}
	require(run(emulator.get()) == kernel_booted,
		"the restart did not proceed once the view was released");
	require_hand_backs(1);
	require(kernel_entries == entries + 1, "the restart did not boot");
	(void)close(world.control);
}

void a_running_thread_stops_the_restart()
{
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), 1, Ending::power_off, true);
	require(run(emulator.get()) == kernel_booted && world.thread_started,
		"the first boot failed");
	const int entries = kernel_entries;
	const auto before = uml::runtime::kernel_resource_counts();
	require(before.descriptors != 0, "the kernel left no descriptors");
	require(run(emulator.get()) == -UML_EMULATOR_RUNTIME_ERROR,
		"a restart with a running kernel thread did not fail");
	require(kernel_entries == entries,
		"a refused restart entered its kernel");
	const auto now = uml::runtime::kernel_resource_counts();
	require(now.descriptors == before.descriptors,
		"a refused restart gave resources back");
	require_hand_backs(0);
	world.release_thread.store(true, std::memory_order_release);
	require(pthread_join(world.held_thread, nullptr) == 0,
		"could not join the held kernel thread");
	require(run(emulator.get()) == kernel_booted,
		"the restart did not proceed once the thread ended");
	require_hand_backs(1);
	(void)close(world.control);
}

/* ---- A stop or a resume racing the hand-back ---- */

/* What the hibernation a boot made sees of an abort request that is still
 * running when the restart drops it. */
struct AbortRace final {
	std::mutex mutex;
	std::condition_variable changed;
	int requests{};
	bool inside{};
	bool released{};
	bool destroyed{};
	bool used_after_destruction{};
};

class RacingSteps final : public uml::runtime::HibernationSteps {
public:
	explicit RacingSteps(std::shared_ptr<AbortRace> race)
		: race_{std::move(race)}
	{
	}
	~RacingSteps() override
	{
		{
			const std::lock_guard lock{race_->mutex};
			race_->destroyed = true;
		}
		race_->changed.notify_all();
	}

	bool prepare() noexcept override { return true; }
	bool hold(uml::runtime::HibernationReport &,
		uml::runtime::HoldCondition) noexcept override
	{
		return true;
	}
	bool write(uml::runtime::HibernationReport &) noexcept override
	{
		return true;
	}
	bool restore(uml::runtime::HibernationReport &) noexcept override
	{
		return true;
	}
	void release() noexcept override {}

	/* The first request, a stop's or a resume's, stays inside until the test
	 * lets it go. A hand-back that destroys this object meanwhile is the
	 * defect: the request would be running in freed memory. */
	void set_abort(bool abort) noexcept override
	{
		if (!abort)
			return;
		const std::shared_ptr<AbortRace> race = race_;
		std::unique_lock lock{race->mutex};
		const int number = ++race->requests;
		race->changed.notify_all();
		if (number != 1)
			return;
		race->inside = true;
		race->changed.notify_all();
		race->changed.wait(lock, [&] { return race->released; });
		race->used_after_destruction = race->destroyed;
	}

private:
	std::shared_ptr<AbortRace> race_;
};

void restore_must_not_run() noexcept
{
	std::fputs("a restore ran on a hibernation that never hibernated\n",
		stderr);
	std::_Exit(EXIT_FAILURE);
}

enum class Interruption { stop, resume };

/* The previous boot hibernated once. A stop or a resume on another thread is
 * inside its abort request, which runs without the lifecycle mutex, when the
 * restart hands the guest back and drops the hibernation. The drop must wait
 * for the request. */
void a_restart_waits_for_an_abort_request(Interruption interruption)
{
	using namespace std::chrono_literals;
	const HandBackHooks hooks;
	auto emulator = make_emulator(boot);
	begin_restarts(emulator.get(), 1, Ending::power_off);
	require(run(emulator.get()) == kernel_booted, "the first boot failed");
	const auto race = std::make_shared<AbortRace>();
	uml_emulator *const handle = emulator.get();
	uml::emulator::testing::use_hibernation(handle,
		std::make_unique<RacingSteps>(race), &restore_must_not_run);

	std::thread interrupting{[handle, interruption] {
		if (interruption == Interruption::stop)
			uml_emulator_request_stop(handle);
		else
			uml_emulator_resume(handle);
	}};
	{
		std::unique_lock lock{race->mutex};
		require(race->changed.wait_for(lock, 10s,
			[&] { return race->inside; }),
			"the abort request did not begin");
	}
	int status = 0;
	/* The hand-back's check compares the run thread's own signal mask and
	 * stack with those before the run, so the thread records them itself. */
	std::thread restarting{[&status, handle] {
		world.before = capture_process_state();
		status = run(handle);
	}};
	{
		std::unique_lock lock{race->mutex};
		/* A hand-back that does not wait for the request reaches the
		 * hibernation at once and destroys it; one that waits is still
		 * blocked when the time is up. */
		if (race->changed.wait_for(lock, 500ms,
			    [&] { return race->requests > 1 || race->destroyed; }))
			(void)race->changed.wait_for(lock, 10s,
				[&] { return race->destroyed; });
		race->released = true;
	}
	race->changed.notify_all();
	interrupting.join();
	restarting.join();

	require(!race->used_after_destruction,
		"the restart destroyed the hibernation under an abort request");
	if (interruption == Interruption::resume) {
		require(status == kernel_booted,
			"the restart failed beside a resume");
	}
	/* A stop that lands in the restarting run may cancel it, before the
	 * hand-back or after. */
	if (status == kernel_booted) {
		require(race->destroyed,
			"the restart did not drop the hibernation");
		require_hand_backs(1);
	}
	(void)close(world.control);
}

} // namespace

/* CTest gives every test it runs at once the same open file description for
 * descriptor 0, and these tests compare its status flags with what the run
 * left while the kernel scope of a test beside them sets and restores the
 * same flags. Descriptor 0 gets a description of this process's own. */
void use_a_private_standard_input()
{
	const int input = open("/dev/null", O_RDONLY);
	require(input >= 0, "cannot open /dev/null");
	require(dup2(input, 0) == 0, "cannot replace descriptor 0");
	if (input != 0)
		close(input);
}

int main(int argc, char **argv)
{
	try {
		use_a_private_standard_input();
		const std::string_view mode = argc == 2 ? argv[1] : "";
		if (mode == "same-handle")
			second_run_on_the_same_handle();
		else if (mode == "fresh-handle")
			second_run_on_a_fresh_handle();
		else if (mode == "unclaimed-first-run")
			run_that_left_guest_ram();
		else if (mode == "release-same-handle")
			clean_power_off_releases_the_process(false, Ending::power_off);
		else if (mode == "release-fresh-handle")
			clean_power_off_releases_the_process(true, Ending::power_off);
		else if (mode == "release-halt")
			clean_power_off_releases_the_process(true, Ending::halt);
		else if (mode == "release-borrowed-view")
			a_clean_release_waits_for_resources(false);
		else if (mode == "release-running-thread")
			a_clean_release_waits_for_resources(true);
		else if (mode == "restart")
			restart_storm();
		else if (mode == "power-off")
			a_spent_end_stays_spent(Ending::power_off,
				UML_EMULATOR_RUN_END_POWER_OFF);
		else if (mode == "halt")
			a_spent_end_stays_spent(Ending::halt,
				UML_EMULATOR_RUN_END_POWER_OFF);
		else if (mode == "stopped")
			a_spent_end_stays_spent(Ending::stopped,
				UML_EMULATOR_RUN_END_STOPPED);
		else if (mode == "failed")
			a_spent_end_stays_spent(Ending::failed,
				UML_EMULATOR_RUN_END_FAILED);
		else if (mode == "another-handle")
			a_restart_belongs_to_its_handle();
		else if (mode == "borrowed-view")
			a_borrowed_view_stops_the_restart();
		else if (mode == "running-thread")
			a_running_thread_stops_the_restart();
		else if (mode == "abort-stop")
			a_restart_waits_for_an_abort_request(Interruption::stop);
		else if (mode == "abort-resume")
			a_restart_waits_for_an_abort_request(Interruption::resume);
		else
			throw std::runtime_error{
				"usage: uml_runtime_sequential_run_tests "
				"same-handle|fresh-handle|unclaimed-first-run|restart|"
				"power-off|halt|stopped|failed|another-handle|"
				"borrowed-view|running-thread|abort-stop|abort-resume|"
				"release-same-handle|release-fresh-handle|release-halt|"
				"release-borrowed-view|release-running-thread"};
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
