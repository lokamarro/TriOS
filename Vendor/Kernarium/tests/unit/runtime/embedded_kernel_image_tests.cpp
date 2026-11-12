// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The kernel a library links into itself (the Kernarium runtime framework's
 * case) is found through the image that holds the lookup's code, and not
 * through the executable that loads it, which has no such section. The probe
 * library carries its own copy of the lookup and a section of its own.
 */
#include "kernel_signal_scope.hpp"
#include "require.hpp"

#include <dlfcn.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using uml::runtime::is_embedded_kernel_image;
using uml::testing::require;

using SectionFunction = const void *(*)(std::size_t *);
using LookupFunction = bool (*)(const void *, std::size_t);

} // namespace

int main(int argc, char *argv[])
{
	require(argc == 2, "usage: embedded_kernel_image_tests PROBE_LIBRARY");
	void *const library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	require(library != nullptr, "could not load the probe library");
	const auto section_of = reinterpret_cast<SectionFunction>(
		dlsym(library, "probe_section"));
	const auto library_lookup = reinterpret_cast<LookupFunction>(
		dlsym(library, "probe_is_embedded"));
	require(section_of != nullptr && library_lookup != nullptr,
		"the probe library lacks its functions");

	std::size_t size = 0;
	const auto *const section = static_cast<const unsigned char *>(
		section_of(&size));
	require(section != nullptr && size > 1, "the probe has no section");

	/* The library finds its own section; this executable has none. */
	require(library_lookup(section, size),
		"a library did not find the kernel it links");
	require(!is_embedded_kernel_image(section, size),
		"an executable took a library's section for its own");

	/* Only the section itself: not a part of it, nor a copy of its bytes. */
	require(!library_lookup(section, size - 1) &&
			!library_lookup(section + 1, size - 1) &&
			!library_lookup(section, 0) && !library_lookup(nullptr, size),
		"part of the section was taken for the kernel");
	const std::vector<unsigned char> copy(section, section + size);
	require(!library_lookup(copy.data(), copy.size()),
		"a copy of the section was taken for the kernel");

	require(dlclose(library) == 0, "could not unload the probe library");
	std::puts("embedded kernel image tests passed");
	return EXIT_SUCCESS;
}
