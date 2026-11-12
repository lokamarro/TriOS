#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Contract of the gadget cache census analyzer on synthetic captures."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = (pathlib.Path(__file__).resolve().parents[2] /
          "perf/analyze_gadget_cache_census.py")
SPEC = importlib.util.spec_from_file_location("analyze_gadget_cache_census",
                                              SCRIPT)
assert SPEC is not None and SPEC.loader is not None
census = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = census  # dataclasses resolve their module by name
SPEC.loader.exec_module(census)


# Two arm64 caches of one 4096-slot geometry. Address space 7 runs on both,
# and both hold its translation at 0x1000; 0x2000 is the same code in
# address space 8, resident but idle in the second cache.
CAPTURE = """\
E 0xa generations=1 occupancy=2 slots=4096 slot_bytes=40 slab_slots=256 \
slab_count=2 stream_index=0 slabs=20480 streams=262144 arena_empty=131072 \
links=4000 indirect=0 \
total=286624 stream_lookups=0 snapshot=1 seconds=10.0 wall=1 lookups=100 \
hits=70 revalidated=5 misses=25 stores=25 first=20 again_evicted=4 \
again_retired=0 again_discarded=1 changed=0 restored=0 other_executor=3 \
other_space=1 evictions=6 evicted_bytes=1200 retirements=0 discards=1 \
departed=2 distance=1,0,2,1
G 7 current=1 rank=0 idle_switches=0 idle_seconds=0.000 links=4000 indirect=0 \
targets=0
L 7 0x1000
L 7 0x3000
P 7 0x1000 4 10 4 0xabc 128 32 80 16
W 0x1000 g100 o5 l1010 g200
P 7 0x3000 4 10 5 0xdef 144 32 80 20
E 0xb generations=2 occupancy=2 slots=4096 slot_bytes=40 slab_slots=256 \
slab_count=2 stream_index=0 slabs=20480 streams=262144 links=6000 indirect=0 \
total=288624 stream_lookups=0 snapshot=1 seconds=10.5 wall=1 lookups=100 \
hits=80 revalidated=0 misses=20 stores=20 first=20 again_evicted=0 \
again_retired=0 again_discarded=0 changed=0 restored=0 other_executor=0 \
other_space=0 evictions=0 evicted_bytes=0 retirements=0 discards=0 \
departed=0 distance=0
G 7 current=1 rank=0 idle_switches=0 idle_seconds=0.000 links=4000 indirect=0 \
targets=0
G 8 current=0 rank=1 idle_switches=3 idle_seconds=12.000 links=2000 indirect=0 \
targets=0
L 7 0x1000
P 7 0x1000 4 10 4 0xabc 128 32 80 16
P 8 0x2000 4 10 4 0x123 128 32 80 16
"""

# Two arm64 caches sharing address space 7 (space 0x70). Both hold the body
# at 0xb00 for 0x1000; only the first holds 0x3000. Each cache owns its slabs
# and slot list, and the space's bytes are on the S line each cache writes.
SHARED = """\
E 0xa generations=1 occupancy=2 slots=4096 slot_bytes=40 slab_slots=256 \
slab_count=2 stream_index=0 slabs=20480 streams=0 arena_empty=0 links=1024 \
indirect=0 total=21504 stream_lookups=0 adoptions=0 shared=1 snapshot=1 \
seconds=10.0 wall=1 lookups=100 hits=90 revalidated=0 misses=10 adopted=0 \
stores=10 first=10 again_evicted=0 again_retired=0 again_discarded=0 \
changed=0 restored=0 other_executor=0 other_space=0 evictions=0 \
evicted_bytes=0 retirements=0 discards=0 departed=0 distance=0
G 7 current=1 rank=0 idle_switches=0 idle_seconds=0.000 link_records=2 \
links=1024 indirect=0 targets=0 space=0x70
S 0x70 generation=7 mode=0 attached=2 stream_index=0 streams=65536 \
arena_empty=32768 links=4096 indirect=0 link_records=2 limbo=0
L 7 0x1000 0x70
P 7 0x1000 4 10 4 0xabc 128 32 80 16 0xb00
P 7 0x3000 4 10 5 0xdef 144 32 80 20 0xc00
E 0xb generations=1 occupancy=1 slots=4096 slot_bytes=40 slab_slots=256 \
slab_count=2 stream_index=0 slabs=20480 streams=0 arena_empty=0 links=1024 \
indirect=0 total=21504 stream_lookups=0 adoptions=1 shared=1 snapshot=1 \
seconds=10.5 wall=1 lookups=100 hits=90 revalidated=0 misses=5 adopted=5 \
stores=5 first=5 again_evicted=0 again_retired=0 again_discarded=0 \
changed=0 restored=0 other_executor=0 other_space=0 evictions=0 \
evicted_bytes=0 retirements=0 discards=0 departed=0 distance=0
G 7 current=1 rank=0 idle_switches=0 idle_seconds=0.000 link_records=2 \
links=1024 indirect=0 targets=0 space=0x70
S 0x70 generation=7 mode=0 attached=2 stream_index=0 streams=65536 \
arena_empty=32768 links=4096 indirect=0 link_records=2 limbo=0
L 7 0x1000 0x70
P 7 0x1000 4 10 4 0xabc 128 32 80 16 0xb00
"""

