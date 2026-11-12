/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_STATIC_MMU_H
#define UML_DARWIN_STATIC_MMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum darwin_uml_static_access {
	DARWIN_UML_STATIC_READ = 0,
	DARWIN_UML_STATIC_WRITE = 1,
	DARWIN_UML_STATIC_EXECUTE = 2,
};

enum darwin_uml_static_translation_result {
	DARWIN_UML_STATIC_TRANSLATED = 0,
	DARWIN_UML_STATIC_UNMAPPED = 1,
	DARWIN_UML_STATIC_PERMISSION = 2,
	DARWIN_UML_STATIC_HOST_ERROR = -1,
};

struct darwin_uml_static_mapping_change {
	uint64_t address;
	uint64_t length;
};

struct darwin_uml_static_code_change {
	uint64_t address;
	uint64_t length;
};

struct darwin_uml_static_sequences {
	uint64_t mapping_changes;
	uint64_t code_changes;
	uint64_t execution_interrupt;
};

struct darwin_uml_static_code_page_stamp {
	uint64_t identity;
	uint64_t version;
};

int darwin_uml_static_vm_map(void *address, int fd,
			     unsigned long long offset, unsigned long length,
			     int protection, int guest_code,
			     int guest_writable);
int darwin_uml_static_vm_map_generation(unsigned long generation,
					 void *address, int fd,
					 unsigned long long offset,
					 unsigned long length,
					 int protection, int guest_code,
					 int guest_writable);
int darwin_uml_static_vm_protect(void *address, unsigned long length,
				 int protection);
int darwin_uml_static_vm_unmap(void *address, unsigned long length);
int darwin_uml_static_vm_unmap_generation(unsigned long generation,
					    void *address,
					    unsigned long length);
/* Discards every cached mapping without retiring the generation. A following
 * synchronized Linux page-table walk may republish it immediately. */
int darwin_uml_static_vm_flush_generation(unsigned long generation);
int darwin_uml_static_vm_activate(unsigned long generation);
int darwin_uml_static_vm_destroy(unsigned long generation);
/* 1 while the generation has an address space, 0 once it was destroyed (a
 * generation is never reused), negative if the host cannot say. */
int darwin_uml_static_vm_generation_live(unsigned long generation);
/* Advances whenever a generation is destroyed, in any guest of the process,
 * so one load tells a holder of per-generation state whether any generation
 * may have died since it last asked darwin_uml_static_vm_generation_live(). */
unsigned long long darwin_uml_static_vm_generation_retirements(void);
/* Unmaps the page at `address` when its mapping names other backing than
 * `fd` at `offset`; an absent or equal mapping is left as it is. The caller's
 * backing is never installed: which of the two is stale is unknown, and the
 * next access faults so a page-table walker replays the current PTE.
 */
int darwin_uml_static_vm_discard_if_stale(unsigned long address, int fd,
					  unsigned long long offset);
int darwin_uml_static_vm_copy_from_buffer(unsigned long address,
					   const void *buffer,
					   unsigned long length);
/* The same copy for a kernel write, a uaccess the kernel made through its own
 * alias of the page. It publishes a code change for a guest-code page only
 * once darwin_uml_static_vm_require_coherent_kernel_code_writes() was called:
 * arm64 and AArch32 code see such a write only after their own instruction
 * cache maintenance, which publishes it. */
int darwin_uml_static_vm_copy_kernel_write(unsigned long address,
					   const void *buffer,
					   unsigned long length);
/* Called before code whose instruction fetch is coherent with every store,
 * x86 code, is first translated; from then on, for the life of the process,
 * every kernel write into a guest-code page publishes a code change. */
void darwin_uml_static_vm_require_coherent_kernel_code_writes(void);
int darwin_uml_static_vm_retry_fault(unsigned long address, int execute,
				     int write);
