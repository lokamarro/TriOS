// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_GPT_HPP
#define UML_DEVICE_BLOCK_ANDROID_GPT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/* The GUID partition table of the two Android disks. The host builds the
 * table of a data disk it creates, and checks both disks' tables byte for byte
 * against the table their layout implies, so one builder serves both. The
 * layout is the release's partition-layout.json (android_layout.hpp). */
namespace uml::android {

inline constexpr std::uint64_t sectorSize = 512;
inline constexpr std::uint64_t mebibyte = 1'048'576ULL;
inline constexpr std::uint64_t gibibyte = 1'073'741'824ULL;
inline constexpr std::size_t gptEntrySize = 128;
inline constexpr std::size_t gptHeaderSize = 92;

/* A GUID in the byte order a GPT stores it (the first three fields little
 * endian). */
struct Guid {
	std::array<std::uint8_t, 16> bytes {};

	friend bool operator==(const Guid &, const Guid &) = default;
};

/* Parses the canonical lower-case text form; throws std::invalid_argument for
 * anything else. */
Guid parseGuid(std::string_view text);

struct GptPartition {
	std::string name;
	std::uint64_t offset;
	std::uint64_t size;
	Guid type;
	Guid unique;
};

/* Every byte of a disk's partition metadata: the protective MBR, both headers
 * and both copies of the entry array. */
struct GptImage {
	std::uint64_t diskBytes;
	std::uint32_t entryCount;
	std::array<std::byte, sectorSize> protectiveMbr;
	std::array<std::byte, sectorSize> primaryHeader;
	std::array<std::byte, sectorSize> backupHeader;
	std::vector<std::byte> entries;

	std::uint64_t sectorCount() const noexcept { return diskBytes / sectorSize; }
	std::uint64_t tableSectors() const noexcept
	{
		return entries.size() / sectorSize;
	}
	std::uint64_t backupEntriesLba() const noexcept
	{
		return sectorCount() - tableSectors() - 1U;
	}
};

/* The metadata of a disk of `diskBytes` (a multiple of 512, at least large
 * enough for both tables) with `entryCount` entry slots and `partitions` in
 * slots 0..n-1. Throws std::invalid_argument for partitions that do not fit
 * between the tables, overlap, or are not whole sectors. */
GptImage buildGpt(std::uint64_t diskBytes, std::uint32_t entryCount,
		  const Guid &diskGuid, std::span<const GptPartition> partitions);

/* A read-only handle on a regular file that is not a link. */
class File final {
public:
	explicit File(const std::filesystem::path &path);
	~File();
	File(const File &) = delete;
	File &operator=(const File &) = delete;

	std::uint64_t size() const noexcept { return size_; }
	void read(std::uint64_t offset, std::span<std::byte> output) const;

private:
	int descriptor_ = -1;
	std::uint64_t size_ = 0;
};

/* Throws std::invalid_argument naming `label` unless the disk file carries
 * exactly the metadata of `expected` and has its size. */
void verifyGpt(const File &disk, const GptImage &expected,
	       std::string_view label);

/* Writes the metadata of `image` to the descriptor of a file already extended
 * to image.diskBytes. */
void writeGpt(int descriptor, const GptImage &image);

/* pwrite of the whole payload; throws std::system_error. */
void writeAll(int descriptor, std::uint64_t offset,
	      std::span<const std::byte> payload);

std::uint32_t crc32(std::span<const std::byte> input);
std::uint32_t little32(const std::byte *input);
std::uint64_t little64(const std::byte *input);

} // namespace uml::android

#endif
