#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Refuse a build for a Mac (Designed for iPad) whose products are in a temporary directory.

An Apple silicon Mac runs Andarium or Kernarium from an install of the build's
products directory, and Gatekeeper (syspolicyd) refuses an app installed from a
system temporary directory: /tmp, /var/tmp and the per-user $TMPDIR (all below
/private) get "Denying target with bad app wrapper", whatever the app's
signature, the group of its files or the framework variant it embeds. The test
run then ends "Test crashed with signal kill before establishing connection"
(Andarium) or "The test runner hung before establishing connection" (Kernarium)
after a Gatekeeper dialog, so the build fails first and says why. A build for a
device or the Simulator may use such a directory, so only a Mac destination
(Xcode's TARGET_DEVICE_PLATFORM_NAME is macosx) is checked.

Each application's Xcode project runs it as the first build phase of its
target, in the environment of a run-script phase (TARGET_DEVICE_PLATFORM_NAME,
TARGET_BUILD_DIR).
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
import sys

# The system's temporary directories as a path names them, with and without
# the /private prefix of the Data volume.
TEMPORARY = ("/tmp", "/var/tmp", "/var/folders", "/private/tmp", "/private/var/tmp", "/private/var/folders")


def temporary_directory(path: str) -> str | None:
    """The temporary directory `path` is in, or None."""
    for candidate in (os.path.abspath(path), os.path.realpath(path)):
        for root in TEMPORARY:
            if candidate == root or candidate.startswith(root + "/"):
                return root
    return None


def main(environment: dict[str, str]) -> int:
    if environment.get("TARGET_DEVICE_PLATFORM_NAME") != "macosx":
        return 0
    products = environment.get("TARGET_BUILD_DIR", "")
    if not products:
        print("error: check-run-location.py: TARGET_BUILD_DIR is not set; run it from an Xcode build phase", file=sys.stderr)
        return 1
    root = temporary_directory(products)
    if root is None:
        return 0
    print(
        f"error: the products directory {products} is below {root}, a system temporary directory. "
        "Gatekeeper rejects an iPad app installed from there on a Mac (syspolicyd: bad app wrapper), "
        "and the app never starts. Build with -derivedDataPath outside /tmp, /var/tmp "
        "and $TMPDIR.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(dict(os.environ)))
