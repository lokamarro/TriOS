#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synchronize the private Virtio-GPU memory-report header into a kernel tree.

Every run, with or without --verify, also checks that no feature bit, event bit,
command or response code of the header is one the tree's own virtio_gpu.h
already uses, so a kernel base that grows a colliding code is caught here
instead of on the wire.
"""
__import__("sys").dont_write_bytecode = True

import argparse
import re
from pathlib import Path

HEADER = "virtio_gpu_uml.h"
DESTINATION = "drivers/gpu/drm/virtio/virtgpu_uml.h"
UPSTREAM = "include/uapi/linux/virtio_gpu.h"

DEFINE = re.compile(r"^\s*#\s*define\s+(VIRTIO_GPU_\w+)\s+(.+?)\s*$")
ENUMERATOR = re.compile(r"^\s*(VIRTIO_GPU_\w+)\s*(?:=\s*(.+?))?\s*,?\s*$")
SAFE = re.compile(r"^[0-9a-fA-FxX<>()|+\-uUlL ]+$")

# The wire codes the header owns, by the class they must not share with the tree.
CLASSES = (
    ("feature bit", re.compile(r"^VIRTIO_GPU_F_")),
    ("event bit", re.compile(r"^VIRTIO_GPU_EVENT_")),
    ("control code", re.compile(r"^VIRTIO_GPU_(CMD|RESP)_")),
)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def numbers(text):
    """Return {name: value} for the integer macros and enumerators of text."""
    values, previous = {}, None

    def evaluate(expression):
        expression = re.sub(r"(?<=[0-9a-fA-F])[uUlL]+\b", "", expression.strip())
        if not SAFE.match(expression):
            return None
        try:
            return int(eval(expression, {"__builtins__": {}}, {}))  # noqa: S307 (digits and operators only)
        except (SyntaxError, ValueError, TypeError):
            return None

    for line in strip_comments(text).splitlines():
        define = DEFINE.match(line)
        if define:
            value = evaluate(define.group(2))
            if value is not None:
                values[define.group(1)] = value
            continue
        enumerator = ENUMERATOR.match(line)
        if not enumerator:
            continue
        name, expression = enumerator.groups()
        if expression is not None:
            value = evaluate(expression)
        else:
            value = None if previous is None else previous + 1
        if value is not None:
            values[name] = value
        previous = value
    return values


def collisions(ours, theirs):
    problems = []
    for label, pattern in CLASSES:
        for name, value in ours.items():
            if not pattern.match(name):
                continue
            for other, other_value in theirs.items():
                if other != name and pattern.match(other) and other_value == value:
                    problems.append(f"{name} ({label} {value:#x}) is also {other} in {UPSTREAM}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-source", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).with_name(HEADER)
    destination = args.kernel_source / DESTINATION
    upstream = args.kernel_source / UPSTREAM
    if not (args.kernel_source / "Makefile").is_file() or not destination.parent.is_dir():
        parser.error("--kernel-source must name a prepared Linux source tree")
    if args.verify:
        if not destination.is_file() or source.read_bytes() != destination.read_bytes():
            parser.error("Virtio-GPU memory-report header copy is stale")
    else:
        destination.write_bytes(source.read_bytes())
    if not upstream.is_file():
        parser.error(f"{upstream} is missing")
    ours = numbers(source.read_text())
    if not any(name.startswith("VIRTIO_GPU_F_UML_") for name in ours):
        parser.error(f"{HEADER} defines no feature bit")
    problems = collisions(ours, numbers(upstream.read_text()))
    if problems:
        parser.error("Virtio-GPU memory-report codes collide with the kernel:\n  " + "\n  ".join(problems))


if __name__ == "__main__":
    main()
