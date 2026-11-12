#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host-only proof that a CTS shard survives one hang and one host abort.

A guest the host had to kill is a session abort, never a case result: the
shard re-runs from the last result the dead guest transported and gives up
as a whole once its resumptions are spent.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import base64
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

INTEGRATION_GPU = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(INTEGRATION_GPU))
import run_vk_gl_cts as gl  # noqa: E402
import run_vk_cts as vk  # noqa: E402
import receipt_fixtures  # noqa: E402
import test_run_vk_gl_cts as gl_fixtures  # noqa: E402


def setUpModule() -> None:
    receipt_fixtures.bind_vulkan_receipt(vk)


CASES = tuple(f"KHR-GL46.fixture.case{index:02d}" for index in range(7))


def qpa_result(case: str, status: str = "Pass") -> str:
    return (
        f"#beginTestCaseResult {case}\n"
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<TestCaseResult Version="0.3.4" CasePath="{case}" '
        'CaseType="SelfValidate">\n'
        ' <Text>virtio-gpu venus</Text>\n'
        f' <Result StatusCode="{status}">result</Result>\n'
        "</TestCaseResult>\n"
        "#endTestCaseResult\n"
    )


def qpa_session(cases, *, ended: bool = True, in_flight: str | None = None) -> str:
    body = "".join(qpa_result(case) for case in cases)
    text = "#sessionInfo releaseName 1.4.5.5\n#beginSession\n" + body
    if in_flight is not None:
        text += f"#beginTestCaseResult {in_flight}\n<?xml version"
    if ended:
        text += "#endSession\n"
    return text


def console_log(cases, *, tail: str) -> str:
    """A guest console that announced each case before running it."""
    return "".join(f"Test case '{case}'..\n  Pass\n" for case in cases) + tail


def framed(name: str, payload: bytes) -> str:
    compressed = gzip.compress(payload, mtime=0)
    digest = hashlib.sha256(compressed).hexdigest()
    encoded = base64.b64encode(compressed).decode("ascii")
    return (
        f"UML_CTS_ARTIFACT_BEGIN|{name}|gzip+base64|{len(compressed)}|{digest}\n"
        f"UML_CTS_ARTIFACT_CHUNK|{name}|0|{encoded}\n"
        f"UML_CTS_ARTIFACT_END|{name}|1\n"
    )


