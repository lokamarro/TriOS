// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_UBD_PATH_HPP
#define UML_DEVICE_BLOCK_UBD_PATH_HPP

#include <string_view>

namespace uml::block {

/* Whether a host file path can be named in a `ubd<n>=` kernel argument as
 * itself. UML joins its arguments into one command line with spaces, and the
 * kernel splits that line at whitespace, treats a double quote as quoting,
 * and reads a comma or colon in a UBD specification as the start of another
 * field. The kernel's whitespace is its own Latin-1 table, which includes
 * byte 0xa0, so a UTF-8 name whose encoding contains that byte splits too.
 * A path carrying any of those, or an empty one, would name a different file
 * or configuration, so it cannot be used. */
[[nodiscard]] constexpr bool usable_ubd_path(std::string_view path) noexcept
{
	if (path.empty())
		return false;
	for (const char character : path) {
		switch (character) {
		case ' ':
		case '\t':
		case '\n':
		case '\v':
		case '\f':
		case '\r':
		case '\xa0':
		case '"':
		case ',':
		case ':':
			return false;
		default:
			break;
		}
	}
	return true;
}

} // namespace uml::block

#endif
