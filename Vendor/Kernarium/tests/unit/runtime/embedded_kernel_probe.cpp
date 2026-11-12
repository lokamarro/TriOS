// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A shared library that links a kernel section into itself, as the Kernarium
 * runtime framework does, and carries its own copy of the runtime's lookup.
 * Its section is these bytes (the test target names this file for it).
 */
#include "kernel_signal_scope.hpp"

#include <cstddef>

extern "C" {

extern const unsigned char probe_section_start[]
	__asm("section$start$__TEXT$__uml_elf");
extern const unsigned char probe_section_end[]
	__asm("section$end$__TEXT$__uml_elf");

__attribute__((visibility("default"))) const void *probe_section(
	std::size_t *size)
{
	*size = static_cast<std::size_t>(probe_section_end - probe_section_start);
	return probe_section_start;
}

/* The library's own answer: the lookup of the image that holds this code. */
__attribute__((visibility("default"))) bool probe_is_embedded(
	const void *image, std::size_t size)
{
	return uml::runtime::is_embedded_kernel_image(image, size);
}

}
