# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic EROFS images shared by the Android tooling tests.

An image carries the superblock mkfs.erofs writes for compressed EROFS on 4 KiB blocks
and, when asked, the metadata of a tree of empty regular files: enough for the
verifiers' superblock and directory reads, with no file data.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import struct


def synthetic_erofs_tree(files: tuple[str, ...]) -> bytes:
    """The metadata block of an EROFS image holding empty regular files at these paths.

    Every inode is a compact one and every directory keeps its entries behind its inode,
    as mkfs.erofs lays out a small tree. The inodes start at node id 0, the root.
    """
    children: list[dict[str, int]] = [{}]
    parents = [0]
    for path in files:
        node = 0
        for part in path.split("/"):
            if part not in children[node]:
                children.append({})
                parents.append(node)
                children[node][part] = len(children) - 1
            node = children[node][part]
    # Each directory lists "." and ".." and then its names in order, as the 12-byte
    # entries (node id, name offset, file type) followed by the names.
    listings = []
    for index, names in enumerate(children):
        listing = [(".", index, 2), ("..", parents[index], 2)]
        for name, child in sorted(names.items()):
            listing.append((name, child, 2 if children[child] else 1))
        listings.append(listing)
    sizes = [
        12 * len(listing) + sum(len(name.encode()) for name, _, _ in listing)
        for listing in listings
    ]
    # A directory takes its inode slot and the slots of its entries; a file one slot.
    slots = [1 + (size + 31) // 32 if children[index] else 1 for index, size in enumerate(sizes)]
    nids = [sum(slots[:index]) for index in range(len(slots))]
    block = bytearray(4096)
    for index, listing in enumerate(listings):
        inode = nids[index] * 32
        if not children[index]:
            struct.pack_into("<HHHHI", block, inode, 0, 0, 0o100644, 1, 0)
            continue
        struct.pack_into("<HHHHI", block, inode, 2 << 1, 0, 0o040755, 2, sizes[index])
        name_offset = 12 * len(listing)
        for position, (name, target, file_type) in enumerate(listing):
            struct.pack_into("<QHB", block, inode + 32 + 12 * position,
                             nids[target], name_offset, file_type)
            block[inode + 32 + name_offset : inode + 32 + name_offset + len(name)] = name.encode()
            name_offset += len(name.encode())
    return bytes(block)


def synthetic_erofs_image(*, algorithms: int = 1 << 3, features: int = 0x3,
                          block_bits: int = 12, sparse: bool = False,
                          files: tuple[str, ...] = ()) -> bytes:
    """The head of an EROFS image: the superblock at 1024, then the metadata of `files`.

    The defaults are what mkfs.erofs writes for zstd and 16 KiB clusters on 4 KiB
    blocks (ZERO_PADDING and BIG_PCLUSTER, the zstd bit of the algorithm mask). A
    sparse image carries the first block as a RAW chunk and a hole behind it.
    """
    block = bytearray(4096)
    block[1024 : 1024 + 4] = b"\xe2\xe1\xf5\xe0"
    block[1024 + 12] = block_bits
    struct.pack_into("<IH", block, 1024 + 80, features, algorithms)
    if files:
        struct.pack_into("<I", block, 1024 + 40, 1)
        return bytes(block) + synthetic_erofs_tree(files)
    if not sparse:
        return bytes(block)
    header = struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, 4096, 4, 2, 0)
    return (
        header
        + struct.pack("<HHII", 0xCAC1, 0, 1, 12 + 4096) + bytes(block)
        + struct.pack("<HHII", 0xCAC3, 0, 3, 12)
    )
