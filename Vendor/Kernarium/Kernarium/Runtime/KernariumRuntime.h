// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_H
#define KERNARIUM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define KERNARIUM_RUNTIME_API_VERSION 29U

#if defined(__OBJC__)
@class MTKView;
typedef MTKView* KernariumMetalView;
#else
typedef void* KernariumMetalView;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* What the framework's embedded kernel boots. One framework carries exactly
 * one flavour, because the kernel ELF is fixed in its signed carrier. */
enum {
    KERNARIUM_RUNTIME_GUEST_LINUX = 0,
    KERNARIUM_RUNTIME_GUEST_ANDROID = 1,
};

/* The send_touch coordinates are absolute positions in the guest's
 * touchscreen range, 0 through this value on both axes, wherever the display
 * is and however it is sized. The host maps its view onto that range, so the
 * range never has to follow a mode change. Each simultaneous host contact
 * keeps a slot in 0..<TOUCH_CONTACTS until it ends. A release affects only
 * that slot; invalid slots and out-of-range coordinates are ignored.
 *
 * A contact the host did not end itself, such as a touch the system takes
 * over, ends with cancel_touch instead of a release. The guest sees the
 * contact turn into a palm and then lift, which Android reports to
 * applications as ACTION_CANCEL (or a cancelled pointer-up while other
 * fingers stay down) and Linux readers as a cancelled touch, so no tap or
 * click follows. The contact stays at the last position it reported, a
 * slot that holds no contact is ignored, and the slot is free for a new
 * contact at once. Android's recovery menu still treats such a still tap as
 * a selection. */
enum {
    KERNARIUM_RUNTIME_TOUCH_MAXIMUM = 32767,
    KERNARIUM_RUNTIME_TOUCH_CONTACTS = 10,
};

enum {
    KERNARIUM_RUNTIME_ANDROID_BOOT_AUTO = 0,
    KERNARIUM_RUNTIME_ANDROID_BOOT_NORMAL = 1,
    KERNARIUM_RUNTIME_ANDROID_BOOT_RECOVERY = 2,
};

/* A boot request. Linux guests supply rootfs_path and overlay_path. Android
 * guests supply the release directory (guest/system.img, the read-only system
 * disk; guest/partition-layout.json; boot/kernel-command-line.txt), the path of
 * the instance's writable data disk, and the desired boot mode. The runtime
 * creates a data disk that is absent, sized android_data_gib GiB or the
 * release's default when that is 0, and grows an existing one to
 * android_data_gib GiB when that is larger; it never shrinks one. The guest
 * has exactly these two disks. The kernel is carried by the runtime.
 *
 * The host GPU's budget is B = clamp(L - A - G - E - S, 384 MiB, G / 2),
 * evaluated once at launch (see the architecture document): L is the
 * application's memory limit, G the guest's memory, and E the gadget cache
 * plus the other memory the guest side of the process uses. The request states
 * the three launch parameters: A, the application's own expected ceiling;
 * E_rest, the guest side's memory beyond the gadget cache; and S, the headroom
 * left unused. Zero, which is what the application states until their values
 * are chosen, adds nothing to the sum.
 *
 * An Android guest authorizes one adb host key for this boot without asking
 * its user: android_adb_public_key is the key as adb writes it, base64 of the
 * RSAPublicKey record without the trailing comment, 1 through 1024 characters
 * of A-Z a-z 0-9 + / =, or null for none. The runtime reads it only from a
 * request whose struct_size covers it and fails the run with
 * -UML_EMULATOR_INVALID_ARGUMENT when it is anything else. The guest's
 * kernel command line, which also carries the release's own and the launch's
 * arguments, holds 4096 bytes; a run whose command line is longer fails
 * with a message that says so. A Linux guest ignores the key.
 *
 * An Android guest also takes the host's time zone for this boot:
 * android_time_zone is its IANA name (for example Asia/Tokyo), 1 through 64
 * characters of A-Z a-z 0-9 _ + - and / in slash-separated components that
 * start with a letter or digit, or null for none. The runtime reads it only
 * from a request whose struct_size covers it, passes a valid name as
 * androidboot.kernarium.timezone, and drops anything else with a line on
 * stderr: a time zone never fails a boot. Whether the guest knows the name,
 * and whether it follows it, is the guest's to decide. A Linux guest ignores
 * it.
 *
 * session_record_path names the file the runtime records how the guest's
 * session ended in (darwin/runtime/session_record.hpp): "running" from the
 * boot on, then the end, a guest kernel panic with its message included. The
 * caller reads it at its next launch, before it boots again. Null keeps no
 * record; the runtime reads it only from a request whose struct_size covers
 * it. */
