// SPDX-License-Identifier: GPL-2.0-or-later

#include "launcher.hpp"
#include "block/boot_initramfs.hpp"
#include "sockets/socket_paths.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

/* The accepted base archive for this build's userspace architecture and the
 * default per-run /init (tools/rootfs/boot-ramdisk), linked in beside the
 * kernel. */
extern "C" {
extern const unsigned char uml_boot_base_start[]
	__asm("section$start$__TEXT$__uml_boot_base");
extern const unsigned char uml_boot_base_end[]
	__asm("section$end$__TEXT$__uml_boot_base");
extern const unsigned char uml_boot_init_start[]
	__asm("section$start$__TEXT$__uml_boot_init");
extern const unsigned char uml_boot_init_end[]
	__asm("section$end$__TEXT$__uml_boot_init");
}

namespace {

/* The guest RAM when the launch states no memory limit; with one, the shared
 * default for that limit (main.cpp). */
constexpr std::string_view macos_default_guest_memory{"mem=8G"};
constexpr std::string_view guest_init_option{"--guest-init="};
constexpr std::string_view guest_file_option{"--guest-file="};
constexpr std::string_view scratch_prefix{"uml-guest-initrd-"};

bool has_prefix(std::span<const std::string> arguments,
		std::string_view prefix)
{
	for (const auto &argument : arguments)
		if (std::string_view{argument}.starts_with(prefix))
			return true;
	return false;
}

bool usable_rootfs(const std::filesystem::path &path)
{
	struct stat details {};
	return stat(path.c_str(), &details) == 0 && S_ISREG(details.st_mode) &&
		access(path.c_str(), R_OK) == 0;
}

std::optional<std::filesystem::path> canonical_rootfs(
	const std::filesystem::path &candidate)
{
	std::error_code error;
	auto path = std::filesystem::canonical(candidate, error);
	if (!error && usable_rootfs(path))
		return path;
	return std::nullopt;
}

std::optional<std::filesystem::path> locate_rootfs()
{
	if (const char *configured = std::getenv("UML_ROOTFS");
	    configured != nullptr && configured[0] != '\0') {
		auto path = canonical_rootfs(configured);
		if (!path)
			std::fprintf(stderr,
				"UML_ROOTFS is not a readable EROFS image: %s\n",
				configured);
		return path;
	}

	std::array<char, 4096> executable {};
	std::uint32_t length = executable.size();
	if (_NSGetExecutablePath(executable.data(), &length) == 0) {
		std::error_code error;
		auto parent = std::filesystem::canonical(executable.data(), error)
			.parent_path();
		if (!error)
			if (auto path = canonical_rootfs(parent / "rootfs.erofs"))
				return path;
	}
	if (auto path = canonical_rootfs("rootfs.erofs"))
		return path;
	std::fputs("rootfs.erofs was not found beside the executable or in the "
		"current directory\n", stderr);
	return std::nullopt;
}

struct GuestBootOptions {
	std::optional<std::filesystem::path> init;
	std::vector<uml::boot::ArchiveFileArgument> files;
	std::vector<std::string> forwarded;
};

GuestBootOptions parse_guest_boot_options(int argc, char *const argv[])
{
	GuestBootOptions options;
	options.forwarded.reserve(static_cast<std::size_t>(argc) + 2U);
	for (int index = 0; index < argc; ++index) {
		const std::string_view argument =
			argv[index] == nullptr ? std::string_view{} : argv[index];
		if (index != 0 && argument.starts_with(guest_init_option)) {
			if (options.init)
				throw std::invalid_argument(
					"--guest-init may be specified only once");
			const auto path = argument.substr(guest_init_option.size());
			if (path.empty())
				throw std::invalid_argument(
					"--guest-init requires a host file");
			options.init = std::filesystem::path(std::string(path));
			continue;
		}
		if (index != 0 && argument.starts_with(guest_file_option)) {
			options.files.push_back(uml::boot::parseArchiveFileArgument(
				argument.substr(guest_file_option.size()),
				"--guest-file"));
			continue;
		}
		options.forwarded.emplace_back(argument);
	}
	return options;
}

/* A kernel command-line argument ends at whitespace and treats a double quote
 * as quoting, so the initrd path may contain neither. */
void require_kernel_argument_path(const std::filesystem::path &path)
{
	for (const unsigned char character : path.string())
		if (character <= ' ' || character == '"' || character >= 0x7f)
			throw std::invalid_argument(
				"the temporary directory cannot be named on the "
				"kernel command line: " + path.string());
}

std::filesystem::path temporary_directory()
{
	if (const char *configured = std::getenv("TMPDIR");
	    configured != nullptr && configured[0] == '/')
		return configured;
	if (auto user = uml::device::user_temporary_directory(); !user.empty())
		return user;
	return "/tmp";
}

/* A run killed outright leaves its scratch directory behind; the next launch
 * removes those whose process has exited. */
void remove_abandoned_scratch(const std::filesystem::path &directory)
{
	std::error_code error;
	for (const auto &entry :
	     std::filesystem::directory_iterator(directory, error)) {
		const auto name = entry.path().filename().string();
		if (!name.starts_with(scratch_prefix))
			continue;
		const auto digits = std::string_view{name}.substr(
			scratch_prefix.size());
		pid_t owner = 0;
		const auto [end, parsed] = std::from_chars(
			digits.data(), digits.data() + digits.size(), owner);
		if (parsed != std::errc{} || end == digits.data() ||
		    end == digits.data() + digits.size() || *end != '-' ||
		    owner <= 0)
			continue;
		if (kill(owner, 0) == 0 || errno != ESRCH)
			continue;
		std::error_code ignored;
		std::filesystem::remove_all(entry.path(), ignored);
	}
}

/* The per-run segment: the chosen /init and every --guest-file. */
std::vector<std::byte> per_run_segment(const GuestBootOptions &options)
{
	uml::boot::InitramfsArchive archive;
	if (options.init) {
		archive.addHostFile("/init", *options.init, 0755);
	} else {
		const auto init = std::as_bytes(std::span{
			uml_boot_init_start,
			static_cast<std::size_t>(uml_boot_init_end -
						 uml_boot_init_start)});
		archive.addFile("/init", init, 0755, 0);
	}
	for (const auto &file : options.files)
		archive.addHostFile(file.archivePath, file.hostFile);
	return archive.serialize();
}

/* Writes base || segment as "initrd.cpio" in a private directory, which
 * `owner` then holds, and returns that directory. */
std::filesystem::path write_run_initrd(std::span<const std::byte> segment,
				       std::optional<uml::device::DirectoryPath> &owner)
{
	const auto directory = temporary_directory();
	remove_abandoned_scratch(directory);
	auto pattern = (directory / (std::string(scratch_prefix) +
				     std::to_string(getpid()) + "-XXXXXX"))
			       .string();
	if (mkdtemp(pattern.data()) == nullptr)
		throw std::system_error(errno, std::generic_category(),
					"cannot create " + pattern);
	const std::filesystem::path scratch{pattern};
	try {
		const auto initrd = scratch / "initrd.cpio";
		require_kernel_argument_path(initrd);
		const auto base = std::as_bytes(std::span{
			uml_boot_base_start,
			static_cast<std::size_t>(uml_boot_base_end -
						 uml_boot_base_start)});
		uml::boot::writeInitrd(initrd, base, segment);
		/* A terminating signal or a kernel-driven exit, which unwinds
		 * nothing, removes it too. */
		owner.emplace(scratch);
		owner->adopt(initrd.filename());
	} catch (...) {
		owner.reset();
		std::error_code ignored;
		std::filesystem::remove_all(scratch, ignored);
		throw;
	}
	return scratch;
}

void print_usage()
{
	std::fputs(
		"Guest boot options:\n"
		"  --guest-init=/host/file         per-run /init (default: the "
		"built-in init)\n"
		"  --guest-file=/archive/path=/host/file\n"
		"                                  add a file to the per-run "
		"initramfs\n"
		"Without --guest-* options the launcher still boots its base "
		"archive and the\n"
		"built-in init, unless the command line names init= or initrd= "
		"itself.\n",
		stderr);
}

} // namespace

