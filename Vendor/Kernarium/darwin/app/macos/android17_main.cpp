// SPDX-License-Identifier: GPL-2.0-or-later

#include "launcher.hpp"
#include "block/android_boot.hpp"
#include "options.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

/* The guest RAM when the launch states no memory limit; with one, the shared
 * default for that limit (main.cpp). */
constexpr std::string_view android_default_guest_memory{"mem=4G"};
constexpr std::string_view release_option{"--android-release="};
constexpr std::string_view data_disk_option{"--android-data-disk="};
constexpr std::string_view data_gib_option{"--android-data-gib="};
constexpr std::string_view renamed_disk_option{"--android-disk="};
constexpr std::string_view boot_mode_option{"--android-boot-mode="};
constexpr std::string_view dpi_option{"--gpu-dpi="};
constexpr std::string_view adb_authorization_prefix{
	"androidboot.kernarium.adb_auth="};

struct AndroidOptions {
	std::filesystem::path release;
	std::filesystem::path data_disk;
	/* 0 keeps an existing data disk's capacity and gives a new one the
	 * release's default. */
	std::uint32_t data_gib = 0;
	uml::android::BootMode boot_mode = uml::android::BootMode::automatic;
	bool boot_mode_set = false;
	/* The EDID density the GPU device advertises; 0 advertises none. */
	std::uint32_t dots_per_inch = 0;
	std::vector<std::string> forwarded;
};

uml::android::BootMode parse_boot_mode(std::string_view value)
{
	if (value == "auto")
		return uml::android::BootMode::automatic;
	if (value == "normal")
		return uml::android::BootMode::normal;
	if (value == "recovery")
		return uml::android::BootMode::recovery;
	throw std::invalid_argument("Android boot mode must be auto, normal, or recovery");
}

std::uint32_t parse_data_gib(std::string_view text)
{
	std::uint32_t gib = 0;
	const auto [end, error] =
		std::from_chars(text.data(), text.data() + text.size(), gib);
	if (error != std::errc{} || end != text.data() + text.size() || gib == 0)
		throw std::invalid_argument(
			"--android-data-gib must be a positive number of GiB");
	return gib;
}

std::string environment_value(const char *name)
{
	const char *value = std::getenv(name);
	return value == nullptr ? std::string{} : std::string{value};
}

AndroidOptions parse_options(int argc, char *const argv[])
{
	AndroidOptions options;
	options.forwarded.reserve(static_cast<std::size_t>(argc) + 32U);
	options.forwarded.emplace_back(argv[0] == nullptr ? "uml-darwin-android17" :
							 argv[0]);
	bool has_gpu = false;
	bool has_gpu_display = false;
	bool has_audio = false;
	bool has_battery = false;
	bool has_video = false;
	bool has_video_encoder = false;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument =
			argv[index] == nullptr ? std::string_view{} : argv[index];
		if (argument.starts_with(adb_authorization_prefix))
			throw std::invalid_argument(
				"macOS fixes Android ADB authorization at boot");
		if (argument.starts_with(uml::android::lcdDensityPrefix))
			throw std::invalid_argument(
				"the Android density follows --gpu-dpi");
		if (argument.starts_with(uml::android::hwTimeoutMultiplierPrefix))
			throw std::invalid_argument(
				"the Android timeout multiplier follows ncpus=");
		if (argument.starts_with(dpi_option))
			options.dots_per_inch = uml::android::parseDotsPerInch(
				argument.substr(dpi_option.size()));
		if (argument.starts_with(release_option)) {
			if (!options.release.empty())
				throw std::invalid_argument(
					"--android-release may be specified only once");
			options.release = argument.substr(release_option.size());
			if (options.release.empty())
				throw std::invalid_argument(
					"--android-release requires a path");
			continue;
		}
		if (argument.starts_with(renamed_disk_option))
			throw std::invalid_argument(
				"--android-disk is now --android-data-disk, the instance's "
				"writable disk; the release carries the read-only system disk");
		if (argument.starts_with(data_disk_option)) {
			if (!options.data_disk.empty())
				throw std::invalid_argument(
					"--android-data-disk may be specified only once");
			options.data_disk = argument.substr(data_disk_option.size());
			if (options.data_disk.empty())
				throw std::invalid_argument(
					"--android-data-disk requires a path");
			continue;
		}
		if (argument.starts_with(data_gib_option)) {
			if (options.data_gib != 0)
				throw std::invalid_argument(
					"--android-data-gib may be specified only once");
			options.data_gib = parse_data_gib(
				argument.substr(data_gib_option.size()));
			continue;
		}
		if (argument.starts_with(boot_mode_option)) {
			if (options.boot_mode_set)
				throw std::invalid_argument(
					"--android-boot-mode may be specified only once");
			options.boot_mode = parse_boot_mode(
				argument.substr(boot_mode_option.size()));
			options.boot_mode_set = true;
			continue;
		}
		has_gpu |= argument == "--gpu";
		has_gpu_display |= argument.starts_with("--gpu-display=");
		has_audio |= argument == "--audio" ||
			     argument.starts_with("--audio-output=") ||
			     argument.starts_with("--audio-input=");
		has_battery |= argument == "--battery";
		has_video |= argument == "--video";
		has_video_encoder |= argument == "--video-encoder";
		options.forwarded.emplace_back(argument);
	}
	if (options.release.empty())
		options.release = environment_value("UML_ANDROID17_RELEASE");
	if (options.data_disk.empty())
		options.data_disk = environment_value("UML_ANDROID17_DATA_DISK");
	if (!environment_value("UML_ANDROID17_DISK").empty())
		throw std::invalid_argument(
			"UML_ANDROID17_DISK is now UML_ANDROID17_DATA_DISK, the "
			"instance's writable disk");
	if (!options.boot_mode_set) {
		const auto environment_mode = environment_value("UML_ANDROID17_BOOT_MODE");
		if (!environment_mode.empty())
			options.boot_mode = parse_boot_mode(environment_mode);
	}
	if (!has_gpu)
		options.forwarded.emplace_back("--gpu");
	if (!has_gpu_display)
		options.forwarded.emplace_back("--gpu-display=native");
	if (!has_audio)
		options.forwarded.emplace_back("--audio");
	if (!has_battery)
		options.forwarded.emplace_back("--battery");
	if (!has_video)
		options.forwarded.emplace_back("--video");
	if (!has_video_encoder)
		options.forwarded.emplace_back("--video-encoder");
	return options;
}