typedef struct kernarium_runtime_boot {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t dots_per_inch;
    uint32_t guest_memory_mib;
    uint32_t guest_processor_count;
    uint32_t android_boot_mode;
    uint32_t auto_resize_guest;
    const char* rootfs_path;
    const char* overlay_path;
    const char* android_data_disk_path;
    const char* android_release_path;
    uint32_t android_data_gib;
    uint32_t gpu_budget_application_mib;
    uint32_t gpu_budget_engine_mib;
    uint32_t gpu_budget_headroom_mib;
    const char* android_adb_public_key;
    const char* android_time_zone;
    const char* session_record_path;
} kernarium_runtime_boot;

/* What a launch's host GPU budget comes to, as the runtime evaluates it for
 * the boot request it receives. */
typedef struct kernarium_runtime_gpu_budget {
    uint32_t struct_size;
    /* Nonzero when L - A - G - E - S is below the budget's 384 MiB floor: the
     * process is likely to be ended, the guest boots anyway with the budget
     * clamped to the floor, and the application is to say so. */
    uint32_t short_of_floor;
    /* B. */
    uint64_t budget_bytes;
    /* L, zero when the host sets no limit. */
    uint64_t process_limit_bytes;
    /* L - A - G - E - S, zero when the sum passes L. */
    uint64_t available_bytes;
    uint64_t guest_memory_bytes;
    /* E: the gadget cache the run will set plus E_rest. */
    uint64_t engine_bytes;
    uint64_t application_bytes;
    uint64_t headroom_bytes;
} kernarium_runtime_gpu_budget;

/* Boots the guest and returns when it is over. A guest that restarts itself is
 * booted again inside this call, on the same view, disk and network, so the
 * call returns only when the guest powers off, the application stops it, a run
 * fails, or the guest restarts itself again and again within seconds of
 * booting. It returns the guest's own status (0 for a power-off or a stop), a
 * negated uml_emulator error constant for a run or start-up that failed, or
 * KERNARIUM_RUNTIME_RESULT_RESTART_LOOP. For Android, 0 means the guest powered
 * off cleanly and all its resources have been released: another run may start
 * in this process. A host stop or an unreported shutdown returns an error. */
typedef int (*kernarium_runtime_run_function)(
    KernariumMetalView view,
    const kernarium_runtime_boot* boot);

enum {
    /* The guest restarted itself five times in a row, each within 15 seconds
     * of its boot, so the runtime stopped booting it. It is outside the range
     * of the negated uml_emulator errors (-1 through -4). */
    KERNARIUM_RUNTIME_RESULT_RESTART_LOOP = -100,
};

/* Evaluates the host GPU's budget for a guest of `guest_memory_mib` and
 * `guest_processor_count` vCPUs with the launch parameters of
 * kernarium_runtime_boot, as a run with that request will (the application's
 * memory limit and the gadget cache, whose cap follows the vCPU count,
 * included), and fills `plan`, whose struct_size the caller sets. The
 * application calls it before run to warn the user when `short_of_floor` is
 * set. Returns 0, or a negative value for a null or short `plan`, a guest
 * memory of zero or a vCPU count of zero. */
