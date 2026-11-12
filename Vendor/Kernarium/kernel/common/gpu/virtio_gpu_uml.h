/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Private Virtio-GPU memory-report extension.
 *
 * The host GPU gives a guest a fixed device-memory budget that the guest's own
 * memory accounting cannot see. This extension lets the host tell the guest
 * kernel how much of the budget is in use and which guest processes hold it.
 * It is negotiated with VIRTIO_GPU_F_UML_MEMORY_REPORT and used by the guest
 * kernel alone: Mesa and libdrm never see it.
 *
 * Bit 23 is the last device-specific feature bit (24 to 40 belong to the
 * transports), and upstream's virtio_gpu.h uses 0 to 4. Command 0x0f00 and
 * response 0x11f0 lie in no range upstream's header uses: its commands are
 * 0x0100, 0x0200 and 0x0300 plus a few, its successes 0x1100 plus a few.
 *
 * The device raises the event with the display event's mechanism (events_read
 * in the configuration space, cleared through events_clear) only after a
 * GET_MEMORY with the SUBSCRIBE flag, and a GET_MEMORY without the flag ends
 * the subscription. A reply is built from the host's counters when the command
 * runs; it never waits for the renderer.
 */
#ifndef VIRTGPU_UML_H
#define VIRTGPU_UML_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/virtio_gpu.h>
#else
#include <stdint.h>
typedef uint8_t __u8;
typedef uint32_t __le32;
typedef uint64_t __le64;
struct virtio_gpu_ctrl_hdr {
	__le32 type;
	__le32 flags;
	__le64 fence_id;
	__le32 ctx_id;
	__u8 ring_idx;
	__u8 padding[3];
} __attribute__((packed));
#endif

#define VIRTIO_GPU_F_UML_MEMORY_REPORT 23

/* events_read bit 1; VIRTIO_GPU_EVENT_DISPLAY is bit 0. */
#define VIRTIO_GPU_EVENT_UML_MEMORY (1 << 1)

#define VIRTIO_GPU_CMD_UML_GET_MEMORY 0x0f00
#define VIRTIO_GPU_RESP_OK_UML_MEMORY 0x11f0

/* GET_MEMORY flags. */
/* Raise VIRTIO_GPU_EVENT_UML_MEMORY when the pool's state changes. */
#define VIRTIO_GPU_UML_MEMORY_SUBSCRIBE (1 << 0)
/*
 * A reclaimer in the guest (Android's lmkd) acts on the reports, so a host
 * that has its own reclaimer stands down while this is set. Set per query: a
 * query without it, and a new connection, clear it.
 */
#define VIRTIO_GPU_UML_MEMORY_GUEST_RECLAIMS (1 << 1)

/* Reply state bits. */
/* Graphics memory use is near the budget and has been for a while. */
#define VIRTIO_GPU_UML_STATE_PRESSURE (1 << 0)
/* An allocation was refused since the previous GET_MEMORY. */
#define VIRTIO_GPU_UML_STATE_REFUSED (1 << 1)

/* The most holders a reply lists. */
#define VIRTIO_GPU_UML_MAX_HOLDERS 16
/* TASK_COMM_LEN, including the terminating NUL. */
#define VIRTIO_GPU_UML_COMM_LEN 16

/*
 * The holders listed are the guest processes that hold at least min_bytes of
 * the device memory and have been idle (no command and no retired fence of
 * any of their contexts) for at least min_idle_ms, the largest first, at most
 * max_holders of them (and no more than fit the reply buffer the guest gave).
 */
struct virtio_gpu_uml_get_memory {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 flags;
	__le32 max_holders;
	__le32 min_idle_ms;
	__le32 padding;
	__le64 min_bytes;
};

struct virtio_gpu_uml_memory_holder {
	__le64 bytes;
	/* The guest process (thread group ID in the initial PID namespace). */
	__le32 pid;
	/* How many of its Virtio-GPU contexts there are. */
	__le32 contexts;
	/* Saturates at 0xffffffff. */
	__le32 idle_ms;
	/* The process's command name, NUL-terminated. */
	char comm[VIRTIO_GPU_UML_COMM_LEN];
	__le32 padding;
};

struct virtio_gpu_uml_memory {
	struct virtio_gpu_ctrl_hdr hdr; /* type VIRTIO_GPU_RESP_OK_UML_MEMORY */
	/* The budget B, and 0 when the host has none. */
	__le64 budget_bytes;
	/* What the host GPU holds against it, caches the drivers keep included. */
	__le64 used_bytes;
	/* What the guest's contexts hold of it. */
	__le64 context_bytes;
	/* Allocations refused since the display was made. */
	__le64 refusals;
	__le32 state;
	/* How many events the host has raised in this connection. */
	__le32 generation;
	__le32 holder_count;
	__le32 padding;
	struct virtio_gpu_uml_memory_holder holders[];
};

#endif /* VIRTGPU_UML_H */
