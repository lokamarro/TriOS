#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-virtio-sound-suspend: playback across a system suspend,
and the other states a Virtio sound substream must restart from.

Usage: virtio_sound_suspend_probe.py [CASE...]   (default: every case in turn,
                                                  but the stall cases)

The guest suspends itself the way a host's quiesce does: an RTC alarm two
seconds ahead, then "mem" into /sys/power/state. The ALSA core restarts a
stream that was suspended at offset 0, so the driver must start it afresh. A
stream's driver state may not carry a position, a queued period or a pending
message across a suspend. Each case prints one "VSND <case> ok" or
"VSND <case> FAIL: <reason>" line, and the exit status is the number of
failed cases.

position      Play one period and suspend, with nothing queued. Recover, play
              one more period, and the stream position must be exactly that
              period, not one plus the position the stream had when it was
              suspended. One message is in flight, so completions cannot batch
              and hide a stale position.
fill          Write a period and a half, drop, prepare, write sixteen whole
              periods and drain. The half period the drop left behind must
              not keep its length, which would overfill it and end in -EIO
              (no suspend: it is the fill level a prepare resets).
prepare2      Write a period and a half into a prepared stream, which queues
              the whole period on the device before any start, and prepare it
              three times. Every prepare returns 0 and the stream plays.
fillprep      The same, with a drop before the prepare.
drains        Drain streams that end inside a period (drainshort: 1024 frames,
              drainpartial: 3072 started at 2048, drainprepared: 3072 never
              started) and one that ends on a period (drainwhole). Every
              drain returns 0, the first three within half a second, and the
              substream ends in SETUP.
paused        Play, pause, suspend. snd_pcm_recover must return 0 and the
              writes after it must succeed. A paused stream has no trigger at
              the suspend, but its device still loses the periods it held.
pauseddrained Play one period, let it complete, pause, suspend. The device
              holds no period, so the back-end keeps the stopped stream when
              the rings are disabled and only the driver can release it. Two
              prepares return 0, and after a reopen the device takes its
              parameters and plays.
pausedreopen  Play, pause, suspend, close, open the device again and play.
              The messages the paused stream counted must not make the next
              open refuse. It runs last, because a driver that fails it fails
              every later open too.
stallboundary A guest stall longer than the buffer while the device keeps
              playing. The runner starts it on a one-vCPU guest and stops the
              guest through the management console for 300 ms once the probe
              says "streaming". With the stop threshold at the boundary the
              stream must carry on: its position advances by at least 0.8 s
              of frames in the 1.5 s after the stall, and no write fails
              later than one second after it.
stalldefault  The same with the default thresholds: the underrun xruns exactly
              once, the stream recovers, and it then plays at the rate of
              audio.
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
LATENCY_US = 300000
STATUS = "/proc/asound/card0/pcm0p/sub0/status"
HW_PARAMS = "/proc/asound/card0/pcm0p/sub0/hw_params"
WAKE_ALARM = "/sys/class/rtc/rtc0/wakealarm"
SUSPEND_STATS = "/sys/power/suspend_stats/success"
ALARM_SECONDS = 2
# A stream plays at the null sink's pace, a period in some forty
# milliseconds; every wait below ends at a deadline.
POLL_SECONDS = 0.005
SETTLE_SECONDS = 0.2
PLAY_SECONDS = 1.0
START_SECONDS = 5.0
CHUNK = 1024
ESTRPIPE = 86
EPIPE = 32
EIO = 5
DRAIN_SECONDS = 0.5
STALL_DETECT_SECONDS = 0.2
STALL_SETTLE_SECONDS = 1.0
STALL_WINDOW_SECONDS = 1.5
STALL_ADVANCE = 0.8
STREAM_WARMUP_SECONDS = 2.0
STALL_GIVE_UP_SECONDS = 8.0
TIMEOUT_SECONDS = 30.0

alsa = ctypes.CDLL("libasound.so.2")
alsa.snd_pcm_writei.restype = ctypes.c_long
alsa.snd_pcm_writei.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
alsa.snd_pcm_recover.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
alsa.snd_strerror.restype = ctypes.c_char_p
alsa.snd_pcm_state.argtypes = [ctypes.c_void_p]
alsa.snd_pcm_state_name.restype = ctypes.c_char_p
alsa.snd_pcm_state_name.argtypes = [ctypes.c_int]
alsa.snd_strerror.argtypes = [ctypes.c_int]
alsa.snd_pcm_sw_params_sizeof.restype = ctypes.c_size_t
for name in ("snd_pcm_sw_params_current", "snd_pcm_sw_params"):
    getattr(alsa, name).argtypes = [ctypes.c_void_p, ctypes.c_void_p]
