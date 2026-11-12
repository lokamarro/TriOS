// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_data_disk.hpp"
#include "ubd_path.hpp"

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uml::android {
namespace {

[[noreturn]] void failWith(int error, const std::string &what)
{
	throw std::system_error(error, std::generic_category(), what);
}

class Descriptor final {
public:
	Descriptor(const std::filesystem::path &path, int flags, mode_t mode = 0)
		: descriptor_(open(path.c_str(), flags, mode))
	{
		if (descriptor_ < 0)
			failWith(errno, "cannot open " + path.string());
	}
	~Descriptor()
	{
		if (descriptor_ >= 0)
			(void)close(descriptor_);
	}
	Descriptor(const Descriptor &) = delete;
	Descriptor &operator=(const Descriptor &) = delete;

	int get() const noexcept { return descriptor_; }

	void closeChecked(const std::string &what)
	{
		const int result = close(descriptor_);
		descriptor_ = -1;
		if (result != 0)
			failWith(errno, what);
	}

private:
	int descriptor_;
};

/* A temporary name beside `path` that no other launcher of the same disk
 * shares. */
std::filesystem::path siblingName(const std::filesystem::path &path,
				  const char *purpose)
{
	return path.parent_path() /
	       ("." + path.filename().string() + "." + purpose + "." +
		std::to_string(getpid()));
}

void flushDirectory(const std::filesystem::path &path)
{
	const auto directory = path.parent_path().empty()
		? std::filesystem::path{"."} : path.parent_path();
	Descriptor descriptor(directory, O_RDONLY | O_DIRECTORY);
	(void)fsync(descriptor.get());
}

void writeDisk(const Descriptor &disk, const GptImage &image)
{
	writeGpt(disk.get(), image);
	if (fsync(disk.get()) != 0)
		failWith(errno, "cannot flush the data disk");
}

} // namespace

bool lockedByAnotherProcess(const std::filesystem::path &path)
{
	Descriptor descriptor(path, O_RDONLY);
	struct flock request {};
	request.l_type = F_WRLCK;
	request.l_whence = SEEK_SET;
	if (fcntl(descriptor.get(), F_GETLK, &request) != 0)
		failWith(errno, "cannot query the locks of " + path.string());
	return request.l_type != F_UNLCK;
}

std::uint32_t validateDataDisk(const Layout &layout,
			       const std::filesystem::path &path)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0)
		throw std::invalid_argument("the Android data disk is missing: " +
					    path.string());
	if (!S_ISREG(details.st_mode))
		throw std::invalid_argument(
			"the Android data disk is not a regular file");
	if (access(path.c_str(), R_OK | W_OK) != 0)
		throw std::invalid_argument("the Android data disk is not writable");
	if (!::uml::block::usable_ubd_path(path.native()))
		throw std::invalid_argument(
			"Android data disk path is unusable as a UBD device");
	const auto bytes = static_cast<std::uint64_t>(details.st_size);
	if (bytes % gibibyte != 0)
		throw std::invalid_argument(
			"the Android data disk is not a whole number of GiB");
	const auto gib = bytes / gibibyte;
	if (gib < layout.data.minimumGiB || gib > layout.data.maximumGiB)
		throw std::invalid_argument(
			"the Android data disk capacity is outside the release's range");
	const File disk(path);
	verifyGpt(disk, layout.dataGpt(static_cast<std::uint32_t>(gib)),
		  "the Android data disk");
	return static_cast<std::uint32_t>(gib);
}

void createDataDisk(const Layout &layout, const std::filesystem::path &path,
		    std::uint32_t gib)
{
	if (!::uml::block::usable_ubd_path(path.native()))
		throw std::invalid_argument(
			"Android data disk path is unusable as a UBD device");
	const auto image = layout.dataGpt(gib);
	struct stat existing {};
	if (lstat(path.c_str(), &existing) == 0)
		failWith(EEXIST, "the Android data disk exists: " + path.string());
	const auto staged = siblingName(path, "new");
	(void)unlink(staged.c_str());
	try {
		Descriptor disk(staged, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
		if (ftruncate(disk.get(), static_cast<off_t>(image.diskBytes)) != 0)
			failWith(errno, "cannot size the data disk");
		writeDisk(disk, image);
		disk.closeChecked("cannot close the data disk");
		/* link() never replaces: a disk another launcher created first
		 * wins. */
		if (link(staged.c_str(), path.c_str()) != 0)
			failWith(errno, "cannot create " + path.string());
	} catch (...) {
		(void)unlink(staged.c_str());
		throw;
	}
	(void)unlink(staged.c_str());
	flushDirectory(path);
}

void growDataDisk(const Layout &layout, const std::filesystem::path &path,
		  std::uint32_t gib)
{
	const auto current = validateDataDisk(layout, path);
	if (gib <= current)
		throw std::invalid_argument(
			"the Android data disk only grows: " + std::to_string(current) +
			" GiB now, " + std::to_string(gib) + " GiB requested");
	if (lockedByAnotherProcess(path))
		throw std::runtime_error("the Android data disk is in use");
	const auto grown = layout.dataGpt(gib);
	const auto oldBackup = layout.dataGpt(current);

	const auto staged = siblingName(path, "grow");
	(void)unlink(staged.c_str());
	const bool cloned = clonefile(path.c_str(), staged.c_str(), 0) == 0;
	const auto &target = cloned ? staged : path;
	try {
		Descriptor disk(target, O_WRONLY | O_NOFOLLOW);
		if (ftruncate(disk.get(), static_cast<off_t>(grown.diskBytes)) != 0)
			failWith(errno, "cannot size the data disk");
		/* The old backup tables lie in what becomes userdata; zero them so
		 * the grown partition carries no stale table. The new tables go
		 * first, so a disk is never left without a backup. */
		writeDisk(disk, grown);
		const std::vector<std::byte> zeros(oldBackup.entries.size() +
						   sectorSize);
		writeAll(disk.get(), oldBackup.backupEntriesLba() * sectorSize, zeros);
		if (fsync(disk.get()) != 0)
			failWith(errno, "cannot flush the data disk");
		disk.closeChecked("cannot close the data disk");
		if (cloned && rename(staged.c_str(), path.c_str()) != 0)
			failWith(errno, "cannot replace " + path.string());
	} catch (...) {
		if (cloned)
			(void)unlink(staged.c_str());
		throw;
	}
	flushDirectory(path);
}

PreparedDataDisk prepareDataDisk(const Layout &layout,
				 const std::filesystem::path &path,
				 std::uint32_t gib)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0) {
		if (errno != ENOENT)
			failWith(errno, "cannot inspect " + path.string());
		const auto capacity = gib == 0 ? layout.data.defaultGiB : gib;
		createDataDisk(layout, path, capacity);
		return {DataDiskChange::created, capacity};
	}
	const auto current = validateDataDisk(layout, path);
	if (gib == 0 || gib == current)
		return {DataDiskChange::none, current};
	if (gib < current)
		throw std::invalid_argument(
			"the Android data disk only grows: " + std::to_string(current) +
			" GiB now, " + std::to_string(gib) + " GiB requested");
	growDataDisk(layout, path, gib);
	return {DataDiskChange::grown, gib};
}

} // namespace uml::android
