# SPDX-License-Identifier: GPL-2.0-or-later
#
# Kernel configuration composition, sourced by each line's build-kernel.sh.
#
# A product's configuration is an ordered list of fragments. The first is
# copied verbatim as the starting .config. Each later one is applied through
# the Linux tree's scripts/config, so it overrides whatever an earlier fragment
# set for the same symbol. The caller then runs `olddefconfig` and verifies the
# result against the same list.
#
# Both functions print their own diagnostics and return nonzero on failure.
# Callers test that status, which suspends errexit inside the function, so
# every step here checks its own result.

# uml_kernel_config_compose SOURCE_DIR CONFIG BASE [FRAGMENT...]
uml_kernel_config_compose()
{
	uml_config_source=$1
	uml_config_file=$2
	shift 2
	[ "$#" -gt 0 ] || {
		printf '%s\n' "no kernel config fragments to compose" >&2
		return 1
	}
	for uml_config_fragment do
		[ -f "$uml_config_fragment" ] || {
			printf '%s\n' "missing kernel config fragment: $uml_config_fragment" >&2
			return 1
		}
	done
	cp -- "$1" "$uml_config_file" || return 1
	shift
	for uml_config_fragment do
		uml_kernel_config_apply || return 1
	done
}

# Apply $uml_config_fragment to $uml_config_file. A line enables a symbol,
# sets a decimal or string value, or disables a symbol; nothing else is
# accepted. --keep-case keeps scripts/config from upper-casing symbols such as
# FONT_8x16.
uml_kernel_config_apply()
{
	while IFS= read -r uml_config_line; do
		uml_config_symbol=${uml_config_line%%=*}
		uml_config_symbol=${uml_config_symbol#CONFIG_}
		uml_config_value=${uml_config_line#*=}
		case "$uml_config_line" in
		CONFIG_*=y)
			set -- -e "$uml_config_symbol"
			;;
		CONFIG_*=\"*\")
			uml_config_value=${uml_config_value#\"}
			uml_config_value=${uml_config_value%\"}
			# scripts/config writes the value through sed, where these
			# characters are not literal.
			case "$uml_config_value" in
			*[\"\\\&]*) set -- ;;
			*) set -- --set-str "$uml_config_symbol" "$uml_config_value" ;;
			esac
			;;
		CONFIG_*=*)
			case "$uml_config_value" in
			''|*[!0-9]*) set -- ;;
			*) set -- --set-val "$uml_config_symbol" "$uml_config_value" ;;
			esac
			;;
		'# CONFIG_'*' is not set')
			uml_config_symbol=${uml_config_line#\# CONFIG_}
			set -- -d "${uml_config_symbol% is not set}"
			;;
		\#*|'')
			continue
			;;
		*)
			set --
			;;
		esac
		[ "$#" -gt 0 ] || {
			printf '%s\n' "unsupported kernel config fragment line in $uml_config_fragment: $uml_config_line" >&2
			return 1
		}
		"$uml_config_source/scripts/config" --file "$uml_config_file" \
			--keep-case "$@" || return 1
	done < "$uml_config_fragment"
}

# uml_kernel_config_verify CONFIG FRAGMENT...
#
# `olddefconfig` resolves a requested symbol against its dependencies in
# silence. A symbol whose menu is closed is simply dropped, and one that
# another symbol selects is force-enabled, so the composed fragments can
# disagree with every kernel they produce without anything saying so.
# CONFIG_NAMESPACES defaulting off under EXPERT cost USER_NS and TIME_NS that
# way, and the guest lost every namespace while the fragment still claimed two
# of them. Compare what was asked for against what the resolved configuration
# actually holds. The last fragment that names a symbol owns its value.
uml_kernel_config_verify()
{
	uml_config_file=$1
	shift
	uml_config_mismatches=$(awk -v config="$uml_config_file" '
	function record(symbol, wanted, got) {
		problems = problems sprintf("  %s: %s requested %s, resolved %s\n",
			symbol, owner[symbol], wanted, got)
		count++
	}
	BEGIN {
		while ((getline line < config) > 0) {
			if (match(line, /^CONFIG_[A-Za-z0-9_]+=/)) {
				split(line, parts, "=")
				built[parts[1]] = substr(line, index(line, "=") + 1)
			} else if (match(line,
				/^# CONFIG_[A-Za-z0-9_]+ is not set$/)) {
				split(line, parts, " ")
				built[parts[2]] = "n"
			}
		}
		close(config)
	}
	/^#/ && !/^# CONFIG_[A-Za-z0-9_]+ is not set$/ { next }
	/^$/ { next }
	{
		if (match($0, /^CONFIG_[A-Za-z0-9_]+=/)) {
			symbol = substr($0, 1, index($0, "=") - 1)
			requested[symbol] = substr($0, index($0, "=") + 1)
		} else if (match($0, /^# CONFIG_[A-Za-z0-9_]+ is not set$/)) {
			symbol = $2
			requested[symbol] = "n"
		} else {
			next
		}
		owner[symbol] = FILENAME
	}
	END {
		for (symbol in requested) {
			wanted = requested[symbol]
			# An absent symbol is one whose dependencies were never met.
			got = (symbol in built) ? built[symbol] : "n"
			if (got == wanted)
				continue
			# A .config records a disabled symbol as a comment or omits it,
			# so both spellings of "off" agree.
			if (wanted == "n" && !(symbol in built))
				continue
			record(symbol, wanted, (symbol in built) ? got : "absent")
		}
		if (count > 0)
			printf "%s", problems
	}
	' "$@") || return 1
	[ -z "$uml_config_mismatches" ] || {
		printf '%s\n' "the resolved kernel configuration differs from its fragments:" \
			"$uml_config_mismatches" >&2
		return 1
	}
}
