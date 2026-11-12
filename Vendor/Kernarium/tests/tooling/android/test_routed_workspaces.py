#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the AOSP source verifier against a routed Repo workspace.

A fake `repo` on PATH answers the two commands the verifier uses: `manifest
-r`, which reports every listed project at its checked-out HEAD, and `forall
-c`, which runs a shell command in each project with REPO_PATH set. The
workspace has one routed project with a generated series, unrouted projects
at their pinned revisions, and the synchronized device tree.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from source_fixture import commit, parse_env, run


FAKE_REPO = '''\
import os
import subprocess
import sys
from pathlib import Path

here = Path.cwd().resolve()
workspace = next((d for d in (here, *here.parents) if (d / ".repo").is_dir()), None)
if workspace is None:
    sys.exit("fake repo: not inside a Repo workspace")
projects = [
    line.split("\\t")
    for line in (workspace / ".repo/fixture-projects.tsv").read_text().splitlines()
    if line
]
if sys.argv[1:] == ["manifest", "-r"]:
    print("<manifest>")
    for name, path, upstream, remote in projects:
        revision = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=workspace / path,
            check=True, stdout=subprocess.PIPE, text=True,
        ).stdout.strip()
        print(f'  <project name="{name}" path="{path}" remote="{remote}" '
              f'revision="{revision}" upstream="{upstream}"/>')
    print("</manifest>")
elif len(sys.argv) == 4 and sys.argv[1:3] == ["forall", "-c"]:
    for name, path, _, _ in projects:
        environment = dict(os.environ, REPO_PATH=path, REPO_PROJECT=name)
        status = subprocess.run(
            ["sh", "-c", sys.argv[3]], cwd=workspace / path, env=environment
        ).returncode
        if status:
            sys.exit(status)
else:
    sys.exit(f"fake repo: unsupported arguments {sys.argv[1:]}")
'''


def fail(message: str) -> None:
    raise SystemExit(f"routed workspace fixture: {message}")


def project(workspace: Path, path: str, timestamp: int) -> tuple[str, str]:
    """Create a checkout with one base commit; return its commit and tree."""
    checkout = workspace / path
    checkout.mkdir(parents=True)
    run("git", "init", "-q", str(checkout))
    commit(checkout, "base.txt", f"{path} base\n", f"{path}: base", timestamp)
    return (
        run("git", "rev-parse", "HEAD", cwd=checkout),
        run("git", "rev-parse", "HEAD^{tree}", cwd=checkout),
    )


def topic(checkout: Path, timestamp: int) -> tuple[str, str]:
    """Add one topic commit; return the topic commit and its tree."""
    commit(checkout, "topic.txt", "topic\n", "fixture: topic", timestamp)
    return (
        run("git", "rev-parse", "HEAD", cwd=checkout),
        run("git", "rev-parse", "HEAD^{tree}", cwd=checkout),
    )


def export(checkout: Path, base: str, series: Path, license_id: str) -> None:
    import patch_series  # noqa: PLC0415

    series.mkdir(parents=True)
    patch_series.export_series(checkout, base, "HEAD", series, license_id=license_id)


def write_projects(
    workspace: Path, rows: list[tuple[str, str, str]], remote: str
) -> None:
    """Record the projects the fake Repo reports, all served by one remote."""
    (workspace / ".repo/fixture-projects.tsv").write_text(
        "".join(
            f"{name}\t{path}\t{upstream}\t{remote}\n" for name, path, upstream in rows
        )
    )


def write_manifest(
    path: Path, rows: list[tuple[str, str, str, str]], remote: str, explicit: bool
) -> str:
    """Write a base manifest whose projects come from `remote`.

    The default remote serves a project that names none, as in the real
    snapshots; `explicit` makes each project name its remote instead.
    """
    attribute = f' remote="{remote}"' if explicit else ""
    text = (
        f'<manifest>\n  <default remote="{remote}"/>\n'
        + "".join(
            f'  <project name="{name}" path="{checkout}"{attribute} '
            f'revision="{revision}" upstream="{upstream}"/>\n'
            for name, checkout, revision, upstream in rows
        )
        + "</manifest>\n"
    )
    path.write_text(text)
    return hashlib.sha256(text.encode()).hexdigest()


