#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise idle wakeups and concurrent wake coalescing across guest CPUs."""

__import__("sys").dont_write_bytecode = True

import os
import sys


workers = int(sys.argv[1])
rounds = 2000
for ring_count in (1, 4):
    pipes = [[os.pipe() for _ in range(workers)] for _ in range(ring_count)]
    children = []
    for ring in pipes:
        for cpu in range(workers):
            child = os.fork()
            if child == 0:
                try:
                    os.sched_setaffinity(0, {cpu})
                    for _ in range(rounds):
                        if os.read(ring[cpu][0], 1) != b"x":
                            os._exit(2)
                        if os.write(ring[(cpu + 1) % workers][1], b"x") != 1:
                            os._exit(3)
                    os._exit(0)
                except BaseException:
                    os._exit(4)
            children.append(child)
    for ring in pipes:
        os.write(ring[0][1], b"x")
    for child in children:
        _, status = os.waitpid(child, 0)
        if status:
            raise RuntimeError(f"wakeup worker failed: {status}")
    for ring in pipes:
        if os.read(ring[0][0], 1) != b"x":
            raise RuntimeError("final wakeup token was lost")
        for pair in ring:
            for descriptor in pair:
                os.close(descriptor)
    print(f"IPI_WAKEUP_OK {workers * rounds * ring_count}", flush=True)
