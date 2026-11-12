# Private Virtio video wire contract

`virtio_video.h` owns the packed little-endian protocol shared by the host
Virtio video devices and the maintained mainline and Android kernel drivers. It
is a project-local version of the stateful Virtio video draft, with the decoder
as device ID 31 and the encoder as device ID 30, and it is independent of the
public API and the host/kernel bridge ABI. The wire format is the same for every
maintained kernel slice.

Edit this canonical header, then use `sync-protocol.py --kernel-source PATH` to
update the copy under `include/uapi/linux/` in each patch-producing kernel
checkout. Amend the owning video topic and regenerate each line's series;
ordinary checks use `sync-protocol.py --kernel-source PATH --verify`. Never edit
generated patch output. A change to the driver runs `guest-v4l2-decode`,
`guest-v4l2-encode` and `guest-video-teardown` before a kernel release.

`virtio_video_v4l2_ids.h` lists the V4L2 numbers the host uses, one
`#define UML_<kernel symbol> <number>` per value. `sync-protocol.py` checks each
against the UAPI headers of the tree it synchronizes, so the host and the
kernel it is built beside cannot disagree.

## Resources and failed buffers

A scatter entry normally names guest-physical memory; with
`VIRTIO_VIDEO_F_RESOURCE_SHM_REGION` negotiated it may name an offset inside a
virtio shared-memory region instead, for a host-visible buffer with no
guest-physical address. The host validates stream IDs, buffer extents, and
response capacities.

`VIRTIO_VIDEO_CMD_RESOURCE_ATTACH` carries a buffer's scatter lists as
device-readable data after the command. A driver may spread them over any
number of descriptors and must not need one block of kernel memory for them.
The device accepts at most 32768 entries per plane and 128 MiB per resource,
and the request has to fit the command queue's descriptors, because the device
accepts no indirect ones.

A capture buffer completed with `VIRTIO_VIDEO_DEQUEUE_FLAG_ERR` or
`VIRTIO_VIDEO_DEQUEUE_FLAG_EOS`, on the decoder and the encoder alike, reaches
the client with no payload and the timestamp of the picture or frame the device
failed to deliver, or 0 for a buffer that stands for none. The
`guest-video-error-answer` test checks it.

## Decoder

The wire names the H.264 levels 1.0 to 6.2, the HEVC levels 1 to 6.2, and
Constrained Baseline and Constrained High as H.264 profiles of their own. The
host lists the levels up to the highest its decoder decodes, and the driver
turns them into the read-only V4L2 profile and level menus; a value appended to
the wire needs its row in `virtio_video_helpers.c`, which reads an unknown value
as the menu's first entry. The [device README](../../../darwin/device/video/README.md)
owns which levels the host decodes.

The device ends a picture size with one more capture buffer, empty and flagged
`VIRTIO_VIDEO_DEQUEUE_FLAG_EOS`, after every picture of that size, and only then
posts `VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED`. The driver reports the
buffer as `V4L2_BUF_FLAG_LAST` and reads the new parameters before it queues
`V4L2_EVENT_SOURCE_CHANGE`; `VIDIOC_DQBUF` then fails with `EPIPE` until the
client restarts the capture queue. A drain the client asked for ends the old size
at the drain's own last buffer, so a client takes the first LAST buffer it sees
as the end of its drain. The `guest-v4l2-decode` probe holds both orders.

## Encoder (device ID 30)

The encoder is its own device instance, and its commands are additions the
decoder never uses. It offers `VIRTIO_VIDEO_F_V4L2_CONTROLS`, the driver binds
device 30 only when it is negotiated, and a decoder never offers it. The input
queue carries raw frames (V4L2 OUTPUT) and the output queue carries coded
buffers (V4L2 CAPTURE). The comment above `virtio_video_resource_queue_resp` in
`virtio_video.h` states the buffer rules: timestamps, decode order, completion
of an input buffer once the host has copied it, and drain.

Controls cross the wire as V4L2 control IDs, menu indices, and enum values,
which are stable UAPI. The host lists every control it supports with its type,
range, step, default, menu skip mask, and flags, and the driver registers
exactly that list and forwards values unchanged, so a capability the host
learns needs no driver change. The
[device README](../../../darwin/device/video/README.md#encoder-controls) owns
which controls the host publishes.

The encoder driver gives the host these guarantees: it sends a raw frame only
once the coded queue streams and no drain is pending, so the host builds its
session from settled controls; it reports the host's own plane layout and
ignores a client's strides; and the colorimetry fields of the raw queue go to
the host as the client states them. A `CODEC_CONFIG` buffer reaches the client
with no frame-type flag, because V4L2 has none.

The V4L2 core resets a transfer function it does not know to
`V4L2_XFER_FUNC_DEFAULT` on every `VIDIOC_S_FMT`, and upstream has none for
hybrid log-gamma. The kernel line therefore carries one local UAPI extension,
`V4L2_XFER_FUNC_HLG = 8` (the topic "media: v4l2: add the HLG transfer
function"). If upstream adds an HLG value, adopt upstream's in that topic and in
`virtio_video_v4l2_ids.h`.