std::filesystem::path validated_data_disk(
	const std::filesystem::path &candidate)
{
	if (!candidate.is_absolute())
		throw std::invalid_argument("--android-data-disk must be absolute");
	return candidate;
}

std::filesystem::path validated_release_root(
	const std::filesystem::path &candidate)
{
	if (!candidate.is_absolute())
		throw std::invalid_argument("--android-release must be absolute");
	struct stat candidate_details {};
	if (lstat(candidate.c_str(), &candidate_details) != 0 ||
	    !S_ISDIR(candidate_details.st_mode))
		throw std::invalid_argument(
			"Android release must be a real local directory");
	std::error_code error;
	auto release = std::filesystem::canonical(candidate, error);
	if (error)
		throw std::invalid_argument("Android release cannot be resolved");
	return release;
}

void print_usage()
{
	std::fputs(
		"Android 17 options:\n"
			"  --android-release=/absolute/release (system disk, layout, command line)\n"
			"  --android-data-disk=/absolute/data.img (the instance's writable disk;\n"
			"      created when absent)\n"
			"  --android-data-gib=N (capacity of a new data disk, or growth of an\n"
			"      existing one; a disk never shrinks)\n"
			"  --android-boot-mode=auto|normal|recovery\n"
			"  --gpu-dpi=N (also the guest density; 213 without it)\n"
			"  ncpus=N (also the guest's timeout multiplier: 3 from six vCPUs,\n"
			"      5 below)\n"
			"  --virtio-console-socket=/absolute/console.sock\n"
			"The same values may be supplied through UML_ANDROID17_RELEASE, "
			"UML_ANDROID17_DATA_DISK, "
			"and UML_ANDROID17_BOOT_MODE.\n"
			"The guest has exactly the release's system disk and this data disk; "
			"ubd, initrd, root and boot-device arguments are refused.\n"
			"The boot mode picks the first boot's ramdisk. A guest that "
			"restarts itself boots again in the same process, and each "
			"restart picks the ramdisk in auto mode.\n",
		stderr);
}

/* The kernel arguments that attach the guest's two disks and give it `boot`'s
 * ramdisk, appended to the host's arguments. */