for name in (
    "snd_pcm_sw_params_set_start_threshold",
    "snd_pcm_sw_params_set_stop_threshold",
):
    getattr(alsa, name).argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong
    ]
alsa.snd_pcm_sw_params_get_boundary.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)
]
alsa.snd_pcm_pause.argtypes = [ctypes.c_void_p, ctypes.c_int]
for name in ("snd_pcm_drop", "snd_pcm_prepare", "snd_pcm_drain", "snd_pcm_close"):
    getattr(alsa, name).argtypes = [ctypes.c_void_p]

POINTERS = re.compile(rb"hw_ptr\s*:\s*(\d+)\s*\nappl_ptr\s*:\s*(\d+)")
PARAMETER = re.compile(r"(period_size|buffer_size): (\d+)")


class Failure(Exception):
    """A case's assertion failed; the message says which defect it is."""


def say(text):
    print(text, flush=True)


def describe(code):
    return f"{code} ({alsa.snd_strerror(int(code)).decode()})"


def state(pcm):
    return alsa.snd_pcm_state_name(alsa.snd_pcm_state(pcm)).decode()


def pointers():
    """The core's (hw_ptr, appl_ptr) in frames; reading the status updates
    the hardware pointer through the driver."""
    try:
        with open(STATUS, "rb") as status:
            match = POINTERS.search(status.read())
    except OSError:
        return (-1, -1)
    return (int(match.group(1)), int(match.group(2))) if match else (-1, -1)


def open_stream(what="the open"):
    pcm = ctypes.c_void_p()
    code = alsa.snd_pcm_open(ctypes.byref(pcm), b"hw:0,0", 0, 0)
    if code < 0:
        raise Failure(f"{what} returned {describe(code)}")
    code = alsa.snd_pcm_set_params(
        pcm, FORMAT_S16_LE, ACCESS_RW_INTERLEAVED, CHANNELS, RATE, 0, LATENCY_US
    )
    if code < 0:
        alsa.snd_pcm_close(pcm)
        raise Failure(f"{what}: snd_pcm_set_params returned {describe(code)}")
    with open(HW_PARAMS) as parameters:
        sizes = dict(
            (key, int(value)) for key, value in PARAMETER.findall(parameters.read())
        )
    return pcm, sizes["period_size"], sizes["buffer_size"]


def set_thresholds(pcm, start, stop):
    """Start threshold `start` frames; `stop` frames, or None for the
    boundary, which never ends a stream by underrun."""
    sw = ctypes.create_string_buffer(alsa.snd_pcm_sw_params_sizeof())
    alsa.snd_pcm_sw_params_current(pcm, sw)
    if stop is None:
        boundary = ctypes.c_ulong()
        alsa.snd_pcm_sw_params_get_boundary(sw, ctypes.byref(boundary))
        stop = boundary.value
    alsa.snd_pcm_sw_params_set_start_threshold(pcm, sw, start)
    alsa.snd_pcm_sw_params_set_stop_threshold(pcm, sw, stop)
    code = alsa.snd_pcm_sw_params(pcm, sw)
    if code < 0:
        raise Failure(f"snd_pcm_sw_params returned {describe(code)}")


def suspends():
    try:
        with open(SUSPEND_STATS) as stats:
            return int(stats.read())
    except (OSError, ValueError):
        return None


def suspend():
    """Suspend the guest until its RTC alarm; fail unless it did suspend."""
    with open(WAKE_ALARM, "w") as alarm:
        alarm.write("0")
    with open(WAKE_ALARM, "w") as alarm:
        alarm.write(f"+{ALARM_SECONDS}")
    before = suspends()
    try:
        with open("/sys/power/state", "w") as power:
            power.write("mem")
    except OSError as error:
        raise Failure(f"the guest's suspend failed: {error}") from error
    after = suspends()
    if before is not None and after != before + 1:
        raise Failure(f"the guest did not suspend (suspend_stats {before} -> {after})")


def silence(frames):
    return ctypes.create_string_buffer(frames * FRAME)


def write(pcm, data, frames):
    """One writei; on an error, recover once. Returns (frames written or the
    error, None or the recovery's result)."""
    written = alsa.snd_pcm_writei(pcm, data, frames)
    if written >= 0:
        return written, None
    return written, alsa.snd_pcm_recover(pcm, int(written), 1)