typedef int (*kernarium_runtime_plan_gpu_budget_function)(
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count,
    uint32_t gpu_budget_application_mib,
    uint32_t gpu_budget_engine_mib,
    uint32_t gpu_budget_headroom_mib,
    kernarium_runtime_gpu_budget* plan);

typedef void (*kernarium_runtime_request_stop_function)(void);
/* Flushes the guest's writable host files. Safe to call from any thread while
 * a guest runs, and reports the number of files that could not be flushed. */
typedef int (*kernarium_runtime_sync_function)(void);
/* Leaves the guest safely abandonable: trims its cold page cache once, writes
 * its filesystems back, enters full system suspend, then flushes the host's
 * own files. Pair it with resume. */
typedef int (*kernarium_runtime_quiesce_function)(void);
/* quiesce for a host that must not write what it is short of: the same
 * order and result, with the guest's RAM kept in the process and no
 * hibernation image written where a quiesce would have written one. */
typedef int (*kernarium_runtime_quiesce_resident_function)(void);
typedef void (*kernarium_runtime_resume_function)(void);
typedef void (*kernarium_runtime_send_touch_function)(
    uint32_t slot,
    uint32_t x,
    uint32_t y,
    int pressed);
/* Withdraws the contact in `slot`, as the comment above the touch constants
 * describes: a cancel in place of the release send_touch would give it. */
typedef void (*kernarium_runtime_cancel_touch_function)(uint32_t slot);
typedef void (*kernarium_runtime_send_key_function)(
    uint16_t code,
    int pressed);
typedef void (*kernarium_runtime_console_output_callback)(
    void* context,
    const void* buffer,
    size_t length);
typedef void (*kernarium_runtime_set_console_output_callback_function)(
    kernarium_runtime_console_output_callback callback,
    void* context);
/* Writes to the guest's console. An App Store runtime (the
 * kernarium-ios-device-android-app-store preset) has no console input: its
 * entry stays in the table, so the table's layout and version are the same in
 * every runtime, and answers -ENOTSUP. */
typedef int (*kernarium_runtime_send_console_input_function)(
    const void* buffer,
    size_t length);
/* Holds (nonzero) or releases (zero) every GPU request the guest makes, so
 * none reaches the host GPU while it is held. A request that arrives waits in
 * its virtqueue, through any suspend of the guest, and runs in order after the
 * release. Work already in the renderer carries on. The application holds
 * before it enters the background, where the host GPU is revoked within
 * seconds, and releases when it returns. Returns 1 when the setting is in
 * effect and no request was still running after one second, 0 when one was
 * (the hold is in effect regardless), and -1 when no guest is running: the
 * setting is kept and applies to the guest's GPU once it starts. */
typedef int (*kernarium_runtime_set_gpu_hold_function)(int held);
/* Whether the guest's display is on: 1 while it has its primary scanout, 0
 * while it has none, as an Android screen that is off has not, and -1 when no
 * guest is running or its GPU has not started. A request the guest sent behind
 * a held GPU has not run, so the answer then is the one the guest's last
 * request that ran left; the application asks before it holds. */
typedef int (*kernarium_runtime_guest_display_active_function)(void);
/* Receives one copy of the guest's display as a CVPixelBufferRef: an
 * IOSurface-backed 32BGRA buffer from a pool, turned the way the user turned
 * the display, no wider than the width asked for. The buffer is borrowed for
 * the call; a receiver that keeps it retains it. It runs on a runtime thread
 * and must return promptly. */
typedef void (*kernarium_runtime_frame_sink)(void* context, void* pixel_buffer);
/* Mirrors the guest's display to `sink`, for a view of it that the
 * application shows while its own window cannot, such as Picture in Picture.
 * At most `frames_per_second` frames arrive each second, and a static display
 * arrives again about once a second. Nothing is rendered while no sink is set.
 * A null sink stops the mirror, and returns only once no call to the previous
 * sink is running or will start, so its context may be released; the sink must
 * not call this. Returns 0, or a negative value for a width or rate of zero.
 * The setting applies to the guest's display whenever one exists. An App Store
 * runtime has no mirror: its entry stays in the table and answers -ENOTSUP for
 * any arguments. */
