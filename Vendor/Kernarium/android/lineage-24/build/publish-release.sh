#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Publish a compact, verified Android 17 runtime release on the Linux builder.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ANDROID_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

exec python3 -B "$ANDROID_ROOT/tools/publish-release.py" "$@"
