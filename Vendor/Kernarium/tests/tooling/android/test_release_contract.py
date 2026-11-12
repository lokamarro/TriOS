#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import copy
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path




def fail(message: str) -> None:
    raise SystemExit(f"android release contract fixture: {message}")


def expect_failure(function, label: str) -> None:
    try:
        function()
    except SystemExit:
        return
    fail(f"release contract accepted {label}")


def read_routes(projects_path: Path, android_root: Path, provenance) -> list[dict]:
    lines = [
        line for line in projects_path.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    reader = csv.DictReader(lines, delimiter="\t")
    routes = []
    for row in reader:
        patch_count, series_digest = provenance(android_root / row["series_dir"])
        routes.append({
            "route_id": row["route_id"],
            "manifest_remote": row["manifest_remote"],
            "manifest_project": row["manifest_project"],
            "checkout_path": row["checkout_path"],
            "base_commit": row["base_commit"],
            "base_tree": row["base_tree"],
            "final_tree": row["final_tree"],
            "patch_count": patch_count,
            "series_manifest_sha256": series_digest,
        })
    return routes


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()

    root = args.source_root.resolve()
    android_root = root / "android/lineage-24"
    sys.path.insert(0, str(android_root / "tools"))
    from release_common import (  # noqa: PLC0415
        BUILD_VARIANTS,
        OTA_CERTIFICATE_BUILD_MEMBER,
        OTA_CERTIFICATE_MEMBERS,
        RELEASE_BUILD_VARIANTS,
        ota_certificate_member,
        require_release_build_variant,
        signed_ota_certificate_member,
        DIRECTORY_PREFIX,
        KERNEL_FAMILY,
        PAYLOAD_SPECS,
        PRODUCT_FAMILY,
        SCHEMA_VERSION,
        LEGACY_SCHEMA_VERSION,
        ACCEPTANCE_KEYS,
        LEGACY_ACCEPTANCE_KEYS,
        INPUT_TREES_KEY,
        acceptance_values,
        kernel_series_provenance,
        parse_env,
        render_acceptance,
        series_provenance,
        sha256_path,
        verify_release,
    )
    import release_inputs  # noqa: PLC0415

    def provenance(*directories: Path) -> tuple[int, str]:
        return series_provenance(root, *directories)

    schema = json.loads(
        (android_root / "support/release/release-manifest.schema.json").read_text()
    )
    if schema.get("properties", {}).get("product_family", {}).get("const") != PRODUCT_FAMILY:
        fail("release schema does not pin the Android product family")

    aosp = parse_env(android_root / "metadata/lineage.env")
    kernel_root = root / "kernel/mainline-6.18"
    kernel = parse_env(kernel_root / "patches/metadata.env")
    kernel_patch_count, kernel_patch_digest = kernel_series_provenance(root)
    abi = parse_env(root / "kernel/common/abi/identity.env")
    tracked_acceptance = parse_env(android_root / "metadata/accepted-release.env")
    # The accepted release may predate the recorded input trees (schema 6); the
    # embedded-artifact check then refuses it as unverifiable until a schema 7
    # release is accepted.
    tracked_schema = tracked_acceptance.get("LINEAGE24_ACCEPTED_SCHEMA")
    if tracked_schema == str(SCHEMA_VERSION):
        tracked_keys = ACCEPTANCE_KEYS
    elif tracked_schema == str(LEGACY_SCHEMA_VERSION):
        tracked_keys = LEGACY_ACCEPTANCE_KEYS
    else:
        fail(f"tracked acceptance descriptor has an unknown schema {tracked_schema!r}")
    if set(tracked_acceptance) != tracked_keys:
        fail("tracked acceptance descriptor keys differ from the release contract")
    if INPUT_TREES_KEY in tracked_acceptance:
        trees = release_inputs.parse_trees(tracked_acceptance[INPUT_TREES_KEY])
        if sorted(trees) != release_inputs.input_paths(root / release_inputs.LIST_RELATIVE):
            fail("tracked acceptance descriptor names input paths the list does not")
    tracked_expected = {
        "LINEAGE24_ACCEPTED_PRODUCT_FAMILY": PRODUCT_FAMILY,
        "LINEAGE24_ACCEPTED_ANDROID_MAJOR": "17",
        "LINEAGE24_ACCEPTED_PAGE_SIZE": "4096",
    }
    if tracked_acceptance["LINEAGE24_ACCEPTED_BUILD_VARIANT"] not in BUILD_VARIANTS:
        fail("tracked acceptance descriptor names an unsupported build variant")
    for key, value in tracked_expected.items():
        if tracked_acceptance.get(key) != value:
            fail(f"tracked acceptance descriptor differs at {key}")
    if tracked_acceptance["LINEAGE24_ACCEPTED_DIRECTORY_NAME"] != (
        DIRECTORY_PREFIX + tracked_acceptance["LINEAGE24_ACCEPTED_RELEASE_ID"]
    ):
        fail("tracked acceptance directory does not match its release ID")
    release_id = "fixture-1"
    directory_name = DIRECTORY_PREFIX + release_id

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "release_id": release_id,
        "directory_name": directory_name,
        "product_family": PRODUCT_FAMILY,
        "acceptance": "accepted",
        "published_at_utc": "2026-08-21T00:00:00Z",
        "android": {
            "major": 17,
            "sdk": 37,
            "build_id": "FIXTURE",
            "build_number": 7,
            "security_patch": "2026-10-05",
            "build_variant": aosp["LINEAGE24_BUILD_VARIANT"],
            "fingerprint": "fixture/fingerprint",
            "vendor_api_level": 202704,
            "product": aosp["LINEAGE24_PRODUCT"],
            "release_config": aosp["LINEAGE24_RELEASE_CONFIG"],
            "abi_list": ["arm64-v8a", "armeabi-v7a", "armeabi"],
        },
        "source": {
            "manifest_repository": aosp["LINEAGE24_MANIFEST_REPOSITORY"],
            "manifest_revision": aosp["LINEAGE24_MANIFEST_REVISION"],
            "manifest_commit": aosp["LINEAGE24_MANIFEST_COMMIT"],
            "manifest_tree": aosp["LINEAGE24_MANIFEST_TREE"],
            "aosp_tag": aosp["LINEAGE24_AOSP_TAG"],
            "base_manifest_sha256": aosp["LINEAGE24_BASE_MANIFEST_SHA256"],
            "projects_manifest_sha256": sha256_path(
                android_root / "patches/projects.tsv"
            ),
            "routes": read_routes(
                android_root / "patches/projects.tsv", android_root, provenance
            ),
        },
        "kernel": {
            "family": KERNEL_FAMILY,
            "repository": kernel["UML_KERNEL_UPSTREAM_REPOSITORY"],
            "branch": kernel["UML_KERNEL_UPSTREAM_BRANCH"],
            "base_commit": kernel["UML_KERNEL_UPSTREAM_COMMIT"],
            "base_tree": kernel["UML_KERNEL_UPSTREAM_TREE"],
            "final_tree": kernel["UML_KERNEL_FINAL_TREE"],
            "patch_count": kernel_patch_count,
            "patch_manifest_sha256": kernel_patch_digest,
            "release": "6.18.21-fixture",
            "page_size": 4096,
            "abi_version": int(abi["DARWIN_UML_ABI_VERSION"]),
            "abi_fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
            "elf_role": "darwin-uml",
        },
        "outer": {
            "revision": "a" * 40,
            "dirty": False,
            "input_trees": release_inputs.object_ids(
                root, release_inputs.input_paths(root / release_inputs.LIST_RELATIVE)
            ),
        },
        "producer": {
            "entry_point": "android/lineage-24/build/publish-release.sh",
            "functional_options": ["pinned AOSP source", "verified Android kernel build"],
            "builder": {
                "os": "Linux fixture",
                "architecture": "x86_64",
                "jdk": "fixture JDK",
                "clang": "fixture clang",
                "linker": "fixture linker",
                "darwin_sdk": "fixture SDK",
                "android_build_tools": "fixture build tools",
            },
        },
        "inventory": [],
    }

    with tempfile.TemporaryDirectory(prefix="android-release-contract-") as temporary:
        temporary_root = Path(temporary)
        release_root = temporary_root / directory_name
        release_root.mkdir()
        for relative, (role, file_format) in sorted(PAYLOAD_SPECS.items()):
            path = release_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x")
            manifest["inventory"].append({
                "path": relative,
                "role": role,
                "format": file_format,
                "size": 1,
            })

        manifest_path = release_root / "release-manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        acceptance = temporary_root / "accepted-release.env"
        acceptance.write_text(render_acceptance(manifest))
        rendered = parse_env(acceptance)
        if rendered != acceptance_values(manifest) or set(rendered) != ACCEPTANCE_KEYS:
            fail("the rendered descriptor does not repeat the manifest")
        if rendered["LINEAGE24_ACCEPTED_SCHEMA"] != "7" or not acceptance.read_text().startswith(
            "# SPDX-License-Identifier: GPL-2.0-or-later\n"
        ):
            fail("the rendered descriptor is not a schema 7 descriptor with its header")
        for path, tree in manifest["outer"]["input_trees"].items():
            if f"{path}={tree}" not in rendered[INPUT_TREES_KEY].split(" "):
                fail(f"the rendered descriptor omits the input tree of {path}")

        verify_release(
            release_root,
            android_root,
            acceptance_path=acceptance,
            semantic=False,
        )

        # --print-acceptance writes the descriptor and nothing else to standard
        # output: the verifiers' own "verified ..." lines, from this process and
        # from the tools it runs, go to standard error.
        print_acceptance = subprocess.run(
            [
                sys.executable,
                "-B",
                "-c",
                (
                    "import importlib.util, subprocess, sys\n"
                    "tools = sys.argv[1]\n"
                    "sys.path.insert(0, tools)\n"
                    "import release_common\n"
                    "def verified(release_root, android_root, *, acceptance_path=None, semantic=True):\n"
                    "    print('verified in process')\n"
                    "    subprocess.run(['echo', 'verified in a tool'], check=True)\n"
                    "    return {}\n"
                    "release_common.verify_release = verified\n"
                    "spec = importlib.util.spec_from_file_location('install_release', tools + '/install-release.py')\n"
                    "module = importlib.util.module_from_spec(spec)\n"
                    "spec.loader.exec_module(module)\n"
                    "sys.argv = ['install-release.py', '--source', sys.argv[2], '--print-acceptance']\n"
                    "module.main()\n"
                ),
                str(android_root / "tools"),
                str(release_root),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if print_acceptance.returncode != 0:
            fail(f"--print-acceptance failed: {print_acceptance.stderr.strip()}")
        if print_acceptance.stdout != render_acceptance(manifest):
            fail("--print-acceptance wrote more than the descriptor to standard output")
        if "verified in process" not in print_acceptance.stderr or "verified in a tool" not in print_acceptance.stderr:
            fail("--print-acceptance lost the verifiers' report")

        historical = copy.deepcopy(manifest)
        historical["source"]["projects_manifest_sha256"] = "b" * 64
        historical["kernel"]["final_tree"] = "c" * 40
        historical["kernel"]["patch_manifest_sha256"] = "d" * 64
        historical["kernel"]["abi_version"] = 7
        historical["kernel"]["abi_fingerprint"] = "0x0123456789abcdef"
        historical_manifest = temporary_root / "historical-release-manifest.json"
        historical_manifest.write_text(
            json.dumps(historical, indent=2, sort_keys=True) + "\n"
        )
        manifest_path.write_bytes(historical_manifest.read_bytes())
        historical_acceptance = temporary_root / "historical-accepted-release.env"
        historical_acceptance.write_text(
            acceptance.read_text()
            .replace(manifest["source"]["projects_manifest_sha256"], "b" * 64)
            .replace(manifest["kernel"]["final_tree"], "c" * 40)
            .replace(manifest["kernel"]["patch_manifest_sha256"], "d" * 64)
            .replace(
                f"LINEAGE24_ACCEPTED_ABI_VERSION={manifest['kernel']['abi_version']}",
                "LINEAGE24_ACCEPTED_ABI_VERSION=7",
            )
            .replace(
                manifest["kernel"]["abi_fingerprint"],
                "0x0123456789abcdef",
            )
        )
        verify_release(
            release_root,
            android_root,
            acceptance_path=historical_acceptance,
            semantic=False,
        )
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a historical release without its acceptance descriptor",
        )
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )

        # A schema 6 release predates the input trees: it stays valid as history
        # under a schema 6 descriptor, and is never the tracked tree's own release.
        legacy = copy.deepcopy(manifest)
        legacy["schema_version"] = LEGACY_SCHEMA_VERSION
        del legacy["outer"]["input_trees"]
        manifest_path.write_text(json.dumps(legacy, indent=2, sort_keys=True) + "\n")
        legacy_acceptance = temporary_root / "legacy-accepted-release.env"
        legacy_acceptance.write_text(render_acceptance(legacy))
        if set(parse_env(legacy_acceptance)) != LEGACY_ACCEPTANCE_KEYS:
            fail("a schema 6 descriptor carries input trees")
        verify_release(
            release_root, android_root, acceptance_path=legacy_acceptance, semantic=False
        )
        expect_failure(
            lambda: verify_release(
                release_root, android_root, acceptance_path=acceptance, semantic=False
            ),
            "a schema 6 release against a schema 7 descriptor",
        )
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a schema 6 release as the tracked tree's own",
        )
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        expect_failure(
            lambda: verify_release(
                release_root, android_root, acceptance_path=legacy_acceptance, semantic=False
            ),
            "a schema 7 release against a schema 6 descriptor",
        )
        for label, edit in (
            ("a schema 6 release that records input trees",
             lambda value: value.update(schema_version=LEGACY_SCHEMA_VERSION)),
            ("a schema 7 release without input trees",
             lambda value: value["outer"].pop("input_trees")),
            ("no input trees",
             lambda value: value["outer"].update(input_trees={})),
            ("a short object ID",
             lambda value: value["outer"]["input_trees"].update(
                 {"android/lineage-24/overlay": "abc"})),
            ("an input path that is not normalized",
             lambda value: value["outer"]["input_trees"].update({"../escape": "a" * 40})),
            ("an object ID in capitals",
             lambda value: value["outer"]["input_trees"].update(
                 {"android/lineage-24/overlay": "A" * 40})),
        ):
            broken = copy.deepcopy(manifest)
            edit(broken)
            manifest_path.write_text(json.dumps(broken, indent=2, sort_keys=True) + "\n")
            expect_failure(
                lambda: verify_release(
                    release_root, android_root, acceptance_path=acceptance, semantic=False
                ),
                label,
            )
        # The descriptor repeats the manifest's input trees, and a release the
        # tracked tree publishes records its own.
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        changed = copy.deepcopy(manifest)
        changed["outer"]["input_trees"]["android/lineage-24/overlay"] = "e" * 40
        stale_acceptance = temporary_root / "stale-accepted-release.env"
        stale_acceptance.write_text(render_acceptance(changed))
        expect_failure(
            lambda: verify_release(
                release_root, android_root, acceptance_path=stale_acceptance, semantic=False
            ),
            "a descriptor whose input trees differ from the manifest's",
        )
        manifest_path.write_text(json.dumps(changed, indent=2, sort_keys=True) + "\n")
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a release whose input trees differ from the tracked tree's",
        )
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        verify_release(release_root, android_root, semantic=False)

        # A release of the mainline kernel line records no kernel tag and
        # carries the mainline product family. The ACK-built releases' tag and
        # family are no longer valid.
        tagged = copy.deepcopy(manifest)
        tagged["kernel"]["tag"] = "android16-6.18-fixture"
        ack_family = copy.deepcopy(manifest)
        ack_family["product_family"] = "lineage-24-ack-6.18-4k"
        for invalid, label in ((tagged, "a kernel tag"), (ack_family, "the ACK product family")):
            manifest_path.write_text(json.dumps(invalid, indent=2, sort_keys=True) + "\n")
            expect_failure(
                lambda: verify_release(
                    release_root, android_root, acceptance_path=acceptance, semantic=False
                ),
                label,
            )
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )

        extra = release_root / "support/undeclared"
        extra.write_bytes(b"x")
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "an undeclared payload",
        )
        extra.unlink()

        extra_dir = release_root / "undeclared-directory"
        extra_dir.mkdir()
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "an undeclared directory",
        )
        extra_dir.rmdir()

        payload = release_root / "kernel/System.map"
        payload.write_bytes(b"xx")
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a changed payload size",
        )
        payload.write_bytes(b"x")

        original = copy.deepcopy(manifest)
        manifest["kernel"]["page_size"] = 16384
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a 16 KiB Android kernel",
        )
        tracked_variant = original["android"]["build_variant"]
        for variant in RELEASE_BUILD_VARIANTS:
            require_release_build_variant(variant)
        expect_failure(
            lambda: require_release_build_variant("userdebug"),
            "a diagnostics build published as a release",
        )
        # Only a user build is signed, with the release keys: its otacerts.zip member is the release
        # certificate's. An unsigned build of either variant holds the AOSP test certificate's file name,
        # and a userdebug build keeps it, as it is never re-signed.
        if set(OTA_CERTIFICATE_MEMBERS) != set(BUILD_VARIANTS):
            fail("the variants with an OTA certificate member are not the build variants")
        if "testkey.x509.pem" != OTA_CERTIFICATE_MEMBERS["userdebug"] or OTA_CERTIFICATE_BUILD_MEMBER != "testkey.x509.pem":
            fail("a userdebug build's otacerts.zip member is not the test certificate's file name")
        if signed_ota_certificate_member("user") != "keys/releasekey.x509.pem":
            fail("a user build's otacerts.zip member is not the release certificate's")
        for variant in BUILD_VARIANTS:
            if ota_certificate_member(variant, False) != "testkey.x509.pem":
                fail(f"an unsigned {variant} build does not hold the test certificate's file name")
        if ota_certificate_member("user", True) != "keys/releasekey.x509.pem":
            fail("a signed user build does not hold the release certificate's member")
        expect_failure(
            lambda: signed_ota_certificate_member("userdebug"),
            "a diagnostics build signed with the release keys",
        )
        expect_failure(
            lambda: ota_certificate_member("userdebug", True),
            "a diagnostics build checked as signed",
        )
        for variant, label in (
            (next(name for name in BUILD_VARIANTS if name != tracked_variant),
             "a build variant other than the tracked one"),
            ("userdebug", "a diagnostics build in a release manifest"),
            ("eng", "an unsupported build variant"),
        ):
            manifest = copy.deepcopy(original)
            manifest["android"]["build_variant"] = variant
            manifest_path.write_text(json.dumps(manifest))
            expect_failure(
                lambda: verify_release(release_root, android_root, semantic=False),
                label,
            )
        for key, value, label in (
            ("build_number", 0, "a zero build number"),
            ("build_number", "7", "a textual build number"),
            ("build_number", True, "a boolean build number"),
            ("build_number", 10_000_000_000, "an oversized build number"),
            ("security_patch", "2026-13-01", "an impossible security patch date"),
            ("security_patch", "20261005", "a security patch without separators"),
            ("security_patch", "", "an empty security patch"),
        ):
            manifest = copy.deepcopy(original)
            manifest["android"][key] = value
            manifest_path.write_text(json.dumps(manifest))
            expect_failure(
                lambda: verify_release(release_root, android_root, semantic=False),
                label,
            )
        for key in ("build_number", "security_patch"):
            manifest = copy.deepcopy(original)
            del manifest["android"][key]
            manifest_path.write_text(json.dumps(manifest))
            expect_failure(
                lambda: verify_release(release_root, android_root, semantic=False),
                f"a release without {key}",
            )
        # The ABI list is optional, so a release published before it was added
        # still verifies; when present it names distinct ABIs.
        manifest = copy.deepcopy(original)
        del manifest["android"]["abi_list"]
        manifest_path.write_text(json.dumps(manifest))
        verify_release(release_root, android_root, semantic=False)
        for value, label in (
            ([], "an empty ABI list"),
            ("arm64-v8a", "a textual ABI list"),
            (["arm64-v8a", "arm64-v8a"], "a repeated ABI"),
            (["arm64-v8a", 7], "a numeric ABI"),
            (["ARM64"], "an ABI with capitals"),
            (["arm64-v8a,armeabi"], "a joined ABI list"),
        ):
            manifest = copy.deepcopy(original)
            manifest["android"]["abi_list"] = value
            manifest_path.write_text(json.dumps(manifest))
            expect_failure(
                lambda: verify_release(release_root, android_root, semantic=False),
                label,
            )
        manifest_path.write_text(json.dumps(original))
        manifest = copy.deepcopy(original)
        manifest["source"]["aosp_tag"] = "android-0.0.0_r0"
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "an Android source release different from the tracked one",
        )
        manifest = copy.deepcopy(original)
        manifest["product_family"] = "wrong-family"
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "the wrong product family",
        )
        manifest = copy.deepcopy(original)
        manifest["inventory"][0]["path"] = "../escape"
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "an inventory path traversal",
        )
        manifest_path.write_text(json.dumps(original, indent=2, sort_keys=True) + "\n")

        acceptance_text = acceptance.read_text()
        acceptance.write_text(acceptance_text.replace(release_id, "wrong", 1))
        expect_failure(
            lambda: verify_release(
                release_root,
                android_root,
                acceptance_path=acceptance,
                semantic=False,
            ),
            "a mismatched acceptance descriptor",
        )

        remote_syntax = subprocess.run(
            [
                sys.executable,
                "-B",
                str(android_root / "tools/install-release.py"),
                "--source",
                "builder.invalid:/incoming/release",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if remote_syntax.returncode == 0:
            fail("local installer accepted non-filesystem source syntax")

    print("Android 17 release contract passed")


if __name__ == "__main__":
    main()
