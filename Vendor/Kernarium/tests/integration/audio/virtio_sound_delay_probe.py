#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-virtio-sound-delay: the delay a playing Virtio sound
stream reports says when the frame written last will be heard.

Usage: virtio_sound_delay_probe.py [MILLISECONDS]

snd_pcm_delay() is what a player keeps its pictures in step with its sound
by. The ALSA core counts it as the frames the application has queued, which
the stream position follows as the device completes periods, plus the
latency past a completion that each completion's status reports. The probe
streams four periods of 40 ms, first keeping all four queued with blocking
writes, then refilling only once two are left, and reads the stream's status
after every write. The latency past a completion, the delay less the queued
frames, must be the same at every read, whatever the queue holds, and no
more than MILLISECONDS (default 0: the null output plays nothing after a
completion). A device that counted its queued periods in it would report
more for a fuller queue. Then the probe fills the queue, reads
snd_pcm_delay() and drains. The drain ends when the last period completes,
which is the delay less that latency after the read, or up to a period
sooner, because the stream position moves a period at a time.

Prints "VSND delay ok" or "VSND delay FAIL: <reason>" with what it measured;
the exit status is the number of failures.
"""
import ctypes
import re
import sys
import time

RATE = 48000
CHANNELS = 2
FRAME = 4
FORMAT_S16_LE = 2
ACCESS_RW_INTERLEAVED = 3
# Four periods of 40 ms.
LATENCY_US = 160000
HW_PARAMS = "/proc/asound/card0/pcm0p/sub0/hw_params"
# The first completions come before the device has reported any latency.
WARMUP_SECONDS = 0.5
FULL_SECONDS = 1.5
SHALLOW_SECONDS = 1.5
REFILL_SECONDS = 0.3
# Periods left queued when the shallow phase refills: two leave 80 ms to
# refill in.
SHALLOW_PERIODS = 2
POLL_SECONDS = 0.002
# How late the guest may notice that the last period completed.
SLACK_SECONDS = 0.03

alsa = ctypes.CDLL("libasound.so.2")
alsa.snd_pcm_open.argtypes = [
    ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_int, ctypes.c_int
]
alsa.snd_pcm_set_params.argtypes = [
    ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
    ctypes.c_int, ctypes.c_uint,
]
alsa.snd_pcm_writei.restype = ctypes.c_long
alsa.snd_pcm_writei.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
alsa.snd_pcm_delay.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_long)]
alsa.snd_pcm_status_sizeof.restype = ctypes.c_size_t
alsa.snd_pcm_status.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
alsa.snd_pcm_status_get_delay.restype = ctypes.c_long
alsa.snd_pcm_status_get_delay.argtypes = [ctypes.c_void_p]
alsa.snd_pcm_status_get_avail.restype = ctypes.c_ulong
alsa.snd_pcm_status_get_avail.argtypes = [ctypes.c_void_p]
alsa.snd_strerror.restype = ctypes.c_char_p
alsa.snd_strerror.argtypes = [ctypes.c_int]
for name in ("snd_pcm_drain", "snd_pcm_close"):
    getattr(alsa, name).argtypes = [ctypes.c_void_p]

PARAMETER = re.compile(r"(period_size|buffer_size): (\d+)")


class Failure(Exception):
    """An assertion failed; the message says what was measured."""


def describe(code):
    return f"{code} ({alsa.snd_strerror(int(code)).decode()})"


def milliseconds(frames):
    return frames * 1000.0 / RATE


class Stream:
    def __init__(self):
        self.pcm = ctypes.c_void_p()
        code = alsa.snd_pcm_open(ctypes.byref(self.pcm), b"hw:0,0", 0, 0)
        if code < 0:
            raise Failure(f"the open returned {describe(code)}")
        code = alsa.snd_pcm_set_params(
            self.pcm, FORMAT_S16_LE, ACCESS_RW_INTERLEAVED, CHANNELS, RATE, 0,
            LATENCY_US,
        )
        if code < 0:
            alsa.snd_pcm_close(self.pcm)
            raise Failure(f"snd_pcm_set_params returned {describe(code)}")
        with open(HW_PARAMS) as parameters:
            sizes = dict(
                (key, int(value))
                for key, value in PARAMETER.findall(parameters.read())
            )
        self.period = sizes["period_size"]
        self.buffer = sizes["buffer_size"]
        self.data = ctypes.create_string_buffer(self.period * FRAME)
        self.status = ctypes.create_string_buffer(alsa.snd_pcm_status_sizeof())

    def close(self):
        alsa.snd_pcm_close(self.pcm)

    def write(self):
        written = alsa.snd_pcm_writei(self.pcm, self.data, self.period)
        if written < 0:
            raise Failure(f"a write returned {describe(written)}")
        if written != self.period:
            raise Failure(f"a write took {written} of {self.period} frames")

    def read(self):
        """The frames queued and the latency past a completion, in frames,
        both of one moment."""
        code = alsa.snd_pcm_status(self.pcm, self.status)
        if code < 0:
            raise Failure(f"snd_pcm_status returned {describe(code)}")
        queued = self.buffer - alsa.snd_pcm_status_get_avail(self.status)
        return queued, alsa.snd_pcm_status_get_delay(self.status) - queued

    def stream(self, seconds, refill_at, latencies, warmup=0.0):
        """Write a period whenever no more than `refill_at` frames are
        queued, for `seconds`, and collect the latency past a completion
        after each write once `warmup` has passed. A blocking write keeps
        the queue full when `refill_at` is the buffer."""
        started = time.monotonic()
        while time.monotonic() - started < seconds:
            while refill_at < self.buffer and self.read()[0] > refill_at:
                if time.monotonic() - started > seconds + 1.0:
                    raise Failure("the queue stopped draining")
                time.sleep(POLL_SECONDS)
            self.write()
            if latencies is not None and time.monotonic() - started >= warmup:
                latencies.add(self.read()[1])


def measure(limit_ms):
    stream = Stream()
    period = stream.period
    latencies = set()
    try:
        stream.stream(FULL_SECONDS, stream.buffer, latencies, WARMUP_SECONDS)
        stream.stream(SHALLOW_SECONDS, SHALLOW_PERIODS * period, latencies)
        stream.stream(REFILL_SECONDS, stream.buffer, None)
        delay = ctypes.c_long()
        code = alsa.snd_pcm_delay(stream.pcm, ctypes.byref(delay))
        read = time.monotonic()
        if code < 0:
            raise Failure(f"snd_pcm_delay returned {describe(code)}")
        latency = stream.read()[1]
        code = alsa.snd_pcm_drain(stream.pcm)
        drained = time.monotonic() - read
        if code < 0:
            raise Failure(f"the drain returned {describe(code)}")
    finally:
        stream.close()

    predicted = milliseconds(delay.value - latency) / 1000.0
    print(
        f"VSND delay period={period} buffer={stream.buffer} latency past a "
        "completion "
        + ", ".join(f"{milliseconds(value):.1f}" for value in sorted(latencies))
        + f" ms; delay {milliseconds(delay.value):.1f} ms, drained in "
        f"{drained * 1000:.1f} ms, predicted {predicted * 1000:.1f} ms",
        flush=True,
    )
    if len(latencies) != 1:
        raise Failure(
            "the latency past a completion changed with the queue: the "
            "device counts queued periods in it"
        )
    (only,) = latencies
    if only < 0 or milliseconds(only) > limit_ms:
        raise Failure(
            f"snd_pcm_delay counts {milliseconds(only):.1f} ms past a "
            f"completion, outside 0 to {limit_ms:g} ms: a player would show "
            "its pictures that much after their sound"
        )
    if latency != only:
        raise Failure("the latency past a completion changed at the end")
    earliest = predicted - milliseconds(period) / 1000.0 - SLACK_SECONDS
    if not earliest <= drained <= predicted + SLACK_SECONDS:
        raise Failure(
            "the last period did not complete when snd_pcm_delay said"
        )


def main():
    limit_ms = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
    try:
        measure(limit_ms)
    except Failure as failure:
        print(f"VSND delay FAIL: {failure}", flush=True)
        return 1
    print("VSND delay ok", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
