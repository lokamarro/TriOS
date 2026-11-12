// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_DATA_DISK_HPP
#define UML_DEVICE_BLOCK_ANDROID_DATA_DISK_HPP

#include "android_layout.hpp"

#include <cstdint>
#include <filesystem>

/* The writable disk of one Android guest instance: a sparse GPT file that the
 * host creates from the release layout's data disk template and only ever
 * grows. It holds `misc` (the bootloader message), `metadata` and `userdata`;
 * the guest formats the last two itself on first boot, so the host writes no
 * filesystem and a new disk costs only its partition tables. */
namespace uml::android {

enum class DataDiskChange { none, created, grown };

struct PreparedDataDisk {
	DataDiskChange change;
	std::uint32_t capacityGiB;
};

/* Checks `path`: a regular file, not a link, readable and writable by this
 * process, at a path a UBD specification can carry, whose size is a whole
 * number of GiB within the template's range and whose partition metadata is
 * exactly the template's for that size. Returns the capacity in GiB. Throws
 * std::invalid_argument otherwise. */
std::uint32_t validateDataDisk(const Layout &layout,
			       const std::filesystem::path &path);

/* Creates a sparse disk of `gib` GiB at a path that does not exist, never
 * replacing a file: the disk is built beside it and linked into place. Throws
 * std::system_error with EEXIST when the path exists, and
 * std::invalid_argument for a capacity outside the template's range. */
void createDataDisk(const Layout &layout, const std::filesystem::path &path,
		    std::uint32_t gib);

/* Raises the capacity of a valid disk to `gib` GiB, which must exceed its
 * current capacity; `userdata` then fills the larger disk. The disk must not
 * be open by a guest (std::runtime_error otherwise). The grow is built on an
 * APFS clone and renamed over the disk, so a failure leaves the old disk; a
 * volume without clones grows the disk in place, which a crash can leave
 * invalid. The guest's own filesystem grows at its next boot. */
void growDataDisk(const Layout &layout, const std::filesystem::path &path,
		  std::uint32_t gib);

/* Brings the disk at `path` to the requested capacity: creates it when the
 * path is absent (`gib` 0 selects the template's default), grows it when
 * `gib` exceeds its capacity, and leaves it when `gib` is 0 or equal. A
 * capacity below the current one is an error: a data disk never shrinks. */
PreparedDataDisk prepareDataDisk(const Layout &layout,
				 const std::filesystem::path &path,
				 std::uint32_t gib);

/* Whether another process, or another open file description of this one,
 * holds a lock on the file, as a running guest does on its writable disk: the
 * kernel's disk lock is an open file description lock, so a guest in this
 * process counts. The probe's own descriptor takes and ends no lock. */
bool lockedByAnotherProcess(const std::filesystem::path &path);

} // namespace uml::android

#endif
