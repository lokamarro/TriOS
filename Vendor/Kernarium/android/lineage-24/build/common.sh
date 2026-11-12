# SPDX-License-Identifier: GPL-2.0-or-later
# Helpers sourced by the Android 17 producer scripts. They are POSIX sh,
# define functions only, and read their arguments rather than any variable,
# so they cannot collide with a caller's or a sourced environment's names.

# fail MESSAGE...: report MESSAGE under the running script's name and exit.
fail()
{
	printf '%s\n' "android/lineage-24/build/${0##*/}: $*" >&2
	exit 1
}

# absolute_path LABEL PATH: require an absolute, normalized local path.
absolute_path()
{
	case "$2" in
	/*) ;;
	*) fail "$1 must be an absolute local path" ;;
	esac
	case "$2" in
	/|*/../*|*/..|*/./*|*/.) fail "$1 must be normalized" ;;
	esac
}

# dirty_repo_projects WORKSPACE: print each Repo project with local changes.
dirty_repo_projects()
{
	(
		CDPATH= cd -- "$1"
		repo forall -c '
			if test -n "$(git status --porcelain)"; then
				printf "%s\n" "$REPO_PATH"
			fi
		'
	)
}
