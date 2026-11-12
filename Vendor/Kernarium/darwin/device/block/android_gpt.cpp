// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_gpt.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uml::android {
namespace {

constexpr std::uint32_t gptRevision = 0x00010000U;
constexpr std::uint8_t protectiveMbrType = 0xeeU;
constexpr std::size_t maximumNameCharacters = 36;

void store32(std::byte *output, std::uint32_t value)
{
	for (unsigned int index = 0; index < 4; ++index)
		output[index] = static_cast<std::byte>(value >> (index * 8U));
}

void store64(std::byte *output, std::uint64_t value)
{
	for (unsigned int index = 0; index < 8; ++index)
		output[index] = static_cast<std::byte>(value >> (index * 8U));
}

int hexDigit(char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	return -1;
}

std::array<std::byte, sectorSize> headerSector(
	const GptImage &image, const Guid &diskGuid, std::uint64_t currentLba,
	std::uint64_t backupLba, std::uint64_t entriesLba, std::uint64_t firstUsable,
	std::uint64_t lastUsable, std::uint32_t entriesCrc)
{
	std::array<std::byte, sectorSize> header {};
	std::memcpy(header.data(), "EFI PART", 8);
	store32(header.data() + 8, gptRevision);
	store32(header.data() + 12, static_cast<std::uint32_t>(gptHeaderSize));
	store64(header.data() + 24, currentLba);
	store64(header.data() + 32, backupLba);
	store64(header.data() + 40, firstUsable);
	store64(header.data() + 48, lastUsable);
	std::memcpy(header.data() + 56, diskGuid.bytes.data(), 16);
	store64(header.data() + 72, entriesLba);
	store32(header.data() + 80, image.entryCount);
	store32(header.data() + 84, static_cast<std::uint32_t>(gptEntrySize));
	store32(header.data() + 88, entriesCrc);
	store32(header.data() + 16,
		crc32(std::span<const std::byte>{header}.first(gptHeaderSize)));
	return header;
}

void requireEqual(const File &disk, std::uint64_t offset,
		  std::span<const std::byte> expected, std::string_view label,
		  std::string_view what)
{
	std::vector<std::byte> actual(expected.size());
	disk.read(offset, actual);
	if (!std::equal(actual.begin(), actual.end(), expected.begin()))
		throw std::invalid_argument(std::string{label} + " " +
					    std::string{what} +
					    " differs from its layout");
}

} // namespace

std::uint32_t little32(const std::byte *input)
{
	return std::to_integer<std::uint32_t>(input[0]) |
		(std::to_integer<std::uint32_t>(input[1]) << 8U) |
		(std::to_integer<std::uint32_t>(input[2]) << 16U) |
		(std::to_integer<std::uint32_t>(input[3]) << 24U);
}

std::uint64_t little64(const std::byte *input)
{
	return static_cast<std::uint64_t>(little32(input)) |
		(static_cast<std::uint64_t>(little32(input + 4)) << 32U);
}

std::uint32_t crc32(std::span<const std::byte> input)
{
	std::uint32_t value = ~std::uint32_t{0};
	for (const auto byte : input) {
		value ^= std::to_integer<std::uint8_t>(byte);
		for (unsigned int bit = 0; bit < 8; ++bit)
			value = (value >> 1U) ^
				((value & 1U) != 0 ? 0xedb88320U : 0U);
	}
	return ~value;
}

