# SPDX-License-Identifier: GPL-2.0-or-later
"""FEX ASM test configuration records.

A record has four u64 and three u32 header fields, then offset/count pairs for
memory regions, registers, memory data and the environment, whose
NUL-terminated key/value pairs end the record.
"""

from __future__ import annotations

import struct

HEADER_SIZE = 76
ENVIRONMENT = 68
REDUCED_PRECISION = b"FEX_X87REDUCEDPRECISION"


def without_reduced_precision(config: bytes) -> tuple[bytes, list[bytes]]:
    """Return CONFIG without FEX's reduced-precision x87 request.

    That request makes FEX's own translator compute x87 in doubles; the
    gadget engine has only full 80-bit x87. FEX's x86-64 host runner executes
    every ASM test natively, where the request has no effect, so the expected
    registers are already full-precision hardware results. Also returns the
    values the removed request carried; CONFIG comes back unchanged with no
    values when it makes no request. Raises ValueError for a malformed record.
    """
    if len(config) < HEADER_SIZE:
        raise ValueError("FEX configuration header is truncated")
    offset, count = struct.unpack_from("<II", config, ENVIRONMENT)
    if not HEADER_SIZE <= offset <= len(config):
        raise ValueError("FEX configuration environment is out of range")
    entries: list[tuple[bytes, bytes]] = []
    cursor = offset
    for _ in range(count):
        key_end = config.find(b"\0", cursor)
        value_end = config.find(b"\0", key_end + 1) if key_end >= 0 else -1
        if value_end < 0:
            raise ValueError("FEX configuration environment is truncated")
        entries.append((config[cursor:key_end], config[key_end + 1 : value_end]))
        cursor = value_end + 1
    if cursor != len(config):
        raise ValueError("FEX configuration has bytes after its environment")
    kept = [entry for entry in entries if entry[0] != REDUCED_PRECISION]
    requested = [value for key, value in entries if key == REDUCED_PRECISION]
    if not requested:
        return config, []
    rebuilt = bytearray(config[:offset])
    struct.pack_into("<II", rebuilt, ENVIRONMENT, offset, len(kept))
    for key, value in kept:
        rebuilt += key + b"\0" + value + b"\0"
    return bytes(rebuilt), requested
