#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Repository tooling carries no Git identity of its own. Replays commit as
# whoever Git resolves for the caller, and a patch's only identity is its
# topic's author, which patch export checks. No first-party tool or test may
# name an identity.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)

TEST_NAME='git identity contract'
. "$(dirname -- "$0")/../support/expect.sh"

# Generated mail series carry the authentic author of each topic and are not
# tooling configuration.
status=0
named=$(git -C "$ROOT_DIR" grep -l -E 'user\.(name|email)' -- \
	android dependencies kernel scripts tests tools \
	':!*/patches/*' ':!*/series/*') || status=$?
case $status in
0) fail "these files name a Git identity: $named" ;;
1) ;;
*) fail "cannot search $ROOT_DIR" ;;
esac

printf '%s\n' 'Git identity contract passed'
