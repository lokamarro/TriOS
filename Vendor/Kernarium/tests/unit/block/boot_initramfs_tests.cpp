// SPDX-License-Identifier: GPL-2.0-or-later

#include "boot_initramfs.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::size_t headerSize = 110;
constexpr time_t hostFileModified = 1700000000;

using uml::testing::require;
using uml::testing::requireThrows;

std::vector<std::byte> bytes(std::string_view text)
{
	std::vector<std::byte> output;
	for (const char character : text)
		output.push_back(static_cast<std::byte>(character));
	return output;
}

std::string text(std::span<const std::byte> data)
{
	std::string output;
	for (const auto byte : data)
		output.push_back(static_cast<char>(byte));
	return output;
}

struct Record {
	std::size_t offset;
	std::uint32_t inode;
	std::uint32_t mode;
	std::uint32_t links;
	std::uint32_t modified;
	std::string name;
	std::string contents;
};

std::uint32_t field(std::span<const std::byte> data, std::size_t offset,
		    std::size_t index)
{
	const auto digits = text(data.subspan(offset + 6 + index * 8, 8));
	return static_cast<std::uint32_t>(std::stoul(digits, nullptr, 16));
}

std::size_t alignFour(std::size_t value)
{
	return (value + 3U) & ~std::size_t{3};
}

/* One newc segment starting at `offset`, through its trailer. */
std::vector<Record> parseSegment(std::span<const std::byte> data,
				 std::size_t &offset)
{
	std::vector<Record> records;
	for (;;) {
		require(offset % 4U == 0U, "a newc header is not four-byte aligned");
		require(offset + headerSize <= data.size(), "a newc header is cut off");
		require(text(data.subspan(offset, 6)) == "070701",
			"a newc header has the wrong magic");
		const auto size = field(data, offset, 6);
		const auto nameSize = field(data, offset, 11);
		require(nameSize >= 1, "a newc name has no terminator");
		const auto nameOffset = offset + headerSize;
		const auto name = text(data.subspan(nameOffset, nameSize - 1U));
		require(data[nameOffset + nameSize - 1U] == std::byte{0},
			"a newc name is not NUL-terminated");
		const auto dataOffset = alignFour(nameOffset + nameSize);
		Record record{
			.offset = offset,
			.inode = field(data, offset, 0),
			.mode = field(data, offset, 1),
			.links = field(data, offset, 4),
			.modified = field(data, offset, 5),
			.name = name,
			.contents = text(data.subspan(dataOffset, size)),
		};
		for (std::size_t pad = dataOffset + size;
		     pad < alignFour(dataOffset + size); ++pad)
			require(data[pad] == std::byte{0}, "newc padding is not zero");
		offset = alignFour(dataOffset + size);
		if (name == "TRAILER!!!")
			return records;
		records.push_back(std::move(record));
	}
}

struct Unpacked {
	std::map<std::string, Record> tree;
	std::vector<std::string> skipped;
	std::size_t segments{};
};

/*
 * The kernel's unpack_to_rootfs() over uncompressed segments: a '0' at a
 * four-byte aligned offset starts a newc segment, and NUL bytes between
 * segments are skipped one at a time. A regular file replaces an earlier one
 * of the same name, a directory merges, and a file whose parent directory
 * does not exist is skipped.
 */
Unpacked unpack(std::span<const std::byte> data)
{
	Unpacked result;
	std::size_t offset = 0;
	while (offset < data.size()) {
		if (data[offset] == std::byte{0}) {
			++offset;
			continue;
		}
		require(data[offset] == std::byte{'0'} && offset % 4U == 0U,
			"a segment does not start at an aligned newc header");
		++result.segments;
		for (auto &record : parseSegment(data, offset)) {
			const auto slash = record.name.rfind('/');
			if (slash != std::string::npos &&
			    !result.tree.contains(record.name.substr(0, slash))) {
				result.skipped.push_back(record.name);
				continue;
			}
			result.tree.insert_or_assign(record.name, record);
		}
	}
	return result;
}

void testRecordLayout()
{
	uml::boot::InitramfsArchive archive;
	archive.addFile("/init", bytes("abc"), 0100755, 0x1234);
	const auto data = archive.serialize();
	require(data.size() % 4U == 0U, "the segment is not padded to four bytes");
	std::size_t offset = 0;
	const auto records = parseSegment(data, offset);
	require(offset == data.size(), "bytes follow the trailer");
	require(records.size() == 1, "a single file produced extra records");
	const auto &init = records.front();
	require(init.name == "init", "the archive name keeps a leading slash");
	require(init.mode == 0100755, "the file mode is not a regular 0755 file");
	require(init.contents == "abc", "the file contents changed");
	require(init.modified == 0x1234, "the modification time changed");
	/* 110-byte header plus "init\0" is 115 bytes, padded to 116; three data
	 * bytes are padded to four; the trailer's 11-byte name pads to 124. */
	require(data.size() == 116U + 4U + 124U,
		"the record or trailer padding is wrong");
	require(text(std::span(data).subspan(0, 6)) == "070701",
		"the segment does not start with the newc magic");
}

