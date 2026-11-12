// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_files.hpp"

#include <TargetConditionals.h>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace uml::platform {

namespace {

/* Whether a disk lock the kernel takes is also taken on the host file. The iOS
 * family (device, Simulator, Designed for iPad) keeps it in this registry
 * alone, because iOS ends a process it suspends while the process holds a file
 * lock, and a guest that refuses or outlasts its suspend is left running with
 * its disks open. The kernel is answered as before: a POSIX lock never
 * conflicts with its own process. A macOS host locks the file, which keeps a
 * second process off a disk in use, with an open file description lock: the
 * lock then belongs to the description the kernel locked, and closing another
 * descriptor of the disk (the kernel opens one to read its size) does not end
 * it. */
#if TARGET_OS_IPHONE || UML_GUEST_FILES_PROCESS_LOCKS
constexpr bool hostDiskLocks = false;
#else
constexpr bool hostDiskLocks = true;
#endif

struct Record final {
	dev_t device;
	ino_t inode;
	bool recorded;
};

std::mutex registryMutex;
std::vector<Record> registry;

struct LockRecord final {
	struct stat identity {};
	struct flock lock {};
	bool recorded{};
	/* Unlocked by a release, and to be locked again by a restore. A lock
	 * whose release failed was never unlocked and stays false. */
	bool released{};
	/* Locked again by the restore in progress, which a failed restore
	 * unlocks and nothing else. */
	bool reacquired{};
};

std::vector<LockRecord> diskLocks;
bool locksSuspended{};

[[nodiscard]] bool sameFile(const struct stat &a, const struct stat &b) noexcept
{
	return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

[[nodiscard]] bool unchanged(const struct stat &a, const struct stat &b) noexcept
{
	return sameFile(a, b) && a.st_size == b.st_size &&
		a.st_nlink == b.st_nlink &&
		a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
		a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
		a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
		a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
}

[[nodiscard]] int lockHostFile(int descriptor, struct flock lock) noexcept
{
	if (!hostDiskLocks)
		return 0;
	/* An open file description lock has no owning process: F_OFD_SETLK
	 * fails unless l_pid is zero. */
	lock.l_pid = 0;
	return ::fcntl(descriptor, F_OFD_SETLK, &lock);
}

[[nodiscard]] int unlock(int descriptor) noexcept
{
	struct flock lock {};
	lock.l_type = F_UNLCK;
	lock.l_whence = SEEK_SET;
	return lockHostFile(descriptor, lock);
}

/* A failed restore gives back the locks it took and leaves the others as they
 * were: a lock whose release failed is still the guest's. */
void undoRestoredLocks() noexcept
{
	for (std::size_t index = 0; index < diskLocks.size(); ++index) {
		auto &record = diskLocks[index];
		if (record.reacquired) {
			(void)unlock(static_cast<int>(index));
			record.released = true;
			record.reacquired = false;
		}
	}
}

[[nodiscard]] bool flushDescriptor(int descriptor) noexcept
{
	/* fsync alone leaves the write in the device's own cache on Darwin. The
	 * caller asked for durability across a host that may not come back, so
	 * ask for the barrier first and accept the weaker flush only where the
	 * filesystem cannot provide one. */
	int result;
	do {
		result = ::fcntl(descriptor, F_FULLFSYNC);
	} while (result < 0 && errno == EINTR);
	if (result >= 0)
		return true;
	do {
		result = ::fsync(descriptor);
	} while (result < 0 && errno == EINTR);
	return result >= 0;
}

[[nodiscard]] bool stillNames(int descriptor, const Record &record) noexcept
{
	struct stat details {};
	return ::fstat(descriptor, &details) == 0 &&
		details.st_dev == record.device &&
		details.st_ino == record.inode;
}

} // namespace

void record_guest_file(int descriptor, int openFlags) noexcept
{
	if (descriptor < 0 || (openFlags & O_ACCMODE) == O_RDONLY)
		return;
	struct stat details {};
	if (::fstat(descriptor, &details) != 0)
		return;
	const auto index = static_cast<std::size_t>(descriptor);
	try {
		std::lock_guard lock(registryMutex);
		if (registry.size() <= index)
			registry.resize(index + 1U, Record {});
		registry[index] = {details.st_dev, details.st_ino, true};
	} catch (...) {
		/* An unrecorded descriptor is flushed by the guest's own writeback
		 * instead of by a host request; losing the record must not fail the
		 * open that the guest is performing. */
	}
}

std::size_t forget_guest_files() noexcept
{
	std::lock_guard lock(registryMutex);
	std::size_t forgotten = 0;
	for (const Record &record : registry)
		forgotten += record.recorded ? 1U : 0U;
	/* The storage goes with the records: a descriptor table that once grew
	 * large must not keep its vector. */
	std::vector<Record>().swap(registry);
	std::vector<LockRecord>().swap(diskLocks);
	locksSuspended = false;
	return forgotten;
}

int set_guest_file_lock(int descriptor, int command,
	const struct flock &requested) noexcept
{
	/* UML's os_lock_file owns an entire disk, never a byte range, and takes
	 * it without waiting. Refuse anything this registry could not restore
	 * faithfully, and a wait it would spend holding the mutex that every
	 * flush and every quiesce needs. */
	if (command != F_SETLK) {
		errno = EINVAL;
		return -1;
	}
	if (requested.l_whence != SEEK_SET || requested.l_start != 0 ||
	    requested.l_len != 0 || (requested.l_type != F_RDLCK &&
	    requested.l_type != F_WRLCK && requested.l_type != F_UNLCK)) {
		errno = EINVAL;
		return -1;
	}
	std::lock_guard guard(registryMutex);
	if (locksSuspended && requested.l_type != F_UNLCK) {
		errno = EAGAIN;
		return -1;
	}
	struct stat identity {};
	if (::fstat(descriptor, &identity) != 0)
		return -1;
	const auto index = static_cast<std::size_t>(descriptor);
	try {
		if (diskLocks.size() <= index)
			diskLocks.resize(index + 1U);
	} catch (...) {
		errno = ENOMEM;
		return -1;
	}
	if (lockHostFile(descriptor, requested) != 0)
		return -1;
	diskLocks[index] = {identity, requested, requested.l_type != F_UNLCK,
		false};
	return 0;
}

bool guest_file_registry_idle() noexcept
{
	if (!registryMutex.try_lock())
		return false;
	registryMutex.unlock();
	return true;
}

unsigned release_guest_file_locks(bool retiring) noexcept
{
	std::lock_guard guard(registryMutex);
	locksSuspended = true;
	unsigned failed = 0;
	for (std::size_t index = 0; index < diskLocks.size(); ++index) {
		auto &record = diskLocks[index];
		if (!record.recorded || record.released)
			continue;
		const auto descriptor = static_cast<int>(index);
		struct stat identity {};
		if (::fstat(descriptor, &identity) != 0 ||
		    !sameFile(identity, record.identity)) {
			record = {};
			continue;
		}
		if (unlock(descriptor) != 0) {
			++failed;
			continue;
		}
		record.identity = identity;
		record.released = true;
	}
	if (retiring && failed == 0) {
		diskLocks.clear();
		locksSuspended = false;
	}
	return failed;
}

bool restore_guest_file_locks() noexcept
{
	std::lock_guard guard(registryMutex);
	if (!locksSuspended)
		return true;
	int error = 0;
	for (std::size_t index = 0; index < diskLocks.size(); ++index) {
		auto &record = diskLocks[index];
		if (!record.recorded || !record.released)
			continue;
		const auto descriptor = static_cast<int>(index);
		struct stat identity {};
		if (::fstat(descriptor, &identity) != 0 ||
		    !sameFile(identity, record.identity)) {
			error = ESTALE;
			break;
		}
		if (lockHostFile(descriptor, record.lock) != 0) {
			error = errno;
			break;
		}
		record.released = false;
		record.reacquired = true;
		if (::fstat(descriptor, &identity) != 0 ||
		    !unchanged(identity, record.identity)) {
			error = ESTALE;
			break;
		}
	}
	if (error != 0) {
		undoRestoredLocks();
		errno = error;
		return false;
	}
	for (auto &record : diskLocks)
		record.reacquired = false;
	locksSuspended = false;
	return true;
}

GuestFileSyncResult sync_guest_files() noexcept
{
	GuestFileSyncResult result {};
	std::lock_guard lock(registryMutex);
	for (std::size_t index = 0; index < registry.size(); ++index) {
		Record &record = registry[index];
		if (!record.recorded)
			continue;
		const auto descriptor = static_cast<int>(index);
		if (!stillNames(descriptor, record)) {
			record = Record {};
			++result.retired;
			continue;
		}
		if (flushDescriptor(descriptor))
			++result.flushed;
		else
			++result.failed;
	}
	return result;
}

} // namespace uml::platform
