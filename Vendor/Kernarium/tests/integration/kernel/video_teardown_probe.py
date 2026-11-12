#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-video-teardown: keep buffers in a Virtio video device.

Usage: video_teardown_probe.py decoder NODE PACKET FIFO
       video_teardown_probe.py encoder NODE FIFO
       video_teardown_probe.py --gone NODE

The decoder client opens NODE, sets an H.264 coded format, fills every coded
buffer with the access unit in PACKET, queues them all and starts the coded
queue. No capture format is ever chosen, so the device keeps the packets it
has read and the driver still owns their buffers.

The encoder client holds the two states an encoder keeps buffers in, on two
streams of NODE. On the first it starts the raw queue and queues pictures
while the coded queue is stopped, so the driver itself holds the pictures
back. On the second it starts both queues and queues coded buffers with no
picture, so the device keeps the empty coded buffers.

A client prints VIDEO_CLIENT_STREAMING and waits for a line on FIFO. It then
tries one dequeue on each queue it holds and closes the device, printing
VIDEO_CLIENT_CLOSED with the dequeue's errno (0 if one succeeded; the
encoder prints the second stream's as held_errno), so a close that waits for
a buffer nobody returns never prints it.

With --gone it succeeds only if opening NODE fails as it does for a removed
device: with ENOENT once the node has gone, or ENXIO or ENODEV.
"""

import array
import errno
import fcntl
import mmap
import os
import struct
import sys

# The arm64 Linux values of these requests and of the structure sizes and
# offsets below.
VIDIOC_S_FMT = 0xC0D05605
VIDIOC_REQBUFS = 0xC0145608
VIDIOC_QUERYBUF = 0xC0585609
VIDIOC_QBUF = 0xC058560F
VIDIOC_DQBUF = 0xC0585611
VIDIOC_STREAMON = 0x40045612
OUTPUT_MPLANE = 10
CAPTURE_MPLANE = 9
MEMORY_MMAP = 1
H264 = int.from_bytes(b"H264", "little")
NV12 = int.from_bytes(b"NV12", "little")
FORMAT_SIZE = 208
REQUEST_SIZE = 20
BUFFER_SIZE = 88
PLANE_SIZE = 64
MAX_PLANES = 8
COUNT = 4


def planes() -> array.array:
    return array.array("B", bytes(MAX_PLANES * PLANE_SIZE))


def buffer(index: int, plane_array: array.array, count: int,
           queue: int = OUTPUT_MPLANE) -> bytearray:
    """A struct v4l2_buffer for buffer ``index`` of a queue and its planes."""
    value = bytearray(BUFFER_SIZE)
    struct.pack_into("<II", value, 0, index, queue)
    struct.pack_into("<qq", value, 24, 7, index + 1)
    struct.pack_into("<I", value, 60, MEMORY_MMAP)
    struct.pack_into("<Q", value, 64, plane_array.buffer_info()[0])
    struct.pack_into("<I", value, 72, count)
    return value


def gone(node: str) -> int:
    try:
        os.close(os.open(node, os.O_RDWR))
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ENXIO, errno.ENODEV):
            print(f"VIDEO_CLIENT_GONE errno={error.errno}", flush=True)
            return 0
        raise
    raise RuntimeError(f"{node} still opens")


def map_queue(fd: int, queue: int, count: int, filler: bytes = b""):
    """Allocate ``count`` buffers of a queue and map their first plane.

    Returns the mappings and the length of each buffer's first plane."""
    request = bytearray(REQUEST_SIZE)
    struct.pack_into("<III", request, 0, count, queue, MEMORY_MMAP)
    fcntl.ioctl(fd, VIDIOC_REQBUFS, request)
    count = struct.unpack_from("<I", request, 0)[0]
    if count < 1:
        raise RuntimeError("REQBUFS gave no buffer")
    mappings = []
    lengths = []
    for index in range(count):
        plane_array = planes()
        query = buffer(index, plane_array, MAX_PLANES, queue)
        fcntl.ioctl(fd, VIDIOC_QUERYBUF, query)
        length, offset = struct.unpack_from("<II", plane_array, 4)
        mapping = mmap.mmap(
            fd, length, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
            offset=offset)
        if len(filler) > length:
            raise RuntimeError(f"buffer {index} holds {length} bytes")
        mapping[:len(filler)] = filler
        mappings.append(mapping)
        lengths.append(length)
    return mappings, lengths


def queue_buffers(fd: int, queue: int, lengths: list, used: int) -> None:
    for index, length in enumerate(lengths):
        plane_array = planes()
        struct.pack_into("<II", plane_array, 0, used, length)
        fcntl.ioctl(fd, VIDIOC_QBUF, buffer(index, plane_array, 1, queue))


def dequeue_errno(fd: int, queue: int) -> int:
    try:
        fcntl.ioctl(fd, VIDIOC_DQBUF, buffer(0, planes(), MAX_PLANES, queue))
    except OSError as error:
        return error.errno or errno.EIO
    return 0


