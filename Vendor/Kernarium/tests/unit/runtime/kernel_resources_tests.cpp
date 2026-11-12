// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The ledger of what an embedded kernel creates through its imports. The
 * loader resolves each creating import to a wrapper, and a guest that
 * restarts itself gives everything recorded back: descriptors closed through
 * darwin_uml_close, pthread keys deleted, allocations freed, and mappings
 * outside the native window unmapped. These tests call the wrappers as the
 * kernel does, through the same lookup the loader uses.
 */
#include "kernel_resources.hpp"
#include "require.hpp"
#include "uml_darwin_host.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using uml::runtime::KernelResourceCounts;
using uml::runtime::kernel_resource_counts;
using uml::runtime::kernel_resource_import;
using uml::runtime::release_kernel_resources;
using uml::testing::require;

template<typename Function>
[[nodiscard]] Function *import(const char *name)
{
	void *const wrapper = kernel_resource_import(name);
	require(wrapper != nullptr, "the ledger does not follow an import");
	return reinterpret_cast<Function *>(wrapper);
}

[[nodiscard]] bool open_descriptor(int descriptor)
{
	return fcntl(descriptor, F_GETFD) != -1;
}

[[nodiscard]] int protection_of(const void *address)
{
	mach_vm_address_t region = reinterpret_cast<mach_vm_address_t>(address);
	mach_vm_size_t size = 0;
	vm_region_basic_info_data_64_t information {};
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	const auto result = mach_vm_region(mach_task_self(), &region, &size,
		VM_REGION_BASIC_INFO_64,
		reinterpret_cast<vm_region_info_t>(&information), &count, &object);
	if (object != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), object);
	if (result != KERN_SUCCESS ||
	    region > reinterpret_cast<mach_vm_address_t>(address))
		return -1;
	return information.protection;
}

void require_empty(const char *when)
{
	const KernelResourceCounts counts = kernel_resource_counts();
	require(counts.descriptors == 0 && counts.keys == 0 &&
		counts.allocations == 0 && counts.allocation_bytes == 0 &&
		counts.mappings == 0 && counts.mapping_bytes == 0 &&
		counts.sbrk_calls == 0,
		when);
}

/* The lookup the loader performs on a symbol name. */
void test_lookup()
{
	for (const char *name : {"darwin_uml_open", "darwin_uml_close",
		     "darwin_uml_fcntl", "socket", "socketpair", "accept",
		     "pipe", "kqueue", "dup", "recvmsg", "malloc", "free", "mmap",
		     "munmap", "sbrk", "pthread_key_create"}) {
		require(kernel_resource_import(name) != nullptr, name);
		/* The Darwin SDK's asm labels carry one leading underscore. */
		char labelled[64];
		std::snprintf(labelled, sizeof(labelled), "_%s", name);
		require(kernel_resource_import(labelled) ==
			kernel_resource_import(name), labelled);
	}
	/* Imports that create nothing are not wrapped. */
	for (const char *name : {"darwin_uml_ioctl", "read", "_read", "write",
		     "kill", "pthread_create", "setitimer", "__error", "", "_"})
		require(kernel_resource_import(name) == nullptr, name);
	require(kernel_resource_import(nullptr) == nullptr);
}

/* Every call that creates a descriptor records it, and a kernel close forgets
 * it. */
