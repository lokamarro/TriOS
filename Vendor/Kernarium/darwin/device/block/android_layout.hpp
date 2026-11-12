// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_LAYOUT_HPP
#define UML_DEVICE_BLOCK_ANDROID_LAYOUT_HPP

#include "android_gpt.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uml::android {

/* A release's guest/partition-layout.json (schema 2), which
 * android/lineage-24/tools/ubd-disk.py writes: the exact partitions of the
 * read-only system disk, and the template of the writable data disk the host
 * creates for each guest instance. The host reads this file and nothing else
 * about the layout, so a size or an offset changes with the release and needs
 * no host change. parseLayout checks every rule ubd-disk.py checks. */

struct SystemPartition {
	std::string name;
	std::uint64_t offset;
	std::uint64_t size;
	/* The bytes the partition's image occupies; the rest of the partition,
	 * under 4 KiB, is zero. */
	std::uint64_t imageBytes;
	std::string sha256;
	Guid type;
	Guid unique;
};

struct DataPartition {
	std::string name;
	std::uint64_t offset;
	/* Absent for the last partition, which fills the disk up to the free
	 * space at its end. */
	std::optional<std::uint64_t> size;
	Guid type;
	Guid unique;
};

struct DataDiskTemplate {
	std::uint32_t layoutVersion;
	std::uint64_t alignment;
	std::uint32_t entryCount;
	Guid diskGuid;
	std::uint64_t trailingFreeBytes;
	std::uint32_t minimumGiB;
	std::uint32_t defaultGiB;
	std::uint32_t maximumGiB;
	/* misc, metadata, userdata, in that order. */
	std::vector<DataPartition> partitions;

	const DataPartition &partition(std::string_view name) const;
};

struct Layout {
	std::uint64_t systemDiskBytes;
	std::string systemDiskSha256;
	std::uint64_t systemAlignment;
	std::uint32_t systemEntryCount;
	Guid systemDiskGuid;
	/* In disk order: init_boot, recovery, then the filesystems. */
	std::vector<SystemPartition> systemPartitions;
	DataDiskTemplate data;

	const SystemPartition &systemPartition(std::string_view name) const;
	/* The GPT metadata the system disk must carry. */
	GptImage systemGpt() const;
	/* The GPT metadata of a data disk of `gib` whole GiB; throws
	 * std::invalid_argument for a capacity outside the template's range. */
	GptImage dataGpt(std::uint32_t gib) const;
};

/* Throws std::invalid_argument for a document that is not exactly a valid
 * schema 2 layout. */
Layout parseLayout(std::string_view json);

/* Reads a layout file (a regular file of at most 1 MiB). */
Layout readLayout(const std::filesystem::path &path);

} // namespace uml::android

#endif
