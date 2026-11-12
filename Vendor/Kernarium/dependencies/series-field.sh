# SPDX-License-Identifier: GPL-2.0-or-later
#
# The manifest's patch-series field, shared by prepare.sh and
# rebase-patches.sh. Sourcing this file only defines names.
#
# A field names one generated series, or ordered layers joined by "+": each
# layer applies to the tree the layers before it produce, and a field with
# more than one layer ends in the layer its dependency owns. A layer
# "rootfs-vulkan:PATH" names a series the paired Vulkan guest owns below
# tools/rootfs/vulkan; the namespace grants no general parent-directory
# access. Any other layer is relative to the dependency metadata directory.

SERIES_FIELD_ROOTFS_VULKAN=tools/rootfs/vulkan

# Succeed when dependency NAME may name the rootfs-vulkan series PATH.
series_field_rootfs_vulkan_allowed()
{
	case "$1:$2" in
	venus-protocol:patches/venus-protocol/series.tsv|\
	mesa:patches/mesa/series.tsv) return 0 ;;
	esac
	return 1
}

# Split FIELD into SERIES_FIELD_OWNED, its last layer, and
# SERIES_FIELD_LOWER, the layers below it joined by "+" (empty for one).
series_field_split()
{
	SERIES_FIELD_OWNED=${1##*+}
	SERIES_FIELD_LOWER=
	[ "$SERIES_FIELD_OWNED" = "$1" ] || SERIES_FIELD_LOWER=${1%+*}
}
