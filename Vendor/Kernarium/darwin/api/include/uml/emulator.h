/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_EMULATOR_H
#define UML_EMULATOR_H

#include <uml/abi_identity.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The callback table is copied by uml_emulator_create. Its context remains
 * caller-owned and must stay valid during every run that can invoke it.
 *
 * read dequeues one whole Ethernet frame into buffer and returns its length,
 * at most capacity. It returns 0 when no frame is pending and a negative errno
 * on failure. The guest drops a frame longer than capacity or shorter than an
 * Ethernet header. */
struct uml_emulator_network_callbacks {
	uint32_t struct_size;
	void *context;
	int (*open)(void *context, uint8_t mac[6], unsigned int *mtu);
	int (*irq_fd)(void *context);
	int (*read)(void *context, void *buffer, size_t capacity);
	int (*write)(void *context, const void *buffer, size_t length);
};

/* Public functions returning int use 0 for success and the negated value of
 * one of these positive error constants for API failures. A successful run
 * returns the embedded kernel's status unchanged, so positive values may also
 * be valid uml_emulator_run results. */
enum uml_emulator_error {
	UML_EMULATOR_OK = 0,
	UML_EMULATOR_INVALID_ARGUMENT = 1,
	UML_EMULATOR_OUT_OF_MEMORY = 2,
	UML_EMULATOR_UNSUPPORTED = 3,
	UML_EMULATOR_RUNTIME_ERROR = 4,
};

/* Retired selections keep their number, so a value this build no longer
 * produces is never reused for a different engine. */
enum uml_emulator_execution_model {
	UML_EMULATOR_EXECUTION_AUTO = 0,
	UML_EMULATOR_STATIC_GADGETS = 1,
	UML_EMULATOR_NATIVE_JIT = 2,
};

/* How the guest of a run ended, as uml_emulator_get_run_end reports it. */
enum uml_emulator_run_end_kind {
	/* No run has begun on the handle, or the last one never ran a guest: it
	 * was refused, cancelled while queued, or only printed information. */
	UML_EMULATOR_RUN_END_NONE = 0,
	/* The guest powered off or halted itself. */
	UML_EMULATOR_RUN_END_POWER_OFF = 1,
	/* The guest asked to restart. */
	UML_EMULATOR_RUN_END_RESTART = 2,
	/* uml_emulator_request_stop ended the run; it wins over whatever the
	 * guest said on its way out. */
	UML_EMULATOR_RUN_END_STOPPED = 3,
	/* Any other end: the kernel returned, or the image failed to load,
	 * without saying how the guest ended. */
	UML_EMULATOR_RUN_END_FAILED = 4,
};

struct uml_emulator_run_end {
	/* Set by the caller to sizeof the structure. */
	uint32_t struct_size;
	enum uml_emulator_run_end_kind kind;
	/* The command the guest passed to its restart, cut to 255 bytes;
	 * empty when it gave none and for every other kind. */
	char restart_command[256];
};

/* What the host measures of a running guest, for a host that shows
 * statistics. Reading it asks nothing of the guest and starts nothing: the
 * figures are the system's own accounting of the guest's vCPU threads and the
 * host's record of the guest RAM it holds. */
struct uml_emulator_statistics {
	/* Set by the caller to sizeof the structure. */
	uint32_t struct_size;
	/* The guest's vCPUs that have come up. */
	uint32_t guest_cpu_count;
	/* The user and system time the guest's vCPU threads have used in this
	 * boot, in nanoseconds. A guest that restarts starts again from zero.
	 * Two readings a second apart differ by the time the vCPUs spent in
	 * that second, so the difference over the elapsed time, divided by
	 * guest_cpu_count, is the share of its vCPUs the guest used. */
	uint64_t guest_cpu_time_ns;
	/* The guest's RAM, kernel image included, and the part of it the host
	 * holds now: what the guest has committed and not released. Both are 0
	 * before the guest's kernel has claimed its RAM and while a restart
	 * hands it back. */
	uint64_t guest_ram_bytes;
	uint64_t guest_ram_held_bytes;
};