void testParentsFirstAndLinks()
{
	uml::boot::InitramfsArchive archive;
	archive.addFile("/payload/usr/bin/tool", bytes("tool"), 0755, 1);
	archive.addFile("/payload/usr/lib/data", bytes("data"), 0644, 1);
	archive.addFile("/boot.env", bytes("UML_BOOT_ENTRY=/bin/sh\n"), 0644, 1);
	const auto data = archive.serialize();
	std::size_t offset = 0;
	const auto records = parseSegment(data, offset);
	const std::vector<std::string> expected{
		"payload", "payload/usr", "payload/usr/bin",
		"payload/usr/bin/tool", "payload/usr/lib", "payload/usr/lib/data",
		"boot.env"};
	require(records.size() == expected.size(),
		"parents were not synthesized exactly once");
	std::set<std::uint32_t> inodes;
	for (std::size_t index = 0; index < records.size(); ++index) {
		const auto &record = records[index];
		require(record.name == expected[index],
			"a parent directory does not precede its children");
		require(record.inode != 0 && inodes.insert(record.inode).second,
			"inode numbers are not unique");
		if ((record.mode & 0170000U) == 0100000U)
			require(record.links == 1,
				"a regular file is not a single link");
		else
			require(record.mode == 040755,
				"a synthesized parent is not a 0755 directory");
	}
	const auto unpacked = unpack(data);
	require(unpacked.skipped.empty(),
		"the kernel would skip a file whose parent is missing");
	require(unpacked.tree.at("payload/usr/bin/tool").contents == "tool",
		"a nested file did not unpack");
}

void testReplacementOrder()
{
	uml::boot::InitramfsArchive base;
	base.addFile("/init", bytes("base init"), 0755, 1);
	base.addFile("/bin/busybox", bytes("busybox"), 0755, 1);
	base.addDirectory("/newroot", 0755, 1);
	auto baseData = base.serialize();
	/* A base whose length is not a multiple of four: a writer that appended
	 * without padding would leave the segment at an unaligned offset. */
	baseData.push_back(std::byte{0});
	baseData.push_back(std::byte{0});
	baseData.push_back(std::byte{0});
	require(baseData.size() % 4U == 3U, "the unaligned base fixture is aligned");

	uml::boot::InitramfsArchive run;
	run.addFile("/init", bytes("run init"), 0755, 2);
	run.addFile("/payload/probe", bytes("probe"), 0755, 2);
	const auto segment = run.serialize();
	const auto initrd = uml::boot::composeInitrd(baseData, segment);
	require(initrd.size() == alignFour(baseData.size()) + segment.size(),
		"the base was not padded to four bytes");
	const auto unpacked = unpack(initrd);
	require(unpacked.segments == 2, "both segments were not unpacked");
	require(unpacked.tree.at("init").contents == "run init",
		"the per-run /init did not replace the base /init");
	require(unpacked.tree.at("bin/busybox").contents == "busybox",
		"a base file the segment does not name was lost");
	require(unpacked.tree.contains("newroot") &&
			unpacked.tree.at("payload/probe").contents == "probe",
		"base directories and segment payloads did not merge");
}

void testGzipBasePadding()
{
	const std::vector<std::byte> gzip{std::byte{0x1f}, std::byte{0x8b},
		std::byte{0x08}, std::byte{0x00}, std::byte{0x42}};
	const auto segment = bytes("0707");
	const auto initrd = uml::boot::composeInitrd(gzip, segment);
	require(initrd.size() == 12, "a five-byte base was not padded to eight");
	for (std::size_t index = 5; index < 8; ++index)
		require(initrd[index] == std::byte{0}, "base padding is not zero");
	require(text(std::span(initrd).subspan(8)) == "0707",
		"the segment does not follow the padded base");
	const auto aligned = uml::boot::composeInitrd(
		std::span(initrd).first(8), segment);
	require(aligned.size() == 12, "an aligned base was padded again");
	requireThrows<std::invalid_argument>(
		[&] { (void)uml::boot::composeInitrd(bytes("7zXZ"), segment); },
		"a base that is neither gzip nor newc was accepted");
	requireThrows<std::invalid_argument>(
		[&] { (void)uml::boot::composeInitrd({}, segment); },
		"an empty base was accepted");
}