Guid parseGuid(std::string_view text)
{
	/* 8-4-4-4-12 lower-case hexadecimal digits. */
	if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
	    text[18] != '-' || text[23] != '-')
		throw std::invalid_argument("GUID is not in canonical form");
	std::array<std::uint8_t, 16> canonical {};
	std::size_t out = 0;
	for (std::size_t index = 0; index < text.size();) {
		if (text[index] == '-') {
			++index;
			continue;
		}
		const int high = hexDigit(text[index]);
		const int low = hexDigit(text[index + 1U]);
		if (high < 0 || low < 0)
			throw std::invalid_argument(
				"GUID is not in lower-case hexadecimal");
		canonical[out++] = static_cast<std::uint8_t>(high * 16 + low);
		index += 2;
	}
	/* The first three fields are stored little endian. */
	Guid result;
	result.bytes = {canonical[3], canonical[2], canonical[1], canonical[0],
			canonical[5], canonical[4], canonical[7], canonical[6],
			canonical[8], canonical[9], canonical[10], canonical[11],
			canonical[12], canonical[13], canonical[14], canonical[15]};
	return result;
}

GptImage buildGpt(std::uint64_t diskBytes, std::uint32_t entryCount,
		  const Guid &diskGuid, std::span<const GptPartition> partitions)
{
	if (entryCount == 0 || entryCount > 4096U ||
	    entryCount * gptEntrySize % sectorSize != 0)
		throw std::invalid_argument("GPT entry count is unsupported");
	if (partitions.size() > entryCount)
		throw std::invalid_argument("more partitions than GPT entries");
	if (diskBytes % sectorSize != 0)
		throw std::invalid_argument("disk size is not whole sectors");
	GptImage image {};
	image.diskBytes = diskBytes;
	image.entryCount = entryCount;
	image.entries.assign(static_cast<std::size_t>(entryCount) * gptEntrySize,
			     std::byte{});
	const auto tableSectors = image.tableSectors();
	/* The protective MBR, both headers and both tables, plus room for one
	 * sector of payload. */
	if (image.sectorCount() < 2U * (tableSectors + 1U) + 3U)
		throw std::invalid_argument("disk is too small for its GPT");
	const auto firstUsable = 2U + tableSectors;
	const auto lastUsable = image.sectorCount() - tableSectors - 2U;

	for (std::size_t index = 0; index < partitions.size(); ++index) {
		const auto &partition = partitions[index];
		if (partition.offset % sectorSize != 0 ||
		    partition.size % sectorSize != 0 || partition.size == 0)
			throw std::invalid_argument(
				"partition " + partition.name +
				" is not whole sectors");
		const auto first = partition.offset / sectorSize;
		const auto last = first + partition.size / sectorSize - 1U;
		if (first < firstUsable || last > lastUsable)
			throw std::invalid_argument(
				"partition " + partition.name +
				" is outside the usable area");
		for (std::size_t other = 0; other < index; ++other) {
			const auto otherFirst = partitions[other].offset / sectorSize;
			const auto otherLast = otherFirst +
				partitions[other].size / sectorSize - 1U;
			if (first <= otherLast && otherFirst <= last)
				throw std::invalid_argument(
					"partition " + partition.name + " overlaps " +
					partitions[other].name);
		}
		if (partition.name.empty() ||
		    partition.name.size() > maximumNameCharacters)
			throw std::invalid_argument("partition name length");
		auto *entry = image.entries.data() + index * gptEntrySize;
		std::memcpy(entry, partition.type.bytes.data(), 16);
		std::memcpy(entry + 16, partition.unique.bytes.data(), 16);
		store64(entry + 32, first);
		store64(entry + 40, last);
		for (std::size_t character = 0; character < partition.name.size();
		     ++character) {
			const auto value = static_cast<unsigned char>(partition.name[character]);
			if (value < 0x20U || value > 0x7eU)
				throw std::invalid_argument(
					"partition name is not printable ASCII");
			entry[56 + character * 2U] = static_cast<std::byte>(value);
		}
	}

	/* Protective MBR: one 0xee entry covering the disk from LBA 1. */
	auto *entry = image.protectiveMbr.data() + 446;
	image.protectiveMbr.fill(std::byte{});
	entry[2] = std::byte{0x02};
	entry[4] = static_cast<std::byte>(protectiveMbrType);
	entry[5] = entry[6] = entry[7] = std::byte{0xff};
	store32(entry + 8, 1U);
	store32(entry + 12,
		static_cast<std::uint32_t>(std::min<std::uint64_t>(
			image.sectorCount() - 1U,
			std::numeric_limits<std::uint32_t>::max())));
	image.protectiveMbr[510] = std::byte{0x55};
	image.protectiveMbr[511] = std::byte{0xaa};

	const auto entriesCrc = crc32(image.entries);
	image.primaryHeader = headerSector(
		image, diskGuid, 1U, image.sectorCount() - 1U, 2U, firstUsable,
		lastUsable, entriesCrc);
	image.backupHeader = headerSector(
		image, diskGuid, image.sectorCount() - 1U, 1U,
		image.backupEntriesLba(), firstUsable, lastUsable, entriesCrc);
	return image;
}

