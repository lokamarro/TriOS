# SPDX-License-Identifier: GPL-2.0-or-later
"""A reader for the access rules of a kernel binary SELinux policy (version 30 to 33).

A recovery ramdisk carries the one policy its domains run under (/sepolicy), and only that
file says which rules the recovery domain holds: the policy of the running system compiles the
recovery-only rules out. This reads the symbol tables the rules name types, classes and
permissions by, and the access vector table: the allow rules and the extended ioctl rules.
Attribute rules are already expanded to types in a kernel policy, and a rule on `self` is a
rule of a type on itself. Everything past the access vector table is left unread.
"""

from __future__ import annotations

import struct

MAGIC = 0xF97CFF8C
SUPPORTED_VERSIONS = range(30, 34)

AVTAB_ALLOWED = 0x0001
AVTAB_XPERMS_ALLOWED = 0x0100
XPERMS_IOCTL_FUNCTION = 0x01
XPERMS_IOCTL_DRIVER = 0x02

CEXPR_NAMES = 5


class PolicyError(ValueError):
    pass


class _Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def take(self, size: int) -> bytes:
        end = self.offset + size
        if size < 0 or end > len(self.data):
            raise PolicyError("the policy ends inside a record")
        chunk = self.data[self.offset:end]
        self.offset = end
        return chunk

    def words(self, count: int) -> tuple[int, ...]:
        return struct.unpack(f"<{count}I", self.take(4 * count))

    def word(self) -> int:
        return self.words(1)[0]

    def name(self, length: int) -> str:
        return self.take(length).decode("utf-8", "replace")

    def ebitmap(self) -> None:
        _, _, count = self.words(3)
        self.take(12 * count)  # (start bit, 64-bit map) per node

    def type_set(self) -> None:
        self.ebitmap()
        self.ebitmap()
        self.word()

    def level(self) -> None:
        self.word()
        self.ebitmap()

    def range(self) -> None:
        items = self.word()
        self.words(items)
        for _ in range(items):
            self.ebitmap()

    def constraints(self, count: int) -> None:
        for _ in range(count):
            _, expressions = self.words(2)
            for _ in range(expressions):
                kind, _, _ = self.words(3)
                if kind == CEXPR_NAMES:
                    self.ebitmap()
                    self.type_set()


class SelinuxPolicy:
    """The names and access rules of a binary policy."""

    def __init__(self, data: bytes) -> None:
        reader = _Reader(data)
        try:
            self._read(reader)
        except struct.error as error:
            raise PolicyError(f"the policy is truncated: {error}") from error

    def _read(self, reader: _Reader) -> None:
        magic, length = reader.words(2)
        if magic != MAGIC or reader.take(length) != b"SE Linux":
            raise PolicyError("not a kernel binary SELinux policy")
        self.version, _config, symbols, _contexts = reader.words(4)
        if self.version not in SUPPORTED_VERSIONS:
            raise PolicyError(f"policy version {self.version} is not one of {list(SUPPORTED_VERSIONS)}")
        if symbols != 8:
            raise PolicyError(f"policy has {symbols} symbol tables, not 8")
        reader.ebitmap()  # policy capabilities
        reader.ebitmap()  # permissive types
        commons: dict[str, dict[str, int]] = {}
        self.classes: dict[str, dict[str, int]] = {}
        self.class_values: dict[str, int] = {}
        self.types: dict[str, int] = {}
        for table in range(symbols):
            _, count = reader.words(2)
            for _ in range(count):
                if table == 0:  # commons: name length, value, permission count, permissions
                    length, _value, _, permissions = reader.words(4)
                    name = reader.name(length)
                    commons[name] = self._permissions(reader, permissions)
                elif table == 1:  # classes
                    length, common_length, value, _, permissions, constraints = reader.words(6)
                    name = reader.name(length)
                    common = reader.name(common_length) if common_length else None
                    own = self._permissions(reader, permissions)
                    reader.constraints(constraints)
                    reader.constraints(reader.word())  # validatetrans
                    reader.words(4)  # default user, role, range and type
                    self.classes[name] = {**(commons[common] if common else {}), **own}
                    self.class_values[name] = value
                elif table == 2:  # roles: name length, value, bounds
                    length = reader.words(3)[0]
                    reader.take(length)
                    reader.ebitmap()
                    reader.ebitmap()
                elif table == 3:  # types: name length, value, properties, bounds
                    length, value, _, _ = reader.words(4)
                    self.types[reader.name(length)] = value
                elif table == 4:  # users: name length, value, bounds
                    length = reader.words(3)[0]
                    reader.take(length)
                    reader.ebitmap()
                    reader.range()
                    reader.level()
                elif table == 5:  # booleans: value, state, name length
                    reader.take(reader.words(3)[2])
                elif table == 6:  # sensitivities: name length, alias flag, level
                    reader.take(reader.words(2)[0])
                    reader.level()
                else:  # categories: name length, value, alias flag
                    reader.take(reader.words(3)[0])
        self.allow: dict[tuple[int, int, int], int] = {}
        self.xperms: dict[tuple[int, int, int], list[tuple[int, int, bytes]]] = {}
        for _ in range(reader.word()):
            source, target, klass, specified = struct.unpack("<4H", reader.take(8))
            key = (source, target, klass)
            if specified & 0x0700:  # extended permissions: type, driver, 256 bits
                kind, driver = struct.unpack("<BB", reader.take(2))
                bitmap = reader.take(32)
                if specified & AVTAB_XPERMS_ALLOWED:
                    self.xperms.setdefault(key, []).append((kind, driver, bitmap))
            else:
                data = reader.word()
                if specified & AVTAB_ALLOWED:
                    self.allow[key] = self.allow.get(key, 0) | data

    @staticmethod
    def _permissions(reader: _Reader, count: int) -> dict[str, int]:
        permissions = {}
        for _ in range(count):
            length, value = reader.words(2)
            permissions[reader.name(length)] = value
        return permissions

    def _key(self, source: str, target: str, klass: str) -> tuple[int, int, int] | None:
        """The access vector key of a class and two types, None when the policy lacks one."""
        if klass not in self.classes or source not in self.types or target not in self.types:
            return None
        return self.types[source], self.types[target], self.class_values[klass]

    def permissions(self, source: str, target: str, klass: str) -> set[str]:
        """The permissions of `klass` that `source` holds on `target`."""
        key = self._key(source, target, klass)
        if key is None:
            return set()
        mask = self.allow.get(key, 0)
        return {name for name, value in self.classes[klass].items() if mask >> (value - 1) & 1}

    def ioctls(self, source: str, target: str, klass: str) -> set[int]:
        """The ioctl commands that the extended rules of `source` on `target` allow, if any."""
        key = self._key(source, target, klass)
        commands: set[int] = set()
        for kind, driver, bitmap in self.xperms.get(key, []) if key else []:
            for bit in range(256):
                if bitmap[bit // 8] >> (bit % 8) & 1:
                    if kind == XPERMS_IOCTL_FUNCTION:
                        commands.add(driver << 8 | bit)
                    elif kind == XPERMS_IOCTL_DRIVER:
                        commands.update(bit << 8 | low for low in range(256))
        return commands