void testRejectedPaths()
{
	for (const std::string_view path :
	     {"init", "/", "//init", "/a//b", "/a/./b", "/a/../b", "/a/"}) {
		uml::boot::InitramfsArchive archive;
		requireThrows<std::invalid_argument>(
			[&] { archive.addFile(path, bytes("x"), 0644, 0); },
			"an invalid archive path was accepted");
	}
	uml::boot::InitramfsArchive archive;
	archive.addFile("/payload/file", bytes("x"), 0644, 0);
	requireThrows<std::invalid_argument>(
		[&] { archive.addFile("/payload/file", bytes("y"), 0644, 0); },
		"a path added twice was accepted");
	requireThrows<std::invalid_argument>(
		[&] { archive.addDirectory("/payload", 0755, 0); },
		"a synthesized parent was added again");
	requireThrows<std::invalid_argument>(
		[&] { archive.addFile("/payload/file/below", bytes("z"), 0644, 0); },
		"a path below a file was accepted");
}

void testHostFilesAndOutput()
{
	const auto directory = uml::testing::scratch_directory();
	const auto source = directory / "boot-initramfs-source";
	const auto output = directory / "boot-initramfs-output";
	std::filesystem::remove(source);
	std::filesystem::remove(output);
	{
		const int descriptor = ::open(source.c_str(),
			O_WRONLY | O_CREAT | O_EXCL, 0600);
		require(descriptor >= 0, "cannot create the host file fixture");
		require(::write(descriptor, "host", 4) == 4,
			"cannot write the host file fixture");
		require(::fchmod(descriptor, 0750) == 0,
			"cannot set the host file fixture's mode");
		const timespec times[2] = {{0, UTIME_OMIT}, {hostFileModified, 0}};
		require(::futimens(descriptor, times) == 0,
			"cannot set the host file fixture's time");
		::close(descriptor);
	}
	const auto empty = directory / "boot-initramfs-empty";
	std::filesystem::remove(empty);
	{
		const int descriptor = ::open(empty.c_str(),
			O_WRONLY | O_CREAT | O_EXCL, 0600);
		require(descriptor >= 0, "cannot create the empty host file fixture");
		::close(descriptor);
	}
	uml::boot::InitramfsArchive archive;
	archive.addHostFile("/payload/host", source);
	archive.addHostFile("/payload/empty", empty, 0700);
	requireThrows<std::invalid_argument>(
		[&] { archive.addHostFile("/payload/dir", directory); },
		"a host directory was added as a file");
	requireThrows<std::system_error>(
		[&] { archive.addHostFile("/payload/missing", directory / "none"); },
		"a missing host file was accepted");
	const auto segment = archive.serialize();
	std::size_t offset = 0;
	const auto records = parseSegment(segment, offset);
	require(records.size() == 3 && records[1].contents == "host" &&
			records[1].mode == 0100750 &&
			records[1].modified == hostFileModified,
		"a host file lost its contents, permission bits or time");
	require(records[2].name == "payload/empty" && records[2].contents.empty() &&
			records[2].mode == 0100700,
		"an empty host file or a permission override was not kept");

	const auto base = bytes("0707010000");
	uml::boot::writeInitrd(output, base, segment);
	struct stat details {};
	require(::stat(output.c_str(), &details) == 0 &&
			(details.st_mode & 0777) == 0600,
		"the initrd was not created private to its owner");
	require(static_cast<std::size_t>(details.st_size) == 12U + segment.size(),
		"the written initrd is not the composed archive");
	requireThrows<std::system_error>(
		[&] { uml::boot::writeInitrd(output, base, segment); },
		"an existing initrd was replaced");
	std::filesystem::remove(source);
	std::filesystem::remove(empty);
	std::filesystem::remove(output);
}

void testArchiveFileArguments()
{
	const auto parsed = uml::boot::parseArchiveFileArgument(
		"/payload/a=/host/b=c", "--guest-file");
	require(parsed.archivePath == "/payload/a" &&
			parsed.hostFile == "/host/b=c",
		"an archive file argument split at the wrong '='");
	for (const std::string_view value : {"=/host", "/payload=", "/payload"})
		requireThrows<std::invalid_argument>(
			[&] {
				(void)uml::boot::parseArchiveFileArgument(
					value, "--guest-file");
			},
			"a malformed archive file argument was accepted");
}

} // namespace

int main()
{
	testRecordLayout();
	testParentsFirstAndLinks();
	testReplacementOrder();
	testGzipBasePadding();
	testRejectedPaths();
	testHostFilesAndOutput();
	testArchiveFileArguments();
	return 0;
}
