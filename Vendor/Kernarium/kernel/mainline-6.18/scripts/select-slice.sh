#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Shared selector for the normal arm64 kernel series and the 4 KiB x86_64
# userspace compatibility overlay. This file is sourced by producer scripts.

uml_kernel_select_slice()
{
	uml_select_line_root=$1
	uml_select_requested=$2
	uml_select_common_metadata="$uml_select_line_root/patches/metadata.env"
	[ -r "$uml_select_common_metadata" ] || {
		printf '%s\n' "missing common kernel metadata: $uml_select_common_metadata" >&2
		return 1
	}

	# shellcheck disable=SC1090
	. "$uml_select_common_metadata"
	[ "${UML_KERNEL_SERIES_SCHEMA:-}" = 4 ] || {
		printf '%s\n' "unsupported common kernel series schema" >&2
		return 1
	}
	UML_KERNEL_COMMON_PATCH_BRANCH=${UML_KERNEL_SERIES##*:}
	UML_KERNEL_COMMON_METADATA_PATH=$uml_select_common_metadata

	case "$uml_select_requested" in
	arm64)
		UML_KERNEL_SLICE=arm64
		UML_KERNEL_METADATA_PATH=$uml_select_common_metadata
		UML_KERNEL_PARENT_BRANCH=''
		UML_KERNEL_SLICE_CONFIG_FRAGMENT=''
		;;
	x86_64)
		uml_select_slice_root="$uml_select_line_root/slices/x86_64"
		UML_KERNEL_METADATA_PATH="$uml_select_slice_root/patches/metadata.env"
		UML_KERNEL_SLICE_CONFIG_FRAGMENT="$uml_select_slice_root/config/userspace.fragment"
		[ -r "$UML_KERNEL_METADATA_PATH" ] || {
			printf '%s\n' "missing x86_64 slice metadata: $UML_KERNEL_METADATA_PATH" >&2
			return 1
		}
		# shellcheck disable=SC1090
		. "$UML_KERNEL_METADATA_PATH"
		[ "${UML_KERNEL_SERIES_SCHEMA:-}" = 4 ] || {
			printf '%s\n' "unsupported x86_64 kernel series schema" >&2
			return 1
		}
		[ "${UML_KERNEL_SLICE:-}" = x86_64 ] || {
			printf '%s\n' "x86_64 metadata has the wrong slice identity" >&2
			return 1
		}
		# The overlay starts where the arm64 series end, on their upstream
		# base. Its metadata names no upstream identity (kernel_series.py
		# rejects one that does), so the common values sourced above stay in
		# effect. Its own final tree covers every series below it, so a stale
		# overlay fails verification.
		UML_KERNEL_PARENT_BRANCH=$UML_KERNEL_COMMON_PATCH_BRANCH
		;;
	*)
		printf '%s\n' "kernel slice must be arm64 or x86_64" >&2
		return 1
		;;
	esac
	UML_KERNEL_PATCH_BRANCH=${UML_KERNEL_SERIES##*:}
}
