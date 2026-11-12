# Video decoder fixtures

These synthetic fixtures feed the platform video decoder tests
(`tests/unit/platform/`) and the guest V4L2 probe
(`tests/integration/video/v4l2_decode_probe.cpp`). They cover H.264 Baseline,
Constrained Baseline, Main, High, Constrained High and High10; HEVC Main and
Main10; baseline 4:2:0 JPEG; and VP9 Profile 0; plus streams that exercise
picture order, cropping, resolution changes, levels and tiers. The file names
state each stream's feature, and the tests that read it state what it checks.

- Each `.bin` is one Annex B access unit, a JPEG image, one VP9 frame, or (for
  the order and resolution-change streams) a whole Annex B stream, with no
  container. Its `.raw` reference is tightly packed NV12 or P010, with no row
  padding.
- The patterns contain no third-party imagery. The order and resolution-change
  streams are flat gray pictures whose luma rises by a fixed step per picture
  in display order, so a test names a decoded picture by its brightness and
  sees any that is lost, repeated or out of order.
- The multi-picture streams repeat their parameter sets at each random access
  point, and none of the added streams carries an encoder identification
  string.
- The platform test decodes only the profiles, levels and tiers the host's
  capability probe reports, so nothing is advertised that the hardware does
  not decode.

They are produced with ffmpeg, libx264 and libx265, for example
`ffmpeg -f lavfi -i "color=c=black:size=64x64:rate=30,format=yuv420p,geq=lum='16+10*N':cb=128:cr=128" -frames:v 20 -c:v libx265 -x265-params "bframes=3:b-pyramid=1:keyint=12:min-keyint=12:open-gop=0:no-scenecut=1:b-adapt=0:no-info=1:repeat-headers=1" -f hevc hevc-bframes.bin`;
H.264 streams use `-bsf:v filter_units=remove_types=6` to drop the
identification SEI.

## Synthetic encoder

`synthetic_encoder.hpp` is a `uml::platform::VideoEncoder` that encodes
nothing: each raw frame becomes a small Annex B (or JFIF) unit that names the
frame, so a test sees from the guest's coded buffer which frame crossed, in
what order, and whether it was a key frame. The Virtio video encoder device
tests link it as `UML::TestSyntheticEncoder`; no production target does.
