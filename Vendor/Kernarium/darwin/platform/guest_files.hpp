// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

struct flock;

namespace uml::platform {

/*
 * Every host file the guest kernel opens passes through the host bridge, so
 * this registry is the only place that knows the complete set of descriptors a
 * running guest can still write to. A durability request flushes exactly that
 * set: the block backing files, and nothing the host itself owns.
 *
 * Read-only descriptors are not recorded. They carry nothing to flush, and
 * keeping them out bounds the registry to the handful of writable images a
 * guest actually has open.
 *
 * A record also remembers which file it described. That makes the registry
 * self-validating, so closing a descriptor needs no hook here: a stale record
 * is recognised at flush time and dropped, whether the descriptor is now
 * closed or has been reused for something else entirely.
 */
void record_guest_file(int descriptor, int open_flags) noexcept;

struct GuestFileSyncResult final {
	unsigned int flushed;
	unsigned int failed;
	unsigned int retired;
};

/*
 * Flushes every recorded descriptor that still names the file it was recorded
 * for. Records that no longer do are retired rather than flushed.
 */
[[nodiscard]] GuestFileSyncResult sync_guest_files() noexcept;

/* The kernel's whole-file disk locks pass through this operation. Only the
 * non-blocking whole-file lock F_SETLK is accepted (EINVAL otherwise): a wait
 * would hold the registry's mutex, which every flush and every quiesce needs,
 * and a restore takes the lock again without waiting. Lock acquisition is
 * refused while the guest is quiesced. On macOS the host file is locked with an
 * open file description lock, which belongs to the descriptor's description:
 * closing another descriptor of the file leaves it, and another description,
 * this process's own included, finds the file locked. On the iOS family
 * (device, Simulator, Designed for iPad) the lock is the registry's alone and
 * no host file is locked, so the process holds no file lock whenever iOS can
 * suspend it, whether or not the guest suspended; the release and the restore
 * below keep their bookkeeping and the restore its checks. */
int set_guest_file_lock(int descriptor, int command,
	const struct flock &lock) noexcept;

/*
 * True when no thread is inside the registry's critical section, which every
 * flush, lock request, release and restore passes through. The mutex is an
 * ordinary one, so a thread stopped inside it would keep it for as long as it
 * is stopped, and a release or a restore on another thread would wait for it
 * forever. A caller that stops the guest's threads and then releases or
 * restores locks therefore asks this with every one of them stopped (as the
 * thread fence's `settled` condition) and accepts the stop only on true. The
 * answer then holds until they run again: a stopped thread cannot take the
 * mutex, and none of them owns it.
 */
[[nodiscard]] bool guest_file_registry_idle() noexcept;

/* Release the disk locks after guest writeback, so that a suspended process
 * holds none, and remember their file metadata for resume. Retiring a run
 * drops the lock records instead. Returns the number of locks that could not
 * be released: they stay held, and a later release tries them again; a
 * restore leaves them alone. */
[[nodiscard]] unsigned release_guest_file_locks(bool retiring = false) noexcept;

/* Reacquire every released disk lock and verify that no disk changed while
 * unlocked. On failure release the locks this attempt reacquired, and only
 * those, and keep the guest quiesced. The caller must not run the guest after
 * a failure. */
[[nodiscard]] bool restore_guest_file_locks() noexcept;

/*
 * Forgets every record, and returns how many there were. A guest that restarts
 * itself hands the process to the next one after the library has closed its
 * descriptors; a record left behind would name a descriptor number the next
 * guest, or the host, may be given for something else and retires itself only
 * at the next flush. Records the next guest makes are its own.
 */
std::size_t forget_guest_files() noexcept;

} // namespace uml::platform