int darwin_uml_static_vm_batch_begin(unsigned long generation);
int darwin_uml_static_vm_batch_end(void);
int darwin_uml_static_vm_walker_lock(unsigned long generation);
int darwin_uml_static_vm_walker_unlock(unsigned long generation);

/* Returns a host data pointer valid through the end of the translated guest
 * page. The mapping is never executable. Each vCPU observes concurrent
 * map/protect/unmap operations at its next bounded guest-dispatch boundary.
 */
int darwin_uml_static_vm_translate(unsigned long generation,
				   unsigned long address,
				   enum darwin_uml_static_access access,
				   void **host_address);
/* darwin_uml_static_vm_translate() through an execution context of the same
 * generation: while the context is entered, a guest address is resolved from
 * the generation it holds; otherwise, or for a null context, this is
 * darwin_uml_static_vm_translate(). */
int darwin_uml_static_vm_execution_context_translate(
	void *context, unsigned long generation, unsigned long address,
	enum darwin_uml_static_access access, void **host_address);
/* Returns an opaque, mapping-lifetime handle for an executable physical page.
 * Callers may retain it only while their cached mapping sequence is current.
 */
int darwin_uml_static_vm_code_page(
	unsigned long generation, unsigned long address, void **handle,
	struct darwin_uml_static_code_page_stamp *stamp);
int darwin_uml_static_vm_code_page_stamp(
	const void *handle, struct darwin_uml_static_code_page_stamp *stamp);
void darwin_uml_static_vm_invalidate_code_page(void *handle);
uint64_t darwin_uml_static_vm_changes(unsigned long generation);
/* Copies mapping ranges changed after `sequence` and returns their count.
 * A negative result means the caller must invalidate its complete TLB.  The
 * returned current_sequence is an atomic snapshot taken with the ranges.
 */
int darwin_uml_static_vm_mapping_changes(
	unsigned long generation, uint64_t sequence,
	struct darwin_uml_static_mapping_change *changes, size_t capacity,
	uint64_t *current_sequence);
/* A cached execution context keeps the generation alive and avoids resolving
 * thread-local address-space state at every translated-block boundary. */
void *darwin_uml_static_vm_execution_context_create(unsigned long generation);
void darwin_uml_static_vm_execution_context_destroy(void *context);
int darwin_uml_static_vm_execution_context_begin(void *context,
						 unsigned long guest_pc);
int darwin_uml_static_vm_execution_context_end(void *context);
struct darwin_uml_static_sequences darwin_uml_static_vm_execution_context_sequences(
	void *context);
/* Where the context's code-change sequence lives: the word that
 * darwin_uml_static_vm_execution_context_sequences() reports as code_changes,
 * which only grows and stays valid while the context does. Translated code
 * reads it with one single-copy-atomic load at a context synchronization point
 * to tell whether guest code changed since its translations were validated. */
const uint64_t *darwin_uml_static_vm_execution_context_code_sequence(
	void *context);
/* Copies the guest ranges whose code changed after `sequence` and returns
 * their count.  A negative result means the caller must retire every
 * translation it holds for the address space; a range of length zero names
 * the whole address space, as a flush does.
 *
 * A mapping change publishes a range only for pages a translation can be
 * wrong about: a guest-code page that grants execute stops granting it,
 * goes away, or holds other bytes (another backing or another place in it).
 * A protection or writability change that keeps execute and the bytes, and
 * a mapping where none was, publish none; they appear only in the mapping
 * changes, which name the pages whose cached host addresses to retire.  A
 * published page also moves its code-page stamp, which no store can once the
 * page cannot be executed.
 */
int darwin_uml_static_vm_execution_context_code_change_ranges(
	void *context, uint64_t sequence,
	struct darwin_uml_static_code_change *changes, size_t capacity,
	uint64_t *current_sequence);
void darwin_uml_static_vm_invalidate_code(unsigned long address);
int darwin_uml_static_vm_invalidate_code_generation(
	unsigned long generation, unsigned long address, unsigned long length);

#ifdef __cplusplus
}
#endif

#endif
