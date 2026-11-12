// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel_resources.hpp"

#include "uml_darwin_host.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

namespace uml::runtime {
namespace {

struct Ledger final {
	std::mutex mutex;
	std::set<int> descriptors;
	std::vector<pthread_key_t> keys;
	std::unordered_map<void *, std::size_t> allocations;
	std::size_t allocation_bytes{};
	/* Disjoint [begin, end) ranges, keyed by begin. */
	std::map<std::uintptr_t, std::uintptr_t> mappings;
	std::size_t sbrk_calls{};
};

/* A kernel thread can be inside a wrapper while the process ends. */
Ledger *const ledger = new Ledger;

/* Bookkeeping must never fail the call it follows: a resource that cannot be
 * recorded is simply not given back, as before the ledger. errno is the
 * wrapped call's. */
template<typename Record>
void record(Record &&function) noexcept
{
	const int error = errno;
	try {
		std::lock_guard lock{ledger->mutex};
		function();
	} catch (...) {
	}
	errno = error;
}

void record_descriptor(int descriptor) noexcept
{
	if (descriptor >= 0)
		record([descriptor] { ledger->descriptors.insert(descriptor); });
}

int kernel_open(const char *path, int flags, int mode)
{
	const int descriptor = darwin_uml_open(path, flags, mode);
	record_descriptor(descriptor);
	return descriptor;
}

int kernel_close(int descriptor)
{
	record([descriptor] { ledger->descriptors.erase(descriptor); });
	return darwin_uml_close(descriptor);
}

int kernel_fcntl(int descriptor, int command, long argument)
{
	const int result = darwin_uml_fcntl(descriptor, command, argument);
	if (command == F_DUPFD || command == F_DUPFD_CLOEXEC)
		record_descriptor(result);
	return result;
}

int kernel_socket(int domain, int type, int protocol)
{
	const int descriptor = socket(domain, type, protocol);
	record_descriptor(descriptor);
	return descriptor;
}

int kernel_socketpair(int domain, int type, int protocol, int pair[2])
{
	const int result = socketpair(domain, type, protocol, pair);
	if (result == 0) {
		record_descriptor(pair[0]);
		record_descriptor(pair[1]);
	}
	return result;
}

int kernel_accept(int listener, sockaddr *address, socklen_t *length)
{
	const int descriptor = accept(listener, address, length);
	record_descriptor(descriptor);
	return descriptor;
}

int kernel_pipe(int pair[2])
{
	const int result = pipe(pair);
	if (result == 0) {
		record_descriptor(pair[0]);
		record_descriptor(pair[1]);
	}
	return result;
}

int kernel_kqueue()
{
	const int descriptor = kqueue();
	record_descriptor(descriptor);
	return descriptor;
}

int kernel_dup(int descriptor)
{
	const int duplicate = dup(descriptor);
	record_descriptor(duplicate);
	return duplicate;
}

/* The kernel receives blob descriptors from host services by SCM_RIGHTS. */
ssize_t kernel_recvmsg(int descriptor, msghdr *message, int flags)
{
	const ssize_t result = recvmsg(descriptor, message, flags);
	if (result < 0 || message == nullptr || message->msg_control == nullptr)
		return result;
	const int error = errno;
	for (cmsghdr *header = CMSG_FIRSTHDR(message); header != nullptr;
	     header = CMSG_NXTHDR(message, header)) {
		if (header->cmsg_level != SOL_SOCKET ||
		    header->cmsg_type != SCM_RIGHTS)
			continue;
		const auto *data = reinterpret_cast<const int *>(CMSG_DATA(header));
		const std::size_t count =
			(header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (std::size_t index = 0; index < count; ++index)
			record_descriptor(data[index]);
	}
	errno = error;
	return result;
}

void *kernel_malloc(std::size_t size)
{
	void *const pointer = std::malloc(size);
	if (pointer != nullptr)
		record([pointer, size] {
			ledger->allocations[pointer] = size;
			ledger->allocation_bytes += size;
		});
	return pointer;
}

void kernel_free(void *pointer)
{
	if (pointer != nullptr)
		record([pointer] {
			const auto found = ledger->allocations.find(pointer);
			if (found == ledger->allocations.end())
				return;
			ledger->allocation_bytes -= found->second;
			ledger->allocations.erase(found);
		});
	std::free(pointer);
}

/* Removes [begin, end) from the recorded mappings, keeping what lies outside
 * it. */
void forget_mapping_locked(std::uintptr_t begin, std::uintptr_t end)
{
	auto entry = ledger->mappings.upper_bound(begin);
	if (entry != ledger->mappings.begin())
		--entry;
	while (entry != ledger->mappings.end() && entry->first < end) {
		const std::uintptr_t first = entry->first;
		const std::uintptr_t last = entry->second;
		if (last <= begin) {
			++entry;
			continue;
		}
		entry = ledger->mappings.erase(entry);
		if (first < begin)
			ledger->mappings.emplace(first, begin);
		if (last > end)
			entry = ledger->mappings.emplace(end, last).first;
	}
}

void *kernel_mmap(void *address, std::size_t length, int protection,
	int flags, int descriptor, off_t offset)
{
	void *const mapped = mmap(address, length, protection, flags,
		descriptor, offset);
	if (mapped != MAP_FAILED && length != 0)
		record([mapped, length] {
			const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
			forget_mapping_locked(begin, begin + length);
			ledger->mappings.emplace(begin, begin + length);
		});
	return mapped;
}

int kernel_munmap(void *address, std::size_t length)
{
	const int result = munmap(address, length);
	if (result == 0 && length != 0)
		record([address, length] {
			const auto begin = reinterpret_cast<std::uintptr_t>(address);
			forget_mapping_locked(begin, begin + length);
		});
	return result;
}

/* Darwin's sbrk allocates nothing: it fails or returns the break. It is
 * counted so a kernel that comes to rely on it shows. */
void *kernel_sbrk(int increment)
{
	record([] { ++ledger->sbrk_calls; });
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	return sbrk(increment);
#pragma clang diagnostic pop
}

int kernel_pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
	const int result = pthread_key_create(key, destructor);
	if (result == 0)
		record([created = *key] { ledger->keys.push_back(created); });
	return result;
}

struct Import final {
	const char *name;
	void *wrapper;
};

} // namespace

void *kernel_resource_import(const char *name) noexcept
{
	static const Import imports[] = {
		{"darwin_uml_open", reinterpret_cast<void *>(&kernel_open)},
		{"darwin_uml_close", reinterpret_cast<void *>(&kernel_close)},
		{"darwin_uml_fcntl", reinterpret_cast<void *>(&kernel_fcntl)},
		{"socket", reinterpret_cast<void *>(&kernel_socket)},
		{"socketpair", reinterpret_cast<void *>(&kernel_socketpair)},
		{"accept", reinterpret_cast<void *>(&kernel_accept)},
		{"pipe", reinterpret_cast<void *>(&kernel_pipe)},
		{"kqueue", reinterpret_cast<void *>(&kernel_kqueue)},
		{"dup", reinterpret_cast<void *>(&kernel_dup)},
		{"recvmsg", reinterpret_cast<void *>(&kernel_recvmsg)},
		{"malloc", reinterpret_cast<void *>(&kernel_malloc)},
		{"free", reinterpret_cast<void *>(&kernel_free)},
		{"mmap", reinterpret_cast<void *>(&kernel_mmap)},
		{"munmap", reinterpret_cast<void *>(&kernel_munmap)},
		{"sbrk", reinterpret_cast<void *>(&kernel_sbrk)},
		{"pthread_key_create",
			reinterpret_cast<void *>(&kernel_pthread_key_create)},
	};
	if (name == nullptr)
		return nullptr;
	if (name[0] == '_')
		++name;
	for (const auto &import : imports)
		if (std::strcmp(import.name, name) == 0)
			return import.wrapper;
	return nullptr;
}

KernelResourceCounts kernel_resource_counts() noexcept
{
	std::lock_guard lock{ledger->mutex};
	std::size_t mapping_bytes = 0;
	for (const auto &[begin, end] : ledger->mappings)
		mapping_bytes += end - begin;
	return {
		.descriptors = ledger->descriptors.size(),
		.keys = ledger->keys.size(),
		.allocations = ledger->allocations.size(),
		.allocation_bytes = ledger->allocation_bytes,
		.mappings = ledger->mappings.size(),
		.mapping_bytes = mapping_bytes,
		.sbrk_calls = ledger->sbrk_calls,
	};
}

KernelResourceCounts release_kernel_resources(std::uintptr_t window_begin,
	std::uintptr_t window_end) noexcept
{
	KernelResourceCounts released{};
	std::set<int> descriptors;
	std::vector<pthread_key_t> keys;
	std::unordered_map<void *, std::size_t> allocations;
	std::map<std::uintptr_t, std::uintptr_t> mappings;
	{
		std::lock_guard lock{ledger->mutex};
		released.sbrk_calls = ledger->sbrk_calls;
		released.allocation_bytes = ledger->allocation_bytes;
		descriptors.swap(ledger->descriptors);
		keys.swap(ledger->keys);
		allocations.swap(ledger->allocations);
		mappings.swap(ledger->mappings);
		ledger->allocation_bytes = 0;
		ledger->sbrk_calls = 0;
	}
	for (const int descriptor : descriptors)
		(void)darwin_uml_close(descriptor);
	for (const pthread_key_t key : keys)
		(void)pthread_key_delete(key);
	for (const auto &[pointer, size] : allocations)
		std::free(pointer);
	const auto unmap = [&released](std::uintptr_t begin, std::uintptr_t end) {
		if (begin >= end)
			return;
		(void)munmap(reinterpret_cast<void *>(begin), end - begin);
		++released.mappings;
		released.mapping_bytes += end - begin;
	};
	for (const auto &[begin, end] : mappings) {
		if (end <= window_begin || begin >= window_end) {
			unmap(begin, end);
			continue;
		}
		unmap(begin, window_begin);
		unmap(window_end, end);
	}
	released.descriptors = descriptors.size();
	released.keys = keys.size();
	released.allocations = allocations.size();
	return released;
}

} // namespace uml::runtime
