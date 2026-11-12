// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::runtime {

struct GuestSyncResult final {
	unsigned int flushed;
	unsigned int failed;
};

/*
 * Flushes every host file the running guest can still write to. This is the
 * durability boundary the host owns: writes the guest has already handed to
 * the block layer reach stable storage. It does not reach into the guest's own
 * page cache, which only the guest kernel can write back.
 */
[[nodiscard]] GuestSyncResult sync_guest_files() noexcept;

/* Whether no thread is inside the guest file registry. Asked with every
 * thread of the guest stopped, it says that none of them owns the registry,
 * which the release and the restore of the disk locks below need. */
[[nodiscard]] bool guest_file_registry_idle() noexcept;

[[nodiscard]] unsigned release_guest_file_locks(bool retiring = false) noexcept;
/* Ends the process if a disk cannot be relocked unchanged: the guest must
 * never resume on storage another process modified during suspension. */
void restore_guest_file_locks() noexcept;

} // namespace uml::runtime