# What the census wrote before it carried snapshots, geometry or parts.
LEGACY = """\
E 0xa generations=1 occupancy=2 stream_index=0 slabs=10240 streams=1024 \
links=10 indirect=0 total=11274 stores=2 stream_lookups=0
P 7 0x1000 4 10 4 0xabc
P 7 0x2000 4 12 4 0xdef
E 0xb generations=1 occupancy=1 stream_index=0 slabs=10240 streams=512 \
links=10 indirect=0 total=10762 stores=1 stream_lookups=0
P 7 0x1000 4 10 4 0xabc
"""


def write(directory: pathlib.Path, text: str) -> pathlib.Path:
    path = directory / "census.txt"
    path.write_text(text)
    return path


def run_main(path: pathlib.Path, *extra: str) -> str:
    output = io.StringIO()
    with mock.patch.object(sys, "argv",
                           ["analyze", str(path), *extra]), \
            contextlib.redirect_stdout(output):
        census.main()
    return output.getvalue()


class CensusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def test_scan_groups_records_by_snapshot(self) -> None:
        path = write(self.root, CAPTURE)
        blocks = census.scan(path)
        self.assertEqual([b.snapshot for b in blocks], [1, 1])
        self.assertEqual(blocks[0].fields["distance"], [1, 0, 2, 1])
        contents = census.load(path, blocks[1])
        self.assertEqual(len(contents.translations), 2)
        self.assertEqual([s.generation for s in contents.spaces], [7, 8])
        self.assertEqual(census.load(path, blocks[0]).words[0][1],
                         ["g100", "o5", "l1010", "g200"])

    def test_duplication_separates_spaces_from_families(self) -> None:
        path = write(self.root, CAPTURE)
        contents = [census.load(path, block) for block in census.scan(path)]
        result = census.duplication(contents)
        self.assertEqual(result.resident, 4)
        # 0x1000 in space 7 is held twice; the 0x2000 code is distinct.
        self.assertEqual((result.space_copies, result.space_bytes), (1, 128))
        self.assertEqual((result.family_copies, result.family_bytes), (0, 0))
        self.assertEqual(result.distinct_in_space, 3)
        # Address space 7's record for 0x1000 is kept by both caches.
        self.assertEqual((result.links_sampled, result.links_distinct), (3, 2))

    def test_victim_tier_serves_returns_closer_than_its_size(self) -> None:
        # Bucket b holds distances below 2**b.
        histogram = [1, 0, 2, 1]
        self.assertEqual(census.victim_served(histogram, 1), 1)
        self.assertEqual(census.victim_served(histogram, 4), 3)
        self.assertEqual(census.victim_served(histogram, 4096), 4)

    def test_report_states_shares_and_hit_rate(self) -> None:
        path = write(self.root, CAPTURE)
        text = run_main(path)
        self.assertIn("arm64 snapshot 1", text)
        self.assertIn("hit rate 77.50%", text)
        # Two slabs of 256 40-byte slots in each cache.
        self.assertRegex(text, r"slot tags\s+0\.0 MiB\s+7\.1%")
        # Half of the first cache's arena is a chunk with no live body.
        self.assertRegex(text, r"arena chunks holding no body\s+0\.1 MiB")
        self.assertIn("again after way eviction                         4", text)
        self.assertRegex(text, r"idle 10-60 s\s+spaces\s+1")
        self.assertIn("sampled streams: 1 bodies, 4 words", text)
        # A capture written before the census carried the budget says nothing
        # of it.
        self.assertNotIn("gadget-cache budget", text)
        # Every departure and store was recorded.
        self.assertNotIn("no memory to record", text)

    def test_report_states_what_the_census_could_not_record(self) -> None:
        path = write(self.root, CAPTURE.replace(
            "departed=2 distance=", "departed=2 unrecorded=3 distance="))
        text = run_main(path)
        self.assertIn("departures and stores the census had no memory to "
                      "record 3: the repeats above are lower bounds", text)

    def test_report_states_the_budget(self) -> None:
        budgeted = CAPTURE.replace(
            "total=286624 stream_lookups=0 ",
            "total=286624 stream_lookups=0 budget=2147483648 "
            "metered=104857600 metered_shedding=0 metered_peak=0 reliefs=0 "
            "shed=0 ").replace(
            "total=288624 stream_lookups=0 ",
            "total=288624 stream_lookups=0 budget=2147483648 "
            "metered=110100480 metered_shedding=1048576 metered_peak=0 "
            "reliefs=2 shed=3 ")
        path = write(self.root, budgeted)
        text = run_main(path)
        # The later snapshot's view of the process.
        self.assertIn("gadget-cache budget 2048.0 MiB: 105.0 MiB charged, "
                      "1.0 MiB of it shedding; 2 reliefs shed 3 address "
                      "spaces", text)

    def test_series_lists_every_snapshot(self) -> None:
        path = write(self.root, CAPTURE)
        lines = run_main(path, "--series").splitlines()
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[1].split()[0] == "1")

    def test_shared_spaces_and_bodies_count_once(self) -> None:
        path = write(self.root, SHARED)
        blocks = census.scan(path)
        contents = [census.load(path, block) for block in blocks]
        # Each cache's own bytes, and the space once.
        parts = census.measured(blocks, contents)
        self.assertEqual(parts["total"], 2 * 21504 + 65536 + 4096)
        self.assertEqual(parts["arena_empty"], 32768)
        self.assertEqual(len(census.distinct_bodies(contents)), 2)
        result = census.duplication(contents)
        self.assertEqual(result.resident, 3)
        self.assertEqual((result.space_copies, result.space_bytes), (0, 0))
        self.assertEqual(result.bytes, 128 + 144)
        # The one record both caches sampled is one record.
        self.assertEqual((result.links_sampled, result.links_distinct), (1, 1))
        text = run_main(path)
        self.assertIn("1 address-space records, held by 2.00 caches each", text)
        self.assertIn("adopted from another cache of the space 5", text)
        self.assertIn("served without translating 92.50%", text)
        self.assertRegex(text, r"bodies\s+2, held by 1\.500 slots each")

    def test_destroyed_generations_are_counted_once(self) -> None:
        # Both caches hold space 0x70, whose generation the MMU destroyed.
        dead = SHARED.replace("link_records=2 limbo=0",
                              "link_records=2 limbo=0 alive=0")
        path = write(self.root, dead)
        blocks = census.scan(path)
        contents = [census.load(path, block) for block in blocks]
        self.assertEqual(census.dead_spaces(contents), (1, 65536 + 4096))
        text = run_main(path)
        self.assertIn("1 address spaces held for destroyed generations, 0.1 MiB", text)
        header, row = run_main(path, "--series").splitlines()
        # The column after the dormant share is the destroyed spaces' MiB.
        self.assertEqual(row.split()[9], "0.1")
        self.assertIn("dead MiB", header)
        live = SHARED.replace("link_records=2 limbo=0",
                              "link_records=2 limbo=0 alive=1")
        path = write(self.root, live)
        contents = [census.load(path, block) for block in census.scan(path)]
        self.assertEqual(census.dead_spaces(contents), (0, 0))
        self.assertNotIn("destroyed generations", run_main(path))
        # A capture that does not say counts nothing as dead.
        path = write(self.root, SHARED)
        contents = [census.load(path, block) for block in census.scan(path)]
        self.assertEqual(census.dead_spaces(contents), (0, 0))

    def test_destroyed_x86_generations_count_what_they_hold(self) -> None:
        # x86 caches keep their spaces to themselves: no S records, the bytes
        # of a generation's translations on its G record.
        capture = """\
E 0xa generations=2 occupancy=2 stream_index=0 slabs=10240 streams=1024 \
links=10 indirect=0 total=11274 stores=2 snapshot=1 seconds=5.0
G 7 current=1 rank=0 idle_switches=0 idle_seconds=0.000 link_records=2 \
links=100 indirect=20 targets=3 alive=1 held=1000
G 8 current=0 rank=1 idle_switches=2 idle_seconds=1.000 link_records=2 \
links=400 indirect=50 targets=6 alive=0 held=2000
"""
        path = write(self.root, capture)
        contents = [census.load(path, block) for block in census.scan(path)]
        self.assertEqual(census.dead_spaces(contents), (1, 400 + 50 + 6 + 2000))

    def test_legacy_capture_still_reads(self) -> None:
        path = write(self.root, LEGACY)
        text = run_main(path)
        self.assertIn("snapshot final", text)
        self.assertIn("factor 1.500x", text)
        self.assertIn("bodies and streams", text)


if __name__ == "__main__":
    unittest.main()