def play(pcm, label, seconds=PLAY_SECONDS):
    """Write half periods for `seconds`; every write must succeed."""
    data = silence(CHUNK)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        written, recovered = write(pcm, data, CHUNK)
        if recovered is not None:
            raise Failure(
                f"{label}: a write returned {describe(written)} and the "
                f"recovery returned {describe(recovered)}"
            )


def recover_from_suspend(pcm, frames):
    """The first write after the suspend must find the stream suspended, and
    snd_pcm_recover must prepare it again."""
    written, recovered = write(pcm, silence(frames), frames)
    if written != -ESTRPIPE:
        raise Failure(
            "the first write after the suspend returned "
            f"{describe(written)}, expected -ESTRPIPE: the stream was not suspended"
        )
    if recovered != 0:
        raise Failure(
            f"snd_pcm_recover returned {describe(recovered)}, expected 0: "
            "the driver refused to prepare the suspended stream"
        )


def wait_for_position(minimum):
    end = time.monotonic() + START_SECONDS
    while pointers()[0] < minimum and time.monotonic() < end:
        time.sleep(POLL_SECONDS)


def case_position():
    pcm, period, buffer = open_stream()
    try:
        say(f"VSND position period={period} buffer={buffer}")
        set_thresholds(pcm, period, None)
        written, recovered = write(pcm, silence(period), period)
        if written != period or recovered is not None:
            raise Failure(f"the first period was not written: {describe(written)}")
        wait_for_position(period)
        time.sleep(SETTLE_SECONDS)
        before = pointers()
        if before[0] != period:
            raise Failure(f"the first period never played (hw_ptr {before[0]})")
        suspend()
        recover_from_suspend(pcm, period)
        written, recovered = write(pcm, silence(period), period)
        if written != period or recovered is not None:
            raise Failure(
                f"the period after the recovery was not written: {describe(written)}"
            )
        # The first completion moves the position; one more is never queued.
        wait_for_position(1)
        time.sleep(SETTLE_SECONDS)
        position = pointers()[0]
        say(f"VSND position hw_ptr {before[0]} before the suspend, {position} "
            f"after one period played since the recovery")
        if position != period:
            raise Failure(
                f"hw_ptr is {position} after one period, expected {period}: "
                "the position the stream had when it was suspended was counted "
                "as played"
            )
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


def case_fill():
    pcm, period, buffer = open_stream()
    try:
        say(f"VSND fill period={period} buffer={buffer}")
        # Started from the first period, so the drop stops a running stream
        # and releases its queued period.
        set_thresholds(pcm, period, None)
        half = period + period // 2
        written, recovered = write(pcm, silence(half), half)
        if written != half or recovered is not None:
            raise Failure(f"the first writes failed: {describe(written)}")
        alsa.snd_pcm_drop(pcm)
        code = alsa.snd_pcm_prepare(pcm)
        if code != 0:
            raise Failure(f"prepare after the drop returned {describe(code)}")
        data = silence(period)
        for index in range(16):
            written = alsa.snd_pcm_writei(pcm, data, period)
            if written < 0:
                raise Failure(
                    f"whole period {index} after the prepare returned "
                    f"{describe(written)}: the half period the drop left kept "
                    "its length"
                )
        code = alsa.snd_pcm_drain(pcm)
        if code != 0:
            raise Failure(f"drain returned {describe(code)}")
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


def case_prepare2():
    pcm, period, buffer = open_stream()
    try:
        say(f"VSND prepare2 period={period} buffer={buffer}")
        # The default start threshold is the buffer, so nothing starts, but
        # the whole period of the 1.5 reaches the device.
        frames = period + period // 2
        written, recovered = write(pcm, silence(frames), frames)
        if written != frames or recovered is not None:
            raise Failure(f"the first writes failed: {describe(written)}")
        codes = [alsa.snd_pcm_prepare(pcm) for _ in range(3)]
        if codes != [0, 0, 0]:
            raise Failure(
                "three prepares after a period queued before any start "
                f"returned {[describe(code) for code in codes]}: the device "
                "kept the period"
            )
        play(pcm, "after the prepares")
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


def case_fillprep():
    pcm, period, buffer = open_stream()
    try:
        say(f"VSND fillprep period={period} buffer={buffer}")
        frames = period + period // 2
        written, recovered = write(pcm, silence(frames), frames)
        if written != frames or recovered is not None:
            raise Failure(f"the first writes failed: {describe(written)}")
        alsa.snd_pcm_drop(pcm)
        code = alsa.snd_pcm_prepare(pcm)
        if code != 0:
            raise Failure(
                f"prepare after a drop before any start returned {describe(code)}: "
                "the device kept the period"
            )
        play(pcm, "after the prepare")
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


