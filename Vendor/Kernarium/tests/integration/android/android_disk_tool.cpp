// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The host's side of an Android guest's disks, for the integration runner.
 *
 *   prepare RELEASE DATA_DISK GIB   creates the data disk (or grows it) with
 *                                   prepareDataDisk, as both launchers do,
 *                                   then checks both disks
 *   check RELEASE DATA_DISK         checks both disks
 *
 * Both print `system-disk=PATH`, `data-disk=PATH` and `capacity-gib=N`, the
 * disks a launcher attaches and nothing else, and exit with status 1 and the
 * reason on standard error when the library refuses a disk. The runner boots a
 * guest on them and then asks again, so a guest that disturbed a partition
 * table is found by the library's own check.
 */
#include "android_boot.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace android = uml::android;

std::filesystem::path absolutePath(const char *text)
{
	return std::filesystem::absolute(std::filesystem::path(text));
}

std::uint32_t parseGiB(const char *text)
{
	const std::string value(text);
	std::size_t used = 0;
	const unsigned long gib = std::stoul(value, &used, 10);
	if (used != value.size() || gib == 0 || gib > 4096)
		throw std::invalid_argument("capacity is not a number of GiB");
	return static_cast<std::uint32_t>(gib);
}

int run(int argc, char **argv)
{
	const std::string_view command = argc > 1 ? argv[1] : "";
	const bool prepare = command == "prepare" && argc == 5;
	if (!prepare && !(command == "check" && argc == 4)) {
		std::cerr << "usage: android_disk_tool prepare RELEASE DATA_DISK GIB\n"
			  << "       android_disk_tool check RELEASE DATA_DISK\n";
		return 2;
	}
	const auto release = absolutePath(argv[2]);
	const auto dataDisk = absolutePath(argv[3]);
	if (prepare) {
		const auto layout = android::readLayout(
			release / android::releaseLayout);
		(void)android::prepareDataDisk(layout, dataDisk, parseGiB(argv[4]));
	}
	const android::Storage storage(release, dataDisk);
	std::cout << "system-disk=" << storage.systemDisk().string() << '\n'
		  << "data-disk=" << storage.dataDisk().string() << '\n'
		  << "capacity-gib="
		  << android::validateDataDisk(storage.layout(), storage.dataDisk())
		  << '\n';
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	try {
		return run(argc, argv);
	} catch (const std::exception &error) {
		std::cerr << "android_disk_tool: " << error.what() << '\n';
		return 1;
	}
}
