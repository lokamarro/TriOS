#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest probe: the kernel describes the virtual machine in SMBIOS tables.

It reads the tables the way userspace does, without any program that uses
them: /sys/class/dmi/id, the raw entry point and structure table under
/sys/firmware/dmi/tables, and the per-structure entries. The entry point must
be a valid SMBIOS 3.0 one whose checksum holds and which names the table's
length; the table must walk structure by structure to its end-of-table
structure, BIOS, system, baseboard, chassis, processor, memory and boot
structures in that order, with the strings and values of the contract in
kernel/common/README.md. /dev/mem must not exist. Prints one line per check
and exits with the number of failures. Runs as root on either guest
architecture.
"""

import errno
import os
import platform
import struct
import subprocess

failures = 0
VENDOR = "Kernarium"
PRODUCT = "Virtual Machine"
EXPECTED_TYPES = [0, 1, 2, 3, 4, 16, 17, 19, 32, 127]


def check(condition, message):
    global failures
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        failures += 1


def read_text(path):
    try:
        with open(path, encoding="ascii", errors="replace") as stream:
            return stream.read().rstrip("\n")
    except OSError as error:
        return f"<{errno.errorcode.get(error.errno, error.errno)}>"


def read_bytes(path):
    with open(path, "rb") as stream:
        return stream.read()


def possible_cpus():
    count = 0
    for part in read_text("/sys/devices/system/cpu/possible").split(","):
        first, _, last = part.partition("-")
        count += int(last or first) - int(first) + 1
    return count


def cpuinfo_model_name():
    for line in read_text("/proc/cpuinfo").splitlines():
        key, _, value = line.partition(":")
        if key.strip() == "model name":
            return value.strip()
    return None


def walk(table):
    """The structures as (type, handle, formatted area, strings, raw bytes)."""
    structures = []
    offset = 0
    while offset + 4 <= len(table):
        kind, length, handle = struct.unpack_from("<BBH", table, offset)
        if length < 4 or offset + length > len(table):
            raise ValueError(f"structure at {offset:#x} has length {length}")
        area = table[offset:offset + length]
        end = table.find(b"\0\0", offset + length)
        if end < 0:
            raise ValueError(f"structure at {offset:#x} has no string-set end")
        text = table[offset + length:end]
        strings = [s.decode("ascii") for s in text.split(b"\0")] if text else []
        structures.append((kind, handle, area, strings, table[offset:end + 2]))
        offset = end + 2
        if kind == 127:
            break
    return structures, offset


def string(structure, index):
    """SMBIOS string number index of a structure, or '' for number 0."""
    strings = structure[3]
    return strings[index - 1] if index else ""


def main():
    release = platform.release()

    # /sys/class/dmi/id: what the DMI core decoded.
    expected = {
        "sys_vendor": VENDOR,
        "product_name": PRODUCT,
        "product_family": PRODUCT,
        "product_version": "",
        "product_serial": "",
        "board_vendor": VENDOR,
        "board_name": PRODUCT,
        "bios_vendor": VENDOR,
        "bios_version": release,
        "bios_date": "",
        "chassis_vendor": VENDOR,
        "chassis_type": "1",
    }
    for name, value in expected.items():
        actual = read_text(f"/sys/class/dmi/id/{name}")
        check(actual == value, f"dmi/id/{name} is {actual!r}, expected {value!r}")
    check(not os.path.exists("/sys/class/dmi/id/product_uuid"),
          "dmi/id/product_uuid is absent: the machine claims no UUID")

    # No window onto physical memory.
    check(not os.path.exists("/dev/mem"), "/dev/mem does not exist")
    check(not os.path.exists("/sys/class/mem/mem"), "the mem device is not registered")

    # The SMBIOS 3.0 entry point.
    try:
        entry = read_bytes("/sys/firmware/dmi/tables/smbios_entry_point")
        table = read_bytes("/sys/firmware/dmi/tables/DMI")
    except OSError as error:
        check(False, f"the raw tables are readable: {error}")
        return
    check(len(entry) == 0x18 and entry[:5] == b"_SM3_",
          f"entry point is a 24-byte _SM3_ one ({entry[:5]!r}, {len(entry)} bytes)")
    check(sum(entry) % 256 == 0, "entry point checksum holds")
    if len(entry) == 0x18:
        length, major, minor, revision = entry[6], entry[7], entry[8], entry[10]
        (max_size,) = struct.unpack_from("<I", entry, 12)
        check(length == 0x18 and (major, minor) == (3, 0) and revision == 1,
              f"entry point names SMBIOS 3.0, revision 1 (length {length}, "
              f"version {major}.{minor}, revision {revision})")
        check(max_size == len(table),
              f"entry point's table size {max_size} is the table's {len(table)}")

    # The structure table.
    try:
        structures, end = walk(table)
    except ValueError as error:
        check(False, f"structure table walks: {error}")
        return
    types = [structure[0] for structure in structures]
    check(types == EXPECTED_TYPES, f"structure types are {types}")
    check(end == len(table), f"end-of-table structure ends the table ({end} of {len(table)})")
    handles = [structure[1] for structure in structures]
    check(len(set(handles)) == len(handles), "structure handles are unique")
    by_type = {structure[0]: structure for structure in structures}
    if types != EXPECTED_TYPES:
        return

    bios = by_type[0]
    area = bios[2]
    check(string(bios, area[4]) == VENDOR and string(bios, area[5]) == release,
          "BIOS vendor and version are the emulator and the kernel release")
    (characteristics,) = struct.unpack_from("<Q", area, 0x0a)
    check(characteristics == 1 << 3, "BIOS characteristics are 'not supported'")
    check(area[0x13] & (1 << 4) != 0, "BIOS extension byte 2 marks a virtual machine")

    system = by_type[1]
    area = system[2]
    check(string(system, area[4]) == VENDOR and string(system, area[5]) == PRODUCT
          and string(system, area[0x1a]) == PRODUCT,
          "system manufacturer, product and family")
    check(area[7] == 0 and area[8:0x18] == bytes(16),
          "system claims no serial number or UUID")

    board = by_type[2]
    area = board[2]
    check(string(board, area[4]) == VENDOR and string(board, area[5]) == PRODUCT
          and area[0x0d] == 0x0a,
          "baseboard is the emulator's motherboard")

    chassis = by_type[3]
    area = chassis[2]
    check(string(chassis, area[4]) == VENDOR and area[5] == 0x01,
          "chassis is the emulator's, of type Other")

    processor = by_type[4]
    area = processor[2]
    (cores2, enabled2, threads2) = struct.unpack_from("<HHH", area, 0x2a)
    cpus = possible_cpus()
    check((cores2, enabled2, threads2) == (cpus, cpus, cpus),
          f"processor counts {cores2}/{enabled2}/{threads2} are the {cpus} possible CPUs")
    version = string(processor, area[0x10])
    model = cpuinfo_model_name()
    if model is not None:
        check(version == model, f"processor version {version!r} is /proc/cpuinfo's model name")
    else:
        check(version != "", f"processor version {version!r} names the virtual CPU")

    device = by_type[17]
    mapping = by_type[19]
    (size_mib,) = struct.unpack_from("<H", device[2], 0x0c)
    if size_mib == 0x7fff:
        (size_mib,) = struct.unpack_from("<I", device[2], 0x1c)
    start_kib, end_kib = struct.unpack_from("<II", mapping[2], 4)
    if start_kib == 0xffffffff:
        _, end_bytes = struct.unpack_from("<QQ", mapping[2], 0x0f)
        mapped_mib = (end_bytes + 1) >> 20
    else:
        mapped_mib = (end_kib - start_kib + 1) >> 10
    mem_total_kib = int(read_text("/proc/meminfo").split()[1])
    check(size_mib == mapped_mib and size_mib * 1024 >= mem_total_kib,
          f"memory device ({size_mib} MiB) and mapping ({mapped_mib} MiB) "
          f"hold MemTotal ({mem_total_kib >> 10} MiB)")

    raw = read_bytes("/sys/firmware/dmi/entries/0-0/raw")
    check(raw == bios[4], "dmi-sysfs entry 0-0 is the BIOS structure")

    log = subprocess.run(["dmesg"], capture_output=True, text=True, check=False).stdout
    check("SMBIOS 3.0.0 present." in log, "the DMI core found the entry point at boot")
    check(f"DMI: {VENDOR} {PRODUCT}/{PRODUCT}, BIOS {release}" in log,
          "the boot log names the machine")


main()
print(f"{failures} failure(s)")
raise SystemExit(failures)
