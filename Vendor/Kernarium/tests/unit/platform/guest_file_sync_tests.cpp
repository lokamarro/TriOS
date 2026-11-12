// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_files.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "wait_until.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <atomic>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using uml::testing::require;

const std::filesystem::path &scratchDirectory()
{
	static const uml::testing::ScratchSubdirectory directory {
		"uml-guest-file-sync.XXXXXX"};
	return directory.path();
}

/* The registry is process-global, and each case here closes what it opened.
 * Draining first retires those stale records so a case reasons only about its
 * own descriptors. */
void drainRegistry()
{
	(void)uml::platform::sync_guest_files();
}

int openScratch(const char *name, int flags)
{
	const auto path = scratchDirectory() / name;
	const int descriptor = ::open(path.c_str(), flags | O_CREAT, 0600);
	require(descriptor >= 0);
	return descriptor;
}

void aWritableDescriptorIsFlushed()
{
	drainRegistry();
	const int descriptor = openScratch("writable", O_RDWR);
	uml::platform::record_guest_file(descriptor, O_RDWR);
	require(::write(descriptor, "guest", 5) == 5);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 1U);
	require(result.failed == 0U);
	require(::close(descriptor) == 0);
}

void aReadOnlyDescriptorIsNotRecorded()
{
	drainRegistry();
	const int descriptor = openScratch("read-only", O_RDONLY);
	uml::platform::record_guest_file(descriptor, O_RDONLY);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 0U);
	require(result.failed == 0U);
	require(::close(descriptor) == 0);
}

/* Closing needs no hook: the record remembers the file it described, so a
 * descriptor that no longer names it is retired instead of flushed. */
void aClosedDescriptorIsRetired()
{
	drainRegistry();
	const int descriptor = openScratch("closed", O_WRONLY);
	uml::platform::record_guest_file(descriptor, O_WRONLY);
	require(::close(descriptor) == 0);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 0U);
	require(result.failed == 0U);
	require(result.retired == 1U);
}

/* The host may reuse a descriptor number for something the guest never opened.
 * Flushing that would be wrong, so identity decides rather than the number. */
void aReusedDescriptorIsRetiredRatherThanFlushed()
{
	drainRegistry();
	const int first = openScratch("reused-first", O_RDWR);
	uml::platform::record_guest_file(first, O_RDWR);
	require(::close(first) == 0);
	const int second = openScratch("reused-second", O_RDWR);
	require(second == first);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 0U);
	require(result.failed == 0U);
	require(result.retired == 1U);
	require(::close(second) == 0);
}

void everyRecordedDescriptorIsFlushed()
{
	drainRegistry();
	const int first = openScratch("many-first", O_RDWR);
	const int second = openScratch("many-second", O_RDWR);
	uml::platform::record_guest_file(first, O_RDWR);
	uml::platform::record_guest_file(second, O_RDWR);
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 2U);
	require(result.failed == 0U);
	require(::close(first) == 0);
	require(::close(second) == 0);
}

/* A guest can open something that has no stable storage behind it. That must
 * be reported rather than counted as durable, and must not stop the files
 * beside it from reaching disk. */
void anUnflushableDescriptorIsReportedWithoutStoppingTheRest()
{
	drainRegistry();
	int pair[2] {-1, -1};
	require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	const int file = openScratch("beside-socket", O_RDWR);
	uml::platform::record_guest_file(pair[0], O_RDWR);
	uml::platform::record_guest_file(file, O_RDWR);
	const auto result = uml::platform::sync_guest_files();
	require(result.failed == 1U);
	require(result.flushed == 1U);
	require(::close(pair[0]) == 0);
	require(::close(pair[1]) == 0);
	require(::close(file) == 0);
}

/* A guest that restarts itself leaves no record to the next one: a record of
 * a descriptor that is still open is not flushed afterwards, and one of a
 * closed descriptor is not retired later, because there is none. */
void forgottenRecordsAreNeitherFlushedNorRetired()
{
	drainRegistry();
	const int open_file = openScratch("forgotten-open", O_RDWR);
	const int closed_file = openScratch("forgotten-closed", O_RDWR);
	uml::platform::record_guest_file(open_file, O_RDWR);
	uml::platform::record_guest_file(closed_file, O_RDWR);
	require(::close(closed_file) == 0);
	require(uml::platform::forget_guest_files() == 2U,
		"the records were not all forgotten");
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 0U && result.failed == 0U &&
		result.retired == 0U, "a forgotten record survived");
	require(uml::platform::forget_guest_files() == 0U,
		"forgetting twice found records");
	/* The next guest records its own. */
	uml::platform::record_guest_file(open_file, O_RDWR);
	require(uml::platform::sync_guest_files().flushed == 1U);
	require(::close(open_file) == 0);
	require(uml::platform::forget_guest_files() == 1U);
}