typedef int (*kernarium_runtime_set_frame_sink_function)(
    kernarium_runtime_frame_sink sink,
    void* context,
    uint32_t maximum_width,
    uint32_t frames_per_second);
/* Moves the guest's vCPU threads to (nonzero) or from (zero) the system's
 * background scheduling policy, which runs them at low priority, prefers the
 * efficiency cores, and throttles their I/O. The application turns it on while
 * the guest is kept running behind a window the user is not looking at, so the
 * guest keeps its clock and its work moving without a foreground's power draw,
 * and off when the application returns. It applies to the vCPU threads that
 * exist when it is called, and leaves the host's other threads alone. Returns
 * the number of threads changed, or a negative value if the threads could not
 * be listed. */
typedef int (*kernarium_runtime_set_background_throttle_function)(int enabled);
/* Turns the guest's periodic page-cache trimming on (nonzero) or off, as
 * uml_emulator_set_page_cache_trimming does, and returns its result. A guest
 * boots with it off, as a guest in front of its user should be; the
 * application turns it on only while the guest is kept running in the
 * background. Quiescing trims once by itself. It may wait for the guest's
 * console, so call it away from the main thread. */
typedef int (*kernarium_runtime_set_page_cache_trimming_function)(int enabled);
/* What set_microphone leaves, as its return value. */
enum {
    /* The Microphone setting is off. The guest records silence. */
    KERNARIUM_RUNTIME_MICROPHONE_OFF = 0,
    /* On, but the system has not been asked for access: the guest records
     * silence until the application has asked, and the application, not the
     * runtime, is the one that asks. */
    KERNARIUM_RUNTIME_MICROPHONE_UNASKED = 1,
    /* On, but the system refuses access. The guest records silence. */
    KERNARIUM_RUNTIME_MICROPHONE_DENIED = 2,
    /* On and permitted: a guest that records hears the microphone. */
    KERNARIUM_RUNTIME_MICROPHONE_ON = 3,
};
/* Turns the guest's microphone on (nonzero) or off (zero) and returns the
 * resulting KERNARIUM_RUNTIME_MICROPHONE_ value. It is off until the
 * application turns it on, and while it is off, or the system has not granted
 * access, a guest that records gets silence and nothing opens the microphone.
 * The runtime never asks the system for access and never shows a permission
 * prompt: the application asks, as the direct result of the user turning the
 * setting on, and then calls this. Calling it again with a nonzero value after
 * the answer, or when the application becomes active, lets a guest that is
 * recording pick the microphone up. It may be called whether or not a guest
 * runs; a guest that starts later follows it. */
typedef int (*kernarium_runtime_set_microphone_function)(int enabled);

/* The host-side figures of the running guest, for an application that shows
 * statistics. Nothing in the runtime measures between calls: a reading is the
 * system's own accounting of the guest's vCPU threads, the display's frame
 * counters and the host's record of the guest RAM it holds, taken when the
 * call is made. */
