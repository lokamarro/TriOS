// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * The guest file registry under the iOS family's disk-lock policy, built for a
 * macOS host: the kernel's whole-file locks are the registry's alone. iOS ends
 * a process it suspends while the process holds a file lock, and the guest is
 * not always suspended by then: it may refuse the suspend, outlast its
 * deadline or not have a console yet. So no step of a guest's life, running,
 * released for a suspension or restored after it, may leave a lock on a host
 * file, while the kernel is still answered as if it held one.
 */

#include "guest_files.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "wait_until.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using uml::testing::require;

const std::filesystem::path &scratchDirectory()
{
	static const uml::testing::ScratchSubdirectory directory {
		"uml-guest-file-process-locks.XXXXXX"};
	return directory.path();
}

std::filesystem::path scratchPath(const char *name)
{
	return scratchDirectory() / name;
}

int openScratch(const char *name, int flags)
{
	const int descriptor = ::open(scratchPath(name).c_str(), flags | O_CREAT,
		0600);
	require(descriptor >= 0);
	return descriptor;
}

/* Whether this process holds any lock on a file. A lock of the process's own
 * never blocks it, but it does block an open file description lock asked for
 * through a descriptor of its own, which sees POSIX and flock locks alike. The
 * probe keeps its descriptor for its whole life: closing any descriptor of a
 * file ends every POSIX lock the process holds on it, so a probe that opened
 * and closed the file for each look would remove the lock it looks for. */
class LockProbe final {
public:
	explicit LockProbe(const std::filesystem::path &path)
		: descriptor_ {::open(path.c_str(), O_RDONLY)}
	{
		require(descriptor_ >= 0, "open the lock probe");
	}

	LockProbe(const LockProbe &) = delete;
	LockProbe &operator=(const LockProbe &) = delete;

	~LockProbe()
	{
		(void)::close(descriptor_);
	}

	[[nodiscard]] bool locked() const
	{
		struct flock query {};
		query.l_type = F_WRLCK;
		query.l_whence = SEEK_SET;
		require(::fcntl(descriptor_, F_OFD_GETLK, &query) == 0,
			"query the file's locks");
		return query.l_type != F_UNLCK;
	}

private:
	int descriptor_;
};

/* The probe sees a lock this process takes on the host file, and does not
 * remove it by looking. */
void theProbeSeesAHostLock()
{
	const int disk = openScratch("probe-check", O_RDWR);
	{
		const LockProbe probe {scratchPath("probe-check")};
		require(!probe.locked());
		struct flock lock {};
		lock.l_type = F_RDLCK;
		lock.l_whence = SEEK_SET;
		require(::fcntl(disk, F_SETLK, &lock) == 0);
		require(probe.locked(),
			"the probe must see the process's own read lock");
		require(probe.locked(), "looking must not remove the lock");
	}
	(void)::close(disk);
}

void waitForChild(pid_t child)
{
	int status = 0;
	require(uml::testing::waitUntil([&] {
		return ::waitpid(child, &status, WNOHANG) == child;
	}, std::chrono::seconds{5}), "the locking process did not exit");
	require(WIFEXITED(status), "the locking process failed");
	require(WEXITSTATUS(status) == 0,
		"another process could not lock a guest disk");
}

/* What a second process finds: it can lock the disk itself. */
void anotherProcessCanLock(const std::filesystem::path &path)
{
	const pid_t child = ::fork();
	require(child >= 0);
	if (child == 0) {
		const int descriptor = ::open(path.c_str(), O_RDWR);
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		::_exit(descriptor >= 0 && ::fcntl(descriptor, F_SETLK, &lock) == 0 ?
			0 : 1);
	}
	waitForChild(child);
}

int lockDisk(int descriptor, short type)
{
	struct flock lock {};
	lock.l_type = type;
	lock.l_whence = SEEK_SET;
	return uml::platform::set_guest_file_lock(descriptor, F_SETLK, lock);
}

/* The kernel's lock is granted, and neither this process nor another one
 * finds it on the file. */