def segmented(payload: bytes, parts: int) -> dict[str, bytes]:
    step = max(1, -(-len(payload) // parts))
    return {
        f"results.qpa.{index:04d}": payload[offset : offset + step]
        for index, offset in enumerate(range(0, max(len(payload), 1), step))
    }


class Step:
    """One scripted guest session."""

    def __init__(
        self,
        *,
        outcome: str,
        cases,
        ended: bool,
        in_flight: str | None = None,
        summary: bytes | None = None,
        selected=None,
        console: str | None = None,
        return_code: int = 0,
        drop_trailer: bool = False,
        no_qpa: bool = False,
        shutdown_stall: bool = False,
    ) -> None:
        self.outcome = outcome
        self.return_code = return_code
        self.shutdown_stall = shutdown_stall
        self.console = console or (
            "Darwin UML stopped: result=0 exitcode=0\n"
            if outcome == "completed"
            else "guest went quiet\n"
        )
        wire: dict[str, bytes] = {}
        if not no_qpa:
            wire.update(
                segmented(
                    qpa_session(cases, ended=ended, in_flight=in_flight).encode(), 2
                )
            )
        wire["renderer.txt"] = (
            b"device=/dev/dri/card0\ndevice=/dev/dri/renderD128\n"
            b"x11_server=xwayland\nguest_page_size=4096\n"
            b"zink over Virtio-GPU Venus\n"
        )
        if selected is not None:
            wire["selected.txt"] = "".join(f"{case}\n" for case in selected).encode()
        if summary is not None:
            wire["summary.txt"] = summary
        self.frames = "".join(framed(name, payload) for name, payload in wire.items())
        if drop_trailer:
            self.frames = "\n".join(self.frames.splitlines()[:-1]) + "\n"


class ScriptedGuests:
    def __init__(self, steps) -> None:
        self.steps = list(steps)
        self.calls: list[dict] = []

    def __call__(
        self,
        command,
        console_path,
        transport_path,
        socket_path,
        *,
        guest_timeout,
        console_stall_timeout=0,
        environment=None,
    ):
        step = self.steps[len(self.calls)]
        self.calls.append(
            {"command": list(command), "console": console_path, "environment": environment}
        )
        console_path.parent.mkdir(parents=True, exist_ok=True)
        console_path.write_text(step.console)
        transport_path.write_text(step.frames)
        artifacts = gl.decode_artifacts(
            step.frames, partial=step.outcome != "completed"
        )
        return gl.RawSession(
            step.console,
            step.frames,
            step.return_code,
            step.outcome,
            None,
            artifacts,
            step.shutdown_stall,
        )


def drive(directory: Path, steps, *, max_resumptions: int = 4):
    guests = ScriptedGuests(steps)
    original = gl.run_guest_session
    gl.run_guest_session = guests
    try:
        sessions, expected, merged = gl.run_shard_sessions(
            directory,
            build_command=lambda resume, guest: ["fixture", str(resume), str(guest)],
            resolve_expected=lambda first: tuple(
                gl.parse_case_list(first["selected.txt"].decode())
            ),
            check_session=lambda index, resume, found: None,
            guest_timeout=60,
            console_stall_timeout=30,
            max_resumptions=max_resumptions,
        )
    finally:
        gl.run_guest_session = original
    return guests, sessions, expected, merged


def provenance_metadata(sessions, root: Path) -> dict:
    return {
        "session_count": len(sessions),
        "max_resumptions": 4,
        "artifacts_provenance": gl.shard_provenance(sessions),
        "sessions": gl.session_records(sessions, root),
    }


class QpaTransportTests(unittest.TestCase):
    def test_checkpoint_segments_assemble_into_one_artifact(self) -> None:
        payload = qpa_session(CASES).encode()
        transcript = "".join(
            framed(name, chunk) for name, chunk in segmented(payload, 3).items()
        )
        self.assertEqual(gl.decode_artifacts(transcript), {"results.qpa": payload})

    def test_segment_gaps_duplicates_and_mixtures_fail_closed(self) -> None:
        payload = qpa_session(CASES).encode()
        parts = segmented(payload, 3)
        names = sorted(parts)
        gap = "".join(
            framed(name, parts[name]) for name in names if name != names[1]
        )
        with self.assertRaisesRegex(gl.CampaignError, "gapless prefix"):
            gl.decode_artifacts(gap)
        mixed = framed(names[0], parts[names[0]]) + framed("results.qpa", payload)
        with self.assertRaisesRegex(gl.CampaignError, "whole-file QPA"):
            gl.decode_artifacts(mixed)
        with self.assertRaisesRegex(gl.CampaignError, "unsafe or unexpected"):
            gl.decode_artifacts(framed("results.qpa.99999", b"x"))

    def test_partial_decode_keeps_complete_segments_only(self) -> None:
        payload = qpa_session(CASES[:4], ended=False).encode()
        parts = segmented(payload, 2)
        names = sorted(parts)
        transcript = framed(names[0], parts[names[0]])
        truncated = framed(names[1], parts[names[1]]).splitlines()[:-1]
        transcript += "\n".join(truncated) + "\n"
        with self.assertRaisesRegex(gl.CampaignError, "missing its trailer"):
            gl.decode_artifacts(transcript)
        rejected: list[str] = []
        self.assertEqual(
            gl.decode_artifacts(transcript, partial=True, rejected=rejected),
            {"results.qpa": parts[names[0]]},
        )
        self.assertEqual(rejected, [names[1]])

    def test_a_zero_length_artifact_is_never_a_payload(self) -> None:
        """A complete frame group can still carry nothing.

        A guest whose filesystem filled framed a zero-byte compressed
        artifact, digest of the empty string and all, which no gzip stream can
        be. Strict decoding refuses the transcript; the salvage lane drops it
        and names it, so the loss is reported rather than inferred from a short
        result set.
        """

        payload = qpa_session(CASES[:2], ended=False).encode()
        empty = (
            "UML_CTS_ARTIFACT_BEGIN|results.qpa.0001|gzip+base64|0|"
            + hashlib.sha256(b"").hexdigest()
            + "\nUML_CTS_ARTIFACT_END|results.qpa.0001|0\n"
        )
        transcript = framed("results.qpa.0000", payload) + empty
        with self.assertRaisesRegex(gl.CampaignError, "invalid compressed size"):
            gl.decode_artifacts(transcript)
        rejected: list[str] = []
        self.assertEqual(
            gl.decode_artifacts(transcript, partial=True, rejected=rejected),
            {"results.qpa": payload},
        )
        self.assertEqual(rejected, ["results.qpa.0001"])


class QpaPrefixTests(unittest.TestCase):
    def test_prefix_scan_separates_complete_results_from_the_trailer(self) -> None:
        text = qpa_session(CASES[:3], ended=False, in_flight=CASES[3])
        prefix = gl.scan_qpa(text)
        self.assertEqual(list(prefix.results), list(CASES[:3]))
        self.assertEqual(prefix.in_flight_case, CASES[3])
        self.assertFalse(prefix.ended_session)
        with self.assertRaisesRegex(gl.CampaignError, "truncated"):
            gl.parse_qpa(text)

    def test_complete_session_parses_identically_through_both_paths(self) -> None:
        text = qpa_session(CASES)
        self.assertEqual(gl.parse_qpa(text), gl.scan_qpa(text).results)
        self.assertTrue(gl.scan_qpa(text).ended_session)


class ConsoleDiagnosticTests(unittest.TestCase):
    def test_the_last_announced_case_is_the_one_kept(self) -> None:
        transcript = console_log(CASES[:3], tail=f"Test case '{CASES[3]}'..\n")
        self.assertEqual(gl.last_console_case(transcript), CASES[3])

    def test_a_console_that_named_no_case_yields_nothing(self) -> None:
        self.assertIsNone(gl.last_console_case(""))
        self.assertIsNone(
            gl.last_console_case("Darwin UML host signal 11 at pc=0x0\n")
        )
        # A name the traversal order could never contain is not a case.
        self.assertIsNone(gl.last_console_case("Test case 'not a case'..\n"))


class QpaMergeTests(unittest.TestCase):
    def evidence(self, index, cases, *, ended, outcome, resume, console_case=None):
        return gl.SessionEvidence(
            index=index,
            outcome=outcome,
            resume_after=resume,
            qpa_text=qpa_session(cases, ended=ended),
            completed_cases=tuple(cases),
            console_case=console_case,
            console="console.log",
            artifact_transport=gl.ARTIFACT_TRANSPORT_LOG,
            artifacts_directory="artifacts",
            return_code=0,
        )

    def test_single_complete_session_is_returned_byte_for_byte(self) -> None:
        session = self.evidence(0, CASES, ended=True, outcome="completed", resume=0)
        self.assertEqual(gl.merge_qpa_sessions([session]), session.qpa_text)

    def test_a_merge_carries_only_what_guests_transported(self) -> None:
        first = self.evidence(
            0, CASES[:3], ended=False, outcome="stalled", resume=0,
            console_case=CASES[3],
        )
        second = self.evidence(
            1, CASES[3:], ended=True, outcome="completed", resume=3,
        )
        merged = gl.merge_qpa_sessions([first, second])
        results = gl.parse_qpa(merged)
        self.assertEqual(list(results), list(CASES))
        # The case the first guest died on was re-run, so its only result is
        # the one the second guest produced.  Nothing is host-authored.
        self.assertEqual(set(results.values()), {"Pass"})
        self.assertEqual(merged.count(f"#beginTestCaseResult {CASES[3]}"), 1)
        self.assertEqual(merged.count("#beginSession"), 1)
        self.assertEqual(merged.count("#endSession"), 1)
        self.assertIn(f"{gl.QPA_BOUNDARY}session 1 resumes at index 3", merged)
        self.assertNotIn("host-recorded", merged)


class ShardResumptionTests(unittest.TestCase):
    def summary(self) -> bytes:
        return b"cts_rc=0\n"

    def hang_then_finish(self, root: Path, *, max_resumptions: int = 4):
        steps = [
            Step(
                outcome="stalled",
                cases=CASES[:3],
                ended=False,
                in_flight=CASES[3],
                selected=CASES,
                console=console_log(CASES[:4], tail="guest went quiet\n"),
            ),
            Step(
                outcome="completed",
                cases=CASES[3:],
                ended=True,
                selected=CASES[3:],
                summary=self.summary(),
            ),
        ]
        return drive(root, steps, max_resumptions=max_resumptions)

    def test_a_hang_re_runs_the_case_its_guest_died_on(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guests, sessions, expected, merged = self.hang_then_finish(root)
            self.assertEqual(expected, CASES)
            self.assertEqual(list(merged), list(CASES))
            # No case was blamed for the emulator exit: every result in the
            # shard came from a guest that actually finished that case.
            self.assertEqual(list(merged.values()), ["Pass"] * 7)
            self.assertEqual(len(sessions), 2)
            self.assertEqual([row.resume_after for row in sessions], [0, 3])
            self.assertEqual(sessions[0].outcome, "stalled")
            self.assertEqual(sessions[1].outcome, "completed")
            # The console name is kept as a diagnostic only.
            self.assertEqual(sessions[0].console_case, CASES[3])
            self.assertEqual(sessions[0].record(root)["console_case"], CASES[3])
            self.assertIsNone(sessions[1].console_case)
            # The fresh guest restarts at the same index the dead one reached.
            self.assertEqual(guests.calls[1]["command"][1], "3")
            self.assertEqual(
                sessions[0].artifacts_directory, "sessions/00/artifacts"
            )
            self.assertTrue((root / "sessions/01/console.log").is_file())
            self.assertTrue((root / "console.log").is_file())

    def test_a_host_abort_is_a_session_abort_not_a_case_result(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            steps = [
                Step(
                    outcome="aborted",
                    cases=CASES[:2],
                    ended=False,
                    selected=CASES,
                    return_code=134,
                    console=console_log(
                        CASES[:3], tail="Darwin UML host signal 6 at pc=0x0\n"
                    ),
                ),
                Step(
                    outcome="completed",
                    cases=CASES[2:],
                    ended=True,
                    selected=CASES[2:],
                    summary=self.summary(),
                ),
            ]
            _, sessions, _, merged = drive(Path(temporary), steps)
            self.assertEqual(merged[CASES[2]], "Pass")
            self.assertEqual(sessions[0].console_case, CASES[2])
            self.assertEqual(sessions[0].return_code, 134)
            self.assertEqual(sessions[1].resume_after, 2)
            self.assertEqual(list(merged), list(CASES))

    def test_resumptions_are_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            steps = [
                Step(outcome="stalled", cases=CASES[:1], ended=False, selected=CASES,
                     console=console_log(CASES[:2], tail="quiet\n")),
                Step(outcome="stalled", cases=CASES[1:2], ended=False,
                     selected=CASES[1:],
                     console=console_log(CASES[1:3], tail="quiet\n")),
                Step(outcome="completed", cases=CASES[2:], ended=True,
                     selected=CASES[2:], summary=self.summary()),
            ]
            with self.assertRaisesRegex(
                gl.CampaignError, f"session aborted 2 times at {CASES[2]}"
            ):
                drive(Path(temporary), steps, max_resumptions=1)

    def test_zero_bound_keeps_the_historical_single_session_shard(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="stalled", cases=CASES[:1], ended=False, selected=CASES),
            ]
            # No console name at all: the shard still says where it stopped,
            # from the traversal order rather than from an invented result.
            with self.assertRaisesRegex(
                gl.CampaignError, f"session aborted 1 times at {CASES[1]}"
            ):
                drive(root, steps, max_resumptions=0)

    def test_a_clean_single_session_shard_keeps_its_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="completed", cases=CASES, ended=True,
                     selected=CASES, summary=self.summary()),
            ]
            _, sessions, _, merged = drive(root, steps)
            self.assertEqual(len(sessions), 1)
            self.assertEqual(sessions[0].artifacts_directory, "artifacts")
            self.assertFalse((root / gl.SESSION_DIRECTORY).exists())
            self.assertEqual(list(merged), list(CASES))

    def test_a_hang_on_the_last_case_costs_one_more_guest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="stalled", cases=CASES[:-1], ended=False,
                     in_flight=CASES[-1], selected=CASES, summary=b"cts_rc=124\n",
                     console=console_log(CASES, tail="guest went quiet\n")),
                Step(outcome="completed", cases=CASES[-1:], ended=True,
                     selected=CASES[-1:], summary=b"cts_rc=0\n"),
            ]
            guests, sessions, expected, merged = drive(root, steps)
            # Even the last case is re-run rather than recorded as a failure.
            self.assertEqual(len(guests.calls), 2)
            self.assertEqual(guests.calls[1]["command"][1], str(len(CASES) - 1))
            self.assertEqual(sessions[0].console_case, CASES[-1])
            self.assertEqual(merged[CASES[-1]], "Pass")
            self.assertEqual(list(merged), list(CASES))
            self.assertEqual(gl.shard_provenance(sessions), "host-merged")
            self.assertEqual(sessions[0].artifacts_directory, "sessions/00/artifacts")
            artifacts = gl.merged_shard_artifacts(sessions, root, len(expected))
            gl.write_artifacts(artifacts, root / "artifacts")
            gl.validate_session_provenance(
                root, provenance_metadata(sessions, root), artifacts, len(expected)
            )
            self.assertEqual(
                gl.parse_qpa(artifacts["results.qpa"].decode())[CASES[-1]], "Pass"
            )

    def test_a_dirty_stop_with_every_result_in_hand_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="stalled", cases=CASES, ended=False,
                     selected=CASES, summary=b"cts_rc=0\n"),
            ]
            _, sessions, expected, _ = drive(root, steps)
            # There is nothing left to re-run, but the guest never closed its
            # session: that shard is not a complete one and must not pass.
            self.assertEqual(sessions[0].outcome, "stalled")
            self.assertEqual(gl.shard_provenance(sessions), "single-session-guest")
            artifacts = {
                path.name: path.read_bytes()
                for path in sorted((root / "artifacts").iterdir())
            }
            with self.assertRaisesRegex(
                gl.CampaignError, "must end on a session that completed"
            ):
                gl.validate_session_provenance(
                    root, provenance_metadata(sessions, root), artifacts, len(expected)
                )

    def test_an_abort_before_the_first_checkpoint_re_runs_everything(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="aborted", cases=(), ended=False, selected=CASES,
                     no_qpa=True, return_code=134,
                     console="Darwin UML host signal 6 at pc=0x0\n"),
                Step(outcome="completed", cases=CASES, ended=True,
                     selected=CASES, summary=b"cts_rc=0\n"),
            ]
            guests, sessions, expected, merged = drive(root, steps)
            # The first guest delivered its enumeration and no results, so the
            # second one starts over at index 0 with nothing written off.
            self.assertEqual(sessions[0].completed_cases, ())
            self.assertIsNone(sessions[0].console_case)
            self.assertEqual(guests.calls[1]["command"][1], "0")
            self.assertEqual(merged[CASES[0]], "Pass")
            self.assertEqual(list(merged), list(CASES))
            artifacts = gl.merged_shard_artifacts(sessions, root, len(expected))
            gl.write_artifacts(artifacts, root / "artifacts")
            gl.validate_session_provenance(
                root, provenance_metadata(sessions, root), artifacts, len(expected)
            )
            self.assertEqual(
                list(gl.parse_qpa(artifacts["results.qpa"].decode())), list(CASES)
            )

    def test_out_of_order_guest_results_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            steps = [
                Step(outcome="stalled", cases=CASES[:2], ended=False, selected=CASES),
                Step(outcome="completed", cases=CASES[5:], ended=True,
                     selected=CASES[5:], summary=self.summary()),
            ]
            with self.assertRaisesRegex(gl.CampaignError, "traversal order"):
                drive(Path(temporary), steps)

    def test_continuation_guests_get_their_own_ownership_journal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            token = "a" * 32
            os.environ[gl.guest_process.OWNERSHIP_ENV] = json.dumps(
                {"path": str(root / gl.guest_process.JOURNAL_NAME), "token": token}
            )
            try:
                guests, sessions, _, _ = self.hang_then_finish(root)
            finally:
                del os.environ[gl.guest_process.OWNERSHIP_ENV]
            self.assertIsNone(guests.calls[0]["environment"])
            setting = json.loads(
                guests.calls[1]["environment"][gl.guest_process.OWNERSHIP_ENV]
            )
            self.assertEqual(setting["token"], token)
            self.assertEqual(
                Path(setting["path"]),
                root / "sessions/01" / gl.guest_process.JOURNAL_NAME,
            )
            self.assertIsNone(sessions[0].ownership_journal)
            self.assertEqual(
                sessions[1].ownership_journal,
                f"sessions/01/{gl.guest_process.JOURNAL_NAME}",
            )


