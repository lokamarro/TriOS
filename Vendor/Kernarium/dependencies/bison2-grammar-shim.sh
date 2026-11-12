#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Drive a Bison 2 generator from a Bison 3 grammar. Mesa's GLSL grammars use
# exactly two Bison 3 declarations, both of which have Bison 2 spellings with
# identical meaning. Apple's toolchain ships Bison 2, so the OpenGL provider
# translates the grammar instead of forking it. Generated output keeps the
# original grammar path so its #line directives stay meaningful.
set -eu

BISON=@BISON@
grammar=
output=
defines=
previous=
for argument in "$@"; do
	case $argument in
	*.y|*.yy) grammar=$argument ;;
	--defines=*) defines=${argument#--defines=} ;;
	esac
	case $previous in
	-o|--output) output=$argument ;;
	esac
	previous=$argument
done

[ -n "$grammar" ] || exec "$BISON" "$@"

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
translated="$work/$(basename -- "$grammar")"
sed -e 's/^%define api\.pure$/%pure-parser/' \
    -e 's/^%define parse\.error verbose$/%error-verbose/' \
    -- "$grammar" > "$translated"

for argument in "$@"; do
	if [ "$argument" = "$grammar" ]; then
		set -- "$@" "$translated"
	else
		set -- "$@" "$argument"
	fi
	shift
done
"$BISON" "$@"

for generated in "$output" "$defines"; do
	[ -n "$generated" ] && [ -f "$generated" ] || continue
	sed -e "s|$translated|$grammar|g" -- "$generated" > "$work/rewritten"
	cat -- "$work/rewritten" > "$generated"
done
