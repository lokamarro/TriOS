// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Writes a guest's per-run initramfs without an emulator, for hosts that boot
 * the guest elsewhere, such as a Mac preparing a run for an iOS device. The
 * archive format and its parameters are tools/rootfs/boot-ramdisk/README.md.
 */

#include "boot_initramfs.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::string_view output_option{"--output="};
constexpr std::string_view base_option{"--base="};
constexpr std::string_view init_option{"--init="};
constexpr std::string_view file_option{"--file="};

void print_usage()
{
	std::fputs(
		"usage: uml-initramfs --output=/new/file [--base=/base.cpio.gz]\n"
		"                     [--init=/host/file] "
		"[--file=/archive/path=/host/file]...\n"
		"Writes the per-run initramfs segment, or with --base the whole "
		"initrd\n"
		"(the base padded to four bytes, then the segment). The output "
		"must not exist.\n",
		stderr);
}

std::string option_value(std::string_view argument, std::string_view option,
			  bool present)
{
	if (present)
		throw std::invalid_argument(std::string(option) +
					    " may be specified only once");
	const auto value = argument.substr(option.size());
	if (value.empty())
		throw std::invalid_argument(std::string(option) +
					    " requires a path");
	return std::string(value);
}

std::vector<std::byte> read_file(const std::filesystem::path &path)
{
	const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		throw std::runtime_error("cannot open " + path.string());
	struct stat details {};
	if (::fstat(descriptor, &details) != 0 || !S_ISREG(details.st_mode)) {
		::close(descriptor);
		throw std::runtime_error("not a readable regular file: " +
					 path.string());
	}
	std::vector<std::byte> contents(static_cast<std::size_t>(details.st_size));
	std::size_t filled = 0;
	while (filled < contents.size()) {
		const auto count = ::read(descriptor, contents.data() + filled,
					  contents.size() - filled);
		if (count <= 0) {
			::close(descriptor);
			throw std::runtime_error("cannot read " + path.string());
		}
		filled += static_cast<std::size_t>(count);
	}
	::close(descriptor);
	return contents;
}

} // namespace

int main(int argc, char *argv[])
{
	try {
		std::optional<std::filesystem::path> output;
		std::optional<std::filesystem::path> base;
		std::optional<std::filesystem::path> init;
		std::vector<uml::boot::ArchiveFileArgument> files;
		for (int index = 1; index < argc; ++index) {
			const std::string_view argument{argv[index]};
			if (argument == "--help" || argument == "-h") {
				print_usage();
				return 0;
			}
			if (argument.starts_with(output_option))
				output = option_value(argument, output_option,
						      output.has_value());
			else if (argument.starts_with(base_option))
				base = option_value(argument, base_option,
						    base.has_value());
			else if (argument.starts_with(init_option))
				init = option_value(argument, init_option,
						    init.has_value());
			else if (argument.starts_with(file_option))
				files.push_back(uml::boot::parseArchiveFileArgument(
					argument.substr(file_option.size()),
					"--file"));
			else
				throw std::invalid_argument(
					"unknown argument: " + std::string(argument));
		}
		if (!output) {
			print_usage();
			return 2;
		}
		uml::boot::InitramfsArchive archive;
		if (init)
			archive.addHostFile("/init", *init, 0755);
		for (const auto &file : files)
			archive.addHostFile(file.archivePath, file.hostFile);
		const auto segment = archive.serialize();
		if (base)
			uml::boot::writeInitrd(*output, read_file(*base), segment);
		else
			uml::boot::writeNewFile(*output, segment);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "uml-initramfs: %s\n", error.what());
		return 1;
	}
	return 0;
}
