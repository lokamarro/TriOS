// SPDX-License-Identifier: GPL-2.0-or-later
#include "shared_backing.hpp"

#include "guest_ram_commitment.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mach/mach.h>
#include <mutex>
#include <new>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace uml::memory {

/* Mach reports an exhausted host with KERN_RESOURCE_SHORTAGE and an exhausted
 * address space with KERN_NO_SPACE. Both are the host running out, and the
 * kernel distinguishes that case by ENOMEM alone: it is what makes it say so
 * rather than treat the failure as a bad address it can only retry.
 */
int mach_failure_errno(int result) noexcept
{
	switch (result) {
	case KERN_NO_SPACE:
	case KERN_RESOURCE_SHORTAGE:
		return ENOMEM;
	default:
		return EFAULT;
	}
}

namespace {
std::atomic<unsigned> mapping_failure_reports{0};

/* wire_host_range() calls mlock() at most this often. */
constexpr unsigned wire_attempts = 2;
} // namespace

void report_mapping_failure(const char *what, const void *address,
	std::size_t length, int error) noexcept
{
	constexpr unsigned reported_limit = 16;
	const unsigned seen =
		mapping_failure_reports.fetch_add(1, std::memory_order_relaxed);
	if (seen >= reported_limit)
		return;
	dprintf(STDERR_FILENO,
		"Darwin guest memory: %s failed for %zu bytes at %p, errno=%d%s\n",
		what, length, address, error,
		seen + 1 == reported_limit ? "; further failures are not reported"
					   : "");
}