def drain_once(name, frames, start):
    """Write `frames`, drain, and say what became of it; None when it is all
    as it should be."""
    pcm, period, buffer = open_stream(f"{name}: the open")
    try:
        set_thresholds(pcm, start, buffer)
        written, recovered = write(pcm, silence(frames), frames)
        if written != frames or recovered is not None:
            return f"{name}: the write returned {describe(written)}"
        began = time.monotonic()
        code = alsa.snd_pcm_drain(pcm)
        took = time.monotonic() - began
        after = state(pcm)
        say(f"VSND drains {name}: wrote {frames} (start {start}), drain returned "
            f"{code} in {took:.2f} s, state {after}")
        if code != 0:
            return f"{name}: drain returned {describe(code)} after {took:.2f} s"
        if name != "drainwhole" and took >= DRAIN_SECONDS:
            return f"{name}: drain took {took:.2f} s, not under {DRAIN_SECONDS} s"
        if after != "SETUP":
            return f"{name}: the substream ended in {after}, expected SETUP"
        return None
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


def case_drains():
    problems = []
    for name, frames, start in (
        ("drainshort", 1024, 1024),
        ("drainpartial", 3072, 2048),
        ("drainwhole", 8192, 2048),
        ("drainprepared", 3072, 8192),
    ):
        problem = drain_once(name, frames, start)
        if problem is not None:
            problems.append(problem)
    if problems:
        raise Failure("; ".join(problems))


def case_pauseddrained():
    pcm, period, buffer = open_stream()
    problems = []
    try:
        say(f"VSND pauseddrained period={period} buffer={buffer}")
        set_thresholds(pcm, period, None)
        written, recovered = write(pcm, silence(period), period)
        if written != period or recovered is not None:
            raise Failure(f"the period was not written: {describe(written)}")
        wait_for_position(period)
        time.sleep(SETTLE_SECONDS)
        if pointers()[0] != period:
            raise Failure(f"the period never played (hw_ptr {pointers()[0]})")
        code = alsa.snd_pcm_pause(pcm, 1)
        if code != 0:
            raise Failure(f"pause returned {describe(code)}")
        suspend()
        codes = [alsa.snd_pcm_prepare(pcm) for _ in range(2)]
        if codes != [0, 0]:
            problems.append(
                f"two prepares after the suspend returned "
                f"{[describe(code) for code in codes]}: the device kept its "
                "stopped stream"
            )
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)
    try:
        pcm, period, buffer = open_stream("the open after the suspend")
    except Failure as failure:
        problems.append(str(failure))
    else:
        try:
            play(pcm, "after the reopen")
        except Failure as failure:
            problems.append(str(failure))
        finally:
            alsa.snd_pcm_drop(pcm)
            alsa.snd_pcm_close(pcm)
    if problems:
        raise Failure("; ".join(problems))


