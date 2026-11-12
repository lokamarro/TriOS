#!/bin/sh
set -eu
cd /home/mobile/Downloads/.linuxrunner/gui
# Generic GUI launcher reproducer: the initial process exits before its child
# opens a real X11 window. The display must live until that child exits.
(sleep 1; exec ./gui-test) &
echo "GROUP_HANDOFF: leader=$$ child=$!"
exit 0