def copy_device_tree(source: Path, workspace: Path) -> None:
    destination = workspace / "device/kernarium/andarium"
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source, destination, copy_function=shutil.copy2)


class Verifier:
    def __init__(self, tool: Path, android_root: Path, source_option: str,
                 workspace: Path, environment: dict[str, str]) -> None:
        self.tool = tool
        self.android_root = android_root
        self.source_option = source_option
        self.workspace = workspace
        self.environment = environment

    def run(self, expect: str, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            (
                sys.executable, "-B", str(self.tool),
                "--android-root", str(self.android_root),
                self.source_option, str(self.workspace),
                "--expect", expect, *extra,
            ),
            env=self.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    def accepts(self, case: str, expect: str, *extra: str) -> None:
        result = self.run(expect, *extra)
        if result.returncode != 0:
            fail(f"{self.tool.name} refused {case}: {result.stdout.strip()}")

    def refuses(self, case: str, expected: str, expect: str, *extra: str) -> None:
        result = self.run(expect, *extra)
        if result.returncode == 0 or expected not in result.stdout:
            fail(
                f"{self.tool.name} did not refuse {case} with '{expected}': "
                f"{result.stdout.strip()}"
            )


def detach(checkout: Path, revision: str) -> None:
    run("git", "checkout", "-q", "--detach", revision, cwd=checkout)


def check_route_states(verifier: Verifier, routes: list[tuple[Path, str, str]],
                       label: str, *final_extra: str) -> None:
    """The base/final rules, the dirty check and the ancestry check.

    Each route is (checkout, base commit, final commit); the first one is
    the route the negative cases change.
    """
    route, base, final = routes[0]
    for checkout, route_base, _ in routes:
        detach(checkout, route_base)
    verifier.accepts(f"the {label} routes at their bases", "base")
    verifier.refuses(
        f"the {label} routes at their bases for --expect final",
        "p0001", "final", *final_extra,
    )
    for checkout, _, route_final in routes:
        detach(checkout, route_final)
    verifier.accepts(f"the {label} routes at their final trees", "final", *final_extra)
    verifier.refuses(
        f"the {label} routes past their bases for --expect base",
        "route p0001 is not at its exact base", "base",
    )

    (route / "untracked.txt").write_text("dirty\n")
    verifier.refuses(f"a dirty {label} project", "build/make", "final", *final_extra)
    (route / "untracked.txt").unlink()

    # The same final tree on a history that does not contain the base.
    orphan = run(
        "git", "commit-tree", f"{final}^{{tree}}", "-m", "fixture: orphan", cwd=route
    )
    detach(route, orphan)
    verifier.refuses(
        f"a {label} route that is not based on its pin",
        "route p0001 is not based on its pinned base", "final", *final_extra,
    )
    detach(route, final)
    verifier.accepts(f"the restored {label} routes", "final", *final_extra)


def check_route_checkout(verifier: Verifier, root: Path, workspace: Path,
                         projects: list[tuple[str, str, str]], remote: str,
                         label: str) -> None:
    """The first route keeps its manifest name and remote, inside the workspace."""
    name, path, upstream = projects[0]
    write_projects(workspace, [(f"{name}-renamed", path, upstream), *projects[1:]], remote)
    verifier.refuses(
        f"a {label} route whose current manifest names another project",
        "route p0001 maps to the wrong manifest project", "final",
    )
    write_projects(workspace, projects, f"{remote}-moved")
    verifier.refuses(
        f"a {label} route whose current manifest names another remote",
        "route p0001 maps to the wrong manifest project", "final",
    )
    write_projects(workspace, projects, remote)

    route = workspace / path
    outside = root / f"outside-{label}"
    route.rename(outside)
    route.symlink_to(outside)
    verifier.refuses(
        f"a {label} route checkout outside the workspace",
        f"route p0001 checkout escapes the {label} source", "final",
    )
    route.unlink()
    outside.rename(route)


def check_aosp(source_root: Path, root: Path, environment: dict[str, str]) -> None:
    android_root = root / "aosp-policy"
    workspace = root / "aosp"
    manifests = workspace / ".repo/manifests"
    manifests.mkdir(parents=True)
    run("git", "init", "-q", str(manifests))
    commit(manifests, "default.xml", "<manifest/>\n", "manifest", 1700000000)
    route_base, route_base_tree = project(workspace, "build/make", 1700000000)
    pinned, _ = project(workspace, "external/zstd", 1700000010)
    route = workspace / "build/make"
    route_final, route_final_tree = topic(route, 1700000020)
    projects = [
        ("platform/build", "build/make", "refs/tags/fixture"),
        ("platform/external/zstd", "external/zstd", "refs/tags/fixture"),
    ]
    write_projects(workspace, projects, "aosp")

    (android_root / "metadata").mkdir(parents=True)
    (android_root / "tools").mkdir()
    shutil.copy2(
        source_root / "android/lineage-24/tools/sync-device-tree.py",
        android_root / "tools/sync-device-tree.py",
    )
    overlay = android_root / "overlay"
    overlay.mkdir()
    (overlay / "device.mk").write_text("# fixture overlay\n")
    copy_device_tree(overlay, workspace)
    digest = write_manifest(android_root / "metadata/base-manifest.xml", [
        ("platform/build", "build/make", route_base, "refs/tags/fixture"),
        ("platform/external/zstd", "external/zstd", pinned, "refs/tags/fixture"),
    ], "aosp", explicit=False)
    (android_root / "metadata/lineage.env").write_text(
        "AOSP_SOURCE_SCHEMA=1\n"
        f"LINEAGE24_BASE_MANIFEST_SHA256='{digest}'\n"
    )
    export(route, route_base, android_root / "patches/projects/p0001/series", "Apache-2.0")
    (android_root / "patches/projects.tsv").write_text(
        "# SPDX-License-Identifier: Apache-2.0\n"
        "order\troute_id\tmanifest_remote\tmanifest_project\tcheckout_path\t"
        "manifest_revision\tbase_commit\tbase_tree\tfinal_tree\tseries_dir\n"
        f"1\tp0001\taosp\tplatform/build\tbuild/make\trefs/tags/fixture\t"
        f"{route_base}\t{route_base_tree}\t{route_final_tree}\t"
        "patches/projects/p0001/series\n"
    )

    verifier = Verifier(
        source_root / "android/lineage-24/tools/verify-aosp-patches.py",
        android_root, "--aosp-source", workspace, environment,
    )
    check_route_states(verifier, [(route, route_base, route_final)], "AOSP")

    commit(workspace / "external/zstd", "moved.txt", "moved\n", "moved", 1700000030)
    verifier.refuses(
        "a moved unrouted AOSP project",
        "unrouted AOSP project moved from its pinned revision: external/zstd",
        "final",
    )
    detach(workspace / "external/zstd", pinned)

    project(workspace, "external/extra", 1700000040)
    write_projects(
        workspace, [*projects, ("platform/external/extra", "external/extra", "x")], "aosp"
    )
    verifier.refuses(
        "an AOSP project the base manifest lacks",
        "Repo project paths differ", "final",
    )
    write_projects(workspace, projects, "aosp")

    (workspace / "device/kernarium/andarium/device.mk").unlink()
    verifier.refuses(
        "an unsynchronized AOSP device overlay", "destination inventory differs", "final"
    )
    copy_device_tree(overlay, workspace)

    check_route_checkout(verifier, root, workspace, projects, "aosp", "AOSP")
    verifier.accepts("the restored AOSP workspace", "final")

    table = android_root / "patches/projects.tsv"
    good = table.read_text()
    for case, expected, text in (
        ("a route table with another header", "table header is invalid",
         good.replace("route_id", "route")),
        ("a route whose series is not at its route directory",
         "route p0001 series must be isolated at patches/projects/p0001/series",
         good.replace("patches/projects/p0001/series\n", "patches/projects/p0002/series\n")),
    ):
        table.write_text(text)
        verifier.refuses(case, expected, "final")
    table.write_text(good)
    verifier.accepts("the restored route table", "final")
    check_snapshot(source_root, android_root, root, workspace, environment)


def check_snapshot(source_root: Path, android_root: Path, root: Path,
                   workspace: Path, environment: dict[str, str]) -> None:
    """Recording a workspace rewrites the snapshot and the digests that pin it."""
    copy = root / "snapshot-policy"
    shutil.copytree(android_root, copy)
    metadata = copy / "metadata/lineage.env"
    metadata.write_text(
        metadata.read_text()
        + "LINEAGE24_MANIFEST_COMMIT='" + "0" * 40 + "'\n"
        + "LINEAGE24_MANIFEST_TREE='" + "0" * 40 + "'\n"
    )
    tool = source_root / "android/lineage-24/tools/snapshot-source.py"
    command = (
        sys.executable, "-B", str(tool), "--android-root", str(copy),
        "--aosp-source", str(workspace),
    )
    done = subprocess.run(
        command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True,
    )
    if done.returncode != 0:
        fail(f"snapshot-source.py refused a clean workspace: {done.stdout.strip()}")
    manifests = workspace / ".repo/manifests"
    values = parse_env(metadata)
    snapshot = copy / "metadata/base-manifest.xml"
    if values["LINEAGE24_BASE_MANIFEST_SHA256"] != hashlib.sha256(
        snapshot.read_bytes()
    ).hexdigest():
        fail("snapshot-source.py left a stale snapshot digest")
    for key, revision in (
        ("LINEAGE24_MANIFEST_COMMIT", "HEAD"), ("LINEAGE24_MANIFEST_TREE", "HEAD^{tree}")
    ):
        if values[key] != run("git", "rev-parse", revision, cwd=manifests):
            fail(f"snapshot-source.py recorded the wrong {key}")
    if 'path="build/make"' not in snapshot.read_text():
        fail("snapshot-source.py did not record the workspace projects")

    (workspace / "build/make/untracked.txt").write_text("dirty\n")
    done = subprocess.run(
        command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True,
    )
    (workspace / "build/make/untracked.txt").unlink()
    if done.returncode == 0 or "build/make" not in done.stdout:
        fail("snapshot-source.py recorded a dirty workspace")


def tree_entries(root: Path) -> dict[str, tuple[bytes | None, int]]:
    """Every entry below `root` with its bytes (None for a directory) and mode."""
    return {
        str(path.relative_to(root)): (
            path.read_bytes() if path.is_file() else None, path.stat().st_mode & 0o777
        )
        for path in sorted(root.rglob("*"))
    }


def check_sync_times(sync, overlay: Path, destination: Path) -> None:
    """A synchronized file is new to the build when it changed, and keeps its time when not.

    Make treats an output newer than its input as up to date, and a checkout keeps the time
    of the commit that wrote a file: content that switches back to an older checkout would
    otherwise sit behind the installed copy of the newer content.
    """
    long_ago = 1_000_000_000_000_000_000  # nanoseconds, September 2001
    marker = 1_700_000_000_000_000_000  # November 2023, newer than long_ago
    started = time.time_ns() - 5_000_000_000
    fstab = destination / "config/fstab"
    device = destination / "device.mk"

    def stamp(path: Path, nanoseconds: int) -> None:
        os.utime(path, ns=(nanoseconds, nanoseconds))

    def modified(path: Path) -> int:
        return path.stat().st_mtime_ns

    # A first copy is all new, whatever the overlay's own times are.
    for path in (overlay / "device.mk", overlay / "config/fstab"):
        stamp(path, long_ago)
    shutil.rmtree(destination)
    sync("did not copy for the times", "synchronized exact device overlay")
    for path in (fstab, device):
        if modified(path) < started:
            fail(f"sync-device-tree.py gave a new {path.name} the overlay's old time")

    # A file that did not change keeps the time the build saw, even when the checkout touched
    # the overlay's copy.
    stamp(fstab, marker)
    stamp(device, marker)
    stamp(overlay / "device.mk", started + 1_000_000_000)
    sync("did not copy an untouched tree", "synchronized exact device overlay")
    for path in (fstab, device):
        if modified(path) != marker:
            fail(f"sync-device-tree.py changed the time of the unchanged {path.name}")

    # Content that switches to an older checkout is newer than the installed copy.
    (overlay / "config/fstab").write_text("older checkout\n")
    stamp(overlay / "config/fstab", long_ago)
    sync("did not copy changed content", "synchronized exact device overlay")
    if fstab.read_text() != "older checkout\n":
        fail("sync-device-tree.py did not copy a changed file")
    if modified(fstab) < started:
        fail("sync-device-tree.py left a changed file older than the copy it replaced")
    if modified(device) != marker:
        fail("sync-device-tree.py touched a file next to a changed one")

    # A mode change alone is a change.
    stamp(fstab, marker)
    (overlay / "config/fstab").chmod(0o600)
    sync("did not copy a mode change", "synchronized exact device overlay")
    if modified(fstab) < started:
        fail("sync-device-tree.py kept the time of a file whose mode changed")
    (overlay / "config/fstab").chmod(0o640)
    sync("did not restore the mode", "synchronized exact device overlay")


def check_device_sync(source_root: Path, root: Path) -> None:
    """The sync tool replaces the workspace's device tree with the overlay's."""
    tool = source_root / "android/lineage-24/tools/sync-device-tree.py"
    android_root = root / "sync-policy"
    workspace = root / "sync-aosp"
    (workspace / ".repo/manifests").mkdir(parents=True)
    overlay = android_root / "overlay"
    (overlay / "config").mkdir(parents=True)
    (overlay / "device.mk").write_text("# fixture overlay\n")
    (overlay / "config/fstab").write_text("fixture\n")
    (overlay / "config/fstab").chmod(0o640)
    destination = workspace / "device/kernarium/andarium"

    def sync(case: str, expected: str, *extra: str, accepted: bool = True,
             target: Path = workspace) -> None:
        result = subprocess.run(
            (
                sys.executable, "-B", str(tool), "--android-root", str(android_root),
                "--workspace", str(target), *extra,
            ),
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True,
        )
        if (result.returncode == 0) != accepted or expected not in result.stdout:
            fail(f"{tool.name} {case}: {result.returncode} {result.stdout.strip()}")

    retired = workspace / "device/kernarium/darwin"
    retired.mkdir(parents=True)
    (retired / "AndroidProducts.mk").write_text("# the retired product\n")
    sync("accepted the retired device directory", "retired device directory is still present",
         "--verify", accepted=False)
    sync("did not create the device tree", "synchronized exact device overlay")
    if retired.exists():
        fail("sync-device-tree.py kept the retired device directory")
    if tree_entries(destination) != tree_entries(overlay):
        fail("sync-device-tree.py did not copy the overlay exactly")
    sync("rejected the synchronized tree", "verified exact device overlay", "--verify")

    check_sync_times(sync, overlay, destination)

    (destination / "stale.mk").write_text("stale\n")
    (destination / "device.mk").write_text("# edited\n")
    sync("accepted a stale device tree", "destination inventory differs",
         "--verify", accepted=False)
    (destination / "stale.mk").unlink()
    sync("accepted an edited file", "destination entry differs: device.mk",
         "--verify", accepted=False)
    sync("did not replace the edited tree", "synchronized exact device overlay")
    if tree_entries(destination) != tree_entries(overlay):
        fail("sync-device-tree.py kept a stale entry")

    shutil.rmtree(destination)
    destination.symlink_to(overlay, target_is_directory=True)
    sync("replaced a linked destination", "destination is not a regular directory",
         accepted=False)
    destination.unlink()
    (overlay / "link.mk").symlink_to("device.mk")
    sync("copied a linked overlay entry", "non-regular device entry is forbidden: link.mk",
         accepted=False)
    (overlay / "link.mk").unlink()
    sync("accepted a workspace without Repo metadata", "AOSP source lacks Repo metadata",
         accepted=False, target=root)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    sys.path.insert(0, str(source_root / "scripts"))

    with tempfile.TemporaryDirectory(prefix="routed-workspaces-") as temporary:
        root = Path(temporary).resolve()
        tools = root / "bin"
        tools.mkdir()
        (tools / "repo").write_text(f"#!{sys.executable}\n{FAKE_REPO}")
        (tools / "repo").chmod(0o755)
        environment = dict(os.environ, PATH=f"{tools}{os.pathsep}{os.environ['PATH']}")
        check_aosp(source_root, root, environment)
        check_device_sync(source_root, root)

    print("routed AOSP workspace verifier passed")


if __name__ == "__main__":
    main()
