# Platform video encoder

`video_encoder.hpp` is the platform's hardware video encoder interface; nothing
in it names VideoToolbox, V4L2 or the Virtio wire. The Apple backend is
`apple/video_encoder.mm` (sessions), `apple/video_encoder_discovery.mm`
(capability discovery and its cache) and `apple/video_encoder_bitstream.hpp`
and `apple/video_encoder_levels.hpp` (parameter-set readers and the level
policy). It builds as `UML::VideoEncoder` for macOS, iOS device and iOS
Simulator, and only through public VideoToolbox API. The [Virtio encoder
device](../device/video/README.md#encoder-device-id-30) maps this interface to
the wire and depends on nothing below it. The decoder counterpart is
`video_decoder.hpp` (`UML::VideoDecoder`).

## Discovery

`videoEncodeCapabilities()` returns what an encode on this host proved: a
profile, input layout, size, rate-control mode or other capability is listed only
after a stream that used it came out right. VideoToolbox lists no ranges, so
discovery drives the production encoder, and `prepareVideoEncodeCapabilities()`
runs it on its own thread; the first query waits for it, and a host that
answers nothing reports no encoder. A capability never lists a picture side
below `kVideoEncodeMinimumSide`, and profiles a guest API cannot name (ProRes,
HEVC range extensions, alpha, depth) are not probed.

The result is cached as JSON keyed by the host model, OS build and a format
revision. `UML_VIDEO_ENCODE_CACHE` names the file and an empty value turns the
cache off; a missing, foreign or unreadable file is ignored and replaced.

## Sessions

Hardware is required. The caller writes each frame once, into a pixel buffer
from the session's own pool (`acquireFrame()`, null when the pool is in flight),
and the encoder takes it without a second copy. Every submitted frame is
answered exactly once.

- **Output** is Annex B in decode order, with parameter sets placed as the
  settings ask. Frame type is read from the sample.
- **B-frames** are coded in one pattern no property shortens: a `bFrames`
  request at or above the discovered run codes the pattern, below it none.
- **Levels.** A stream states at least the level it was asked for and at least
  the lowest level its picture size, frame rate and bit rate fit, and the
  encoder lowers VideoToolbox's choice only to a level the stream provably
  fits (`apple/video_encoder_levels.hpp`). The level is that of the settings a
  session is made from; a rate or frame-rate update does not restate it, so a
  caller that will raise the rate asks for the level it needs.
- **Input and colour.** A layout is listed for a profile only when an encode
  from it came out right. `rgba` is `bgra` with its red and blue bytes
  exchanged on submit, and a full-range request from either is converted by the
  platform because VideoToolbox converts RGB to video range only. The colour
  description (`VideoEncodeColor`) is attached to the buffers and states itself
  in the stream's VUI, each aspect independently.
- **Rate control.** The standard encoder runs real-time. The low-latency encoder
  is chosen by the platform, never by the caller, when a stream needs temporal
  layers or a QP per frame type. `update()` applies a bitrate, peak or frame
  rate change to the running session; a changed quality or QP bound replaces
  the session between two frames and continues from a key frame.

## Errors and recovery

`configure()` answers `invalid`, `unsupported`, `exhausted` or `failed`. A
session the system revokes is rebuilt at the same settings and the unanswered
frames are encoded again, the first as a key frame. A second loss with no
output in between ends the stream: every frame in flight and every later one
is answered with an error until `reset()` starts a new stream.

## Tests

`platform-video-encoder-bitstream` needs no hardware. `platform-video-encoder`
exercises every advertised capability on the host's encoder and decodes the
streams with the platform decoder. `platform-video-encoder-recovery` links a
variant of the backend that fails sessions on demand, and
`platform-video-encoder-cache` runs child processes against cache files. The
three hardware tests share the `video-encoder-hardware` resource lock.
`Kernarium/KernariumTests/HostVideoEncoderTests.mm` checks the same on an
iPad.
