#!/usr/bin/env python3
"""Bounded, interleaved Firefox comparison in a disposable Mac guest fixture.

Uses the production GUI-session harness and the same RFB/OCR probe for both
engines. Includes X11 startup, local JavaScript, display transport and OCR;
these results are neither pure browser CPU time nor physical-iPhone results.
The fixture and its benchmark-only Firefox profile must already be prepared.
"""
import argparse
import hashlib
import json
import os
import pathlib
import resource
import statistics
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "candidate", "rootfs", "exchange", "probe", "script", "output"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--geometry", default="960x540", choices=("640x480", "960x540"))
    parser.add_argument("--fresh-profile", action="store_true",
                        help="First launch in a new private profile per run; OS filesystem caches are not flushed")
    parser.add_argument("--candidate-shared-images", choices=("off", "sysv"), default="off",
                        help="Compare experimental X11 shared-image capture; baseline always off")
    args = parser.parse_args()
    if not 3 <= args.runs <= 7:
        parser.error("Use 3–7 bounded repetitions")
    repo = pathlib.Path(__file__).resolve().parents[1]
    output = pathlib.Path(args.output).resolve()
    if output.exists():
        parser.error("Choose a new output directory; previous results are preserved")
    output.mkdir(parents=True)
    env = dict(os.environ, LR_FIXTURE_GLX="software")
    for key in ("ISH_TRACE_PCS", "ISH_SYSTRACE", "ISH_ARM64_BLOCK_STATS", "ISH_ARM64_FUSION_STATS"):
        env.pop(key, None)
    template = pathlib.Path(args.script).read_text()
    if args.fresh_profile and "__LR_FIREFOX_PROFILE__" not in template:
        parser.error("Fresh-profile mode requires the explicit profile placeholder in the guest script")
    report = {"environment": "Mac ARM64 interpreter fixture; physical iPhone NOT TESTED",
              "host_ocr_device": "CPU" if env.get("LR_PROBE_OCR_CPU") else "Vision default",
              "geometry": args.geometry, "runs": args.runs,
              "candidate_shared_images": args.candidate_shared_images,
              "scope": "warm benchmark profile; X11/RFB startup included; probe timing_method specifies OCR treatment; not pure Firefox startup",
              "hashes": {}, "results": [], "summary": {}}
    if args.fresh_profile:
        report["scope"] = "first launch in a fresh private profile per run; OS cache not flushed; X11/RFB included; see probe timing_method"
    for key in ("baseline", "candidate", "probe", "script"):
        path = pathlib.Path(getattr(args, key)).resolve()
        report["hashes"][key] = {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    page = pathlib.Path(args.exchange) / "firefox_performance.html"
    report["hashes"]["html"] = hashlib.sha256(page.read_bytes()).hexdigest()
    support = repo / "app/LinuxRunner/GUISupport.bundle"
    report["support_source_sha256"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                        for path in support.iterdir() if path.is_file()}
    def verify_inputs():
        for key in ("baseline", "candidate", "probe", "script"):
            identity = report["hashes"][key]
            if hashlib.sha256(pathlib.Path(identity["path"]).read_bytes()).hexdigest() != identity["sha256"]:
                raise SystemExit("BENCHMARK_INPUT_CHANGED: " + key + "; results are not comparable")
        if hashlib.sha256(page.read_bytes()).hexdigest() != report["hashes"]["html"]:
            raise SystemExit("BENCHMARK_INPUT_CHANGED: benchmark page")
        if {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in support.iterdir() if path.is_file()} != report["support_source_sha256"]:
            raise SystemExit("BENCHMARK_INPUT_CHANGED: production GUI helpers")
    for index in range(args.runs):
        order = ("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")
        for label in order:
            verify_inputs()
            run = output / f"{index + 1}-{label}"
            script = args.script
            guest_profile = None
            if args.fresh_profile:
                guest_profile = f"/tmp/linuxrunner-fx-fresh-{uuid.uuid4().hex}"
                script = output / f"{index + 1}-{label}-command.sh"
                # Generated fixed-prefix hexadecimal paths only. Never clear
                # an existing guest profile or change the owner's profiles.
                script.write_text(f"test ! -e {guest_profile} || exit 2\nmkdir {guest_profile}\n" +
                                  template.replace("__LR_FIREFOX_PROFILE__", guest_profile))
            cmd = ["python3", str(repo / "tests/linuxrunner/gui/run_guest_session.py"),
                   "--runtime", report["hashes"][label]["path"], "--probe", args.probe,
                   "--rootfs", args.rootfs, "--exchange", args.exchange, "--script", str(script),
                   "--mode", "custom", "--geometry", args.geometry, "--output", str(run), "--timeout", "180",
                   "--shared-images", args.candidate_shared_images if label == "candidate" else "off"]
            cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
            with (output / f"{index + 1}-{label}.log").open("w") as log:
                result = subprocess.run(cmd, cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=220)
            cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
            verify_inputs()
            path = run / "firefox-performance.json"
            data = json.loads(path.read_text()) if path.exists() else {}
            data.update(label=label, run=index + 1, exit=result.returncode,
                        host_child_user_cpu_seconds=cpu_after.ru_utime-cpu_before.ru_utime,
                        host_child_system_cpu_seconds=cpu_after.ru_stime-cpu_before.ru_stime,
                        cpu_scope="whole host fixture and native OCR probe; not Firefox-only CPU or iPhone energy")
            if guest_profile:
                data.update(guest_profile=guest_profile, profile_state="fresh",
                            guest_script_sha256=hashlib.sha256(pathlib.Path(script).read_bytes()).hexdigest(),
                            scope=report["scope"])
            report["results"].append(data)
            (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
            print(label, index + 1, "exit", result.returncode,
                  "ready", data.get("startup_to_local_js_frame_seconds"),
                  "scroll", data.get("scroll_to_visible_frame_seconds"), flush=True)
            if result.returncode:
                raise SystemExit("Failed run retained; repair the fixture before comparing performance")
    for metric in ("startup_to_local_js_frame_seconds", "scroll_to_visible_frame_seconds"):
        summary = {}
        for label in ("baseline", "candidate"):
            values = [item[metric] for item in report["results"] if item["label"] == label]
            summary[label] = {"values": values, "median": statistics.median(values),
                              "range": [min(values), max(values)]}
        summary["reduction_percent"] = 100 * (1 - summary["candidate"]["median"] / summary["baseline"]["median"])
        report["summary"][metric] = summary
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
