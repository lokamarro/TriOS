#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-console-write-memory: console writes with allocations traced.

Usage: console_write_memory_probe.py RECORDS WINDOW_BYTES DELAY_SECONDS

Traces slab and non-sleeping page allocations with their stacks while it
writes RECORDS numbered 64-byte lines to /dev/hvc0, in raw mode, and then to
/dev/ttyS1, with the default output processing that turns each newline into
a carriage return and a newline.

The host drops what the Virtio console sends while its reader is behind, so
the probe never has more than WINDOW_BYTES of hvc0 output outstanding: after
each window it reads one byte from /dev/hvc0, which the host sends once it
has read that much, and the last window is acknowledged too. Nothing about
how fast the host reads changes what arrives.

The host holds ttyS1's channel unread until the probe says the line held its
writer back: the probe writes to ttyS1 without waiting until a write fails
with EAGAIN, which is the line saying its output ring and the host's pipe
have no room, prints CONSOLE_LINE_HELD, and then waits in a blocking write
for the host to take the rest. An allocation is a console write's when its
stack holds the Virtio console's put_chars() or the UML line's line_write().

DELAY_SECONDS is slept before ttyS1 is opened. It stands for a host so
loaded that this part of the probe starts late.

Prints one line per result:
  CONSOLE_LINE_START
  CONSOLE_LINE_HELD bytes=BYTES   (the line refused a write after BYTES)
  CONSOLE_WRITES hvc=BYTES line=BYTES held=BYTES line_seconds=S
  CONSOLE_ALLOCATIONS events=N hvc=N line=N lost=N
  CONSOLE_STACK <frames>     (for at most three console write allocations)
"""

import os
import re
import subprocess
import sys
import termios
import time
import tty

TRACING = "/sys/kernel/tracing"
RECORD = 64
# __GFP_DIRECT_RECLAIM: a page allocation that may not sleep lacks it, as
# every allocation a console write makes with its lock held must.
DIRECT_RECLAIM = 0x400
HVC_FRAME = re.compile(r"^put_chars\b")
LINE_FRAME = re.compile(r"^line_write\b")
EVENT = re.compile(r"^\s*\S+-\d+\s+\[\d+\]\s+\S+\s+[\d.]+:\s+(\w+):")


def record(index: int) -> bytes:
    body = f"R{index:06d}:" + "abcdefghijklmnopqrstuvwxyz0123456789" * 2
    return (body[:RECORD - 1] + "\n").encode()


def write(path: str, text: str) -> None:
    with open(path, "w") as handle:
        handle.write(text)


def start_tracing() -> None:
    subprocess.run(
        f"mountpoint -q {TRACING} || mount -t tracefs nodev {TRACING}",
        shell=True,
        check=True,
    )
    write(f"{TRACING}/tracing_on", "0")
    write(f"{TRACING}/trace", "")
    write(f"{TRACING}/buffer_size_kb", "8192")
    for event in ("kmalloc", "kmem_cache_alloc"):
        write(f"{TRACING}/events/kmem/{event}/enable", "1")
    page_alloc = f"{TRACING}/events/kmem/mm_page_alloc"
    write(f"{page_alloc}/filter", f"!(gfp_flags & {DIRECT_RECLAIM:#x})")
    write(f"{page_alloc}/enable", "1")
    write(f"{TRACING}/options/stacktrace", "1")
    write(f"{TRACING}/tracing_on", "1")


def lost_events() -> int:
    lost = 0
    for cpu in os.listdir(f"{TRACING}/per_cpu"):
        with open(f"{TRACING}/per_cpu/{cpu}/stats") as stats:
            for line in stats:
                name, _, value = line.partition(":")
                if name.strip() in ("overrun", "dropped events"):
                    lost += int(value.split()[0])
    return lost


def allocations() -> tuple[list[list[str]], int]:
    write(f"{TRACING}/tracing_on", "0")
    lost = lost_events()
    events: list[list[str]] = []
    with open(f"{TRACING}/trace", errors="replace") as trace:
        for line in trace:
            if EVENT.match(line):
                events.append([])
            elif events and "=>" in line:
                events[-1].append(line.split("=>", 1)[1].strip())
    for event in ("kmalloc", "kmem_cache_alloc", "mm_page_alloc"):
        write(f"{TRACING}/events/kmem/{event}/enable", "0")
    write(f"{TRACING}/options/stacktrace", "0")
    return events, lost


def write_hvc(data: bytes, window: int) -> int:
    """Write to the Virtio console a window at a time.

    Each window is followed by one byte read from the console, the host's
    word that it has read everything written so far, so output the host was
    too slow to take is never sent. The console is raw: no output
    processing, and no echo of the acknowledgements."""
    descriptor = os.open("/dev/hvc0", os.O_RDWR | os.O_NOCTTY)
    tty.setraw(descriptor)
    written = 0
    view = memoryview(data)
    while written < len(data):
        end = min(written + window, len(data))
        while written < end:
            count = os.write(descriptor, view[written:min(written + 4096, end)])
            if count <= 0:
                break
            written += count
        if not os.read(descriptor, 1):
            break
    os.close(descriptor)
    return written


def write_line(device: str, data: bytes) -> tuple[int, int, float]:
    """Write to the UML line without waiting until it refuses a write.

    The refusal, EAGAIN, is the line saying it has no room: the host's pipe
    is full and so is the line's ring. The probe says so, and the host,
    which held the channel unread until then, takes it from there. The rest
    is written with the descriptor blocking, so the writer waits for the
    host as a program with a slow reader does. Returns the bytes written,
    the bytes written when the line first refused (-1 if it never did) and
    the seconds from the refusal to the end."""
    descriptor = os.open(device, os.O_WRONLY | os.O_NOCTTY | os.O_NONBLOCK)
    written = 0
    held = -1
    held_at = 0.0
    view = memoryview(data)
    while written < len(data):
        try:
            count = os.write(descriptor, view[written:written + 4096])
        except BlockingIOError:
            if held < 0:
                held = written
                held_at = time.monotonic()
                print(f"CONSOLE_LINE_HELD bytes={held}", flush=True)
                os.set_blocking(descriptor, True)
            continue
        if count <= 0:
            break
        written += count
    termios.tcdrain(descriptor)
    elapsed = time.monotonic() - held_at if held >= 0 else 0.0
    os.close(descriptor)
    return written, held, elapsed


def main() -> int:
    records = int(sys.argv[1])
    window = int(sys.argv[2])
    delay = float(sys.argv[3])
    data = b"".join(record(index) for index in range(records))
    start_tracing()
    hvc = write_hvc(data, window)
    print("CONSOLE_LINE_START", flush=True)
    time.sleep(delay)
    line, held, line_seconds = write_line("/dev/ttyS1", data)
    events, lost = allocations()
    hvc_stacks = [s for s in events if any(HVC_FRAME.match(f) for f in s)]
    line_stacks = [s for s in events if any(LINE_FRAME.match(f) for f in s)]
    print(
        f"CONSOLE_WRITES hvc={hvc} line={line} held={held} "
        f"line_seconds={line_seconds:.2f}"
    )
    print(
        f"CONSOLE_ALLOCATIONS events={len(events)} hvc={len(hvc_stacks)} "
        f"line={len(line_stacks)} lost={lost}"
    )
    for stack in (hvc_stacks + line_stacks)[:3]:
        print("CONSOLE_STACK " + " ".join(stack[:12]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