typedef struct kernarium_runtime_statistics {
    /* Set by the caller to sizeof the structure. */
    uint32_t struct_size;
    /* The guest's vCPUs that have come up. */
    uint32_t processor_count;
    /* How many times this launch's guest has restarted: 0 for the first boot.
     * Two readings with different values span two guests, and the CPU time of
     * the second starts again from zero. */
    uint32_t boot_index;
    /* As guest_display_active: 1 on, 0 off, -1 unknown. */
    int32_t display_active;
    /* When the CPU time was read, in nanoseconds of the steady clock, which
     * does not count the time the device sleeps. */
    uint64_t timestamp_ns;
    /* The user and system time the guest's vCPU threads have used in this
     * boot, in nanoseconds. Two readings of one boot differ by the time the
     * vCPUs spent between them: divided by the elapsed timestamps and
     * processor_count it is the share of the guest's processors that
     * computed. */
    uint64_t processor_time_ns;
    /* Frames that reached the screen, and frames the guest offered, since the
     * display was made. The display outlives a restart, so neither starts
     * again; both are 0 while there is no display. */
    uint64_t presented_frames;
    uint64_t offered_frames;
    /* The guest's RAM, and the part of it the host holds now. Both are 0
     * before the guest's kernel has claimed its RAM. */
    uint64_t memory_bytes;
    uint64_t memory_held_bytes;
    /* How many times the host told the guest that its GPU had no room for a
     * graphics allocation, since the display was made, so a restart does not
     * reset it. Only an answer the guest received counts, after the host has
     * freed what it could and tried again; a Venus (Vulkan) allocation the
     * host refused is not counted. A rise means a guest application may have
     * lost its graphics state; 0 while there is no display. */
    uint64_t gpu_memory_refusals;
    /* The host GPU's device memory budget, B, and what the process holds
     * against it, in bytes. The usage is the count the budget refuses an
     * allocation by: the larger of Metal's allocation less the host memory the
     * drivers only import and the graphics footprint, plus the reservations
     * in flight. It includes memory the host's drivers keep for reuse, so it
     * is above what the guest's own allocations add up to. Both are 0 while
     * there is no budget: no GPU yet, or a runtime that gives its guest none. */
    uint64_t gpu_memory_budget_bytes;
    uint64_t gpu_memory_used_bytes;
} kernarium_runtime_statistics;
/* Fills `statistics` (the caller sets struct_size) with the running guest's
 * host-side figures. Returns 0, -ENODEV with no guest running (before its
 * kernel starts, between two boots, after it ends), or -EINVAL for a null or
 * short structure. It reads counters, about 0.1 ms, and never waits for the
 * guest; any thread. */
typedef int (*kernarium_runtime_read_statistics_function)(
    kernarium_runtime_statistics* statistics);

/* How many holders one graphics memory reading carries, and the room for a
 * holder's command name (the guest kernel's, at most 15 characters). */
#define KERNARIUM_RUNTIME_GRAPHICS_HOLDERS 8U
#define KERNARIUM_RUNTIME_GRAPHICS_NAME_BYTES 16U

/* One guest process (or one context the host cannot tell the process of) and
 * the host GPU memory its graphics contexts hold. */
typedef struct kernarium_runtime_graphics_holder {
    /* The guest process, as its kernel numbers it; 0 for a context whose
     * creator the host cannot tell, which a guest kernel without the process
     * naming gives for every context. */
    uint32_t pid;
    /* How many of the process's contexts exist. */
    uint32_t context_count;
    /* The bytes it holds. A buffer two processes share is counted in full for
     * each. */
    uint64_t bytes;
    /* Milliseconds since the process's most recently active context last ran a
     * command. */
    uint64_t idle_ms;
    /* The process's command name, or an unnamed context's debug name,
     * truncated and null-terminated. */
    char name[KERNARIUM_RUNTIME_GRAPHICS_NAME_BYTES];
} kernarium_runtime_graphics_holder;

/* The guest's graphics memory budget and who holds it, for an application
 * that frees graphics memory by ending the guest app that holds it. The
 * caller sets the first three fields; the rest is the answer. */