std::vector<std::string> boot_command_line(
	const std::vector<std::string> &host_arguments,
	const uml::android::Storage &storage, const uml::android::BootSelection &boot,
	std::uint32_t dots_per_inch)
{
	auto forwarded = host_arguments;
	for (auto &argument : storage.kernelArguments(boot))
		forwarded.push_back(std::move(argument));
	constexpr std::array launcher_prefixes{
		adb_authorization_prefix,
		uml::android::lcdDensityPrefix,
		uml::android::hwTimeoutMultiplierPrefix};
	for (auto &argument : uml::android::readKernelCommandLine(
		     storage.release() / uml::android::releaseCommandLine,
		     launcher_prefixes))
		forwarded.push_back(std::move(argument));
	/* Android init imports this as ro.boot.lcd_density, which the vendor
	 * init script publishes as ro.sf.lcd_density: a guest without it has an
	 * empty property. */
	forwarded.emplace_back(uml::android::lcdDensityArgument(dots_per_inch));
	/* The framework scales its ANR, watchdog and input timeouts by this: the
	 * vendor init script publishes it as ro.hw_timeout_multiplier. It follows
	 * the vCPU count the launch boots with, which main.cpp defaults when the
	 * host states none. */
	forwarded.emplace_back(uml::android::hwTimeoutMultiplierArgument(
		uml::emulator::launch_guest_cpu_count(host_arguments)));
	forwarded.emplace_back(std::string{adb_authorization_prefix} + "0");
	std::fprintf(
		stderr,
			"Android rootfs %s, %s boot, system disk %s, data disk %s\n",
			storage.release().c_str(),
			boot.mode == uml::android::BootMode::recovery
				? "recovery" : "normal",
			storage.systemDisk().c_str(), storage.dataDisk().c_str());
	return forwarded;
}

void announce_data_disk(const uml::android::PreparedDataDisk &prepared)
{
	switch (prepared.change) {
	case uml::android::DataDiskChange::created:
		std::fprintf(stderr, "Android: created a %u GiB data disk\n",
			     prepared.capacityGiB);
		break;
	case uml::android::DataDiskChange::grown:
		std::fprintf(stderr, "Android: grew the data disk to %u GiB\n",
			     prepared.capacityGiB);
		break;
	case uml::android::DataDiskChange::none:
		break;
	}
}

/* There is no bootloader to take the request `reboot bootloader` leaves, and
 * left in place it stops init from writing the next `reboot recovery`'s. */
void say_bootloader_consumed()
{
	std::fputs("Android: the guest asked for the bootloader; there is none, "
		   "so the request is cleared and the guest boots normally\n",
		   stderr);
}

} // namespace

int main(int argc, char *argv[], char *envp[])
{
	try {
		const bool information_only = uml::app::informational(argc, argv);
		if (uml::app::has_argument(argc, argv, "--help"))
			print_usage();
		auto options = parse_options(argc, argv);
		uml::app::LaunchConfiguration configuration{
			.rootfs_path = nullptr,
			.default_guest_memory = android_default_guest_memory,
			.top_byte_ignore = true,
			.input_mode = uml::app::DisplayInput::touchscreen,
		};
		if (!information_only) {
			if (options.release.empty() || options.data_disk.empty())
				throw std::invalid_argument(
					"Android launch requires release and data disk paths");
			const auto release = validated_release_root(options.release);
			const auto data_disk = validated_data_disk(options.data_disk);
			/* The host's own arguments follow argv[0]; none may attach a
			 * disk. */
			uml::android::rejectStorageArguments(
				std::span{options.forwarded}.subspan(1));
			announce_data_disk(uml::android::prepareDataDisk(
				uml::android::readLayout(
					release / uml::android::releaseLayout),
				data_disk, options.data_gib));
			const uml::android::Storage storage{release, data_disk};
			if (storage.consumeBootloaderCommand())
				say_bootloader_consumed();
			const auto boot = storage.selectBoot(options.boot_mode);
			const auto host_arguments = options.forwarded;
			options.forwarded =
				boot_command_line(
					host_arguments, storage, boot, options.dots_per_inch);
			/* The mode the user gave picks the first boot's ramdisk only: a
			 * guest that restarts has said what it wants in the disk's
			 * bootloader message. */
			configuration.restart_command_line =
				[host_arguments, storage,
				 dots_per_inch = options.dots_per_inch] {
					const auto next = storage.selectRestartBoot();
					if (next.bootloaderConsumed)
						say_bootloader_consumed();
					return boot_command_line(
						host_arguments, storage, next.boot, dots_per_inch);
				};
		}
		return uml::app::runEmbeddedKernel(
			options.forwarded, envp, configuration);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to configure Android guest: %s\n",
			     error.what());
		return 1;
	}
}