class MergedProvenanceTests(unittest.TestCase):
    def build(self, root: Path):
        steps = [
            Step(outcome="stalled", cases=CASES[:3], ended=False,
                 in_flight=CASES[3], selected=CASES,
                 console=console_log(CASES[:4], tail="guest went quiet\n")),
            Step(outcome="completed", cases=CASES[3:], ended=True,
                 selected=CASES[3:], summary=b"cts_rc=0\n"),
        ]
        _, sessions, expected, merged = drive(root, steps)
        artifacts = gl.merged_shard_artifacts(sessions, root, len(expected))
        gl.write_artifacts(artifacts, root / "artifacts")
        return provenance_metadata(sessions, root), artifacts, expected

    def test_merged_artifacts_are_reproducible_from_session_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            metadata, artifacts, expected = self.build(root)
            gl.validate_session_provenance(root, metadata, artifacts, len(expected))
            summary = artifacts["summary.txt"].decode()
            # A shard that re-ran its way to a complete result set has nothing
            # failing of the host's own making.
            self.assertIn("cts_rc=0", summary)
            self.assertIn("provenance=host-merged", summary)
            self.assertIn("sessions=2", summary)
            self.assertIn(f"expected_case_count={len(CASES)}", summary)
            self.assertNotIn("host_recorded_", summary)
            self.assertNotIn("host_recorded_results", metadata)

    def test_tampered_merge_or_dropped_record_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            metadata, artifacts, expected = self.build(root)
            forged = dict(artifacts)
            forged["results.qpa"] = artifacts["results.qpa"].replace(
                b'<Result StatusCode="Pass">result</Result>',
                b'<Result StatusCode="Fail">result</Result>',
                1,
            )
            with self.assertRaisesRegex(gl.CampaignError, "not reproducible"):
                gl.validate_session_provenance(root, metadata, forged, len(expected))
            relabelled = dict(metadata, artifacts_provenance="single-session-guest")
            with self.assertRaisesRegex(gl.CampaignError, "contradicts its sessions"):
                gl.validate_session_provenance(root, relabelled, artifacts, len(expected))
            over_bound = dict(metadata, max_resumptions=0)
            with self.assertRaisesRegex(gl.CampaignError, "recorded resumption bound"):
                gl.validate_session_provenance(root, over_bound, artifacts, len(expected))

    def test_replaced_session_evidence_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            metadata, artifacts, expected = self.build(root)
            (root / "sessions/01/console.log").write_text("replaced\n")
            with self.assertRaisesRegex(gl.CampaignError, "session evidence changed"):
                gl.validate_session_provenance(root, metadata, artifacts, len(expected))


