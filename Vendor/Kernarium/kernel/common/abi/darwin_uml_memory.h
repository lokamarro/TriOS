/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_MEMORY_ABI_H
#define UML_DARWIN_MEMORY_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

int darwin_uml_vm_map(void *address, int fd, unsigned long long offset,
		      unsigned long length, int protection, int guest_code,
		      int guest_writable);
int darwin_uml_vm_map_generation(unsigned long generation, void *address,
				 int fd, unsigned long long offset,
				 unsigned long length, int protection,
				 int guest_code, int guest_writable);
int darwin_uml_vm_protect(void *address, unsigned long length,
			  int protection);
int darwin_uml_vm_unmap(void *address, unsigned long length);
int darwin_uml_vm_unmap_generation(unsigned long generation, void *address,
				   unsigned long length);
int darwin_uml_vm_drop(void *address, unsigned long length);
int darwin_uml_vm_activate(unsigned long generation);
int darwin_uml_vm_batch_begin(unsigned long generation);
/* Returns 0 once the batch is published, DARWIN_UML_VM_BATCH_REJECTED
 * (uml_tlb_sync_ranges.h) when an invalidation of the generation arrived
 * before it ended and the walker must walk it again, or a negative value on
 * failure. */
int darwin_uml_vm_batch_end(void);
int darwin_uml_vm_walker_lock(unsigned long generation);
int darwin_uml_vm_walker_unlock(unsigned long generation);
int darwin_uml_vm_destroy(unsigned long generation);
int darwin_uml_vm_flush_generation(unsigned long generation);
int darwin_uml_vm_invalidate_code(unsigned long generation,
				  unsigned long address, unsigned long length);
int darwin_uml_vm_sync_from_backing(unsigned long address);
int darwin_uml_vm_sync_from_buffer(unsigned long address,
				   const void *buffer, unsigned long length);
/* A uaccess pinned the page at fd and offset for address: discard the
 * software mapping of address when it names other backing, because either
 * sample may be the stale one. An absent or equal mapping is left as it is,
 * and nothing is installed. Returns 0, or a negative value on failure. */
int darwin_uml_vm_discard_if_stale(unsigned long address, int fd,
				   unsigned long long offset);
/* Returns 1 when the access can be retried, 0 when Linux must resolve the
 * fault, or a negative value on failure. */
int darwin_uml_vm_retry_fault(unsigned long address, int execute, int write);

/* Guest RAM commitment. Ranges are byte offsets into the anonymous physical-
 * memory backing, aligned to the host page. Release hands a range the guest
 * no longer uses back to the host; commit makes a released range the guest's
 * again before the guest uses it. Both are idempotent per host page. */
int darwin_uml_physmem_release(unsigned long long offset,
			       unsigned long long length);
int darwin_uml_physmem_commit(unsigned long long offset,
			      unsigned long long length);

#ifdef __cplusplus
}
#endif

#endif
