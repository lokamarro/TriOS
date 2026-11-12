#!/bin/sh
# Linux side of Settings › Files › Shared iPhone Folders › Check Access, run as
# the default Linux user. Usage: share-check.sh MOUNTPOINT OUTPUT TOKEN
# Each step prints key=ok or key=failed <first error line>. The renamed file
# (LinuxRunner-check-TOKEN-renamed.txt) is left for the app to find on the iOS
# side, which then removes it.
d=$1 out=$2 token=${3:-$$}
err=/tmp/lr-share-err.$$
step() { name=$1; shift; if "$@" 2>"$err"; then echo "$name=ok"; else echo "$name=failed $(head -n 1 "$err")"; fi; }
{ [ -d "$d" ] && echo exists=1 || echo exists=0
 echo entries=$(ls -A "$d" 2>/dev/null | wc -l)
 echo list_error=$(ls -A "$d" 2>&1 >/dev/null | head -n 1)
 first=$(find "$d" -maxdepth 1 -type f ! -name '.*' 2>/dev/null | head -n 1)
 if [ -n "$first" ]; then step read_existing sh -c 'head -c 4096 "$1" > /dev/null' sh "$first"; else echo read_existing=no-files; fi
 a="$d/LinuxRunner-check-$token.txt" b="$d/LinuxRunner-check-$token-renamed.txt" c="$d/LinuxRunner-check-$token-dir"
 step create sh -c 'echo linuxrunner-check > "$1"' sh "$a"
 step modify sh -c 'echo modified >> "$1" && [ "$(sed -n 2p "$1")" = modified ]' sh "$a"
 step rename mv "$a" "$b"
 step mkdir mkdir "$c"
 step rmdir rmdir "$c"
 step delete_probe sh -c 'echo x > "$1.del" && rm "$1.del" && [ ! -e "$1.del" ]' sh "$b"
 echo kept="${b##*/}"
 echo user=$(id -un) uid=$(id -u)
} > "$out.tmp"; rm -f "$err"; mv "$out.tmp" "$out"