typedef struct kernarium_runtime_graphics_memory {
    /* Set by the caller to sizeof the structure. */
    uint32_t struct_size;
    /* Only holders quiet for at least this many milliseconds, and holding at
     * least minimum_holder_bytes, are listed. */
    uint32_t minimum_idle_ms;
    uint64_t minimum_holder_bytes;
    /* How many entries of `holders` are filled. */
    uint32_t holder_count;
    /* 1 while the guest's own low-memory killer asks the host for the budget's
     * holders and ends them itself (runtime API 29); an application that ends
     * holders on its own stands down meanwhile. 0 otherwise. */
    uint32_t guest_reclaims;
    /* The device memory budget, and what the process holds against it. The
     * host's drivers keep memory they could reuse, so usage_bytes is not what
     * the guest holds. 0 while no renderer has a budget. */
    uint64_t budget_bytes;
    uint64_t usage_bytes;
    /* What the guest's contexts hold of it, named or not, every buffer once:
     * the part of usage_bytes the host can credit to a guest context. */
    uint64_t context_bytes;
    /* As kernarium_runtime_statistics.gpu_memory_refusals, from the same
     * instant. */
    uint64_t memory_refusals;
    /* Largest first. */
    kernarium_runtime_graphics_holder holders[KERNARIUM_RUNTIME_GRAPHICS_HOLDERS];
} kernarium_runtime_graphics_memory;
/* Fills `memory` (the caller sets struct_size and the two filters) with the
 * running guest's graphics memory. Returns 0, -ENODEV with no guest running
 * or before its GPU starts, or -EINVAL for a null or short structure. It reads
 * counters, about 0.1 ms, and never waits for the guest; any thread. */
typedef int (*kernarium_runtime_read_graphics_memory_function)(
    kernarium_runtime_graphics_memory* memory);

typedef struct kernarium_runtime_guest_memory {
    /* Set by the caller to sizeof the structure. */
    uint32_t struct_size;
    uint32_t reserved;
    /* The guest kernel's own total and available memory (MemTotal and
     * MemAvailable), in bytes. Its memory in use is total_bytes minus
     * available_bytes. */
    uint64_t total_bytes;
    uint64_t available_bytes;
} kernarium_runtime_guest_memory;
/* Asks the guest kernel for its own memory figures through its management
 * console. Returns 0; -ENODEV with no guest running; -EAGAIN, with nothing
 * sent, when asking could wake a guest that sleeps (the application quiesced
 * it, a lifecycle call such as quiesce or sync is in progress, or its display
 * is off, as an Android guest's is before it suspends itself); -ENOTSUP for a
 * kernel without the request; -ETIMEDOUT with no answer in 500 ms; or -EINVAL
 * for a null or short structure. It leaves `memory` alone unless it returns 0.
 * It blocks up to 500 ms, and a quiesce, resume or sync called meanwhile waits
 * for it: call it away from the main thread. */
typedef int (*kernarium_runtime_read_guest_memory_function)(
    kernarium_runtime_guest_memory* memory);

/* What a restart event reports. */
enum {
    /* The guest asked to restart and the old guest is gone: its devices are
     * stopped and the runtime is preparing the next boot, which waits while
     * the application holds the guest's GPU requests or has quiesced the guest
     * (the new guest's GPU probe would otherwise time out and its boot would
     * run without 3D). */
    KERNARIUM_RUNTIME_RESTART_BEGAN = 0,
    /* The restarted guest's management console answers, so what belongs to a
     * kernel, the vCPU throttle and the page-cache trimming, can be applied to
     * the new one: it boots with neither. If the console has not answered
     * within two minutes the event is sent anyway. It is not sent when the
     * guest ends first. */
    KERNARIUM_RUNTIME_RESTART_BOOTED = 1,
};
/* `restart_count` is the number of times this launch's guest has restarted,
 * 1 for the first restart. An event follows BEGAN with the same count, or the
 * guest ends. It runs on a runtime thread and must return promptly. */
typedef void (*kernarium_runtime_restart_callback)(
    void* context,
    uint32_t event,
    uint32_t restart_count);
/* Sets, or with a null callback clears, the one receiver of restart events.
 * Once it returns, no call to the previous callback is running or will start,
 * so its context may be released; the callback must not call this. */
typedef void (*kernarium_runtime_set_restart_callback_function)(
    kernarium_runtime_restart_callback callback,
    void* context);