class ShutdownBoundTests(unittest.TestCase):
    """A guest that delivered its evidence must still stop within a bound.

    An unkillable wedged case can hold the poweroff open while the guest
    kernel keeps printing stall reports, so a bound derived from console
    silence never fires and the shard would burn its whole guest timeout.
    """

    def wait(self, *, deliver: bool, grace: int, timeout: int):
        frames = [
            "UML_CTS_ARTIFACT_BEGIN|summary.txt|gzip+base64|10|" + "0" * 64 + "\n",
            "UML_CTS_ARTIFACT_CHUNK|summary.txt|0|AAAA\n",
        ]
        if deliver:
            frames.append(f"{gl.FINAL_ARTIFACT_FRAME}1\n")

        class Transport:
            connection = None

            def __init__(self, path: Path) -> None:
                self.lines: list[str] = []
                self.pending = list(frames)

            def service(self, timeout: float = 0.0) -> None:
                time.sleep(0.02)
                if self.pending:
                    self.lines.append(self.pending.pop(0))

            def finish(self) -> str:
                return "".join(self.lines)

            def close(self) -> None:
                pass

        class Process:
            returncode = None

            def poll(self) -> None:
                return None

            def wait(self, timeout: float | None = None) -> None:
                return None

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            console = root / "console.log"
            console.write_text("[  900.0] rcu: kthread starved\n")
            with mock.patch.object(gl, "AcknowledgedArtifactSocket", Transport):
                gl.wait_for_guest(
                    Process(), console, timeout, root / "socket",
                    console_stall_timeout=0, shutdown_grace=grace,
                )

    def test_a_delivered_guest_that_never_stops_is_bounded(self) -> None:
        with self.assertRaises(gl.GuestSessionInterrupted) as caught:
            self.wait(deliver=True, grace=1, timeout=60)
        self.assertEqual(caught.exception.reason, "stalled")
        self.assertIn("did not stop within 1 seconds", str(caught.exception))
        # The partial evidence travels with the interruption, which is what a
        # resumption is rebuilt from.
        self.assertIn(gl.FINAL_ARTIFACT_FRAME, caught.exception.artifact_transcript)
        self.assertIn("rcu:", caught.exception.transcript)
        # A shard that already holds every acknowledged frame lost no coverage.
        self.assertTrue(caught.exception.delivered)

    def test_the_bound_is_off_before_the_final_frame_and_when_disabled(self) -> None:
        for deliver, grace in ((False, 1), (True, 0)):
            with self.assertRaises(gl.GuestSessionInterrupted) as caught:
                self.wait(deliver=deliver, grace=grace, timeout=2)
            self.assertIn("exceeded 2 seconds", str(caught.exception))
            self.assertEqual(caught.exception.delivered, deliver)

    def test_a_delivered_stall_completes_its_shard_and_stays_recorded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guests, sessions, expected, merged = drive(root, [
                Step(
                    outcome="stalled",
                    cases=CASES,
                    ended=True,
                    selected=CASES,
                    summary=b"cts_rc=0\n",
                    console="guest delivered its evidence and never stopped\n",
                    return_code=-15,
                    shutdown_stall=True,
                ),
            ])
            # One guest, no resumption, and every selected case kept.
            self.assertEqual(len(guests.calls), 1)
            self.assertEqual(expected, CASES)
            self.assertEqual(tuple(merged), CASES)
            self.assertEqual(sessions[0].outcome, "completed")
            self.assertIsNone(sessions[0].console_case)
            self.assertTrue(sessions[0].shutdown_stall)
            self.assertTrue(sessions[0].record(root)["shutdown_stall"])