/* The guest kernel's own memory figures, as uml_emulator_get_guest_memory
 * reads them. */
struct uml_emulator_guest_memory {
	/* Set by the caller to sizeof the structure. */
	uint32_t struct_size;
	uint32_t reserved;
	/* The guest's total memory, and the part of it available without
	 * swapping (MemTotal and MemAvailable), in bytes. The guest's memory in
	 * use is total_bytes - available_bytes. */
	uint64_t total_bytes;
	uint64_t available_bytes;
};

struct uml_emulator_config {
	uint32_t abi_version;
	uint32_t struct_size;
	const char *rootfs_path;
	const void *kernel_image;
	size_t kernel_image_size;
	const struct uml_emulator_network_callbacks *network;
};

struct uml_emulator_capabilities {
	uint32_t abi_version;
	uint32_t struct_size;
	enum uml_emulator_execution_model userspace_execution;
	int can_run;
	int sandbox_safe;
	const char *platform;
};

struct uml_emulator;

/* Creation copies rootfs_path, kernel_image, and the network callback table.
 * It does not retain the config object or those input buffers, with one
 * exception: a kernel_image that is exactly the __TEXT,__uml_elf section
 * (its start and its size) of the image that holds this library, or of the
 * main executable, is not copied. A product that links its kernel into itself
 * as that section passes it as kernel_image, and the handle runs the section
 * in place. No caller can free it, and it stays mapped for as long as the
 * handle can run. Every other buffer may be freed once creation returns. */
int uml_emulator_create(const struct uml_emulator_config *config,
		       struct uml_emulator **result);
/* Destruction must not overlap any other operation on this handle. */
void uml_emulator_destroy(struct uml_emulator *emulator);
/* Capability queries may overlap a run or stop request on the same handle. */
int uml_emulator_get_capabilities(const struct uml_emulator *emulator,
				 struct uml_emulator_capabilities *capabilities);
/* A run is synchronous. It copies argv, envp, and all referenced strings
 * before entering the kernel and retains none of them after returning. At
 * most one run may be active on a handle; concurrent runs fail. An embedded
 * kernel run is admitted only while no other run is active. Native mappings
 * belong to the run and are flushed and unmapped before that run returns.
 *
 * A process runs one guest, and that guest may restart itself. A kernel
 * claims guest RAM early in its boot. After a run has claimed it, every later
 * run in the process, on this handle or another, returns
 * -UML_EMULATOR_UNSUPPORTED before it names a console, maps a kernel window
 * or enters a kernel, except the next run on the handle whose last run ended
 * with UML_EMULATOR_RUN_END_RESTART (uml_emulator_get_run_end): that run first
 * hands the guest back to the process and then boots it again. A power-off, a
 * stop and any other end leave the guest spent. After a clean power-off,
 * uml_emulator_release_guest can return it to the process explicitly.
 * A run that ends before its
 * kernel claims guest RAM, such as one refused for its arguments or cancelled
 * while queued, leaves the process's guest unspent.
 *
 * Before it runs again after a restart, the caller must stop every service
 * that maps guest RAM, such as a device, display, disk or network backend that
 * holds a view of it, and the kernel's threads must have ended. The services
 * of the old boot are not carried over: the caller builds them again for the
 * next one. While a service still borrows guest RAM or a kernel thread still
 * runs, the hand-back fails with -UML_EMULATOR_RUNTIME_ERROR and changes
 * nothing. A run refused so may be retried once the cause is gone: the
 * hand-back repeats safely and the handle stays restartable. Such a refusal
 * ran no guest, so uml_emulator_get_run_end then reports
 * UML_EMULATOR_RUN_END_NONE. The check and the hand-back are not one step. A
 * service that maps guest RAM after the check makes the hand-back itself fail
 * with -UML_EMULATOR_RUNTIME_ERROR, after the kernel's resources were already
 * given back; a retry once that service has stopped succeeds.
 *
 * Every run of a process must use the same JIT translation-cache budget and
 * the same number of guest CPUs; a run that asks for another fails with
 * -UML_EMULATOR_RUNTIME_ERROR.
 *
 * The kernel changes process-wide state while it runs, and the run restores
 * what it found when it ends: the signal dispositions, the calling thread's
 * signal mask and signal stack, ITIMER_REAL, and the status flags and terminal
 * modes of descriptors 0 to 2. A host must not rely on a disposition the
 * kernel set, such as its ignoring SIGPIPE, SIGIO or SIGWINCH, outliving the
 * run: the host's own disposition is back when the run returns. A host that
 * needs a signal ignored, SIGPIPE above all, sets that itself at startup. */
