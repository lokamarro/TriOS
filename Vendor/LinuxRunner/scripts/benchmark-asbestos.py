#!/usr/bin/env python3
"""Quiet, interleaved Asbestos comparison using identical guest binaries.

Compile tests/linuxrunner/runtime_workloads.c once with guest GCC into the
disposable exchange. Never run against a user's installed filesystem. Host
results include guest bootstrap and do not establish physical iPhone speedups.
"""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import resource
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "candidate", "rootfs", "exchange", "output"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--only", help="Comma-separated workload names")
    parser.add_argument("--baseline-memory-fusion", choices=("0", "1"), help="Set startup fusion switch for baseline only")
    parser.add_argument("--candidate-memory-fusion", choices=("0", "1"), help="Set startup fusion switch for candidate only")
    args = parser.parse_args()
    if not 3 <= args.runs <= 15:
        parser.error("Use 3–15 bounded repetitions")
    guest = "/home/mobile/Downloads/runtime_workloads"
    workloads = {
        "python": ["/usr/bin/python3", "-c", "x=0\nfor i in range(1000000): x=(x+i*i)&0xffffffff\nprint(x)"],
        "integer": [guest, "integer", "300000000"],
        "memory": [guest, "memory", "100000000"],
        "branch": [guest, "branch", "80000000"],
        "float": [guest, "float", "100000000"],
        "syscall": [guest, "syscall", "3000000"],
        "threads": [guest, "threads", "400000"],
        "gcc": ["/usr/bin/gcc", "-O2", "-pthread", "-c", "/home/mobile/Downloads/runtime_workloads.c", "-o", "/tmp/linuxrunner-runtime-benchmark.o"],
        "files": ["/usr/bin/python3", "-c", "import os\np='/tmp/linuxrunner-runtime-file-benchmark'\ndata=bytes(range(256))*4096\nfor i in range(30):\n with open(p,'wb') as f: f.write(data)\n with open(p,'rb') as f: assert f.read()==data\n os.stat(p)\nos.unlink(p)\nprint('files PASS bytes=31457280')"],
    }
    if args.only:
        names = args.only.split(",")
        if any(name not in workloads for name in names):
            parser.error("Unknown workload")
        workloads = {name: workloads[name] for name in names}
    env = dict(os.environ)
    # Diagnostic counters must not distort dispatch or syscall timing.
    for name in ("ISH_ARM64_BLOCK_STATS", "ISH_ARM64_FUSION_STATS", "ISH_SYSTRACE", "ISH_TRACE_PCS"):
        env.pop(name, None)
    env["ISH_BIND_MOUNTS"] = "/home/mobile/Downloads=" + str(pathlib.Path(args.exchange).resolve())
    report = {
        "environment": platform.platform(), "machine": platform.machine(),
        "physical_iPhone": "NOT TESTED", "runs": args.runs,
        "method": "One warmup per binary/workload, alternating pair order, identical guest executable/checksum; wall and host child CPU include bootstrap",
        "binaries": {}, "workloads": {},
    }
    environments = {}
    for label in ("baseline", "candidate"):
        path = pathlib.Path(getattr(args, label)).resolve()
        report["binaries"][label] = {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        environments[label] = dict(env)
        switch = getattr(args, label + "_memory_fusion")
        if switch is not None:
            environments[label]["ISH_ARM64_MEMORY_PC_FUSION"] = switch
        report["binaries"][label]["memory_pc_fusion_env"] = environments[label].get("ISH_ARM64_MEMORY_PC_FUSION", "unset (binary default)")
    fixture_binary = pathlib.Path(args.exchange) / "runtime_workloads"
    report["guest_binary_sha256"] = hashlib.sha256(fixture_binary.read_bytes()).hexdigest()
    for name, command in workloads.items():
        data = {label: {"wall": [], "cpu": [], "stdout": []} for label in report["binaries"]}
        # Untimed warmups remove first-use file-cache effects from the comparison.
        for label in data:
            subprocess.run([report["binaries"][label]["path"], "-f", args.rootfs, *command],
                           env=environments[label], capture_output=True, timeout=90, check=True)
        for run in range(args.runs):
            order = ("baseline", "candidate") if run % 2 == 0 else ("candidate", "baseline")
            for label in order:
                before = resource.getrusage(resource.RUSAGE_CHILDREN)
                start = time.perf_counter()
                result = subprocess.run([report["binaries"][label]["path"], "-f", args.rootfs, *command],
                                        env=environments[label], capture_output=True, text=True, timeout=90)
                wall = time.perf_counter() - start
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                if result.returncode:
                    raise RuntimeError(f"{name}/{label}: exit={result.returncode}, {result.stderr[-2000:]}")
                data[label]["wall"].append(wall)
                data[label]["cpu"].append(after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime)
                data[label]["stdout"].append(result.stdout.strip())
        if len(set(data["baseline"]["stdout"] + data["candidate"]["stdout"])) != 1:
            raise RuntimeError(f"{name}: output mismatch")
        for values in data.values():
            values["median_wall"] = statistics.median(values["wall"])
            values["median_cpu"] = statistics.median(values["cpu"])
            values["wall_range"] = [min(values["wall"]), max(values["wall"])]
        data["wall_reduction_percent"] = 100 * (1 - data["candidate"]["median_wall"] / data["baseline"]["median_wall"])
        report["workloads"][name] = data
        output = pathlib.Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + "\n")
        print(name, {label: round(data[label]["median_wall"], 4) for label in ("baseline", "candidate")},
              f"reduction={data['wall_reduction_percent']:.2f}%", flush=True)


if __name__ == "__main__":
    main()
