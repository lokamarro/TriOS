// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "kernel_signal_scope.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::runtime {

/*
 * A crash report attributes a program counter to an image only inside that
 * image's __TEXT, and the kernel runs from an alias of the image's
 * __TEXT,__uml_elf section placed at the kernel's own addresses (the loader
 * records it with record_kernel_text_alias()). Every thread's saved return
 * addresses into the kernel therefore symbolize to nothing, however the image's
 * dSYM is built. Just before a guest kernel's panic aborts the process, the
 * frames are rewritten to the same instructions in the section itself, which
 * the dSYM does symbolize (scripts/kernel-dsym-symbols.py).
 *
 * The functions never allocate, never take a lock and bound every walk, so a
 * panic can run them from any state a kernel stack, a signal handler or a
 * corrupt frame chain leaves: a record is read and written through the task's
 * own vm_read_overwrite and vm_write, and a failure ends only the chain it
 * concerns. They do not abort or resume anything.
 */

/* The carrier address of the instruction at `address`, or `address` itself
 * when it lies in none of the `count` aliases. */
[[nodiscard]] std::uintptr_t kernel_text_carrier_address(
	const KernelTextAlias *aliases, std::size_t count,
	std::uintptr_t address) noexcept;

/* Rewrite the saved return address of each frame record in the chain that
 * begins at `frame` (x29 of the running function) that lies in an alias, and
 * return how many it rewrote. The walk stops at a null, misaligned or
 * unreadable frame, at a frame it has just left, or after a fixed depth. */
std::size_t translate_frame_chain(const KernelTextAlias *aliases,
	std::size_t count, std::uintptr_t frame) noexcept;

/* The panic path with the aliases the loader recorded: if there are none it
 * does nothing. Otherwise it suspends every other thread of the task for good
 * and rewrites, for each, the program counter and link register that lie in an
 * alias and its frame chain, then the calling thread's frame chain. */
void translate_kernel_frames_for_crash_report() noexcept;

/* The same with the aliases given. */
void translate_kernel_frames(const KernelTextAlias *aliases,
	std::size_t count) noexcept;

} // namespace uml::runtime