int uml_emulator_run(struct uml_emulator *emulator, int argc,
		    const char *const argv[], const char *const envp[]);
/* Reports how the guest of the handle's most recent run ended, once that run
 * has returned. A run that is admitted to the handle starts from
 * UML_EMULATOR_RUN_END_NONE, so while it runs, and after a run that was
 * refused, the end is NONE and does not repeat an earlier run's. Safe to call
 * from any thread, idle or during a run. -UML_EMULATOR_INVALID_ARGUMENT for a
 * null handle or output, or a struct_size smaller than the structure. */
int uml_emulator_get_run_end(const struct uml_emulator *emulator,
			     struct uml_emulator_run_end *end);
/* Releases the guest owned by this idle handle after its own clean power-off
 * (including halt). Stop all device services that borrow guest RAM first.
 * Returns 0 once the kernel resources and guest RAM have been returned, so
 * this or another handle can boot again. Does not change the recorded run end.
 * A failed or host-stopped guest cannot be released. Returns
 * -UML_EMULATOR_UNSUPPORTED when no cleanly powered-off guest is owned, and
 * -UML_EMULATOR_RUNTIME_ERROR while the handle is running, a service borrows
 * its RAM, or a kernel thread still runs. A refused release may be retried
 * after those services and threads stop. */
int uml_emulator_release_guest(struct uml_emulator *emulator);
/* This and uml_emulator_sync are the operations intended to overlap
 * uml_emulator_run on the same handle. A stop request made while idle is not
 * inherited by a later run. A request made while a run is queued at process
 * admission cancels that run with -UML_EMULATOR_RUNTIME_ERROR and is not
 * inherited by a retry. */
void uml_emulator_request_stop(struct uml_emulator *emulator);
/* Flushes every host file the running guest can still write to, so that the
 * writes it has already issued survive the process. Like a stop request, this
 * is intended to overlap uml_emulator_run on the same handle; on an idle
 * handle it flushes whatever the previous run left open. It reports the number
 * of descriptors that could not be flushed, 0 when all of them were, and the
 * negated error constant for API failures. The guest's own page cache is
 * outside this boundary: only the guest kernel can write that back. */
int uml_emulator_sync(struct uml_emulator *emulator);
/* Puts the guest somewhere it can be abandoned. It has the guest hand its cold
 * page cache and free memory to the host, as uml_emulator_trim_page_cache
 * does, writes the guest's filesystems back, enters full system suspend, and
 * then flushes what the host received. A suspended guest issues no further
 * writes and its monotonic time does not advance, so a host that never runs
 * this process again leaves the disk as it was left.
 * Reports the number of host files that could not be flushed, 0 when all of
 * them were, and the negated error constant for API failures. Pair it with
 * uml_emulator_resume; a guest with no management console is only flushed,
 * exactly as uml_emulator_sync would. */
int uml_emulator_quiesce(struct uml_emulator *emulator);
/* uml_emulator_quiesce for a host that must not write what it is short of:
 * the same order and the same result, but the guest's RAM stays in the
 * process and no hibernation image is made, where a quiesce would have
 * hibernated it. A guest an earlier quiesce already hibernated stays so.
 * Pair it with uml_emulator_resume. */