/* Opens a stream to the guest's vsock port `port` (an Android guest's adbd
 * listens on 5555) and returns a connected SOCK_STREAM descriptor that the
 * caller owns and closes, or a negated errno: -ENODEV when no guest runs,
 * -ECONNREFUSED when nothing listens on the port (yet), -ETIMEDOUT when the
 * guest did not answer within timeout_ms (a guest without the vsock driver, or
 * one the host quiesced), -EMFILE when 32 connections are open, -ECONNRESET
 * when the guest stops or restarts first, and -EINVAL for a port that cannot
 * be named. A guest that suspended itself is woken first, as network traffic
 * wakes it. The stream ends when the guest closes it, stops or restarts, and
 * no other process or guest application can reach it. Wait for the descriptor
 * with poll or kqueue and read what they report: the guest's half-close
 * arrives as shutdown(SHUT_WR) on the peer end, which a blocking read can miss
 * (docs/architecture.md, "Local stream sockets"). It blocks: call it away from
 * the main thread. */
typedef int (*kernarium_runtime_connect_vsock_function)(
    uint32_t port,
    uint32_t timeout_ms);

/* The application resolves only this versioned getter after loading exactly
 * one runtime framework. The table is immutable and remains valid until
 * process exit. The selected framework owns one host runtime and its
 * matching code-signed kernel carrier. */
typedef struct kernarium_runtime_api {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t guest_page_size;
    uint32_t guest_flavor;
    kernarium_runtime_run_function run;
    kernarium_runtime_request_stop_function request_stop;
    kernarium_runtime_sync_function sync;
    kernarium_runtime_quiesce_function quiesce;
    kernarium_runtime_resume_function resume;
    kernarium_runtime_send_touch_function send_touch;
    kernarium_runtime_send_key_function send_key;
    kernarium_runtime_set_console_output_callback_function
        set_console_output_callback;
    kernarium_runtime_send_console_input_function send_console_input;
    kernarium_runtime_set_gpu_hold_function set_gpu_hold;
    kernarium_runtime_set_frame_sink_function set_frame_sink;
    kernarium_runtime_set_background_throttle_function set_background_throttle;
    kernarium_runtime_set_page_cache_trimming_function
        set_page_cache_trimming;
    kernarium_runtime_set_microphone_function set_microphone;
    kernarium_runtime_set_restart_callback_function set_restart_callback;
    kernarium_runtime_guest_display_active_function guest_display_active;
    kernarium_runtime_plan_gpu_budget_function plan_gpu_budget;
    kernarium_runtime_connect_vsock_function connect_vsock;
    kernarium_runtime_read_statistics_function read_statistics;
    kernarium_runtime_read_guest_memory_function read_guest_memory;
    kernarium_runtime_cancel_touch_function cancel_touch;
    kernarium_runtime_quiesce_resident_function quiesce_resident;
    kernarium_runtime_read_graphics_memory_function read_graphics_memory;
} kernarium_runtime_api;

#if defined(__GNUC__)
#define KERNARIUM_RUNTIME_EXPORT __attribute__((visibility("default")))
#else
#define KERNARIUM_RUNTIME_EXPORT
#endif

KERNARIUM_RUNTIME_EXPORT
const kernarium_runtime_api* kernarium_runtime_get_api_v28(void);

/* An automation-only extension, outside the versioned table: only a runtime
 * built for automation exports this symbol, and the application looks it up
 * without requiring it. It returns nonzero when the launch asked, through the
 * environment, for the background job to start as soon as the guest runs, and
 * zero otherwise. The setting's name is in the automation runtimes alone, so a
 * release runtime cannot be asked. */
KERNARIUM_RUNTIME_EXPORT
int kernarium_runtime_automation_job_at_start(void);

/* Another automation-only extension, found the same way: how many
 * milliseconds after the application enters the background the launch asked
 * for the guest's GPU to be held, instead of at once, so a measurement can
 * find how long the host GPU stays usable there. Zero means no delay, and a
 * runtime built for release has no such symbol. */
KERNARIUM_RUNTIME_EXPORT
int kernarium_runtime_automation_hold_delay_ms(void);

#ifdef __cplusplus
}
#endif

#endif