class VulkanResumptionTests(unittest.TestCase):
    def test_resumed_launcher_skips_only_the_accounted_prefix(self) -> None:
        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        selection = vk.resolve_receipt_selection(
            "current-must-pass", [], None, vk.Fraction(0, 8)
        )
        first = vk.build_receipt_guest_launcher(
            receipt, selection, timeout=900, progress_timeout=600
        )
        resumed = vk.build_receipt_guest_launcher(
            receipt, selection, timeout=900,
            progress_timeout=600, resume_after=1234,
        )
        self.assertIn("--deqp-fraction=0,8", first)
        self.assertIn("--deqp-fraction-mandatory-caselist-file=", first)
        self.assertIn('tail -n +1235 "$expected" > "$selected"', resumed)
        # The resumed guest re-enumerates the identical fraction and then runs
        # the remainder as an explicit list, so the filter must not run twice.
        run_block = resumed.rsplit("emit_prerun ||", 1)[1]
        self.assertNotIn("--deqp-fraction=", run_block)
        self.assertIn("results.qpa.%04d", resumed)
        self.assertIn("emit_prerun", resumed)

    def test_a_resumed_receipt_shard_records_no_case_result(self) -> None:
        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        selection = vk.resolve_receipt_selection(
            "exact-family", ["dEQP-VK.info.build"], None, vk.Fraction(0, 1)
        )
        # A resumed shard has nothing to declare beyond its merge: it reaches
        # the ordinary artifact contract, not a host-recorded-case guard.
        for session_count in (1, 2):
            with self.assertRaisesRegex(vk.CampaignError, "artifact set is incomplete"):
                vk.validate_receipt_attempt(
                    receipt, selection, "", {}, session_count=session_count
                )
        with self.assertRaisesRegex(vk.CampaignError, "at least one guest session"):
            vk.validate_receipt_attempt(receipt, selection, "", {}, session_count=0)


