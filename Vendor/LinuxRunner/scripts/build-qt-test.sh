#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec scripts/build-ipa.sh --qt-test "$@"