void anEmptyRegistryFlushesNothing()
{
	const auto result = uml::platform::sync_guest_files();
	require(result.flushed == 0U);
	require(result.failed == 0U);
}

void waitForChild(pid_t child)
{
	int status = 0;
	require(uml::testing::waitUntil([&] {
		return waitpid(child, &status, WNOHANG) == child;
	}, std::chrono::seconds{5}), "the lock observer did not exit");
	require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"the lock observer failed");
}

char receiveByte(int descriptor)
{
	pollfd ready {descriptor, POLLIN, 0};
	require(poll(&ready, 1, 5000) == 1, "the lock observer did not answer");
	char result = 0;
	require(read(descriptor, &result, 1) == 1, "read the lock observer");
	return result;
}

/* POSIX locks belong to a process, so another process must observe them. */
bool anotherProcessSeesALock(int descriptor)
{
	int result[2];
	require(pipe(result) == 0);
	const pid_t child = fork();
	require(child >= 0);
	if (child == 0) {
		close(result[0]);
		struct flock query {};
		query.l_type = F_WRLCK;
		query.l_whence = SEEK_SET;
		const char answer = fcntl(descriptor, F_GETLK, &query) == 0 ?
			(query.l_type == F_UNLCK ? 'n' : 'y') : 'e';
		_exit(write(result[1], &answer, 1) == 1 ? 0 : 1);
	}
	close(result[1]);
	const char answer = receiveByte(result[0]);
	close(result[0]);
	waitForChild(child);
	require(answer != 'e', "query the file lock");
	return answer == 'y';
}

void lockDisk(int descriptor, short type = F_WRLCK)
{
	struct flock lock {};
	lock.l_type = type;
	lock.l_whence = SEEK_SET;
	require(uml::platform::set_guest_file_lock(descriptor, F_SETLK, lock) == 0,
		"lock the guest disk");
}

void diskLocksLeaveBeforeSuspensionAndReturnBeforeResume()
{
	(void)uml::platform::forget_guest_files();
	const int disk = openScratch("locked-disk", O_RDWR);
	lockDisk(disk);
	require(anotherProcessSeesALock(disk), "the running disk must be locked");
	for (int cycle = 0; cycle < 2; ++cycle) {
		require(uml::platform::release_guest_file_locks() == 0);
		require(!anotherProcessSeesALock(disk),
			"the suspended guest must hold no file lock");
		struct flock requested {};
		requested.l_type = F_WRLCK;
		requested.l_whence = SEEK_SET;
		require(uml::platform::set_guest_file_lock(disk, F_SETLK, requested) == -1 &&
			errno == EAGAIN, "a suspended guest must acquire no new lock");
		require(uml::platform::restore_guest_file_locks());
		require(anotherProcessSeesALock(disk), "resume must restore the disk lock");
	}
	require(uml::platform::release_guest_file_locks(true) == 0);
	require(!anotherProcessSeesALock(disk), "a finished run must hold no lock");
	require(uml::platform::restore_guest_file_locks());
	require(!anotherProcessSeesALock(disk), "an ended run must not be relocked");
	close(disk);
}

/* The kernel opens a disk again to read its size or its identity, and closes
 * that descriptor while the guest runs. The lock belongs to the open file
 * description that took it, so no such close may end it; a POSIX lock ends with
 * the first close of any descriptor of the file in the process. */
void closingAnotherDescriptorOfALockedDiskKeepsTheLock()
{
	(void)uml::platform::forget_guest_files();
	const auto path = scratchDirectory() / "close-another";
	const int disk = openScratch("close-another", O_RDWR);
	lockDisk(disk);
	const auto openAndClose = [&] {
		const int other = ::open(path.c_str(), O_RDONLY);
		require(other >= 0);
		require(::close(other) == 0);
	};
	openAndClose();
	require(anotherProcessSeesALock(disk),
		"closing another descriptor released the disk lock");
	/* The lock the resume takes again is the description's, too. */
	require(uml::platform::release_guest_file_locks() == 0);
	require(uml::platform::restore_guest_file_locks());
	openAndClose();
	require(anotherProcessSeesALock(disk),
		"closing another descriptor released the restored disk lock");
	require(uml::platform::release_guest_file_locks(true) == 0);
	require(!anotherProcessSeesALock(disk));
	close(disk);
	(void)uml::platform::forget_guest_files();
}

/* Locks of one open file description conflict with every other description,
 * the process's own included. */
