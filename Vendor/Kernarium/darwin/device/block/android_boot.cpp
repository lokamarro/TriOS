// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_boot.hpp"
#include "ubd_path.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uml::android {
namespace {

constexpr std::uint64_t bootImagePageSize = 4096;
constexpr std::size_t bootImageHeaderSize = 1584;
constexpr std::string_view recoveryCommand = "boot-recovery";
constexpr std::string_view bootloaderCommand = "bootonce-bootloader";
/* The command field that opens the bootloader message (struct
 * bootloader_message in AOSP's bootloader_message.h). */
constexpr std::size_t bootloaderCommandSize = 32;
constexpr std::size_t bootloaderMessageSize = 2048;

constexpr std::array<std::string_view, 4> storagePrefixes {
	"ubd", "initrd", "root=", "rootfstype="};
constexpr std::string_view bootDevicesPrefix{"androidboot.boot_devices="};

void validateSystemDisk(const Layout &layout, const std::filesystem::path &path)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0 || !S_ISREG(details.st_mode))
		throw std::invalid_argument(
			"the Android system disk is not a regular file: " + path.string());
	if (access(path.c_str(), R_OK) != 0)
		throw std::invalid_argument("the Android system disk is not readable");
	validateUbdPath(path, "Android system disk");
	const File disk(path);
	verifyGpt(disk, layout.systemGpt(), "the Android system disk");
}

FileRange bootRamdisk(const File &disk, const SystemPartition &partition)
{
	std::array<std::byte, bootImageHeaderSize> header {};
	disk.read(partition.offset, header);
	if (std::memcmp(header.data(), "ANDROID!", 8) != 0 ||
	    little32(header.data() + 8) != 0U ||
	    little32(header.data() + 20) != bootImageHeaderSize ||
	    little32(header.data() + 40) != 4U ||
	    little32(header.data() + 1580) != 0U ||
	    !std::all_of(header.begin() + 24, header.begin() + 40,
			 [](std::byte value) { return value == std::byte{}; }))
		throw std::invalid_argument("Android boot partition is not kernelless v4");
	const auto ramdiskSize = little32(header.data() + 12);
	if (ramdiskSize == 0 || partition.imageBytes <= bootImagePageSize ||
	    ramdiskSize > partition.imageBytes - bootImagePageSize)
		throw std::invalid_argument("Android boot partition ramdisk is invalid");
	return FileRange{partition.offset + bootImagePageSize, ramdiskSize};
}

std::array<std::byte, bootloaderMessageSize> readBootloaderMessage(
	const Layout &layout, const std::filesystem::path &dataDisk)
{
	std::array<std::byte, bootloaderMessageSize> message {};
	const File disk(dataDisk);
	disk.read(layout.data.partition("misc").offset, message);
	return message;
}

/* The command field, or nothing when it has no terminator. */
std::optional<std::string_view> commandOf(
	const std::array<std::byte, bootloaderMessageSize> &message)
{
	const auto end = std::find(message.begin(),
				   message.begin() + bootloaderCommandSize, std::byte{});
	if (end == message.begin() + bootloaderCommandSize)
		return std::nullopt;
	return std::string_view{reinterpret_cast<const char *>(message.data()),
				static_cast<std::size_t>(end - message.begin())};
}

std::filesystem::path requireAbsolute(std::filesystem::path path, std::string_view label)
{
	if (!path.is_absolute())
		throw std::invalid_argument(std::string{label} + " path must be absolute");
	return path;
}

bool startsWithAny(std::string_view argument,
		   std::span<const std::string_view> prefixes)
{
	return std::any_of(prefixes.begin(), prefixes.end(),
			   [argument](std::string_view prefix) {
				   return argument.starts_with(prefix);
			   });
}

} // namespace

Storage::Storage(std::filesystem::path release, std::filesystem::path dataDisk)
	: release_(requireAbsolute(std::move(release), "Android release")),
	  systemDisk_(release_ / releaseSystemDisk),
	  dataDisk_(requireAbsolute(std::move(dataDisk), "Android data disk")),
	  layout_(readLayout(release_ / releaseLayout))
{
	validate();
}

void Storage::validate() const
{
	validateSystemDisk(layout_, systemDisk_);
	/* The two checks cannot both pass for one file: a system disk's
	 * partition tables are not a data disk's. */
	(void)validateDataDisk(layout_, dataDisk_);
}

BootSelection Storage::selectBoot(BootMode requestedMode) const
{
	if (requestedMode != BootMode::automatic &&
	    requestedMode != BootMode::normal && requestedMode != BootMode::recovery)
		throw std::invalid_argument("unknown Android boot mode");
	validate();
	const File system(systemDisk_);
	const auto normal = bootRamdisk(system, layout_.systemPartition("init_boot"));
	const auto recovery = bootRamdisk(system, layout_.systemPartition("recovery"));
	auto selected = requestedMode;
	if (requestedMode == BootMode::automatic) {
		const auto command = commandOf(readBootloaderMessage(layout_, dataDisk_));
		if (!command)
			throw std::invalid_argument(
				"Android bootloader message command is malformed");
		selected = *command == recoveryCommand ? BootMode::recovery
						       : BootMode::normal;
	}
	return BootSelection{selected,
			     selected == BootMode::recovery ? recovery : normal};
}

