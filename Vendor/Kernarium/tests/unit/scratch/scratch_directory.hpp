// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace uml::testing {

/* The directory for this test's scratch files. A uml_add_unit_test()
 * registration names a per-test directory under the build tree in
 * UML_TEST_SCRATCH_DIRECTORY; a binary run outside CTest uses the host
 * temporary directory ($TMPDIR, else /tmp). The directory is created on first
 * use. Tests still remove what they create, so a leftover file names the test
 * that leaked it. */
[[nodiscard]] const std::filesystem::path &scratch_directory();

/* A mkstemp()/mkdtemp() template for `name`, which ends in XXXXXX, inside
 * scratch_directory(). */
[[nodiscard]] std::string scratch_template(std::string_view name);

/* The same for a path that becomes a Unix-domain socket, directly or inside a
 * mkdtemp() directory. sockaddr_un::sun_path holds 104 bytes, which a
 * build-tree path cannot promise, so socket paths stay in the host temporary
 * directory. */
[[nodiscard]] std::string socket_scratch_template(std::string_view name);

/* The directory socket_scratch_template() names its paths in. */
[[nodiscard]] std::filesystem::path socket_scratch_parent();

/* A private directory made from the mkdtemp() template `name`, which ends in
 * XXXXXX, inside `parent` (this test's scratch directory unless a socket path
 * needs socket_scratch_parent()). It and whatever the test left in it are
 * removed when the object is destroyed, so a test holds one for the scope, or
 * as a function-local static for the whole binary. A binary that ends on an
 * uncaught exception or an abort leaves it, naming the test. */
class ScratchSubdirectory final {
public:
	explicit ScratchSubdirectory(std::string_view name,
		const std::filesystem::path &parent = scratch_directory());
	~ScratchSubdirectory();
	ScratchSubdirectory(const ScratchSubdirectory &) = delete;
	ScratchSubdirectory &operator=(const ScratchSubdirectory &) = delete;

	[[nodiscard]] const std::filesystem::path &path() const noexcept
	{
		return path_;
	}

private:
	std::filesystem::path path_;
};

} // namespace uml::testing