void aSecondDescriptionCannotLockALockedDisk()
{
	(void)uml::platform::forget_guest_files();
	const auto path = scratchDirectory() / "second-description";
	const int disk = openScratch("second-description", O_RDWR);
	lockDisk(disk);
	const int second = ::open(path.c_str(), O_RDWR);
	require(second >= 0);
	for (const short type : {F_WRLCK, F_RDLCK}) {
		struct flock requested {};
		requested.l_type = type;
		requested.l_whence = SEEK_SET;
		errno = 0;
		require(uml::platform::set_guest_file_lock(second, F_SETLK, requested) == -1 &&
			(errno == EAGAIN || errno == EACCES),
			"a second description locked a locked disk");
	}
	/* The refusals left nothing for the registry to release or restore. */
	require(uml::platform::release_guest_file_locks() == 0);
	require(!anotherProcessSeesALock(disk), "a refused lock request took a lock");
	require(uml::platform::restore_guest_file_locks());
	require(anotherProcessSeesALock(disk));
	/* The lock ends with its description, and then the other may take it. */
	close(disk);
	lockDisk(second);
	require(anotherProcessSeesALock(second));
	require(uml::platform::release_guest_file_locks(true) == 0);
	close(second);
	(void)uml::platform::forget_guest_files();
}

void aReadOnlyDiskLockIsReleasedToo()
{
	const int disk = openScratch("locked-read-only", O_RDONLY);
	lockDisk(disk, F_RDLCK);
	require(anotherProcessSeesALock(disk));
	require(uml::platform::release_guest_file_locks() == 0);
	require(!anotherProcessSeesALock(disk));
	require(uml::platform::restore_guest_file_locks());
	require(anotherProcessSeesALock(disk));
	close(disk);
	(void)uml::platform::forget_guest_files();
}

void aChangedDiskRefusesResumeAndReleasesEarlierLocks()
{
	const int first = openScratch("unchanged-first", O_RDWR);
	const int second = openScratch("changed-second", O_RDWR);
	require(pwrite(second, "before!", 7, 0) == 7);
	lockDisk(first);
	lockDisk(second);
	require(uml::platform::release_guest_file_locks() == 0);
	require(pwrite(second, "changed", 7, 0) == 7);
	errno = 0;
	require(!uml::platform::restore_guest_file_locks() && errno == ESTALE,
		"a disk modified while unlocked must never resume");
	require(!anotherProcessSeesALock(first) && !anotherProcessSeesALock(second),
		"a failed restore must release every lock it reacquired");
	close(first);
	close(second);
	(void)uml::platform::forget_guest_files();
}

void aReusedDiskDescriptorMustNotBeRelocked()
{
	const int first = openScratch("released-first", O_RDWR);
	lockDisk(first);
	require(uml::platform::release_guest_file_locks() == 0);
	close(first);
	const int second = openScratch("released-reused", O_RDWR);
	require(first == second);
	require(!uml::platform::restore_guest_file_locks() && errno == ESTALE,
		"a reused descriptor is not the suspended disk");
	require(!anotherProcessSeesALock(second), "the replacement must stay unlocked");
	close(second);
	(void)uml::platform::forget_guest_files();
}

void aContendedDiskRefusesResume()
{
	const int first = openScratch("contended-first", O_RDWR);
	const int second = openScratch("contended-second", O_RDWR);
	lockDisk(first);
	lockDisk(second);
	require(uml::platform::release_guest_file_locks() == 0);
	int ready[2], release[2];
	require(pipe(ready) == 0 && pipe(release) == 0);
	const pid_t child = fork();
	require(child >= 0);
	if (child == 0) {
		close(ready[0]);
		close(release[1]);
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		const char answer = fcntl(second, F_SETLK, &lock) == 0 ? 'y' : 'n';
		if (write(ready[1], &answer, 1) != 1)
			_exit(1);
		pollfd request {release[0], POLLIN, 0};
		_exit(poll(&request, 1, 10000) == 1 ? 0 : 1);
	}
	close(ready[1]);
	close(release[0]);
	require(receiveByte(ready[0]) == 'y', "another process can lock a paused disk");
	close(ready[0]);
	require(!uml::platform::restore_guest_file_locks(),
		"the guest must not resume while another process owns its disk");
	require(!anotherProcessSeesALock(first), "roll back the first disk's lock");
	require(write(release[1], "x", 1) == 1);
	close(release[1]);
	waitForChild(child);
	require(uml::platform::restore_guest_file_locks(),
		"an unchanged disk can resume after its other owner leaves");
	close(first);
	close(second);
	(void)uml::platform::forget_guest_files();
}