void test_descriptors()
{
	require_empty("the ledger did not start empty");
	const auto open_file = import<int(const char *, int, int)>(
		"darwin_uml_open");
	const auto close_file = import<int(int)>("darwin_uml_close");
	const auto fcntl_file = import<int(int, int, long)>("darwin_uml_fcntl");
	const auto make_socket = import<int(int, int, int)>("socket");
	const auto make_socketpair = import<int(int, int, int, int[2])>(
		"_socketpair");
	const auto make_pipe = import<int(int[2])>("pipe");
	const auto make_kqueue = import<int()>("kqueue");
	const auto duplicate = import<int(int)>("dup");

	const int file = open_file("/dev/null", O_RDWR, 0);
	const int listener = make_socket(AF_UNIX, SOCK_STREAM, 0);
	int pair[2];
	require(make_socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	int pipes[2];
	require(make_pipe(pipes) == 0);
	const int queue = make_kqueue();
	const int copy = duplicate(file);
	const int duplicated_again = fcntl_file(file, F_DUPFD, 0);
	/* A command that duplicates nothing records nothing. */
	require(fcntl_file(file, F_GETFD, 0) >= 0);
	require(file >= 0 && listener >= 0 && queue >= 0 && copy >= 0 &&
		duplicated_again >= 0);
	require(kernel_resource_counts().descriptors == 9,
		"a creating import was not recorded");

	/* A failed call records nothing. */
	require(make_socket(-1, -1, -1) < 0);
	require(open_file("/nonexistent/uml-ledger-test", O_RDONLY, 0) < 0);
	require(kernel_resource_counts().descriptors == 9,
		"a failed call was recorded");

	/* The kernel closes some of them. */
	require(close_file(pipes[0]) == 0 && close_file(copy) == 0);
	require(kernel_resource_counts().descriptors == 7,
		"a kernel close did not forget its descriptor");

	const int held[] = {file, listener, pair[0], pair[1], pipes[1], queue,
		duplicated_again};
	for (const int descriptor : held)
		require(open_descriptor(descriptor));
	const auto released = release_kernel_resources(0, 0);
	require(released.descriptors == 7, "the release miscounted descriptors");
	for (const int descriptor : held)
		require(!open_descriptor(descriptor),
			"a kernel descriptor survived the release");
	require_empty("descriptors left the ledger non-empty");
}

/* A descriptor the kernel receives with SCM_RIGHTS is its own. */
void test_received_descriptors()
{
	require_empty("the ledger did not start empty");
	int pair[2];
	require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	int blob[2];
	require(pipe(blob) == 0);

	/* The host side sends two descriptors and a plain byte. */
	char sent_control[CMSG_SPACE(2 * sizeof(int))] {};
	char byte = 'b';
	iovec sent_vector{&byte, 1};
	msghdr sent{};
	sent.msg_iov = &sent_vector;
	sent.msg_iovlen = 1;
	sent.msg_control = sent_control;
	sent.msg_controllen = sizeof(sent_control);
	cmsghdr *header = CMSG_FIRSTHDR(&sent);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(2 * sizeof(int));
	std::memcpy(CMSG_DATA(header), blob, 2 * sizeof(int));
	require(sendmsg(pair[0], &sent, 0) == 1);

	const auto receive = import<ssize_t(int, msghdr *, int)>("_recvmsg");
	char received_control[CMSG_SPACE(2 * sizeof(int))] {};
	char received_byte = 0;
	iovec received_vector{&received_byte, 1};
	msghdr received{};
	received.msg_iov = &received_vector;
	received.msg_iovlen = 1;
	received.msg_control = received_control;
	received.msg_controllen = sizeof(received_control);
	require(receive(pair[1], &received, 0) == 1 && received_byte == 'b');
	header = CMSG_FIRSTHDR(&received);
	require(header != nullptr && header->cmsg_type == SCM_RIGHTS);
	int arrived[2];
	std::memcpy(arrived, CMSG_DATA(header), sizeof(arrived));
	require(kernel_resource_counts().descriptors == 2,
		"descriptors received by recvmsg were not recorded");

	/* A receive without a control buffer, and one with none to read, records
	 * nothing and keeps the caller's errno. */
	char plain = 'p';
	require(send(pair[0], &plain, 1, 0) == 1);
	iovec plain_vector{&received_byte, 1};
	msghdr plain_message{};
	plain_message.msg_iov = &plain_vector;
	plain_message.msg_iovlen = 1;
	require(receive(pair[1], &plain_message, 0) == 1 && received_byte == 'p');
	require(kernel_resource_counts().descriptors == 2);

	release_kernel_resources(0, 0);
	require(!open_descriptor(arrived[0]) && !open_descriptor(arrived[1]),
		"a received descriptor survived the release");
	/* The host's own ends were never the kernel's. */
	require(open_descriptor(pair[0]) && open_descriptor(pair[1]) &&
		open_descriptor(blob[0]) && open_descriptor(blob[1]),
		"the release closed a descriptor the kernel did not create");
	for (const int descriptor : {pair[0], pair[1], blob[0], blob[1]})
		close(descriptor);
	require_empty("received descriptors left the ledger non-empty");
}

void test_keys()
{
	require_empty("the ledger did not start empty");
	const auto create = import<int(pthread_key_t *, void (*)(void *))>(
		"pthread_key_create");
	pthread_key_t first;
	pthread_key_t second;
	require(create(&first, nullptr) == 0 && create(&second, [](void *) {}) == 0);
	require(pthread_setspecific(first, &first) == 0);
	require(kernel_resource_counts().keys == 2);
	const auto released = release_kernel_resources(0, 0);
	require(released.keys == 2);
	/* A deleted key is invalid, and its destructor can never run. */
	require(pthread_setspecific(first, &first) == EINVAL &&
		pthread_setspecific(second, &second) == EINVAL,
		"a kernel pthread key survived the release");
	require_empty("keys left the ledger non-empty");
}

void test_allocations()
{
	require_empty("the ledger did not start empty");
	const auto allocate = import<void *(std::size_t)>("malloc");
	const auto release = import<void(void *)>("free");
	void *const stack = allocate(128U * 1024U);
	void *const small = allocate(48);
	void *const freed = allocate(64);
	require(stack != nullptr && small != nullptr && freed != nullptr);
	require(kernel_resource_counts().allocations == 3 &&
		kernel_resource_counts().allocation_bytes ==
			128U * 1024U + 48U + 64U);
	release(freed);
	/* A pointer the ledger never saw, and null, free as usual. */
	release(std::malloc(16));
	release(nullptr);
	require(kernel_resource_counts().allocations == 2 &&
		kernel_resource_counts().allocation_bytes == 128U * 1024U + 48U);
	const auto released = release_kernel_resources(0, 0);
	require(released.allocations == 2 &&
		released.allocation_bytes == 128U * 1024U + 48U);
	require_empty("allocations left the ledger non-empty");
}

/* Mappings outside the native window go back to the process. Mappings inside
 * it are left to the window's replacement, and one across an edge loses only
 * its outside. */
void test_mappings()
{
	require_empty("the ledger did not start empty");
	const auto map = import<void *(void *, std::size_t, int, int, int, off_t)>(
		"_mmap");
	const auto unmap = import<int(void *, std::size_t)>("_munmap");
	const std::size_t page = static_cast<std::size_t>(getpagesize());

	/* A region standing for the process, 12 pages; the window is pages 4..8. */
	void *const region = mmap(nullptr, page * 12, PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	require(region != MAP_FAILED);
	auto *const base = static_cast<std::uint8_t *>(region);
	const auto window_begin = reinterpret_cast<std::uintptr_t>(base + page * 4);
	const auto window_end = reinterpret_cast<std::uintptr_t>(base + page * 8);
	const int rw = PROT_READ | PROT_WRITE;
	const int fixed = MAP_FIXED | MAP_PRIVATE | MAP_ANON;
	/* Pages 0..1 outside, 3..4 across the first edge, 5..6 inside, 7..8 across
	 * the second edge, 10 outside, and page 1 unmapped again by the kernel. */
	require(map(base, page * 2, rw, fixed, -1, 0) == base);
	require(map(base + page * 3, page * 2, rw, fixed, -1, 0) ==
		base + page * 3);
	require(map(base + page * 5, page * 2, rw, fixed, -1, 0) ==
		base + page * 5);
	require(map(base + page * 7, page * 2, rw, fixed, -1, 0) ==
		base + page * 7);
	require(map(base + page * 10, page, rw, fixed, -1, 0) == base + page * 10);
	require(unmap(base + page, page) == 0);
	require(kernel_resource_counts().mappings == 5 &&
		kernel_resource_counts().mapping_bytes == page * 8,
		"the ledger miscounted the kernel's mappings");
	/* A failed map records nothing. */
	require(map(nullptr, 0, rw, MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED);
	require(kernel_resource_counts().mappings == 5);

	const auto released = release_kernel_resources(window_begin, window_end);
	/* Outside: page 0, 10 and the outside halves of 3..4 and 7..8. */
	require(released.mappings == 4 && released.mapping_bytes == page * 4,
		"the release unmapped the wrong ranges");
	require(protection_of(base) == -1, "an outside mapping survived");
	require(protection_of(base + page * 3) == -1,
		"the outside half of a straddling mapping survived");
	require(protection_of(base + page * 4) == rw,
		"the inside half of a straddling mapping was unmapped");
	require(protection_of(base + page * 5) == rw &&
		protection_of(base + page * 6) == rw,
		"a mapping inside the window was unmapped");
	require(protection_of(base + page * 7) == rw,
		"the inside half of the second straddling mapping was unmapped");
	require(protection_of(base + page * 8) == -1,
		"the outside half of the second straddling mapping survived");
	require(protection_of(base + page * 10) == -1);
	(void)munmap(base + page * 4, page * 4);
	(void)munmap(base + page * 2, page);
	(void)munmap(base + page * 9, page);
	(void)munmap(base + page * 11, page);
	require_empty("mappings left the ledger non-empty");
}

/* sbrk allocates nothing on Darwin, so it is counted and not a resource. */
void test_sbrk_is_not_a_resource()
{
	require_empty("the ledger did not start empty");
	const auto grow = import<void *(int)>("sbrk");
	(void)grow(0);
	(void)grow(0);
	const auto counts = kernel_resource_counts();
	require(counts.sbrk_calls == 2 && counts.descriptors == 0 &&
		counts.keys == 0 && counts.allocations == 0 && counts.mappings == 0,
		"sbrk was recorded as a resource");
	const auto released = release_kernel_resources(0, 0);
	require(released.sbrk_calls == 2 && released.descriptors == 0 &&
		released.keys == 0 && released.allocations == 0 &&
		released.mappings == 0,
		"the release treated sbrk as a resource");
	require_empty("sbrk left the ledger non-empty");
}

/* A second release finds nothing. */
void test_release_is_idempotent()
{
	require_empty("the ledger did not start empty");
	const auto make_pipe = import<int(int[2])>("pipe");
	int pipes[2];
	require(make_pipe(pipes) == 0);
	require(release_kernel_resources(0, 0).descriptors == 2);
	const auto again = release_kernel_resources(0, 0);
	require(again.descriptors == 0 && again.keys == 0 &&
		again.allocations == 0 && again.mappings == 0 &&
		again.sbrk_calls == 0);
}

} // namespace

int main()
{
	test_lookup();
	test_descriptors();
	test_received_descriptors();
	test_keys();
	test_allocations();
	test_mappings();
	test_sbrk_is_not_a_resource();
	test_release_is_idempotent();
	std::puts("kernel resource ledger tests passed");
	return EXIT_SUCCESS;
}
