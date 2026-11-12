// SPDX-License-Identifier: GPL-2.0-or-later

#include "boot_initramfs.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
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

namespace uml::boot {
namespace {

constexpr std::uint32_t regularFileType = 0100000;
constexpr std::uint32_t directoryType = 0040000;
constexpr std::uint32_t permissionBits = 07777;
constexpr std::uint32_t defaultDirectoryPermissions = 0755;
/* The kernel's name buffer is PATH_MAX including the terminating NUL. */
constexpr std::size_t maximumNameLength = 4095;
constexpr std::string_view newcMagic = "070701";
constexpr std::string_view trailerName = "TRAILER!!!";

void throwErrno(int error, const std::string &what)
{
	throw std::system_error(error, std::generic_category(), what);
}

/* "/a/b" -> "a/b", checking every component. */
std::string archiveName(std::string_view path)
{
	if (path.size() < 2 || path.front() != '/')
		throw std::invalid_argument(
			"initramfs path must be absolute and below /: " +
			std::string(path));
	const auto name = path.substr(1);
	if (name.size() > maximumNameLength)
		throw std::invalid_argument("initramfs path is too long: " +
					    std::string(path));
	std::size_t start = 0;
	while (start <= name.size()) {
		const auto end = std::min(name.find('/', start), name.size());
		const auto component = name.substr(start, end - start);
		if (component.empty() || component == "." || component == ".." ||
		    component.find('\0') != std::string_view::npos)
			throw std::invalid_argument(
				"initramfs path has an invalid component: " +
				std::string(path));
		start = end + 1;
	}
	return std::string(name);
}

std::uint32_t headerTime(std::int64_t modified)
{
	return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
		modified, 0, std::numeric_limits<std::uint32_t>::max()));
}

void appendHex(std::vector<std::byte> &output, std::uint32_t value)
{
	constexpr std::string_view digits = "0123456789ABCDEF";
	for (int shift = 28; shift >= 0; shift -= 4)
		output.push_back(static_cast<std::byte>(
			digits[(value >> static_cast<unsigned>(shift)) & 0xfU]));
}

void appendText(std::vector<std::byte> &output, std::string_view text)
{
	for (const char character : text)
		output.push_back(static_cast<std::byte>(character));
}

void padToFour(std::vector<std::byte> &output)
{
	while (output.size() % 4U != 0U)
		output.push_back(std::byte{0});
}

struct Header {
	std::uint32_t inode;
	std::uint32_t mode;
	std::uint32_t links;
	std::uint32_t modified;
	std::uint32_t size;
	std::string_view name;
};

void appendRecord(std::vector<std::byte> &output, const Header &header,
		  std::span<const std::byte> contents)
{
	appendText(output, newcMagic);
	appendHex(output, header.inode);
	appendHex(output, header.mode);
	appendHex(output, 0); /* uid */
	appendHex(output, 0); /* gid */
	appendHex(output, header.links);
	appendHex(output, header.modified);
	appendHex(output, header.size);
	appendHex(output, 0); /* devmajor */
	appendHex(output, 0); /* devminor */
	appendHex(output, 0); /* rdevmajor */
	appendHex(output, 0); /* rdevminor */
	appendHex(output, static_cast<std::uint32_t>(header.name.size() + 1U));
	appendHex(output, 0); /* check */
	appendText(output, header.name);
	output.push_back(std::byte{0});
	padToFour(output);
	output.insert(output.end(), contents.begin(), contents.end());
	padToFour(output);
}

void writeAll(int descriptor, std::span<const std::byte> bytes,
	      const std::filesystem::path &path)
{
	while (!bytes.empty()) {
		const auto written = ::write(descriptor, bytes.data(), bytes.size());
		if (written < 0) {
			if (errno == EINTR)
				continue;
			throwErrno(errno, "cannot write " + path.string());
		}
		bytes = bytes.subspan(static_cast<std::size_t>(written));
	}
}

bool startsWith(std::span<const std::byte> bytes, std::string_view prefix)
{
	if (bytes.size() < prefix.size())
		return false;
	for (std::size_t index = 0; index < prefix.size(); ++index)
		if (bytes[index] != static_cast<std::byte>(prefix[index]))
			return false;
	return true;
}

} // namespace

void InitramfsArchive::addEntry(std::string_view path, std::uint32_t mode,
				std::int64_t modified,
				std::vector<std::byte> contents)
{
	auto name = archiveName(path);
	if (contents.size() > std::numeric_limits<std::uint32_t>::max())
		throw std::invalid_argument(
			"initramfs file is 4 GiB or larger: " + std::string(path));
	const auto existing = [&](std::string_view candidate) {
		return std::ranges::find_if(entries_, [&](const Entry &entry) {
			return entry.name == candidate;
		});
	};
	if (existing(name) != entries_.end())
		throw std::invalid_argument("initramfs path added twice: " +
					    std::string(path));
	for (auto slash = name.find('/'); slash != std::string::npos;
	     slash = name.find('/', slash + 1)) {
		const auto parent = std::string_view(name).substr(0, slash);
		const auto found = existing(parent);
		if (found == entries_.end()) {
			entries_.push_back(Entry{
				.name = std::string(parent),
				.mode = directoryType | defaultDirectoryPermissions,
				.modified = modified,
				.contents = {},
			});
		} else if ((found->mode & directoryType) == 0U) {
			throw std::invalid_argument(
				"initramfs path lies below a file: " +
				std::string(path));
		}
	}
	entries_.push_back(Entry{
		.name = std::move(name),
		.mode = mode,
		.modified = modified,
		.contents = std::move(contents),
	});
}