/* The registry restores a lock without waiting, and its mutex is the one every
 * flush needs, so a request to wait for a lock is refused rather than made
 * while another process owns the disk. */
void aBlockingLockRequestIsRefusedInsteadOfWaiting()
{
	(void)uml::platform::forget_guest_files();
	const int disk = openScratch("blocking-lock", O_RDWR);
	int ready[2], release[2];
	require(pipe(ready) == 0 && pipe(release) == 0);
	const pid_t child = fork();
	require(child >= 0);
	if (child == 0) {
		close(ready[0]);
		close(release[1]);
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		const char answer = fcntl(disk, F_SETLK, &lock) == 0 ? 'y' : 'n';
		if (write(ready[1], &answer, 1) != 1)
			_exit(1);
		pollfd request {release[0], POLLIN, 0};
		_exit(poll(&request, 1, 10000) == 1 ? 0 : 1);
	}
	close(ready[1]);
	close(release[0]);
	require(receiveByte(ready[0]) == 'y', "another process owns the disk");
	close(ready[0]);
	struct flock requested {};
	requested.l_type = F_WRLCK;
	requested.l_whence = SEEK_SET;
	const auto started = std::chrono::steady_clock::now();
	errno = 0;
	require(uml::platform::set_guest_file_lock(disk, F_SETLKW, requested) == -1 &&
		errno == EINVAL, "a waiting lock request must be refused");
	require(std::chrono::steady_clock::now() - started <
		std::chrono::seconds {5}, "the refused request waited for the disk");
	errno = 0;
	require(uml::platform::set_guest_file_lock(disk, F_GETLK, requested) == -1 &&
		errno == EINVAL, "only the non-blocking lock is a lock request");
	require(uml::platform::release_guest_file_locks() == 0 &&
		uml::platform::restore_guest_file_locks(),
		"a refused request left nothing for the registry to restore");
	require(write(release[1], "x", 1) == 1);
	close(release[1]);
	waitForChild(child);
	close(disk);
	(void)uml::platform::forget_guest_files();
}

/* The registry is idle unless a thread is inside it: a thread that asks for
 * its disk's lock without pause is inside most of the time, and the answer
 * follows it in both directions. */
void theRegistryIsIdleUnlessAThreadIsInsideIt()
{
	(void)uml::platform::forget_guest_files();
	require(uml::platform::guest_file_registry_idle(),
		"nobody is inside an unused registry");
	const int disk = openScratch("busy-disk", O_RDWR);
	lockDisk(disk);
	require(uml::platform::guest_file_registry_idle(),
		"nobody is inside the registry between requests");
	std::atomic<bool> stop {false};
	std::thread guest {[&] {
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		while (!stop.load(std::memory_order_acquire))
			(void)uml::platform::set_guest_file_lock(disk, F_SETLK, lock);
	}};
	bool sawInside = false;
	bool sawOutside = false;
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds {10};
	while (!(sawInside && sawOutside) &&
	       std::chrono::steady_clock::now() < deadline) {
		if (uml::platform::guest_file_registry_idle())
			sawOutside = true;
		else
			sawInside = true;
	}
	stop.store(true, std::memory_order_release);
	guest.join();
	require(sawInside, "a thread inside the registry must be seen");
	require(sawOutside, "a thread outside the registry must be seen");
	require(uml::platform::guest_file_registry_idle(),
		"nobody is inside the registry once the thread is gone");
	close(disk);
	(void)uml::platform::forget_guest_files();
}

} // namespace

int main()
{
	anEmptyRegistryFlushesNothing();
	aWritableDescriptorIsFlushed();
	aReadOnlyDescriptorIsNotRecorded();
	aClosedDescriptorIsRetired();
	aReusedDescriptorIsRetiredRatherThanFlushed();
	everyRecordedDescriptorIsFlushed();
	anUnflushableDescriptorIsReportedWithoutStoppingTheRest();
	forgottenRecordsAreNeitherFlushedNorRetired();
	diskLocksLeaveBeforeSuspensionAndReturnBeforeResume();
	closingAnotherDescriptorOfALockedDiskKeepsTheLock();
	aSecondDescriptionCannotLockALockedDisk();
	aReadOnlyDiskLockIsReleasedToo();
	aChangedDiskRefusesResumeAndReleasesEarlierLocks();
	aReusedDiskDescriptorMustNotBeRelocked();
	aContendedDiskRefusesResume();
	aBlockingLockRequestIsRefusedInsteadOfWaiting();
	theRegistryIsIdleUnlessAThreadIsInsideIt();
	std::puts("guest file sync tests passed");
	return 0;
}