int wire_host_range(void *address, std::size_t length) noexcept
{
	/* Every signal that only arrives asynchronously. A fault signal is
	 * never held: a thread that faults with it blocked re-executes the
	 * faulting instruction forever. */
	sigset_t held;
	sigfillset(&held);
	for (const int synchronous : {SIGILL, SIGTRAP, SIGABRT, SIGEMT, SIGFPE,
		     SIGBUS, SIGSEGV, SIGSYS})
		sigdelset(&held, synchronous);
	sigset_t previous;
	const bool masked = pthread_sigmask(SIG_BLOCK, &held, &previous) == 0;
	/* A signal can still abort the first attempt. XNU's psignal reads this
	 * thread's mask under the process lock and, if the signal is not held,
	 * sets the thread's abort flag a moment later; pthread_sigmask() changes
	 * the mask without that lock. A post that read the old mask can so set
	 * the flag after the hold took effect, inside the wire, whose next
	 * abortable wait then fails. Posts are serialized by that lock, so one
	 * is in flight at most. The aborted mlock unwires what it had wired and
	 * fails with ENOMEM. The flag can outlive it, and an mlock that starts
	 * with the flag set fails at once; any abortable wait takes the flag,
	 * and a sleep of the shortest length is one. So the second attempt
	 * follows one, and starts clean and finds the pages the first read in.
	 * Only ENOMEM is tried again, and once: a shortage, which mlock reports
	 * as EAGAIN, any other failure and a persistent ENOMEM fail as they
	 * did. */
	int result = -1;
	int error = 0;
	for (unsigned attempt = 0; attempt < wire_attempts; ++attempt) {
		if (attempt != 0)
			(void)usleep(1);
		result = mlock(address, length);
		error = errno;
		if (result == 0 || error != ENOMEM)
			break;
	}
	if (masked)
		(void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	errno = error;
	return result;
}

namespace {

struct AnonymousSharedBacking final {
	std::mutex mutex;
	dev_t device{};
	ino_t inode{};
	void *root{};
	std::size_t size{};
	bool registered{};
	/* Views of the root that borrow its mapping rather than owning one. */
	std::size_t borrowed_views{};
	/* Host pages of the root whose retirement waits for the last borrowed
	 * view, one bit per host page. */
	std::vector<std::uint64_t> retained;
	std::size_t retained_pages{};
};

/* Never destroyed. Carriers map through it until their process ends, and a
 * host application can call exit() while they still run. It stays constant-
 * initialized, so a mapping made during another object's initialization still
 * finds it. */
[[clang::no_destroy]] constinit AnonymousSharedBacking anonymous_backing;

[[nodiscard]] bool root_page_index_locked(std::uintptr_t address,
	std::size_t &index) noexcept
{
	const auto root = reinterpret_cast<std::uintptr_t>(anonymous_backing.root);
	if (anonymous_backing.root == nullptr || address < root ||
	    address - root >= anonymous_backing.size)
		return false;
	index = (address - root) / vm_page_size;
	return true;
}

/* A new mapping over a retained page supersedes the retirement that retained
 * it; the page is no longer the retired domain's to hand back. */
void forget_retained_locked(const void *address, std::size_t length) noexcept
{
	if (anonymous_backing.retained_pages == 0 || length == 0)
		return;
	const auto begin = reinterpret_cast<std::uintptr_t>(address);
	for (std::size_t offset = 0; offset < length; offset += vm_page_size) {
		std::size_t index;
		if (!root_page_index_locked(begin + offset, index))
			continue;
		auto &word = anonymous_backing.retained[index / 64];
		const auto bit = std::uint64_t{1} << (index % 64);
		if ((word & bit) == 0)
			continue;
		word &= ~bit;
		--anonymous_backing.retained_pages;
	}
}

/* Hand every retained page back to the window's reservation, as the
 * retirement which retained it would have. */
void release_retained_locked() noexcept
{
	const auto root = reinterpret_cast<std::uintptr_t>(anonymous_backing.root);
	const std::size_t pages = anonymous_backing.retained.size() * 64;
	std::size_t index = 0;
	while (anonymous_backing.retained_pages != 0 && index < pages) {
		const auto retained = [](std::size_t page) noexcept {
			return (anonymous_backing.retained[page / 64] &
				(std::uint64_t{1} << (page % 64))) != 0;
		};
		if (!retained(index)) {
			++index;
			continue;
		}
		const auto first = index;
		while (index < pages && retained(index)) {
			anonymous_backing.retained[index / 64] &=
				~(std::uint64_t{1} << (index % 64));
			--anonymous_backing.retained_pages;
			++index;
		}
		auto *address = reinterpret_cast<void *>(root + first * vm_page_size);
		const std::size_t length = (index - first) * vm_page_size;
		if (reserve_host_range(address, length) != 0)
			report_mapping_failure("retained guest RAM release", address,
				length, errno);
	}
	anonymous_backing.retained.clear();
	anonymous_backing.retained.shrink_to_fit();
	anonymous_backing.retained_pages = 0;
}

struct AnonymousTail final {
	void *address{};
	std::size_t length{};
};

[[nodiscard]] bool same_backing(const struct stat &status) noexcept
{
	return anonymous_backing.registered &&
		status.st_dev == anonymous_backing.device &&
		status.st_ino == anonymous_backing.inode;
}

[[nodiscard]] bool validate_anonymous_range_locked(
	const struct stat &status, std::uint64_t offset,
	std::size_t length) noexcept
{
	if (!same_backing(status)) {
		errno = EBADF;
		return false;
	}
	if (status.st_size <= 0 ||
	    static_cast<std::uintmax_t>(status.st_size) > SIZE_MAX ||
	    length == 0 || offset % vm_page_size != 0 ||
	    length % vm_page_size != 0) {
		errno = EINVAL;
		return false;
	}
	const auto backing_size = static_cast<std::size_t>(status.st_size);
	if (offset > backing_size || length > backing_size - offset) {
		errno = EINVAL;
		return false;
	}
	if (anonymous_backing.root != nullptr &&
	    anonymous_backing.size != backing_size) {
		errno = EINVAL;
		return false;
	}
	return true;
}

/* The physical window begins with the signed kernel image, which the loader
 * maps through its final ELF segment. UML rounds its reserved physical prefix
 * farther, so one tail lies between the image and the allocatable suffix.
 * Where the host reserved the guest window (iOS), that tail is still part of
 * the reservation and is mapped over it in place; elsewhere it is unmapped and
 * is placed by a hint that must be honoured. Nothing else may lie in it. */
[[nodiscard]] bool materialize_anonymous_tail_locked(
	std::uintptr_t root, std::uintptr_t limit,
	AnonymousTail &tail) noexcept
{
	vm_address_t cursor = root;
	bool reserved = false;
	while (cursor < limit) {
		vm_address_t region = cursor;
		vm_size_t region_size = 0;
		vm_region_basic_info_data_64_t information {};
		mach_msg_type_number_t information_count =
			VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		const kern_return_t result = vm_region_64(
			mach_task_self(), &region, &region_size,
			VM_REGION_BASIC_INFO_64,
			reinterpret_cast<vm_region_info_t>(&information),
			&information_count, &object);
		if (object != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), object);
		if (result == KERN_INVALID_ADDRESS || region >= limit)
			break;
		if (result != KERN_SUCCESS || region > cursor || region_size == 0 ||
		    region > std::numeric_limits<vm_address_t>::max() - region_size ||
		    region + region_size <= cursor) {
			errno = mach_failure_errno(result);
			report_mapping_failure("guest RAM tail inspection",
				reinterpret_cast<const void *>(cursor),
				limit - cursor, errno);
			return false;
		}
		/* The window's reservation is protectionless and runs on past the
		 * suffix. A protectionless alignment gap inside the image does not:
		 * the image resumes after it, and its last page always belongs to a
		 * segment. */
		if (information.protection == VM_PROT_NONE &&
		    region + region_size >= limit) {
			reserved = true;
			break;
		}
		cursor = region + region_size;
	}
	if (cursor >= limit)
		return true;

	const auto length = static_cast<std::size_t>(limit - cursor);
	void *mapping = mmap(reinterpret_cast<void *>(cursor), length,
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON | (reserved ? MAP_FIXED : 0), -1, 0);
	if (mapping == MAP_FAILED)
		return false;
	if (mapping != reinterpret_cast<void *>(cursor)) {
		(void)munmap(mapping, length);
		errno = ENOMEM;
		return false;
	}
	tail = {mapping, length};
	return true;
}

[[nodiscard]] void *establish_anonymous_root_locked(
	void *address, std::size_t length, std::uint64_t offset,
	const struct stat &status) noexcept
{
	if (!validate_anonymous_range_locked(status, offset, length))
		return MAP_FAILED;
	const auto backing_size = static_cast<std::size_t>(status.st_size);
	const auto target = reinterpret_cast<std::uintptr_t>(address);
	if (address == nullptr || target % vm_page_size != 0 || offset > target ||
	    length != backing_size - static_cast<std::size_t>(offset)) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	const auto physical_root = target - static_cast<std::size_t>(offset);
	AnonymousTail tail;
	if (!materialize_anonymous_tail_locked(physical_root, target, tail))
		return MAP_FAILED;

	/* UML's physical window begins with the signed kernel image. Only the
	 * allocatable suffix is created from the physical-memory descriptor. Make
	 * that first fixed suffix mapping the anonymous allocation itself and use
	 * address - offset as the linear physical base. A separate anywhere
	 * allocation would leave iOS accounting both full-span resident mappings. */
	void *root = mmap(address, length, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
	if (root == MAP_FAILED) {
		if (tail.address != nullptr)
			(void)reserve_host_range(tail.address, tail.length);
		return MAP_FAILED;
	}
#if UML_WIRE_ANONYMOUS_GUEST_RAM
	/* Guest RAM is wired so that it is never compressed, but only as the
	 * guest commits it; see guest_ram_commitment.hpp. The reserved tail sits
	 * below everything the guest allocates and is wired now. */
	if (tail.address != nullptr &&
	    wire_host_range(tail.address, tail.length) != 0) {
		const int saved_errno = errno;
		dprintf(STDERR_FILENO,
			"Darwin anonymous guest RAM: failed to wire the %zu-byte "
			"reserved tail, errno=%d\n",
			tail.length, saved_errno);
		(void)reserve_host_range(root, length);
		(void)reserve_host_range(tail.address, tail.length);
		errno = saved_errno;
		return MAP_FAILED;
	}
	/* Give the whole suffix one VM object before any of it is wired. Wiring
	 * a chunk of object-less memory makes XNU create a private object for
	 * that chunk alone, and it never treats such an object as reusable, so a
	 * released chunk would stay in the footprint. A shared alias forces the
	 * single object into existence, and the object outlives the alias. */
	vm_address_t alias = 0;
	vm_prot_t alias_current = VM_PROT_NONE;
	vm_prot_t alias_maximum = VM_PROT_NONE;
	const kern_return_t alias_result = vm_remap(mach_task_self(), &alias,
		length, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
		reinterpret_cast<vm_address_t>(root), FALSE, &alias_current,
		&alias_maximum, VM_INHERIT_NONE);
	if (alias_result == KERN_SUCCESS)
		(void)vm_deallocate(mach_task_self(), alias, length);
	else
		dprintf(STDERR_FILENO,
			"Darwin anonymous guest RAM: failed to give the suffix one "
			"object, kr=%d; released pages may stay in the footprint\n",
			alias_result);
	constexpr bool wire_guest_ram = true;
#else
	constexpr bool wire_guest_ram = false;
#endif
	dprintf(STDERR_FILENO,
		"Darwin anonymous guest RAM: reserved %zu suffix bytes plus "
		"%zu reserved-tail bytes in the %zu-byte physical window; "
		"committed on demand%s; direct-root=yes offset=%llu\n",
		length, tail.length, backing_size,
		wire_guest_ram ? " and wired" : "",
		static_cast<unsigned long long>(offset));
	const std::size_t managed_begin = tail.address != nullptr ?
		reinterpret_cast<std::uintptr_t>(tail.address) - physical_root :
		static_cast<std::size_t>(offset);
	/* A wired host starts the suffix released so that the guest's first use
	 * wires it; an unwired one has nothing to do until the guest releases. */
	publish_guest_ram_commitment(std::make_unique<GuestRamCommitment>(
		reinterpret_cast<std::byte *>(physical_root), backing_size,
		managed_begin,
		wire_guest_ram ? static_cast<std::size_t>(offset) : backing_size,
		wire_guest_ram, vm_page_size));
	anonymous_backing.root = reinterpret_cast<void *>(physical_root);
	anonymous_backing.size = backing_size;
	return root;
}

/* Undo a half-built alias. Every MAP_FIXED caller of this file maps inside the
 * reserved guest window, so a fixed range is handed back to the window's
 * reservation rather than to the process; an anywhere alias is this call's own
 * allocation and is released outright. */
int release_alias(void *address, std::size_t length, bool fixed) noexcept
{
	return fixed ? reserve_host_range(address, length) :
		munmap(address, length);
}

[[nodiscard]] void *map_anonymous_alias(void *address, std::size_t length,
	int protection, int flags, std::uint64_t offset,
	const struct stat &status) noexcept
{
	std::lock_guard lock{anonymous_backing.mutex};
	const bool fixed = (flags & MAP_FIXED) != 0;
	if (!validate_anonymous_range_locked(status, offset, length))
		return MAP_FAILED;
	if (anonymous_backing.root == nullptr) {
		if (!fixed || protection != (PROT_READ | PROT_WRITE)) {
			errno = EAGAIN;
			return MAP_FAILED;
		}
		return establish_anonymous_root_locked(
			address, length, offset, status);
	}

	if (fixed)
		forget_retained_locked(address, length);
	vm_address_t target = fixed ? reinterpret_cast<vm_address_t>(address) : 0;
	const vm_address_t source =
		reinterpret_cast<vm_address_t>(anonymous_backing.root) + offset;
	if (fixed && target == source) {
		if (mprotect(address, length, protection) != 0) {
			report_mapping_failure("guest RAM reprotect", address,
				length, errno);
			return MAP_FAILED;
		}
#if UML_WIRE_ANONYMOUS_GUEST_RAM
		/* Early UML releases its final reserved-prefix pages into guest RAM
		 * with an in-place writable mapping. Wire them at that transition so
		 * no newly allocatable physical page can enter host compression.
		 * Only the prefix: GuestRamCommitment wires the rest as the guest
		 * commits it, and Darwin counts wirings, so a page wired here as well
		 * would stay wired, and in the footprint, after the guest released
		 * it.
		 *
		 * The mapping itself is already in place and correct by this point,
		 * so a refused wiring is reported and kept rather than failed: the
		 * caller would answer a failure by dropping its record of a mapping
		 * the host still has, and a host mapping nothing owns outlives every
		 * page table that named it. Compression costs latency; a mapping the
		 * kernel has forgotten costs correctness.
		 */
		std::size_t unmanaged = length;
		if (const GuestRamCommitment *const commitment =
			    guest_ram_commitment()) {
			const std::size_t managed = commitment->managed_begin();
			unmanaged = offset < managed ?
				std::min<std::size_t>(length, managed - offset) : 0;
		}
		if (protection != PROT_NONE && unmanaged != 0 &&
		    wire_host_range(address, unmanaged) != 0)
			report_mapping_failure("guest RAM wiring", address,
				unmanaged, errno);
#endif
		return address;
	}
	vm_prot_t current = VM_PROT_NONE;
	vm_prot_t maximum = VM_PROT_NONE;
	const int mach_flags = fixed ? VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE :
		VM_FLAGS_ANYWHERE;
	const kern_return_t result = vm_remap(
		mach_task_self(), &target, length, 0, mach_flags,
		mach_task_self(), source, FALSE, &current, &maximum,
		VM_INHERIT_NONE);
	if (result != KERN_SUCCESS ||
	    (fixed && target != reinterpret_cast<vm_address_t>(address))) {
		if (result == KERN_SUCCESS)
			(void)release_alias(
				reinterpret_cast<void *>(target), length, fixed);
		errno = mach_failure_errno(result);
		report_mapping_failure("guest RAM alias", address, length, errno);
		return MAP_FAILED;
	}
	void *mapped = reinterpret_cast<void *>(target);
	if (mprotect(mapped, length, protection) != 0) {
		const int saved_errno = errno;
		(void)release_alias(mapped, length, fixed);
		errno = saved_errno;
		report_mapping_failure("guest RAM alias protect", address, length,
			saved_errno);
		return MAP_FAILED;
	}
	return mapped;
}

} // namespace

SharedBackingView::SharedBackingView(
	void *data, std::size_t size, bool owns_mapping) noexcept
	: data_(data), size_(size), owns_mapping_(owns_mapping)
{
}

SharedBackingView::SharedBackingView(SharedBackingView &&other) noexcept
	: data_(other.data_), size_(other.size_),
	  owns_mapping_(other.owns_mapping_)
{
	other.data_ = nullptr;
	other.size_ = 0;
	other.owns_mapping_ = false;
}

SharedBackingView &SharedBackingView::operator=(
	SharedBackingView &&other) noexcept
{
	if (this == &other)
		return *this;
	reset();
	data_ = other.data_;
	size_ = other.size_;
	owns_mapping_ = other.owns_mapping_;
	other.data_ = nullptr;
	other.size_ = 0;
	other.owns_mapping_ = false;
	return *this;
}

SharedBackingView::~SharedBackingView()
{
	reset();
}

void SharedBackingView::reset() noexcept
{
	if (owns_mapping_ && data_ != nullptr)
		(void)munmap(data_, size_);
	else if (data_ != nullptr) {
		std::lock_guard lock{anonymous_backing.mutex};
		if (anonymous_backing.borrowed_views != 0 &&
		    --anonymous_backing.borrowed_views == 0)
			release_retained_locked();
	}
	data_ = nullptr;
	size_ = 0;
	owns_mapping_ = false;
}

bool register_anonymous_shared_backing(int fd) noexcept
{
	struct stat status {};
	if (fd < 0 || fstat(fd, &status) != 0)
		return false;
	std::lock_guard lock{anonymous_backing.mutex};
	if (anonymous_backing.registered) {
		errno = EBUSY;
		return false;
	}
	anonymous_backing.device = status.st_dev;
	anonymous_backing.inode = status.st_ino;
	anonymous_backing.registered = true;
	dprintf(STDERR_FILENO,
		"Darwin anonymous guest RAM: registered sparse descriptor token "
		"fd=%d\n",
		fd);
	return true;
}

bool anonymous_shared_backing_registered() noexcept
{
	std::lock_guard lock{anonymous_backing.mutex};
	return anonymous_backing.registered;
}

bool guest_ram_borrowed() noexcept
{
	std::lock_guard lock{anonymous_backing.mutex};
	return anonymous_backing.borrowed_views != 0;
}

int hand_back_anonymous_guest_ram(void *window, std::size_t length) noexcept
{
	std::lock_guard lock{anonymous_backing.mutex};
	if (anonymous_backing.borrowed_views != 0) {
		errno = EBUSY;
		return -1;
	}
	/* The window is replaced whole, around the ranges the host keeps. A
	 * failure leaves the guest registered, so it can still be handed back
	 * later. */
	if (window != nullptr && reserve_host_range(window, length) != 0)
		return -1;
	retire_guest_ram_commitment();
	/* No view is borrowed, so no page was waiting for the last one. */
	anonymous_backing.retained.clear();
	anonymous_backing.retained.shrink_to_fit();
	anonymous_backing.retained_pages = 0;
	anonymous_backing.root = nullptr;
	anonymous_backing.size = 0;
	anonymous_backing.device = 0;
	anonymous_backing.inode = 0;
	anonymous_backing.registered = false;
	dprintf(STDERR_FILENO,
		"Darwin anonymous guest RAM: handed back to the process\n");
	return 0;
}

bool is_anonymous_shared_backing(int fd) noexcept
{
	struct stat status {};
	if (fd < 0 || fstat(fd, &status) != 0)
		return false;
	std::lock_guard lock{anonymous_backing.mutex};
	return same_backing(status);
}

AnonymousBackingMemory anonymous_shared_backing_memory(
	std::uint64_t device, std::uint64_t inode) noexcept
{
	std::lock_guard lock{anonymous_backing.mutex};
	if (!anonymous_backing.registered ||
	    static_cast<std::uint64_t>(anonymous_backing.device) != device ||
	    static_cast<std::uint64_t>(anonymous_backing.inode) != inode)
		return {};
	if (anonymous_backing.root == nullptr)
		return {true, {}};
	return {true, {static_cast<std::byte *>(anonymous_backing.root),
		anonymous_backing.size}};
}

SharedBackingView acquire_shared_backing_view(int fd,
	std::uint64_t offset, std::size_t length, int protection) noexcept
{
	struct stat status {};
	if (fd < 0 || fstat(fd, &status) != 0)
		return {};
	{
		std::lock_guard lock{anonymous_backing.mutex};
		if (same_backing(status) &&
		    protection == (PROT_READ | PROT_WRITE)) {
			if (!validate_anonymous_range_locked(status, offset, length))
				return {};
			if (anonymous_backing.root == nullptr) {
				errno = EAGAIN;
				return {};
			}
			auto *data = static_cast<std::byte *>(
				anonymous_backing.root) + offset;
			++anonymous_backing.borrowed_views;
			return SharedBackingView{data, length, false};
		}
	}

	void *mapping = map_shared_backing(nullptr, length, protection,
		MAP_SHARED, fd, offset);
	if (mapping == MAP_FAILED)
		return {};
	return SharedBackingView{mapping, length, true};
}

bool retain_borrowed_guest_ram(void *address, std::size_t length) noexcept
{
	if (length == 0)
		return false;
	std::lock_guard lock{anonymous_backing.mutex};
	std::size_t first;
	std::size_t last;
	const auto begin = reinterpret_cast<std::uintptr_t>(address);
	if (anonymous_backing.borrowed_views == 0 ||
	    begin % vm_page_size != 0 || length % vm_page_size != 0 ||
	    !root_page_index_locked(begin, first) ||
	    !root_page_index_locked(begin + length - 1, last))
		return false;
	if (anonymous_backing.retained.empty()) {
		const std::size_t pages =
			(anonymous_backing.size + vm_page_size - 1) / vm_page_size;
		try {
			anonymous_backing.retained.assign((pages + 63) / 64, 0);
		} catch (const std::bad_alloc &) {
			/* Without room to remember the page, retire it now. */
			return false;
		}
	}
	for (auto index = first; index <= last; ++index) {
		auto &word = anonymous_backing.retained[index / 64];
		const auto bit = std::uint64_t{1} << (index % 64);
		if ((word & bit) != 0)
			continue;
		word |= bit;
		++anonymous_backing.retained_pages;
	}
	return true;
}

namespace {

struct KeptHostRange final {
	std::uintptr_t begin{};
	std::uintptr_t end{};
};

/* The ranges the host keeps (keep_host_range). Every reservation reads them
 * and only the loader adds one, so an entry is written once, before the count
 * that publishes it, and never changes. A kernel image has a few executable
 * segments, and a process loads one image into one window at a time. */
struct KeptHostRanges final {
	std::mutex mutex;
	std::array<KeptHostRange, 32> ranges{};
	std::atomic<std::size_t> count{0};
};

[[clang::no_destroy]] constinit KeptHostRanges kept_host_ranges;

[[nodiscard]] int reserve_exact_host_range(
	std::uintptr_t begin, std::uintptr_t end) noexcept
{
	if (mmap(reinterpret_cast<void *>(begin), end - begin, PROT_NONE,
		    MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0) ==
	    MAP_FAILED)
		return -1;
	return 0;
}

} // namespace

int reserve_host_range(void *address, std::size_t length) noexcept
{
	if (length == 0)
		return 0;
	auto begin = reinterpret_cast<std::uintptr_t>(address);
	if (begin > std::numeric_limits<std::uintptr_t>::max() - length) {
		errno = EINVAL;
		return -1;
	}
	const std::uintptr_t end = begin + length;
	const std::size_t kept =
		kept_host_ranges.count.load(std::memory_order_acquire);
	/* Each part before the lowest kept range left in [begin, end), then the
	 * part after the last. XNU refuses a replacement that touches a
	 * permanent mapping whole, so the parts are reserved separately. */
	while (begin < end) {
		const KeptHostRange *next = nullptr;
		for (std::size_t index = 0; index < kept; ++index) {
			const auto &range = kept_host_ranges.ranges[index];
			if (range.begin < end && range.end > begin &&
			    (next == nullptr || range.begin < next->begin))
				next = &range;
		}
		const std::uintptr_t stop =
			next == nullptr ? end : std::max(next->begin, begin);
		if (stop > begin && reserve_exact_host_range(begin, stop) != 0)
			return -1;
		if (next == nullptr)
			break;
		begin = next->end;
	}
	return 0;
}

bool keep_host_range(void *address, std::size_t length) noexcept
{
	const auto begin = reinterpret_cast<std::uintptr_t>(address);
	if (length == 0 || begin % vm_page_size != 0 ||
	    length % vm_page_size != 0 ||
	    begin > std::numeric_limits<std::uintptr_t>::max() - length)
		return false;
	std::lock_guard lock{kept_host_ranges.mutex};
	const std::size_t count =
		kept_host_ranges.count.load(std::memory_order_relaxed);
	for (std::size_t index = 0; index < count; ++index) {
		const auto &range = kept_host_ranges.ranges[index];
		if (range.begin == begin && range.end == begin + length)
			return true;
	}
	if (count == kept_host_ranges.ranges.size())
		return false;
	kept_host_ranges.ranges[count] = {begin, begin + length};
	kept_host_ranges.count.store(count + 1, std::memory_order_release);
	return true;
}

bool host_range_kept(const void *address, std::size_t length) noexcept
{
	const auto begin = reinterpret_cast<std::uintptr_t>(address);
	const std::size_t count =
		kept_host_ranges.count.load(std::memory_order_acquire);
	for (std::size_t index = 0; index < count; ++index) {
		const auto &range = kept_host_ranges.ranges[index];
		if (range.begin == begin && range.end - range.begin == length)
			return true;
	}
	return false;
}

void *map_shared_backing(void *address, std::size_t length,
	int protection, int flags, int fd, std::uint64_t offset) noexcept
{
	struct stat status {};
	if (fstat(fd, &status) != 0)
		return MAP_FAILED;
	{
		std::lock_guard lock{anonymous_backing.mutex};
		if (!same_backing(status)) {
			if (offset > static_cast<std::uint64_t>(
					std::numeric_limits<off_t>::max())) {
				errno = EOVERFLOW;
				return MAP_FAILED;
			}
			if ((flags & MAP_FIXED) != 0)
				forget_retained_locked(address, length);
			return mmap(address, length, protection, flags, fd,
				static_cast<off_t>(offset));
		}
	}
	return map_anonymous_alias(
		address, length, protection, flags, offset, status);
}

} // namespace uml::memory