void aGuestDiskLockLocksNoHostFile()
{
	(void)uml::platform::forget_guest_files();
	const int data = openScratch("data.img", O_RDWR);
	const int system = openScratch("system.img", O_RDONLY);
	const LockProbe dataProbe {scratchPath("data.img")};
	const LockProbe systemProbe {scratchPath("system.img")};
	require(lockDisk(data, F_WRLCK) == 0, "the kernel locks its data disk");
	require(lockDisk(system, F_RDLCK) == 0, "the kernel locks its system disk");
	require(!dataProbe.locked() && !systemProbe.locked(),
		"a running guest's disks must carry no host lock");
	anotherProcessCanLock(scratchPath("data.img"));
	require(lockDisk(data, F_UNLCK) == 0, "the kernel unlocks its disk");
	(void)::close(data);
	(void)::close(system);
	(void)uml::platform::forget_guest_files();
}

/* Release and restore keep their bookkeeping: a released guest acquires no new
 * lock until the restore, and no step leaves a host lock. */
void suspensionKeepsTheBookkeepingAndNoHostLock()
{
	(void)uml::platform::forget_guest_files();
	const int disk = openScratch("cycled.img", O_RDWR);
	const LockProbe probe {scratchPath("cycled.img")};
	require(lockDisk(disk, F_WRLCK) == 0);
	for (int cycle = 0; cycle < 2; ++cycle) {
		require(uml::platform::release_guest_file_locks() == 0,
			"every lock is released");
		require(!probe.locked());
		errno = 0;
		require(lockDisk(disk, F_WRLCK) == -1 && errno == EAGAIN,
			"a suspended guest acquires no new lock");
		require(uml::platform::restore_guest_file_locks(),
			"an unchanged disk resumes");
		require(!probe.locked(),
			"a restored guest's disk must carry no host lock");
		require(lockDisk(disk, F_WRLCK) == 0,
			"a resumed guest locks its disks again");
	}
	require(uml::platform::release_guest_file_locks(true) == 0);
	require(uml::platform::restore_guest_file_locks());
	require(lockDisk(disk, F_WRLCK) == 0,
		"an ended run leaves nothing suspended");
	require(!probe.locked());
	(void)::close(disk);
	(void)uml::platform::forget_guest_files();
}

/* The restore still refuses a disk that changed while the guest was away, or a
 * descriptor that names another file now. */
void theRestoreStillChecksTheDisks()
{
	(void)uml::platform::forget_guest_files();
	const int changed = openScratch("changed.img", O_RDWR);
	const LockProbe probe {scratchPath("changed.img")};
	require(::pwrite(changed, "before!", 7, 0) == 7);
	require(lockDisk(changed, F_WRLCK) == 0);
	require(uml::platform::release_guest_file_locks() == 0);
	require(::pwrite(changed, "changed", 7, 0) == 7);
	errno = 0;
	require(!uml::platform::restore_guest_file_locks() && errno == ESTALE,
		"a disk modified while the guest was away must never resume");
	require(!probe.locked());
	(void)::close(changed);
	(void)uml::platform::forget_guest_files();

	const int first = openScratch("released.img", O_RDWR);
	require(lockDisk(first, F_WRLCK) == 0);
	require(uml::platform::release_guest_file_locks() == 0);
	(void)::close(first);
	const int second = openScratch("reused.img", O_RDWR);
	require(first == second);
	errno = 0;
	require(!uml::platform::restore_guest_file_locks() && errno == ESTALE,
		"a reused descriptor is not the suspended disk");
	(void)::close(second);
	(void)uml::platform::forget_guest_files();
}

/* Only the non-blocking whole-file lock is a lock request, under either
 * policy. */
void onlyTheNonBlockingWholeFileLockIsAccepted()
{
	(void)uml::platform::forget_guest_files();
	const int disk = openScratch("requests.img", O_RDWR);
	const LockProbe probe {scratchPath("requests.img")};
	struct flock lock {};
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	errno = 0;
	require(uml::platform::set_guest_file_lock(disk, F_SETLKW, lock) == -1 &&
		errno == EINVAL, "a waiting lock request must be refused");
	lock.l_len = 1;
	errno = 0;
	require(uml::platform::set_guest_file_lock(disk, F_SETLK, lock) == -1 &&
		errno == EINVAL, "a byte range must be refused");
	require(!probe.locked());
	(void)::close(disk);
	(void)uml::platform::forget_guest_files();
}

} // namespace

int main()
{
	theProbeSeesAHostLock();
	aGuestDiskLockLocksNoHostFile();
	suspensionKeepsTheBookkeepingAndNoHostLock();
	theRestoreStillChecksTheDisks();
	onlyTheNonBlockingWholeFileLockIsAccepted();
	std::puts("guest file process lock tests passed");
	return 0;
}