bool Storage::consumeBootloaderCommand() const
{
	validate();
	if (commandOf(readBootloaderMessage(layout_, dataDisk_)) != bootloaderCommand)
		return false;
	const int descriptor = open(dataDisk_.c_str(), O_WRONLY | O_NOFOLLOW);
	if (descriptor < 0)
		throw std::system_error(errno, std::generic_category(),
					"cannot open " + dataDisk_.string());
	const std::array<std::byte, bootloaderCommandSize> cleared {};
	const auto written = pwrite(
		descriptor, cleared.data(), cleared.size(),
		static_cast<off_t>(layout_.data.partition("misc").offset));
	const int writeError = written < 0 ? errno : EIO;
	const int syncError = fsync(descriptor) == 0 ? 0 : errno;
	const int closeError = close(descriptor) == 0 ? 0 : errno;
	if (written != static_cast<ssize_t>(cleared.size()))
		throw std::system_error(writeError, std::generic_category(),
					"cannot clear the bootloader command");
	if (syncError != 0 || closeError != 0)
		throw std::system_error(syncError != 0 ? syncError : closeError,
					std::generic_category(),
					"cannot flush the bootloader command");
	return true;
}

RestartBoot Storage::selectRestartBoot() const
{
	const bool consumed = consumeBootloaderCommand();
	return RestartBoot{selectBoot(BootMode::automatic), consumed};
}

std::vector<std::string> Storage::kernelArguments(const BootSelection &boot) const
{
	return {
		"ubd0r=" + systemDisk_.string(),
		"ubd1=" + dataDisk_.string(),
		"initrd=" + systemDisk_.string(),
		"initrd_offset=" + std::to_string(boot.ramdisk.offset),
		"initrd_size=" + std::to_string(boot.ramdisk.size),
		"root=/dev/ram0",
		"rootfstype=ramfs",
	};
}

std::span<const std::string_view> storageArgumentPrefixes() noexcept
{
	return storagePrefixes;
}

void rejectStorageArguments(std::span<const std::string> arguments)
{
	for (const auto &argument : arguments)
		if (startsWithAny(argument, storagePrefixes) ||
		    argument.starts_with(bootDevicesPrefix))
			throw std::invalid_argument(
				"the Android launcher attaches the guest's disks itself; "
				"the kernel argument is refused: " + argument);
}

void validateUbdPath(const std::filesystem::path &path, std::string_view label)
{
	if (!::uml::block::usable_ubd_path(path.native()))
		throw std::invalid_argument(std::string{label} +
			" path is unusable as a UBD device");
}

std::vector<std::string> readKernelCommandLine(
	const std::filesystem::path &path,
	std::span<const std::string_view> launcherPrefixes)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0 || !S_ISREG(details.st_mode) ||
	    details.st_size < 0)
		throw std::runtime_error(
			"the kernel command line is missing or unsafe: " +
			path.string());
	std::string text(static_cast<std::size_t>(details.st_size), '\0');
	std::FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
		throw std::system_error(errno, std::generic_category(),
			"cannot open " + path.string());
	const auto read = std::fread(text.data(), 1, text.size(), file);
	const int closed = std::fclose(file);
	if (read != text.size() || closed != 0)
		throw std::runtime_error("cannot read " + path.string());

	std::vector<std::string> arguments;
	bool bootDevices = false;
	constexpr std::string_view separators{" \n\t"};
	std::size_t cursor = 0;
	while (cursor < text.size()) {
		const auto begin = text.find_first_not_of(separators, cursor);
		if (begin == std::string::npos)
			break;
		cursor = std::min(text.find_first_of(separators, begin),
			text.size());
		const std::string_view argument{text.data() + begin,
			cursor - begin};
		/* A launcher that sets the multiplier gets the legacy one dropped
		 * rather than refused: every release before it carries it. */
		if (argument == legacyHwTimeoutMultiplierArgument &&
		    std::ranges::find(launcherPrefixes,
				      hwTimeoutMultiplierPrefix) !=
			    launcherPrefixes.end())
			continue;
		for (const auto prefix : launcherPrefixes)
			if (argument.starts_with(prefix))
				throw std::invalid_argument(
					"the release kernel command line sets " +
					std::string{prefix} +
					", which the launcher fixes");
		if (startsWithAny(argument, storagePrefixes))
			throw std::invalid_argument(
				"the release kernel command line sets " +
				std::string{argument} +
				", which names a block device or ramdisk the launcher fixes");
		if (argument.starts_with(bootDevicesPrefix)) {
			if (argument != bootDevicesArgument || bootDevices)
				throw std::invalid_argument(
					"the release kernel command line must name exactly " +
					std::string{bootDevicesArgument} + " once");
			bootDevices = true;
		}
		arguments.emplace_back(argument);
	}
	if (arguments.empty())
		throw std::invalid_argument(
			"the release kernel command line is empty");
	if (!bootDevices)
		throw std::invalid_argument(
			"the release kernel command line lacks " +
			std::string{bootDevicesArgument});
	return arguments;
}

std::string lcdDensityArgument(std::uint32_t dotsPerInch)
{
	return std::string{lcdDensityPrefix} +
	       std::to_string(dotsPerInch == 0 ? defaultLcdDensity : dotsPerInch);
}

std::string hwTimeoutMultiplierArgument(unsigned vcpus)
{
	return std::string{hwTimeoutMultiplierPrefix} +
	       std::to_string(hwTimeoutMultiplier(vcpus));
}

std::uint32_t parseDotsPerInch(std::string_view text)
{
	std::uint32_t parsed = 0;
	const auto [end, error] =
		std::from_chars(text.data(), text.data() + text.size(), parsed);
	if (error != std::errc{} || end != text.data() + text.size() ||
	    parsed == 0)
		throw std::invalid_argument("invalid dots per inch: " +
					    std::string{text});
	return parsed;
}

} // namespace uml::android
