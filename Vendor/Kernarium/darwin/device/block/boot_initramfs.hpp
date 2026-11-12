// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_BOOT_INITRAMFS_HPP
#define UML_DEVICE_BLOCK_BOOT_INITRAMFS_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::boot {

/*
 * A launcher that boots through the per-run initramfs names this init= too.
 * The kernel runs init= only when the initramfs has no /init it can execute,
 * as when it never loaded the initrd behind a mistyped initrd_offset=, and
 * panics when that fails as well. The panic then names the missing
 * initramfs, where the kernel would otherwise fall back to the image's own
 * init or to the base archive's shell.
 */
inline constexpr std::string_view missingInitArgument{
	"init=/uml-per-run-initramfs-did-not-run"};

/*
 * One per-run initramfs segment in the kernel's "newc" cpio format.
 *
 * Linux unpacks every segment of an initrd in order into the same root, so a
 * launcher passes pad4(base archive) || segment and the segment's files
 * replace the base's (tools/rootfs/boot-ramdisk/README.md). The kernel creates
 * no missing parent directory and skips a file it cannot open, so the archive
 * emits every parent before its children. Every entry is a single link with
 * its own inode number: an entry with nlink >= 2 is hard-linked to an earlier
 * one of the same inode, and truncating it would rewrite that file too.
 *
 * Paths are absolute inside the archive ("/init", "/payload/usr/bin/tool"),
 * made of components that are neither empty, ".", nor "..". Adding a path
 * twice, or a path below a file, throws std::invalid_argument.
 */
class InitramfsArchive final {
public:
	/* Missing parent directories are added first, with mode 0755. Only the
	 * permission bits (07777) of `permissions` are kept. */
	void addFile(std::string_view path, std::span<const std::byte> contents,
		     std::uint32_t permissions, std::int64_t modified);
	void addDirectory(std::string_view path, std::uint32_t permissions,
			  std::int64_t modified);

	/* Reads `source` and adds it with its modification time and its own
	 * permission bits, or `permissions` when given. Throws
	 * std::system_error when it cannot be read and std::invalid_argument
	 * when it is not a regular file. */
	void addHostFile(std::string_view path,
			 const std::filesystem::path &source,
			 std::optional<std::uint32_t> permissions = std::nullopt);

	/* Every entry in insertion order, then the TRAILER!!! record, padded to
	 * a multiple of four bytes so another segment can follow. */
	[[nodiscard]] std::vector<std::byte> serialize() const;

private:
	struct Entry {
		std::string name;
		std::uint32_t mode;
		std::int64_t modified;
		std::vector<std::byte> contents;
	};

	void addEntry(std::string_view path, std::uint32_t mode,
		      std::int64_t modified, std::vector<std::byte> contents);

	std::vector<Entry> entries_;
};

/*
 * The whole initrd: `base` zero-padded to four bytes, then `segment`.
 *
 * After a compressed member the kernel looks for the next header at the exact
 * compressed length, and it reads a raw newc header only at a four-byte
 * aligned offset, so an unpadded base leaves the segment unread. `base` must
 * start with a gzip or newc signature; anything else throws
 * std::invalid_argument.
 */
[[nodiscard]] std::vector<std::byte> composeInitrd(
	std::span<const std::byte> base, std::span<const std::byte> segment);

/* Writes `contents` to a file this call creates with mode 0600; it never
 * replaces an existing file and removes what it wrote if it fails. Throws
 * std::system_error. */
void writeNewFile(const std::filesystem::path &path,
		  std::span<const std::byte> contents);

/* writeNewFile(path, composeInitrd(base, segment)). */
void writeInitrd(const std::filesystem::path &path,
		 std::span<const std::byte> base,
		 std::span<const std::byte> segment);

/* Splits "<archive path>=<host file>" at its first '='. Throws
 * std::invalid_argument, naming `option`, when either side is empty. */
struct ArchiveFileArgument {
	std::string archivePath;
	std::filesystem::path hostFile;
};
[[nodiscard]] ArchiveFileArgument parseArchiveFileArgument(
	std::string_view value, std::string_view option);

} // namespace uml::boot

#endif