int uml_emulator_quiesce_resident(struct uml_emulator *emulator);
/* Wakes a quiesced guest. Harmless on a guest that is already running. */
void uml_emulator_resume(struct uml_emulator *emulator);
/* Does what a phone's power key does: ends a system suspend the guest entered
 * by itself, such as Android's once its display is off. A running guest is
 * left as it is, and a guest that uml_emulator_quiesce suspended stays
 * suspended until uml_emulator_resume. Send it after forwarding the key, which
 * the guest then receives once. It may wait for the guest's console, so call
 * it away from a user interface thread. */
void uml_emulator_wake(struct uml_emulator *emulator);
/* Every guest disk is a host file the host caches as well, so the guest's
 * page cache mostly holds memory twice, and only the guest's copy counts
 * against this process. A trimming guest hands its cold page cache to the
 * host every few seconds and pays a host read for each evicted page it reads
 * again, backing off while that happens often. A guest boots with trimming
 * off, which suits a guest in front of its user; turn it on while the guest
 * runs unattended, as in the background, and off again on return. A change
 * made while uml_emulator_quiesce has the guest suspended reaches the guest
 * when uml_emulator_resume wakes it.
 * Returns 0 once the guest has applied the change, -UML_EMULATOR_UNSUPPORTED
 * when no guest console accepts it, as with no running guest or a kernel
 * without trimming, and -UML_EMULATOR_RUNTIME_ERROR when the guest did not
 * answer in time. It may wait for the guest's console, so call it away from a
 * user interface thread. */
int uml_emulator_set_page_cache_trimming(struct uml_emulator *emulator,
					 int enabled);
/* Has the guest trim all of its cold page cache now and hand its free memory
 * to the host, and waits a bounded time for it to finish; whatever trimming
 * is set to stays. Returns as uml_emulator_set_page_cache_trimming does, and
 * -UML_EMULATOR_RUNTIME_ERROR leaves the guest finishing the pass. A guest
 * that uml_emulator_quiesce suspended was trimmed then, and this returns 0
 * without waking it. Call it away from a user interface thread. */
int uml_emulator_trim_page_cache(struct uml_emulator *emulator);
/* Fills `statistics` (whose struct_size the caller has set) with the figures
 * of the guest this handle is running. Safe to call from any thread, during
 * the run and only then: it reads counters, takes no time worth measuring and
 * never waits for the guest. The handle's own guest is the one measured;
 * -UML_EMULATOR_UNSUPPORTED when no guest runs on it, before the run reaches
 * its kernel and after the run returns, a handle that is queued for admission
 * or idle included. -UML_EMULATOR_INVALID_ARGUMENT for a null handle or
 * output, or a struct_size smaller than the structure, and
 * -UML_EMULATOR_RUNTIME_ERROR when the system would not list the process's
 * threads. Nothing is measured between calls: a host that never reads
 * statistics pays nothing for them. */
int uml_emulator_get_statistics(const struct uml_emulator *emulator,
				struct uml_emulator_statistics *statistics);
/* Asks the guest kernel for its memory figures through the management
 * console, and waits up to 500 ms for the reply. The guest answers from its
 * interrupt, with no file read, so it also works where the guest's policy
 * denies /proc/meminfo to its console.
 *
 * Like every console request it wakes a guest that has suspended itself, as
 * an Android guest does once its display is off, so a host that knows the
 * guest may be asleep must not ask. It does not ask while the host has
 * quiesced the guest (uml_emulator_quiesce), nor while another lifecycle call
 * of this handle (quiesce, resume, trimming) is in progress: either way it
 * sends nothing and returns -UML_EMULATOR_UNSUPPORTED at once, and a quiesce
 * never waits for a memory request. -UML_EMULATOR_UNSUPPORTED also when no
 * console accepts the request, as with no running guest, and when the kernel
 * does not have the request. -UML_EMULATOR_RUNTIME_ERROR when no reply came
 * in time, or the reply was not the figures. -UML_EMULATOR_INVALID_ARGUMENT
 * for a null handle or output, or a struct_size smaller than the structure.
 * It may wait for the guest's console, so call it away from a user interface
 * thread. */
int uml_emulator_get_guest_memory(struct uml_emulator *emulator,
				  struct uml_emulator_guest_memory *memory);

#ifdef __cplusplus
}
#endif

#endif