class GuestCheckpointProgramTests(unittest.TestCase):
    """Run the generated guest checkpoint function in a real POSIX shell."""

    def shell_function(self, launcher: str, name: str) -> str:
        match = re.search(rf"^{name}\(\)\n\{{\n.*?^\}}\n", launcher, re.M | re.S)
        self.assertIsNotNone(match, f"{name} is not a top-level shell function")
        return match.group(0)

    def device_launcher(self) -> str:
        """The launcher the baked device campaigns run, exclusions and all."""

        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        selection = vk.resolve_receipt_selection(
            "current-must-pass", [], None, vk.Fraction(0, 16)
        )
        return vk.build_receipt_guest_launcher(
            receipt,
            selection,
            timeout=vk.APPLE_MUSTPASS_TIMEOUT,
            progress_timeout=vk.APPLE_MUSTPASS_PROGRESS_TIMEOUT,
            device_exclusions=vk.DEVICE_EXCLUSIONS,
        )

    def guest_utilities(self, root: Path) -> dict[str, str]:
        """Stand in for the guest's coreutils, with a gzip that can fail.

        The guest runs GNU coreutils; this host does not have ``base64 -w`` or
        ``sha256sum``. The gzip shim refuses to compress while
        ``FULL_FILESYSTEM`` is set and writes nothing, which is what a tmpfs
        with no room left made the real one do.
        """

        binaries = root / "bin"
        binaries.mkdir()
        (binaries / "base64").write_text(
            f"#!{sys.executable}\n"
            "import base64, sys\n"
            "a = sys.argv[1:]\n"
            "w = 76\n"
            "if a and a[0] == '-w':\n"
            "    w = int(a[1]); a = a[2:]\n"
            "d = open(a[0], 'rb').read() if a else sys.stdin.buffer.read()\n"
            "t = base64.b64encode(d).decode()\n"
            "for i in range(0, len(t), w):\n"
            "    print(t[i:i + w])\n"
        )
        (binaries / "sha256sum").write_text(
            f"#!{sys.executable}\n"
            "import hashlib, sys\n"
            "d = open(sys.argv[1], 'rb').read()\n"
            "print(hashlib.sha256(d).hexdigest() + '  ' + sys.argv[1])\n"
        )
        (binaries / "gzip").write_text(
            "#!/bin/sh\n"
            'if [ -n "${FULL_FILESYSTEM:-}" ] && [ "${1:-}" != "-dc" ]; then\n'
            "    echo 'gzip: stdout: No space left on device' >&2\n"
            "    exit 1\n"
            "fi\n"
            'exec /usr/bin/gzip "$@"\n'
        )
        for name in ("base64", "sha256sum", "gzip"):
            (binaries / name).chmod(0o755)
        environment = dict(os.environ)
        environment["PATH"] = f"{binaries}:{environment['PATH']}"
        return environment

    def test_a_changed_acknowledgement_fails_its_frame_in_every_launcher(self) -> None:
        """Only the host's exact acknowledgement lets a frame count as sent.

        Each launcher's emit_frame writes the frame to descriptor 3 and reads
        the reply back from it. A regular file holding the frame and then a
        reply stands in for the channel: the write rewrites the first line in
        place and the read takes the second.
        """

        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        identity = vk.load_identity()
        development = vk.resolve_selection(
            identity, "smoke", (), None, vk.Fraction(0, 1)
        )
        leaf = gl.leaf_identities(gl.load_identity())["gles3"]
        launchers = (
            ("UML_VK_CTS_ERROR:", vk.build_guest_launcher(
                identity, development, timeout=900,
                case_list=vk.GUEST_CASE_LIST_PATH,
            )),
            ("UML_VK_CTS_ERROR:", self.device_launcher()),
            ("UML_CTS_ERROR:", gl.build_guest_launcher(
                gl.Selection(leaf, (), 1, 8), 7, 300,
                gl_fixtures.erofs_contract(), artifact_transport="virtio-serial",
            )),
        )
        frame = "UML_CTS_ARTIFACT_END|results.qpa|1"
        expected = "UML_CTS_ARTIFACT_ACK|results.qpa|end"
        for index, (prefix, launcher) in enumerate(launchers):
            for reply in (expected, "UML_CTS_ARTIFACT_ACK|results.qpa|0"):
                with self.subTest(launcher=index, reply=reply), \
                        tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    binaries = root / "bin"
                    binaries.mkdir()
                    (binaries / "timeout").write_text(
                        '#!/bin/sh\nshift 2\nexec "$@"\n'
                    )
                    (binaries / "timeout").chmod(0o755)
                    channel = root / "channel"
                    channel.write_text(f"{frame}\n{reply}\n")
                    harness = f"""set -u
artifact_transport=virtio-serial
channel_ready=1
exec 3<> {channel}
{self.shell_function(launcher, "emit_frame")}
emit_frame '{frame}' '{expected}'
"""
                    environment = dict(os.environ)
                    environment["PATH"] = f"{binaries}:{environment['PATH']}"
                    finished = subprocess.run(
                        [shell, "-c", harness], capture_output=True, text=True,
                        env=environment, timeout=30, check=False,
                    )
                    self.assertEqual(finished.returncode, int(reply != expected))
                    self.assertEqual(
                        finished.stdout,
                        "" if reply == expected
                        else f"{prefix} artifact acknowledgement changed\n",
                    )
                    self.assertEqual(channel.read_text(), f"{frame}\n{reply}\n")

    def test_a_failed_checkpoint_keeps_its_bytes_and_frames_nothing(self) -> None:
        """The defect: a checkpoint no one could compress looked delivered.

        Neither ``gzip`` nor ``base64`` was checked, so a guest whose tmpfs
        filled framed ``|gzip+base64|0|`` with the empty string's digest, said
        it had succeeded, and advanced past 17 MB of results that never left
        the guest. The bytes must instead stay pending and go out next time.
        """

        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        launcher = self.device_launcher()
        program = "".join(
            self.shell_function(launcher, name)
            for name in (
                "artifact_discard",
                "compress_artifact",
                "emit_compressed",
                "emit_artifact",
                "flush_qpa",
            )
        )
        payload = qpa_session(CASES).encode()
        half = len(payload) // 2
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            environment = self.guest_utilities(root)
            (root / "source").write_bytes(payload)
            harness = f"""set -u
output={root}
qpa="$output/results.qpa"
qpa_offset=0
qpa_segments=0
artifact_framed=0
emit_frame()
{{
    printf '%s\\n' "$1" >> "$output/frames"
}}
{program}
head -c {half} "$output/source" > "$qpa"
flush_qpa 0 || echo 'first checkpoint failed'
FULL_FILESYSTEM=1
export FULL_FILESYSTEM
cat "$output/source" > "$qpa"
flush_qpa 0 || printf 'refused %s %s %s\\n' "$qpa_offset" "$qpa_segments" \\
    "$artifact_framed" >> "$output/state"
unset FULL_FILESYSTEM
flush_qpa 1 || echo 'final checkpoint failed'
printf 'final %s %s %s\\n' "$qpa_offset" "$qpa_segments" "$artifact_framed" \\
    >> "$output/state"
"""
            (root / "harness.sh").write_text(harness)
            finished = subprocess.run(
                [shell, str(root / "harness.sh")],
                check=True,
                capture_output=True,
                text=True,
                env=environment,
            )
            # The refusal names itself on the console the host records.
            self.assertIn(
                "artifact results.qpa.0001 could not be compressed",
                finished.stdout,
            )
            # The offset stays put, and nothing of that segment was framed, so
            # its name is still free for the attempt that carries the bytes.
            self.assertEqual(
                (root / "state").read_text().splitlines(),
                [f"refused {half} 1 0", f"final {len(payload)} 2 0"],
            )
            transcript = (root / "frames").read_text()
            # No empty frame group exists, and what did arrive is the whole
            # file: the refused range went out with the next checkpoint.
            self.assertNotIn("|gzip+base64|0|", transcript)
            self.assertEqual(
                gl.decode_artifacts(transcript), {"results.qpa": payload}
            )
            self.assertEqual(
                sorted(item.name for item in root.glob("*.gz*")), []
            )

    def shell_region(self, launcher: str, first: str, last: str) -> str:
        """The top-level statements from ``first`` up to (not into) ``last``."""

        start = launcher.index(first)
        end = launcher.index(last, start)
        return launcher[start:end]

    def test_a_session_that_ends_early_continues_the_rest_of_its_list(
        self,
    ) -> None:
        """The defect: one ResourceError cost a fraction every case after it.

        dEQP ends its whole session at the first resource error, so an iPad
        fraction that met one at case 20,725 delivered 20,725 of 216,952
        results and called that a finished session. The guest now runs the
        remainder of the same enumerated order in a fresh CTS process: no case
        is re-run, none is invented, and the host receives one ordered stream.
        """

        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        launcher = self.device_launcher()
        region = self.shell_region(
            launcher, "run_list=", "printf 'UML_VK_CTS_STATUS"
        )
        self.assertIn("--deqp-caselist-file=/run/uml-vk-cts/continue.txt", region)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binaries = root / "bin"
            binaries.mkdir()
            # dEQP is reached through runuser and timeout, which this host has
            # neither of: both shims drop their own arguments and run the rest.
            (binaries / "runuser").write_text(
                '#!/bin/sh\nshift 3\nexec "$@"\n'
            )
            (binaries / "timeout").write_text(
                '#!/bin/sh\nshift 3\nexec "$@"\n'
            )
            # The stub answers the first caselist with a session that stops at
            # its third case, exactly as a resource error ends one.
            (binaries / "deqp-vk").write_text(
                f"#!{sys.executable}\n"
                "import pathlib, sys\n"
                "options = dict(\n"
                "    argument.removeprefix('--deqp-').split('=', 1)\n"
                "    for argument in sys.argv[1:] if '=' in argument\n"
                ")\n"
                "cases = pathlib.Path(options['caselist-file']).read_text()"
                ".split()\n"
                "state = pathlib.Path(options['archive-dir']) / 'sessions'\n"
                "index = len(state.read_text().split()) if state.exists() else 0\n"
                "state.write_text('x ' * (index + 1))\n"
                "run = cases[:3] if index == 0 else cases\n"
                "log = ['#sessionInfo releaseName 1.4.6.2', '#beginSession']\n"
                "for position, case in enumerate(run):\n"
                "    status = 'ResourceError' if index == 0 and position == 2 "
                "else 'Pass'\n"
                "    log.append(f'#beginTestCaseResult {case}')\n"
                "    log.append(\n"
                "        f'<TestCaseResult CasePath=\"{case}\">'\n"
                "        f'<Result StatusCode=\"{status}\">ok</Result>'\n"
                "        '</TestCaseResult>'\n"
                "    )\n"
                "    log.append('#endTestCaseResult')\n"
                "log.append('#endSession')\n"
                "pathlib.Path(options['log-filename']).write_text(\n"
                "    '\\n'.join(log) + '\\n'\n"
                ")\n"
                "sys.exit(1 if index == 0 else 0)\n"
            )
            for name in ("runuser", "timeout", "deqp-vk"):
                (binaries / name).chmod(0o755)
            for name in ("expected.txt", "selected.txt"):
                (root / name).write_text(
                    "".join(f"{case}\n" for case in CASES)
                )
            (root / "results.qpa").write_text("")
            harness = f"""set -eu
output={root}
qpa="$output/results.qpa"
expected="$output/expected.txt"
selected="$output/selected.txt"
continuation="$output/continue.txt"
cts_output="$output/cts-output.txt"
progress_interval=1
progress_timeout=0
checkpoint_bytes=1048576
checkpoint_failures=0
progress_timed_out=0
qpa_offset=0
qpa_segments=0
status=1
cts_sessions=0
cts_cases=0
flush_qpa()
{{
    cat "$qpa" >> "$output/transported"
    qpa_offset=$(wc -c < "$qpa" | tr -d ' ')
    qpa_segments=$((qpa_segments + 1))
    printf 'flush %s\n' "$qpa_offset" >> "$output/order"
}}
find_cts_process()
{{
    cts_process=
}}
cts_process_field()
{{
    printf '0'
}}
{region}
# The exit path sends whatever the last session added, as finish() does.
flush_qpa 1
printf 'sessions=%s cases=%s status=%s\n' \
    "$cts_sessions" "$cts_cases" "$status" > "$output/verdict"
"""
            (root / "harness.sh").write_text(
                harness.replace(
                    "/opt/vk-gl-cts/vulkan/deqp-vk", str(binaries / "deqp-vk")
                ).replace("/opt/vk-gl-cts/vulkan", str(root)).replace(
                    "/run/uml-vk-cts", str(root)
                )
            )
            environment = dict(os.environ)
            environment["PATH"] = f"{binaries}:{environment['PATH']}"
            subprocess.run(
                [shell, str(root / "harness.sh")],
                check=True,
                env=environment,
                capture_output=True,
            )
            verdict = dict(
                field.split("=", 1)
                for field in (root / "verdict").read_text().split()
            )
            self.assertEqual(
                verdict, {"sessions": "2", "cases": "7", "status": "1"}
            )
            # The continuation runs the traversal's remainder and nothing else.
            self.assertEqual(
                (root / "continue.txt").read_text().split(), list(CASES[3:])
            )
            results = gl.scan_qpa((root / "transported").read_text())
            self.assertTrue(results.ended_session)
            self.assertEqual(list(results.results), list(CASES))
            self.assertEqual(results.results[CASES[2]], "ResourceError")
            self.assertEqual(
                [status for case, status in results.results.items()
                 if case != CASES[2]],
                ["Pass"] * (len(CASES) - 1),
            )

    def test_the_summary_is_sent_last_even_after_a_failed_segment(self) -> None:
        """A fraction that lost results still reports its own verdict.

        Every host lane waits for the ``summary.txt`` trailer, and the exit
        path used to skip it whenever the final QPA segment failed -- and to
        abort outright, under errexit, if the first summary write failed on a
        full filesystem. One iPad fraction ran all 199,818 of its cases and
        delivered no verdict at all that way.
        """

        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        finish = self.shell_function(self.device_launcher(), "finish")
        # A segment that failed before framing anything is retried once, after
        # the never-transported CTS output is released; one that failed with
        # frames already sent is not, because that name cannot be opened twice.
        for framed, flushes in ((0, ["flush", "flush"]), (1, ["flush"])):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "selected.txt").write_text(
                    "".join(f"{case}\n" for case in CASES)
                )
                (root / "skipped.txt").write_text("guest-memory\tdEQP-VK.x\n")
                (root / "results.qpa").write_bytes(b"x" * 4096)
                harness = f"""set -eu
output={root}
qpa="$output/results.qpa"
selected="$output/selected.txt"
skipped="$output/skipped.txt"
skipped_guest_memory=1
summary="$output/summary.txt"
cts_output="$output/cts-output.txt"
channel_ready=1
qpa_offset=1024
qpa_segments=1
artifact_framed={framed}
status=1
prepare_channel()
{{
    return 0
}}
emit_prerun()
{{
    printf 'prerun\\n' >> "$output/order"
}}
flush_qpa()
{{
    printf 'flush\\n' >> "$output/order"
    return 1
}}
emit_artifact()
{{
    printf 'artifact %s\\n' "$1" >> "$output/order"
    cp "$2" "$output/sent-$1"
}}
# The guest's sync. The host's would wait on every filesystem the host has
# mounted, a stalled network volume included.
sync()
{{
    printf 'sync\\n' >> "$output/order"
}}
{finish}
finish
"""
                (root / "harness.sh").write_text(harness)
                finished = subprocess.run(
                    [shell, str(root / "harness.sh")],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    finished.stdout,
                    "UML_VK_CTS_ERROR: artifact transfer failed\n",
                )
                self.assertEqual(
                    (root / "order").read_text().split(),
                    ["prerun", *flushes, "artifact", "summary.txt", "sync"],
                )
                summary = dict(
                    line.split("=", 1)
                    for line in (
                        root / "sent-summary.txt"
                    ).read_text().splitlines()
                )
                self.assertEqual(summary["qpa_transfer"], "failed")
                self.assertEqual(summary["qpa_missing_bytes"], str(4096 - 1024))
                self.assertEqual(summary["cts_rc"], "1")
                self.assertEqual(summary["device_excluded_guest_memory"], "1")

    def test_checkpoint_segments_reconstruct_the_guest_file(self) -> None:
        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        selection = vk.resolve_receipt_selection(
            "current-must-pass", [], None, vk.Fraction(0, 8)
        )
        launcher = vk.build_receipt_guest_launcher(
            receipt, selection, timeout=900, progress_timeout=600
        )
        flush = self.shell_function(launcher, "flush_qpa")
        compress = self.shell_function(launcher, "compress_artifact")
        discard = self.shell_function(launcher, "artifact_discard")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = qpa_session(CASES[:4], ended=False).encode()
            harness = f"""set -eu
output={root}
qpa="$output/results.qpa"
qpa_offset=0
qpa_segments=0
{discard}{compress}emit_compressed()
{{
    gzip -dc "$output/$1.gz" > "$output/sent-$1"
    artifact_discard "$1"
    printf '%s\\n' "$1" >> "$output/order"
}}
{flush}
head -c 40 "$output/source" > "$qpa"
flush_qpa 0
cat "$output/source" > "$qpa"
flush_qpa 0
flush_qpa 1
flush_qpa 1
printf '%s %s\\n' "$qpa_offset" "$qpa_segments" > "$output/state"
"""
            (root / "source").write_bytes(payload)
            (root / "harness.sh").write_text(harness)
            subprocess.run([shell, str(root / "harness.sh")], check=True)
            order = (root / "order").read_text().split()
            self.assertEqual(order, ["results.qpa.0000", "results.qpa.0001"])
            joined = b"".join(
                (root / f"sent-{name}").read_bytes() for name in order
            )
            self.assertEqual(joined, payload)
            self.assertEqual(
                (root / "state").read_text().split(), [str(len(payload)), "2"]
            )
            transcript = "".join(
                framed(name, (root / f"sent-{name}").read_bytes()) for name in order
            )
            self.assertEqual(
                gl.decode_artifacts(transcript), {"results.qpa": payload}
            )

    def test_empty_guest_qpa_still_transports_one_segment(self) -> None:
        shell = shutil.which("sh")
        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        selection = vk.resolve_receipt_selection(
            "smoke", [], None, vk.Fraction(0, 1)
        )
        launcher = vk.build_receipt_guest_launcher(
            receipt, selection, timeout=900,
            case_list=vk.GUEST_CASE_LIST_PATH,
        )
        flush = self.shell_function(launcher, "flush_qpa")
        compress = self.shell_function(launcher, "compress_artifact")
        discard = self.shell_function(launcher, "artifact_discard")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            harness = f"""set -eu
output={root}
qpa="$output/results.qpa"
qpa_offset=0
qpa_segments=0
{discard}{compress}emit_compressed()
{{
    gzip -dc "$output/$1.gz" > "$output/sent-$1"
    artifact_discard "$1"
    printf '%s\\n' "$1" >> "$output/order"
}}
{flush}
: > "$qpa"
flush_qpa 0
flush_qpa 1
"""
            (root / "harness.sh").write_text(harness)
            subprocess.run([shell, str(root / "harness.sh")], check=True)
            self.assertEqual(
                (root / "order").read_text().split(), ["results.qpa.0000"]
            )
            self.assertEqual((root / "sent-results.qpa.0000").read_bytes(), b"")


if __name__ == "__main__":
    unittest.main()
