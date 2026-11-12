# Virtio video decoder

`UML::VideoDevice` exposes a stateful V4L2 decoder through Virtio device 31. The
device owns command validation, stream state, guest buffer mappings, and
completion events. It reuses the common vhost-user transport; decoding belongs
to `UML::VideoDecoder` in the platform layer, which requires VideoToolbox
hardware decoding. No GPU context is required. Encoding is a separate device,
ID 30, described [below](#encoder-device-id-30).

macOS enables the service with `--video`. Kernarium supplies it to each
supported kernel profile when hardware decoding is available. The shared
mainline and Android driver creates `/dev/video0`; the Steam image provides the
`/dev/video-dec0` udev alias used by CEF.

The device advertises the H.264, HEVC, VP9 and JPEG profiles the platform
capability reports and the wire can name, and drops any other rather than
publishing an unnamed format. Decoded surfaces are copied into guest NV12,
planar YUV420, P010, or packed BGRA buffers. Rows lie at the pitch
`raw_pitch.hpp` states and the guest drivers report as `bytesperline`, so a
client and the guest's allocator agree on a layout the host GPU can sample
without asking each other.

## Decoder contract

- **Capture format.** A client chooses its capture format only after the
  source-change event. The device reads each packet's headers without decoding,
  announces the size, and holds the packet, within a bounded count, until the
  client names a format, so an ordinary session converts nothing. A client that
  changes format mid-stream gets the new layout at the next random access
  point, and frames that still arrive planar are converted to what the client
  asked for.
- **Shared-memory buffers.** With `VIRTIO_VIDEO_F_RESOURCE_SHM_REGION` a capture
  plane may name a region and offset of a host-visible Virtio-GPU blob. The
  device offers the feature only when it was given the shared-memory index, and a
  buffer whose blob has gone completes with an error.
- **Display order.** The H.264 and HEVC decoders derive output order from the
  stream (`apple/h264_stream.cpp`, `apple/hevc_stream.cpp`), because a V4L2
  client cannot reorder. H.264 field pictures are not modelled and leave at once
  in decode order; VP9 and JPEG pictures leave as decoded.
- **Capture minimum.** From the size announcement on, the capture queue's
  `min_buffers` is the stream's reorder depth plus one, and four at the least.
- **Seek.** `QUEUE_CLEAR` on the coded queue drops pictures, references and
  order state, and keeps the parameter sets and the capture format.
- **Resolution change.** The device delivers every picture of the old size,
  then one empty capture buffer flagged `VIRTIO_VIDEO_DEQUEUE_FLAG_EOS` (V4L2
  LAST), then `VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED`. It holds back the
  coded buffer that carries the new size until the client sets up the new
  capture queue, stops it, drains or clears the coded queue. A drain a client
  asked for ends at the first LAST buffer, so a change inside it gets no marker
  of its own. The first size of a stream is the event alone, unless the client
  set the capture queue up for another size before it (v4l2_codec2's provisional
  capture), which the device treats as a size change. The wire side is in
  [`../../../kernel/common/video/README.md`](../../../kernel/common/video/README.md).
- **Limits.** The device lists a format's levels from the lowest up to the
  highest the platform capability reports, and picture sizes up to its maximum
  per axis. HEVC states levels 1 to 6.2 with no tier. H.264 stops at 5.2,
  because the hardware does not open the 8192 x 4320 frame a level 6 stream may
  carry. `platform-video-decoder` decodes at the stated maximums, so a host whose
  hardware does less fails that test and not a guest.
- **Flow.** Input is complete Annex B access units or complete JPEG and VP9
  frames. The coded queue's buffer size follows the picture size the client
  states. Event buffers are bounded by the transport's virtqueue size, and a
  full queue applies backpressure instead of completing empty events. The
  device holds an encoded packet back while the hardware has no capture buffer
  to decode into.

`UML_VIDEO_ACTIVITY_LOG` names a file in which the device appends the profiles
it advertised and each stream's codec and totals, since a guest that falls back
to software decoding looks the same from inside. Unset, nothing is written.

`virtio-video-protocol` drives the decoder with a synthetic decoder;
`platform-video-decoder` decodes the checked-in fixtures on Apple hardware; the
optional `guest-v4l2-decode` runs the real V4L2 driver against every advertised
profile (see [testing](../../../docs/testing.md) for the guest probe input). The
tests that use the host's hardware share the `video-encoder-hardware` resource
lock and run in `check-gpu`.

## Encoder (device ID 30)

The encoder is a second Virtio device instance with the standard encoder ID,
beside the decoder's 31. It has its own vhost-user service, so its asynchronous
callbacks and queues stay apart from the synchronous decoder, a guest kernel
without the encoder never binds it, and a host without a hardware encoder never
creates it. The [wire contract](../../../kernel/common/video/README.md#encoder-device-id-30)
and the platform interface (`darwin/platform/video_encoder.hpp`) are its
contract; the device depends only on that interface, the memory layer and the
device contracts, never on VideoToolbox. The capability and control tables are
pure functions of the platform's capabilities (`encoder_capabilities.cpp`,
`encoder_controls.cpp`), so tests check them without a device.

macOS enables the encoder with `--video-encoder`; `--video` stays decoder-only.
The Android launcher passes both, and Kernarium creates the encoder service
beside the decoder's whenever the platform reports a hardware encoder. An
explicit `--video-encoder` on a host without one fails to start.

A guest reaches the encoder as a stateful V4L2 encoder (`stateful-encoder` video
node). Raw frames are copied once into a buffer the platform owns, and the
guest's input buffer completes as soon as that copy is done. Coded buffers are
Annex B for H.264 and HEVC and a complete JFIF image for JPEG, in decode order,
each carrying its source frame's timestamp. Discovery is the platform's, so a
control, format or menu item that no encode on this host proved is absent, and
the guest learns the host's capability from what exists.

The device guarantees:

- **Session.** The platform session is created at the stream's first raw frame
  from the controls and formats settled by then; afterwards controls that are
  not dynamic and the formats answer `ERR_BUSY`. A set of controls applies
  entirely or not at all. `QUEUE_CLEAR` ends the session, returns the buffers
  that queue held with an error, and the next frame starts a new session at a key
  frame.
- **Layout.** The device decides each raw plane's size and stride and reports
  them in `GET_PARAMS`; a smaller buffer is refused at attach.
- **Buffers.** A raw buffer completes once copied, before encoding. When the
  platform has no free frame the device holds the raw buffers; a bounded number
  of coded frames wait in the device for coded buffers.
- **Output.** A frame larger than its buffer completes with `ERR` and the next
  frame is a key frame. `PREPEND_SPSPPS_TO_IDR` and `HEADER_MODE_SEPARATE`
  decide where parameter sets appear.
- **Drain and failure.** `STREAM_DRAIN` completes when every earlier frame has
  returned, and the next coded buffer completes empty with `EOS`. A session the
  platform refuses completes the stream's frames with `ERR` and sends
  `VIRTIO_VIDEO_EVENT_ERROR`; the guest recovers with `QUEUE_CLEAR`.
- **Limits.** At most eight streams, or the platform's concurrent session limit
  if lower.

With `UML_VIDEO_ACTIVITY_LOG` set the encoder also appends lines marked
`encoder`: what it advertised, each stream's settings and its totals at close.
They are the evidence that the hardware encoded.

### Encoder controls

The host publishes controls through `VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS` and
the guest driver registers exactly what is published. IDs and menu indices are
the Linux UAPI values in
[`virtio_video_v4l2_ids.h`](../../../kernel/common/video/virtio_video_v4l2_ids.h),
which `sync-protocol.py` checks against the kernel. Every control is grabbed
while the stream encodes unless it is dynamic (bit rate, peak, quality, force
key frame, parameter-set prepending, JPEG quality, maximum QP). Which controls
exist, and when, is `encoder_controls.cpp`:

- Bit-rate mode, bit rate, GOP size, force key frame, header mode and
  parameter-set prepending are always published; B-frames only for a codec with
  a B-frame run; the peak, constant-quality, constant-QP, VBV/CPB and
  hierarchical-layer controls only when discovery verified the matching platform
  capability; H.264 and HEVC controls only for a codec the host encodes.
- The profile, level and tier menus list only items an encode wrote a matching
  SPS for. The stream states the level asked for, or the lowest its size, frame
  rate and bit rate fit if that is higher.
- Raw formats are NV12, YUV420, P010 (HEVC Main 10), packed BGRA and RGBA (the
  memory order of Android's AB24 surfaces), and NV16 and NV24 for H.264 High
  4:2:2 and 4:4:4; each is listed only when some profile encoded from it. The
  coded formats are `H264`, `HEVC` and `JPEG`.
- Colour and HDR travel as V4L2 colorspace, transfer, matrix and quantization
  values in `SET_STREAM_INFO` and as the HEVC Main 10 HDR10 compound controls;
  they are written into every pixel buffer and the stream's VUI, and
  `GET_STREAM_INFO` on the coded queue returns what was written.
- The device publishes nothing the host can neither honor nor emulate:
  multi-slice, intra refresh, long-term references, average QP, frame skip and
  B-frame hierarchical coding. Profiles no guest API can name (ProRes, HEVC
  range extensions and alpha) are not exposed.

`virtio-video-encoder-protocol` drives both queues in front of a synthetic
encoder (`tests/support/video`) and then the host's hardware encoder;
`video-encoder-capabilities` checks the capability and control tables without a
device and that the wire lists exactly what discovery found.
