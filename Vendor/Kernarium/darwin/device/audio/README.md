# Virtio Sound

`UML::AudioDevice` serves Virtio device 25 over the common vhost-user
transport. It exposes two 48 kHz signed-16-bit streams in one function group,
which the guest driver presents as the two directions of PCM device 0: stereo
playback, and capture of one or two channels. Mixer controls and jacks are not
exposed. The device owns protocol validation and stream state; the host output
belongs to a `PlaybackSink` and the captured audio to a `CaptureSource`.

macOS and Android enable the service with `--audio`; Kernarium supplies it to
every guest it runs.

## Timing

The transport worker runs the device and polls one descriptor, which the
device's `PeriodClock` owns. The clock is the only time base the device uses,
so tests inject one and advance it by hand. The Apple output signals the worker
from AVFAudio callbacks through a bounded completion ring, and the callback path
does not allocate or lock. iOS owns `AVAudioSession` interruption and route
recovery inside `ApplePlaybackSink`.

## Output cycle

While a stream plays, the host output runs at an IO cycle of a quarter of the
guest's buffer, between 64 and 512 frames (`OutputCycle`): the output consumes
a cycle at a time, so a guest with a short buffer keeps time to refill it, and
a long buffer keeps the usual cycle. macOS sets the cycle on the output device
for this process. iOS asks the audio session, which runs at the shortest cycle
its clients ask for, rounded to one the hardware supports.

## Playback pacing

The guest's playback pointer advances with each completion, so the device must
complete periods at the rate real playback would. When the host output cannot
play, the device discards the data and completes each period one period of real
time after the one before, on its `PeriodClock`. A start the output refuses
does not fail the guest's `PCM_START`, and the guest never sees an I/O error
from an output problem. A stopped stream holds its queued periods; a released
stream returns every queued period at once, as the Virtio sound specification
requires.

## Latency

A completed period's status reports, as the device latency, how long its sound
still takes to be heard. The guest adds it to the frames it still has queued
(ALSA's `snd_pcm_delay()`), so it never counts queued periods. A period the
output consumed reports the output's own latency, including the IO cycle by
which it reads ahead, measured whenever the output starts; a period nothing
plays (paced, muted or released) reports 0.

## Interruptions on iOS

When the system takes the audio session (a call, Siri, another app's audio),
`ApplePlaybackSink` mutes: nothing reaches the player, and the sink completes
the guest's periods as `played` on the schedule the hardware would have kept
(`MuteClock`), so the guest sees no pause and its clock keeps its rate. The sink
takes the session back when the system recommends resuming or once no other app
plays primary audio, and it never takes the session from an app that still
plays. The interruption belongs to the session, not the stream: a stream that
starts during one, or the guest suspending and resuming, joins the mute. Taking
the session back runs on another thread while the mute clock goes on, so no
guest write waits longer than its stream's buffer lasts.

## Capture

While the guest records, the device completes one receive buffer per period of
real time on its `PeriodClock`, from `PCM_START` on, whatever the capture source
holds, so the guest's capture clock never depends on a source. A guest that
falls behind loses the oldest audio. The `CaptureSource` fills the front of each
period and leaves the rest zero, and it cannot fail a stream: one that cannot
deliver audio returns nothing and the guest records silence. The default source
is silent, which is what the Android audio policy's built-in microphone needs
to open input streams.

## Microphone

The product records from the host microphone only when the user turns the
Microphone setting on. Off is the default: the guest records silence, nothing
reads or opens the microphone, and no permission prompt can appear.

- `MicrophoneSwitch` holds the setting and the system's permission behind it
  (`UML::MicrophoneAuthorization`). Only the user's act of turning the setting
  on, `enable()`, can show the system prompt; `restore()` carries an earlier
  choice into a new run and never asks. A denied or not-yet-asked microphone is
  the same silence as an off one.
- `MicrophoneCaptureSource` runs the `MicrophoneInput` only while the guest
  records and the switch is active, so the system's recording indicator is lit
  only while a guest listens.
- On iOS the microphone never takes the audio session from an app that plays or
  records, such as a call. The session is shared with the output
  (`audio_session.hpp`); macOS has no audio session.

macOS enables it with `--audio-input=host` (`silent` is the default): the
launcher turns the switch on at start, which is when macOS asks, on behalf of the
app that started it. Kernarium's Microphone setting is described in
[`../../../Kernarium/README.md`](../../../Kernarium/README.md).

## Null output

`--audio-output=null` selects a sink that plays nothing: every period is
discarded and paced as above. A conformance campaign uses it so that tone-playing
tests make no sound. `host` is the default.

## Tests

`virtio-sound-protocol`, `virtio-sound-microphone` (over fakes, never opening
the microphone or showing a prompt), `audio-mute-clock`, `audio-output-cycle`,
`guest-virtio-sound-suspend` (a guest suspended under a playing and a paused
stream, see [`../../../kernel/common/README.md`](../../../kernel/common/README.md))
and `guest-virtio-sound-delay` (a guest's `snd_pcm_delay()` against when its
last period completes) cover the device. `UML_TEST_APPLE_AUDIO=ON` adds
`apple-audio-playback`, `apple-audio-interruption` and
`guest-virtio-sound-delay-host` (see
[`../../../docs/testing.md`](../../../docs/testing.md)); a real interruption on
a device comes from Kernarium's `AudioInterrupter` test application (see
[`../../../Kernarium/README.md`](../../../Kernarium/README.md)).

The real microphone is checked by hand, because granting access is the user's
act: start a guest with `--audio-input=host`, answer the prompt, and record in
the guest (`tinycap` on Android, `arecord` on Linux). The recording indicator
lights while the guest records and goes out when it stops; with the setting off,
or access denied, the capture is silence at the same rate.