int main(int argc, char *argv[], char *envp[])
{
	GuestBootOptions options;
	std::optional<uml::device::DirectoryPath> scratch_owner;
	std::filesystem::path scratch;
	try {
		const bool information_only = uml::app::informational(argc, argv);
		if (uml::app::has_argument(argc, argv, "--help"))
			print_usage();
		options = parse_guest_boot_options(argc, argv);
		const std::span<const std::string> kernel{options.forwarded};
		const bool requested = options.init || !options.files.empty();
		if (requested && has_prefix(kernel, "initrd="))
			throw std::invalid_argument(
				"--guest-init and --guest-file build the initrd; do "
				"not also pass initrd=");
		const bool caller_owns_boot =
			has_prefix(kernel, "init=") || has_prefix(kernel, "initrd=");
		if (!information_only && (requested || !caller_owns_boot)) {
			const bool explicit_init = has_prefix(kernel, "init=");
			scratch = write_run_initrd(per_run_segment(options),
						   scratch_owner);
			options.forwarded.push_back(
				"initrd=" + (scratch / "initrd.cpio").string());
			if (!explicit_init)
				options.forwarded.emplace_back(uml::boot::missingInitArgument);
		}
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to prepare the guest initramfs: %s\n",
			     error.what());
		return 1;
	}

	std::string rootfs;
	if (!uml::app::informational(argc, argv) &&
	    !has_prefix(options.forwarded, "ubd0")) {
		auto located = locate_rootfs();
		if (!located) {
			std::error_code ignored;
			std::filesystem::remove_all(scratch, ignored);
			return 1;
		}
		rootfs = located->string();
	}
	const int status = uml::app::runEmbeddedKernel(
		options.forwarded, envp,
		{
			.rootfs_path = rootfs.empty() ? nullptr : rootfs.c_str(),
			.default_guest_memory = macos_default_guest_memory,
			.run_scratch = scratch,
		});
	if (!scratch.empty()) {
		std::error_code ignored;
		std::filesystem::remove_all(scratch, ignored);
	}
	return status;
}
