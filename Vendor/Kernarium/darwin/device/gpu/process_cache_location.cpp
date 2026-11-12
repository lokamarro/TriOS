// SPDX-License-Identifier: GPL-2.0-or-later

#include "process_cache_location.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <unistd.h>

namespace uml::gpu {

std::optional<std::filesystem::path> darwin_user_cache_directory()
{
	const std::size_t length = ::confstr(_CS_DARWIN_USER_CACHE_DIR, nullptr, 0);
	if (length <= 1U)
		return std::nullopt;
	std::string path(length, '\0');
	if (::confstr(_CS_DARWIN_USER_CACHE_DIR, path.data(), length) != length)
		return std::nullopt;
	path.resize(length - 1U);
	return std::filesystem::path(path);
}

std::optional<std::string> main_bundle_identifier()
{
	const CFBundleRef bundle = CFBundleGetMainBundle();
	if (bundle == nullptr)
		return std::nullopt;
	const CFStringRef identifier = CFBundleGetIdentifier(bundle);
	if (identifier == nullptr)
		return std::nullopt;
	char buffer[256];
	if (CFStringGetCString(identifier, buffer, sizeof buffer,
			       kCFStringEncodingUTF8) == 0)
		return std::nullopt;
	return std::string(buffer);
}

} // namespace uml::gpu