def case_stall(boundary):
    """The host stops the guest for 300 ms once this says it is streaming."""
    leg = "boundary" if boundary else "default"
    pcm, period, buffer = open_stream()
    try:
        say(f"VSND stall{leg} period={period} buffer={buffer}")
        if boundary:
            set_thresholds(pcm, buffer, None)
        data = silence(CHUNK)
        started = begin = last = time.monotonic()
        announced = False
        go = hw_go = advance = recovered_at = None
        errors = []
        window = []
        while True:
            now = time.monotonic()
            if go is None:
                if now - started > TIMEOUT_SECONDS:
                    raise Failure("the host never stopped the guest")
            else:
                since = now - go
                if boundary:
                    if advance is None and since >= STALL_WINDOW_SECONDS:
                        advance = pointers()[0] - hw_go
                    if since >= STALL_WINDOW_SECONDS + STALL_SETTLE_SECONDS:
                        break
                else:
                    if recovered_at is not None:
                        if not window and now - recovered_at >= 0.5:
                            window.append((now, pointers()[0]))
                        elif window and now - window[0][0] >= STALL_WINDOW_SECONDS:
                            window.append((now, pointers()[0]))
                            break
                    if since > STALL_GIVE_UP_SECONDS:
                        break
            written = alsa.snd_pcm_writei(pcm, data, CHUNK)
            now = time.monotonic()
            if go is None and announced and now - last > STALL_DETECT_SECONDS:
                go = now
                hw_go = pointers()[0]
                say(f"VSND stall{leg} the guest was stopped for {now - last:.2f} s")
            if written < 0:
                if go is None:
                    # An overrun of the warm-up, before the stop: start over.
                    alsa.snd_pcm_recover(pcm, int(written), 1)
                    begin = last = time.monotonic()
                    announced = False
                    continue
                errors.append((round(now - go, 2), int(written)))
                say(f"VSND stall{leg} write error {describe(written)} "
                    f"{now - go:.2f} s after the stop")
                if alsa.snd_pcm_recover(pcm, int(written), 1) < 0:
                    time.sleep(0.2)
                else:
                    recovered_at = time.monotonic()
                last = time.monotonic()
                continue
            last = now
            if not announced and now - begin >= STREAM_WARMUP_SECONDS:
                announced = True
                say(f"VSND stall{leg} streaming")
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)
    if boundary:
        late = [code for when, code in errors if when > STALL_SETTLE_SECONDS]
        say(f"VSND stall{leg} the position advanced {advance} frames in "
            f"{STALL_WINDOW_SECONDS} s after the stop; write errors {errors}")
        if advance is None or advance < STALL_ADVANCE * RATE:
            raise Failure(
                f"the position advanced {advance} frames in "
                f"{STALL_WINDOW_SECONDS} s after the stall, under "
                f"{STALL_ADVANCE * RATE:.0f}: the stream stopped for good "
                f"(write errors {errors})"
            )
        if late:
            raise Failure(
                f"{len(late)} writes failed later than {STALL_SETTLE_SECONDS} s "
                f"after the stall ({describe(late[0])}): errors {errors}"
            )
        return
    underruns = [when for when, code in errors if code == -EPIPE]
    stray = [(when, code) for when, code in errors
             if code != -EPIPE and (when > STALL_SETTLE_SECONDS or code != -EIO)]
    say(f"VSND stall{leg} write errors {errors}, rate window {window}")
    if len(underruns) != 1:
        raise Failure(
            f"{len(underruns)} underruns after the stall, expected exactly one "
            f"(write errors {errors})"
        )
    if stray:
        raise Failure(f"unexpected write errors after the stall: {stray}")
    if len(window) != 2:
        raise Failure(f"the stream never settled after its underrun (errors {errors})")
    frames = window[1][1] - window[0][1]
    seconds = window[1][0] - window[0][0]
    rate = frames / (seconds * RATE)
    say(f"VSND stall{leg} {frames} frames in {seconds:.2f} s after the underrun: "
        f"{rate:.2f}x")
    if not 0.8 <= rate <= 1.25:
        raise Failure(f"the stream played at {rate:.2f}x after its underrun, expected 1x")


def case_paused():
    pcm, period, buffer = open_stream()
    try:
        say(f"VSND paused period={period} buffer={buffer}")
        play(pcm, "before the pause")
        code = alsa.snd_pcm_pause(pcm, 1)
        if code != 0:
            raise Failure(f"pause returned {describe(code)}")
        suspend()
        recover_from_suspend(pcm, CHUNK)
        play(pcm, "after the recovery")
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


def case_pausedreopen():
    pcm, period, buffer = open_stream("the first open")
    say(f"VSND pausedreopen period={period} buffer={buffer}")
    try:
        play(pcm, "before the pause")
        code = alsa.snd_pcm_pause(pcm, 1)
        if code != 0:
            raise Failure(f"pause returned {describe(code)}")
        suspend()
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)
    pcm, period, buffer = open_stream("the open after the suspend")
    try:
        play(pcm, "after the reopen")
    finally:
        alsa.snd_pcm_drop(pcm)
        alsa.snd_pcm_close(pcm)


CASES = {
    "position": case_position,
    "fill": case_fill,
    "prepare2": case_prepare2,
    "fillprep": case_fillprep,
    "drains": case_drains,
    "paused": case_paused,
    "pauseddrained": case_pauseddrained,
    "pausedreopen": case_pausedreopen,
    "stallboundary": lambda: case_stall(True),
    "stalldefault": lambda: case_stall(False),
}
# The stall cases wait for the host to stop the guest, so they are named.
DEFAULT_CASES = [name for name in CASES if not name.startswith("stall")]


def main():
    names = sys.argv[1:] or DEFAULT_CASES
    unknown = [name for name in names if name not in CASES]
    if unknown:
        say(f"VSND unknown case {unknown[0]}; the cases are {' '.join(CASES)}")
        return 2
    failed = 0
    for name in names:
        try:
            CASES[name]()
        except Failure as failure:
            failed += 1
            say(f"VSND {name} FAIL: {failure}")
        except Exception as error:  # noqa: BLE001 - a case reports and goes on
            failed += 1
            say(f"VSND {name} FAIL: {error!r}")
        else:
            say(f"VSND {name} ok")
    say(f"VSND finished, {failed} failed")
    return failed


if __name__ == "__main__":
    sys.exit(main())
