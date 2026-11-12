// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::runtime {

/*
 * The host resources an embedded kernel creates through its imports.
 *
 * A kernel closes only what it opens through `darwin_uml_close`, deletes no
 * pthread key and frees no signal stack at shutdown: it expects its process
 * to end. A guest that restarts itself hands the process to the next boot, so
 * the loader resolves the kernel's creating imports to the wrappers here, which
 * record what the kernel created and still holds, and the library gives all of
 * it back when it hands the guest back (emulator.cpp):
 *
 *   descriptors   darwin_uml_open, socket, socketpair, accept, pipe, kqueue,
 *                 dup, fcntl F_DUPFD and the descriptors received by recvmsg
 *                 (SCM_RIGHTS), closed through darwin_uml_close, which the
 *                 memory layer follows;
 *   pthread keys  deleted, so no destructor inside the released image is ever
 *                 called and Darwin's 512 keys last;
 *   allocations   malloc'ed and not freed, such as a carrier's signal stack;
 *   mappings      mmap'ed outside the native window and not unmapped. The
 *                 window itself is replaced whole by the hand-back.
 *
 * sbrk allocates nothing on Darwin, so it is only counted.
 */

/* The wrapper for a kernel import, or nullptr when the ledger does not follow
 * it. `name` is the symbol as the kernel names it; a leading underscore, which
 * the Darwin SDK's asm labels add, is ignored. */
[[nodiscard]] void *kernel_resource_import(const char *name) noexcept;

struct KernelResourceCounts final {
	std::size_t descriptors;
	std::size_t keys;
	std::size_t allocations;
	std::size_t allocation_bytes;
	std::size_t mappings;
	std::size_t mapping_bytes;
	std::size_t sbrk_calls;
};

/* What the kernel created and still holds. */
[[nodiscard]] KernelResourceCounts kernel_resource_counts() noexcept;

/* Gives back everything counted, and returns what it gave back. A mapping is
 * unmapped only outside [window_begin, window_end), which the caller replaces
 * whole; one that straddles an edge loses its outside part. The call must come
 * after the run's memory domain was retired, so retirement never meets a closed
 * descriptor, and while no kernel thread runs. */
KernelResourceCounts release_kernel_resources(std::uintptr_t window_begin,
	std::uintptr_t window_end) noexcept;

} // namespace uml::runtime