File::File(const std::filesystem::path &path)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0)
		throw std::system_error(errno, std::generic_category(),
					"cannot inspect " + path.string());
	if (!S_ISREG(details.st_mode) || details.st_size < 0)
		throw std::invalid_argument("unsafe regular file: " + path.string());
	size_ = static_cast<std::uint64_t>(details.st_size);
	descriptor_ = open(path.c_str(), O_RDONLY);
	if (descriptor_ < 0)
		throw std::system_error(errno, std::generic_category(),
					"cannot open " + path.string());
}

File::~File()
{
	if (descriptor_ >= 0)
		(void)close(descriptor_);
}

void File::read(std::uint64_t offset, std::span<std::byte> output) const
{
	if (offset > size_ || output.size() > size_ - offset ||
	    offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
		throw std::invalid_argument("file range is outside its payload");
	std::size_t completed = 0;
	while (completed < output.size()) {
		const auto absolute = offset + completed;
		const auto count = pread(descriptor_, output.data() + completed,
					 output.size() - completed,
					 static_cast<off_t>(absolute));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			throw std::system_error(count < 0 ? errno : EIO,
						std::generic_category(),
						"cannot read file range");
		completed += static_cast<std::size_t>(count);
	}
}

void verifyGpt(const File &disk, const GptImage &expected, std::string_view label)
{
	if (disk.size() != expected.diskBytes)
		throw std::invalid_argument(std::string{label} +
					    " has the wrong size");
	requireEqual(disk, 0, expected.protectiveMbr, label, "protective MBR");
	requireEqual(disk, sectorSize, expected.primaryHeader, label,
		     "primary GPT header");
	requireEqual(disk, 2U * sectorSize, expected.entries, label,
		     "primary GPT entries");
	requireEqual(disk, expected.backupEntriesLba() * sectorSize,
		     expected.entries, label, "backup GPT entries");
	requireEqual(disk, (expected.sectorCount() - 1U) * sectorSize,
		     expected.backupHeader, label, "backup GPT header");
}

void writeAll(int descriptor, std::uint64_t offset,
	      std::span<const std::byte> payload)
{
	std::size_t completed = 0;
	while (completed < payload.size()) {
		const auto count = pwrite(
			descriptor, payload.data() + completed,
			payload.size() - completed,
			static_cast<off_t>(offset + completed));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			throw std::system_error(count < 0 ? errno : EIO,
						std::generic_category(), "pwrite");
		completed += static_cast<std::size_t>(count);
	}
}

void writeGpt(int descriptor, const GptImage &image)
{
	writeAll(descriptor, 0, image.protectiveMbr);
	writeAll(descriptor, 2U * sectorSize, image.entries);
	writeAll(descriptor, image.backupEntriesLba() * sectorSize, image.entries);
	writeAll(descriptor, (image.sectorCount() - 1U) * sectorSize,
		 image.backupHeader);
	/* The primary header last, so a table is never announced before its
	 * entries are in place. */
	writeAll(descriptor, sectorSize, image.primaryHeader);
}

} // namespace uml::android