def wait_for_trigger(fifo: str) -> None:
    with open(fifo) as trigger:
        trigger.readline()


def decoder(node: str, packet_path: str, fifo: str) -> int:
    packet = open(packet_path, "rb").read()
    fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)

    form = bytearray(FORMAT_SIZE)
    struct.pack_into("<I", form, 0, OUTPUT_MPLANE)
    struct.pack_into("<III", form, 8, 128, 96, H264)
    struct.pack_into("<I", form, 28, len(packet))
    form[188] = 1
    fcntl.ioctl(fd, VIDIOC_S_FMT, form)

    mappings, lengths = map_queue(fd, OUTPUT_MPLANE, COUNT, packet)
    for index, length in enumerate(lengths):
        if length < len(packet):
            raise RuntimeError(f"coded buffer {index} holds {length} bytes")
    fcntl.ioctl(fd, VIDIOC_STREAMON, struct.pack("<i", OUTPUT_MPLANE))
    queue_buffers(fd, OUTPUT_MPLANE, lengths, len(packet))
    print(f"VIDEO_CLIENT_STREAMING buffers={len(lengths)}", flush=True)

    wait_for_trigger(fifo)
    result = dequeue_errno(fd, OUTPUT_MPLANE)
    for mapping in mappings:
        mapping.close()
    os.close(fd)
    print(f"VIDEO_CLIENT_CLOSED dqbuf_errno={result}", flush=True)
    return 0


def encoder_stream(node: str):
    """An encoder stream with both formats set and both queues allocated."""
    fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)

    coded = bytearray(FORMAT_SIZE)
    struct.pack_into("<I", coded, 0, CAPTURE_MPLANE)
    struct.pack_into("<III", coded, 8, 0, 0, H264)
    struct.pack_into("<I", coded, 28, 1 << 16)
    coded[188] = 1
    fcntl.ioctl(fd, VIDIOC_S_FMT, coded)

    raw = bytearray(FORMAT_SIZE)
    struct.pack_into("<I", raw, 0, OUTPUT_MPLANE)
    struct.pack_into("<III", raw, 8, 320, 240, NV12)
    fcntl.ioctl(fd, VIDIOC_S_FMT, raw)
    if struct.unpack_from("<I", raw, 16)[0] != NV12 or raw[188] != 1:
        raise RuntimeError("the encoder does not take single-buffer NV12")
    picture = struct.unpack_from("<I", raw, 28)[0]

    coded_maps, coded_lengths = map_queue(fd, CAPTURE_MPLANE, 2)
    raw_maps, raw_lengths = map_queue(fd, OUTPUT_MPLANE, 2, bytes(picture))
    return fd, coded_maps + raw_maps, coded_lengths, raw_lengths, picture


def encoder(node: str, fifo: str) -> int:
    # The first stream starts only the raw queue, so its pictures wait in the
    # driver for the coded queue to start.
    held, held_maps, _, held_lengths, picture = encoder_stream(node)
    fcntl.ioctl(held, VIDIOC_STREAMON, struct.pack("<i", OUTPUT_MPLANE))
    queue_buffers(held, OUTPUT_MPLANE, held_lengths, picture)

    # The second starts both and queues coded buffers with nothing to encode.
    kept, kept_maps, kept_lengths, _, _ = encoder_stream(node)
    fcntl.ioctl(kept, VIDIOC_STREAMON, struct.pack("<i", OUTPUT_MPLANE))
    fcntl.ioctl(kept, VIDIOC_STREAMON, struct.pack("<i", CAPTURE_MPLANE))
    queue_buffers(kept, CAPTURE_MPLANE, kept_lengths, 0)
    print(f"VIDEO_CLIENT_STREAMING buffers={len(kept_lengths)}", flush=True)

    wait_for_trigger(fifo)
    kept_errno = dequeue_errno(kept, CAPTURE_MPLANE)
    held_errno = dequeue_errno(held, OUTPUT_MPLANE)
    for mapping in held_maps + kept_maps:
        mapping.close()
    os.close(held)
    os.close(kept)
    print(f"VIDEO_CLIENT_CLOSED dqbuf_errno={kept_errno} "
          f"held_errno={held_errno}", flush=True)
    return 0


def main() -> int:
    arguments = sys.argv[1:]
    if len(arguments) == 2 and arguments[0] == "--gone":
        return gone(arguments[1])
    if len(arguments) == 4 and arguments[0] == "decoder":
        return decoder(*arguments[1:])
    if len(arguments) == 3 and arguments[0] == "encoder":
        return encoder(*arguments[1:])
    raise SystemExit(__doc__)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - reported to the runner
        print(f"VIDEO_CLIENT_ERROR {type(error).__name__}: {error}",
              flush=True)
        raise SystemExit(1)
