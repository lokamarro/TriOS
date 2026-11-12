// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_BOOT_HPP
#define UML_DEVICE_BLOCK_ANDROID_BOOT_HPP

#include "android_data_disk.hpp"
#include "android_layout.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::android {

enum class BootMode : std::uint32_t {
	automatic = 0,
	normal = 1,
	recovery = 2,
};

struct FileRange {
	std::uint64_t offset;
	std::uint64_t size;
};

struct BootSelection {
	BootMode mode;
	FileRange ramdisk;
};

struct RestartBoot {
	BootSelection boot;
	/* The disk held a bootloader request, now cleared. */
	bool bootloaderConsumed;
};

/* What a release carries, relative to its root: the read-only system disk, the
 * layout that describes it and the writable disk's template, and the kernel
 * command line. */
inline constexpr std::string_view releaseSystemDisk{"guest/system.img"};
inline constexpr std::string_view releaseLayout{"guest/partition-layout.json"};
inline constexpr std::string_view releaseCommandLine{"boot/kernel-command-line.txt"};

/* The only block devices an Android guest has: the release's system disk as
 * `ubd0r` and the instance's own data disk as `ubd1`, whose platform devices
 * the release's command line names to first-stage init. First-stage init
 * takes a partition by name from any Android GPT disk it finds, and waits
 * twenty seconds and reboots when one that matters is missing, so a third disk
 * (or a second instance's) must never be attached. Storage is the single
 * source of the guest's `ubd` arguments, and rejectStorageArguments refuses
 * every other way to ask for one.
 *
 * Construction checks the layout, both disks and that they are two files; the
 * boot operations check them again, since a guest that restarts boots on
 * whatever the files are by then. Every check throws std::invalid_argument, or
 * std::system_error for an I/O failure. */
class Storage final {
public:
	/* `release` is the release root and `dataDisk` an existing data disk
	 * (prepareDataDisk creates one), both absolute. */
	Storage(std::filesystem::path release, std::filesystem::path dataDisk);

	const Layout &layout() const noexcept { return layout_; }
	const std::filesystem::path &release() const noexcept { return release_; }
	const std::filesystem::path &systemDisk() const noexcept { return systemDisk_; }
	const std::filesystem::path &dataDisk() const noexcept { return dataDisk_; }

	/* Checks both disks and that they are different files. */
	void validate() const;

	/* The ramdisk of the boot the guest should make. In automatic mode the
	 * data disk's bootloader message decides between recovery and a normal
	 * boot. */
	BootSelection selectBoot(BootMode requestedMode) const;

	/* There is no bootloader to take the one-shot request that `reboot
	 * bootloader` leaves in the misc partition's bootloader message (its
	 * command field reads "bootonce-bootloader"). A bootloader that continues
	 * clears the request and boots normally, and so does this: it clears the
	 * 32-byte command field and returns true, or changes nothing and returns
	 * false when the field holds anything else. Left in place, the request
	 * makes Android's init skip the "boot-recovery" it writes for a later
	 * `reboot recovery`. Throws std::system_error when the field cannot be
	 * written. */
	bool consumeBootloaderCommand() const;

	/* The boot after the guest restarted itself: consumes a bootloader
	 * request, then selects the ramdisk in automatic mode whatever mode the
	 * first boot was given, so that `reboot recovery` boots recovery and
	 * anything else boots normally. Recovery clears its own "boot-recovery"
	 * request when it is done. */
	RestartBoot selectRestartBoot() const;

	/* The kernel arguments that attach the two disks and give the guest
	 * `boot`'s ramdisk. */
	std::vector<std::string> kernelArguments(const BootSelection &boot) const;

private:
	std::filesystem::path release_;
	std::filesystem::path systemDisk_;
	std::filesystem::path dataDisk_;
	Layout layout_;
};

/* The argument prefixes only Storage may set: the block devices and the
 * ramdisk they carry. */
std::span<const std::string_view> storageArgumentPrefixes() noexcept;

/* Throws std::invalid_argument for a kernel argument of `arguments` that
 * names a block device or a ramdisk. */
void rejectStorageArguments(std::span<const std::string> arguments);

/* The name of a release's two UBD platform devices as first-stage init takes
 * them: `uml-blkdev.0` is the system disk and `uml-blkdev.1` the data disk. */
inline constexpr std::string_view bootDevicesArgument{
	"androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1"};

/* Throws std::invalid_argument naming `label`, for a path that a UBD kernel
 * argument cannot carry as itself (see uml::block::usable_ubd_path). */
void validateUbdPath(const std::filesystem::path &path, std::string_view label);

/* The whitespace-separated arguments of a release's kernel command-line file.
 * Throws when the file is not a readable regular file, when it holds no
 * argument, when an argument starts with one of `launcherPrefixes`, which the
 * launcher sets itself, or with one of storageArgumentPrefixes(), or when it
 * lacks bootDevicesArgument. A launcher that lists hwTimeoutMultiplierPrefix
 * has the one argument legacyHwTimeoutMultiplierArgument dropped from the
 * result instead of refused. */
std::vector<std::string> readKernelCommandLine(
	const std::filesystem::path &path,
	std::span<const std::string_view> launcherPrefixes);

/* The kernel argument that gives the guest its display density. Android init
 * imports it as ro.boot.lcd_density and the product's vendor init copies it to
 * ro.sf.lcd_density, so every launcher sets it and a release's command line
 * may not. */
inline constexpr std::string_view lcdDensityPrefix{"androidboot.lcd_density="};

/* The density SurfaceFlinger uses, in dots per inch, when a display has no
 * physical size to derive one from. */
inline constexpr std::uint32_t defaultLcdDensity = 213;

/* `androidboot.lcd_density=` followed by the density of the EDID the launcher
 * advertises (its --gpu-dpi), or defaultLcdDensity when it advertises none
 * (`dotsPerInch` is 0). */
std::string lcdDensityArgument(std::uint32_t dotsPerInch);

/* The dots per inch in the text of a --gpu-dpi value: a positive decimal
 * number with no other character. Throws std::invalid_argument otherwise. */
std::uint32_t parseDotsPerInch(std::string_view text);

/* The kernel argument that scales the framework's ANR, watchdog and input
 * timeouts. Android init imports it as ro.boot.hw_timeout_multiplier and the
 * product's vendor init copies it to ro.hw_timeout_multiplier, so every
 * launcher sets it from the guest's vCPU count and a release's command line
 * may not. */
inline constexpr std::string_view hwTimeoutMultiplierPrefix{
	"androidboot.hw_timeout_multiplier="};

/* The multiplier for a guest of `vcpus` vCPUs: 3 with six or more, 5 below,
 * where the first minutes after a boot are too slow for 3. */
[[nodiscard]] constexpr std::uint32_t hwTimeoutMultiplier(
	unsigned vcpus) noexcept
{
	return vcpus >= 6U ? 3U : 5U;
}

/* `androidboot.hw_timeout_multiplier=` followed by hwTimeoutMultiplier(vcpus). */
std::string hwTimeoutMultiplierArgument(unsigned vcpus);

/* The argument every release published before the launchers set the
 * multiplier carries. readKernelCommandLine() drops it for a launcher that
 * sets the multiplier, so older releases keep booting. */
inline constexpr std::string_view legacyHwTimeoutMultiplierArgument{
	"androidboot.hw_timeout_multiplier=5"};

} // namespace uml::android

#endif