void InitramfsArchive::addFile(std::string_view path,
			       std::span<const std::byte> contents,
			       std::uint32_t permissions, std::int64_t modified)
{
	addEntry(path, regularFileType | (permissions & permissionBits), modified,
		 std::vector<std::byte>(contents.begin(), contents.end()));
}

void InitramfsArchive::addDirectory(std::string_view path,
				    std::uint32_t permissions,
				    std::int64_t modified)
{
	addEntry(path, directoryType | (permissions & permissionBits), modified,
		 {});
}

void InitramfsArchive::addHostFile(std::string_view path,
				   const std::filesystem::path &source,
				   std::optional<std::uint32_t> permissions)
{
	const int descriptor = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		throwErrno(errno, "cannot open " + source.string());
	struct Closer {
		int descriptor;
		~Closer() { ::close(descriptor); }
	} closer{descriptor};
	struct stat details {};
	if (::fstat(descriptor, &details) != 0)
		throwErrno(errno, "cannot inspect " + source.string());
	if (!S_ISREG(details.st_mode))
		throw std::invalid_argument("not a regular file: " +
					    source.string());
	if (static_cast<std::uint64_t>(details.st_size) >
	    std::numeric_limits<std::uint32_t>::max())
		throw std::invalid_argument("initramfs file is 4 GiB or larger: " +
					    source.string());
	std::vector<std::byte> contents(static_cast<std::size_t>(details.st_size));
	std::size_t filled = 0;
	while (filled < contents.size()) {
		const auto count = ::read(descriptor, contents.data() + filled,
					  contents.size() - filled);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			throwErrno(errno, "cannot read " + source.string());
		}
		if (count == 0)
			throw std::invalid_argument("file shrank while read: " +
						    source.string());
		filled += static_cast<std::size_t>(count);
	}
	addFile(path, contents,
		permissions.value_or(static_cast<std::uint32_t>(details.st_mode)),
		details.st_mtime);
}

std::vector<std::byte> InitramfsArchive::serialize() const
{
	std::vector<std::byte> output;
	std::uint32_t inode = 0;
	for (const auto &entry : entries_) {
		const bool directory = (entry.mode & directoryType) != 0U;
		appendRecord(output,
			     Header{
				     .inode = ++inode,
				     .mode = entry.mode,
				     .links = directory ? 2U : 1U,
				     .modified = headerTime(entry.modified),
				     .size = static_cast<std::uint32_t>(
					     entry.contents.size()),
				     .name = entry.name,
			     },
			     entry.contents);
	}
	appendRecord(output,
		     Header{
			     .inode = 0,
			     .mode = 0,
			     .links = 1,
			     .modified = 0,
			     .size = 0,
			     .name = trailerName,
		     },
		     {});
	return output;
}

std::vector<std::byte> composeInitrd(std::span<const std::byte> base,
				     std::span<const std::byte> segment)
{
	constexpr std::array gzipMagic{std::byte{0x1f}, std::byte{0x8b}};
	const bool gzip = base.size() >= gzipMagic.size() &&
		std::ranges::equal(base.first(gzipMagic.size()), gzipMagic);
	if (!gzip && !startsWith(base, newcMagic))
		throw std::invalid_argument(
			"initramfs base is neither gzip nor newc cpio");
	std::vector<std::byte> output(base.begin(), base.end());
	padToFour(output);
	output.insert(output.end(), segment.begin(), segment.end());
	return output;
}

void writeNewFile(const std::filesystem::path &path,
		  std::span<const std::byte> contents)
{
	const int descriptor = ::open(path.c_str(),
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (descriptor < 0)
		throwErrno(errno, "cannot create " + path.string());
	try {
		writeAll(descriptor, contents, path);
	} catch (...) {
		::close(descriptor);
		::unlink(path.c_str());
		throw;
	}
	if (::close(descriptor) != 0) {
		const int error = errno;
		::unlink(path.c_str());
		throwErrno(error, "cannot write " + path.string());
	}
}

void writeInitrd(const std::filesystem::path &path,
		 std::span<const std::byte> base,
		 std::span<const std::byte> segment)
{
	writeNewFile(path, composeInitrd(base, segment));
}

ArchiveFileArgument parseArchiveFileArgument(std::string_view value,
					     std::string_view option)
{
	const auto separator = value.find('=');
	if (separator == std::string_view::npos || separator == 0 ||
	    separator + 1 == value.size())
		throw std::invalid_argument(
			std::string(option) +
			" requires <archive path>=<host file>");
	return ArchiveFileArgument{
		.archivePath = std::string(value.substr(0, separator)),
		.hostFile = std::filesystem::path(
			std::string(value.substr(separator + 1))),
	};
}

} // namespace uml::boot
