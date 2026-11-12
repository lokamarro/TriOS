#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive the public GL46 runner scope from authenticated upstream XML.

The collection order and API predicates match getTestRunsForGL in the pinned
glcTestRunner.cpp. Text files not referenced by these XML documents do not
define executable coverage. EGL configurations remain an ordered runtime input.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import re
import shlex
from typing import Any, Callable
from xml.etree import ElementTree


ROOT = "/opt/vk-gl-cts/openglcts/gl_cts/data/mustpass/gl"
COLLECTIONS = (
    ("es-for-gl", "aosp_mustpass", "Khronos Mustpass AOSP for GL", "supported"),
    ("no-context", "khronos_mustpass_noctx", "Khronos Mustpass GL NoContext", "supported"),
    ("single", "khronos_mustpass_single", "Khronos Mustpass GL Single Config", "exact"),
    ("main", "khronos_mustpass", "Khronos Mustpass GL", "exact"),
)
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
CASE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]*\Z")
SCHEMA = 1
RUNNER_SOURCE = {
    "path": "external/openglcts/modules/runner/glcTestRunner.cpp",
    "size": 33982,
    "sha256": "85b56c755c4e615ed8bf8b9fd6ee43abf3dc03a879352a68a83c08b13f950426",
}


class PlanError(ValueError):
    """An XML, receipt, or runtime configuration does not match the contract."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PlanError(message)


def digest(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def file_identity(path: str, payload: bytes) -> dict[str, Any]:
    return {"path": path, "size": len(payload), "sha256": digest(payload)}


def command_values(arguments: list[str], collection: str) -> dict[str, Any]:
    allowed = {
        "deqp-screen-rotation", "deqp-surface-width", "deqp-surface-height",
        "deqp-base-seed", "deqp-gl-config-name", "deqp-surface-type",
        "deqp-watchdog",
    }
    values: dict[str, str] = {}
    for argument in arguments:
        match = re.fullmatch(r"--([a-z0-9-]+)=([^\s]+)", argument)
        require(match is not None, "malformed OpenGL must-pass argument")
        key, value = match.groups()
        require(key in allowed and key not in values, "unknown or duplicate OpenGL must-pass argument")
        values[key] = value
    required = allowed - {"deqp-base-seed", "deqp-gl-config-name", "deqp-surface-type"}
    require(required.issubset(values), "incomplete OpenGL must-pass command")
    require(values["deqp-screen-rotation"] == "unspecified", "unsupported OpenGL rotation")
    require(values["deqp-watchdog"] == "disable", "unsupported OpenGL watchdog policy")
    try:
        width = int(values["deqp-surface-width"])
        height = int(values["deqp-surface-height"])
        # The generated AOSP-for-GL RunParams table uses -1 when XML omits seed.
        seed = int(values.get("deqp-base-seed", "-1"))
    except ValueError as error:
        raise PlanError("invalid OpenGL must-pass numeric argument") from error
    require(width == -1 or 0 < width <= 0x7FFFFFFF, "invalid OpenGL surface width")
    require(height == -1 or 0 < height <= 0x7FFFFFFF, "invalid OpenGL surface height")
    require(0 <= seed <= 0xFFFFFFFF or (seed == -1 and collection == "es-for-gl"), "invalid OpenGL base seed")
    config = values.get("deqp-gl-config-name")
    surface = values.get("deqp-surface-type")
    require(config is None or re.fullmatch(r"[a-z0-9]+", config) is not None, "invalid OpenGL named config")
    require(surface is None or surface == "fbo", "unsupported OpenGL XML surface type")
    require(surface is None or config is not None, "OpenGL FBO has no named config")
    require(config is None or surface == "fbo" or collection == "es-for-gl", "incomplete OpenGL FBO configuration")
    # The named config is the runner's fboConfig only for a record that also
    # selects that surface; an ES-for-GL record names an AOSP GL config and
    # keeps the EGL config's own surface. uses_fbo_surface decides from the
    # record's own arguments, so this field stays the name the XML carries.
    return {"surface_width": width, "surface_height": height,
            "base_seed": seed, "fbo_config": config}


def uses_fbo_surface(configuration: dict[str, Any]) -> bool:
    """Whether a run record draws into the framework-owned framebuffer object.

    appendConfigArgs adds ``--deqp-surface-type=fbo`` only where the generated
    RunParams table carries an fboConfig, which it does for the records whose
    command line already names that surface. getTestRunsForESForGL passes a
    null fboConfig, so the ES-for-GL records draw into the surface their EGL
    config provides and observe default-framebuffer state.
    """
    return "--deqp-surface-type=fbo" in configuration["xml_arguments"]


def parse_documents(documents: Any) -> list[dict[str, Any]]:
    require(isinstance(documents, list) and len(documents) == len(COLLECTIONS), "incomplete OpenGL XML collection")
    configurations: list[dict[str, Any]] = []
    attributes = {"caseListFile", "commandLine", "name", "os", "useForFirstEGLConfig"}
    for document, (collection, directory, package_name, predicate) in zip(documents, COLLECTIONS):
        path = f"{ROOT}/{directory}/main/mustpass.xml"
        require(isinstance(document, dict) and set(document) == {"collection", "path", "size", "sha256", "text"}, "invalid OpenGL XML record")
        require(document["collection"] == collection and document["path"] == path, "OpenGL XML collection order/path changed")
        require(isinstance(document["text"], str), "invalid OpenGL XML payload")
        payload = document["text"].encode("utf-8")
        require(type(document["size"]) is int and 0 < document["size"] <= 64 * 1024, "invalid OpenGL XML size")
        require(file_identity(path, payload) == {key: document[key] for key in ("path", "size", "sha256")}, "OpenGL XML identity changed")
        require(b"<!DOCTYPE" not in payload and b"<!ENTITY" not in payload, "unsupported OpenGL XML declarations")
        try:
            root = ElementTree.fromstring(payload)
        except ElementTree.ParseError as error:
            raise PlanError("malformed OpenGL must-pass XML") from error
        require(root.tag == "Mustpass" and root.attrib == {"version": "main"}, "unsupported OpenGL XML document")
        require(len(root) == 1, "unexpected OpenGL XML package set")
        package = root[0]
        require(package.tag == "TestPackage" and package.attrib == {"name": package_name}, "unexpected OpenGL XML package")
        selected = 0
        for xml_index, element in enumerate(package):
            require(element.tag == "Configuration" and set(element.attrib) == attributes and not list(element), "invalid OpenGL XML configuration")
            require(not (element.text or "").strip(), "unexpected OpenGL configuration content")
            row = element.attrib
            require(row["os"] == "any" and row["useForFirstEGLConfig"] in {"True", "False"}, "unsupported OpenGL configuration scope")
            match = re.fullmatch(r"gl(3[0-3]|4[0-6])(-compat)?-([a-z0-9-]+)\.txt", row["caseListFile"])
            require(match is not None, "unsafe or unsupported OpenGL caselist reference")
            version, compatibility, name = match.groups()
            require(row["name"] == name, "OpenGL configuration name differs from caselist")
            try:
                arguments = shlex.split(row["commandLine"])
            except ValueError as error:
                raise PlanError("malformed OpenGL must-pass command") from error
            values = command_values(arguments, collection)
            if compatibility or (predicate == "exact" and version != "46"):
                continue
            # Every supported core version in these pinned tables is <= GL46.
            first = row["useForFirstEGLConfig"] == "True"
            require(collection == "main" or first, "one-time OpenGL collection has an other-config row")
            stem = row["caseListFile"][:-4]
            leaf_id = "gl46" if collection == "main" else f"{stem}-{collection}"
            configurations.append({
                "index": len(configurations), "collection": collection,
                "xml_index": xml_index, "leaf_id": leaf_id,
                "leaf_path": f"{ROOT}/{directory}/main/{row['caseListFile']}",
                "name": name, "use_for_first_egl_config": first,
                "xml_arguments": arguments, **values,
            })
            selected += 1
        require(selected > 0, f"OpenGL XML collection has no GL46 runs: {collection}")
    main = [row for row in configurations if row["collection"] == "main"]
    require(any(row["use_for_first_egl_config"] for row in main) and any(not row["use_for_first_egl_config"] for row in main), "OpenGL main XML lacks first/other roles")
    return configurations


def case_list(payload: bytes) -> list[str]:
    try:
        cases = payload.decode("utf-8", errors="strict").splitlines()
    except UnicodeDecodeError as error:
        raise PlanError("OpenGL caselist is not UTF-8") from error
    require(bool(cases) and all(CASE.fullmatch(case) for case in cases), "invalid OpenGL caselist")
    require(len(cases) == len(set(cases)), "OpenGL caselist repeats a case")
    require(payload == "".join(f"{case}\n" for case in cases).encode("utf-8"),
            "OpenGL caselist is not canonical newline-delimited text")
    return cases


def build_manifest(read_file: Callable[[str], bytes]) -> dict[str, Any]:
    documents = []
    for collection, directory, _, _ in COLLECTIONS:
        path = f"{ROOT}/{directory}/main/mustpass.xml"
        payload = read_file(path)
        documents.append({"collection": collection, **file_identity(path, payload),
                          "text": payload.decode("utf-8", errors="strict")})
    configurations = parse_documents(documents)
    leaves: dict[str, dict[str, Any]] = {}
    unique: set[str] = set()
    count = 0
    for row in configurations:
        if row["leaf_id"] in leaves:
            require(leaves[row["leaf_id"]]["path"] == row["leaf_path"],
                    "OpenGL leaf identifier names multiple files")
            continue
        payload = read_file(row["leaf_path"])
        cases = case_list(payload)
        leaves[row["leaf_id"]] = {"id": row["leaf_id"],
                                  **file_identity(row["leaf_path"], payload),
                                  "case_count": len(cases)}
        unique.update(cases)
        count += len(cases)
    return {"schema_version": SCHEMA, "api": "gl46",
            "runner_source": dict(RUNNER_SOURCE), "documents": documents,
            "configurations": configurations, "leaves": list(leaves.values()),
            "leaf_count": len(leaves), "case_count": count,
            "unique_case_count": len(unique),
            "duplicate_occurrence_count": count - len(unique)}


def validate_manifest(manifest: Any) -> dict[str, Any]:
    require(isinstance(manifest, dict) and set(manifest) == {
        "schema_version", "api", "runner_source", "documents", "configurations", "leaves",
        "leaf_count", "case_count", "unique_case_count", "duplicate_occurrence_count",
    }, "invalid OpenGL must-pass manifest")
    require(type(manifest["schema_version"]) is int and manifest["schema_version"] == SCHEMA and manifest["api"] == "gl46", "unsupported OpenGL must-pass manifest")
    require(manifest["runner_source"] == RUNNER_SOURCE, "OpenGL runner scope source changed")
    configurations = parse_documents(manifest["documents"])
    require(manifest["configurations"] == configurations, "OpenGL run records differ from authenticated XML")
    expected = dict((row["leaf_id"], row["leaf_path"]) for row in configurations)
    leaves = manifest["leaves"]
    require(isinstance(leaves, list) and len(leaves) == len(expected), "incomplete OpenGL leaf set")
    count = 0
    for leaf, (identifier, path) in zip(leaves, expected.items()):
        require(isinstance(leaf, dict) and set(leaf) == {"id", "path", "size", "sha256", "case_count"}, "invalid OpenGL leaf identity")
        require(leaf["id"] == identifier and leaf["path"] == path, "OpenGL leaf order/path differs from XML")
        require(type(leaf["size"]) is int and leaf["size"] > 0 and type(leaf["case_count"]) is int and leaf["case_count"] > 0, "invalid OpenGL leaf size/count")
        require(isinstance(leaf["sha256"], str) and SHA256.fullmatch(leaf["sha256"]) is not None, "invalid OpenGL leaf digest")
        count += leaf["case_count"]
    for key in ("leaf_count", "case_count", "unique_case_count", "duplicate_occurrence_count"):
        require(type(manifest[key]) is int, "invalid OpenGL coverage count")
    require(manifest["leaf_count"] == len(leaves) and manifest["case_count"] == count, "OpenGL coverage count mismatch")
    require(0 < manifest["unique_case_count"] <= count and manifest["duplicate_occurrence_count"] == count - manifest["unique_case_count"], "invalid OpenGL unique coverage count")
    return manifest


def validate_egl_configs(configs: Any, api: str) -> None:
    """Check the ordered EGL configuration list a run plan expands over.

    api names the plan ("OpenGL" or "OpenGL ES") in the error messages.
    """
    require(isinstance(configs, list) and bool(configs), f"{api} run plan has no EGL configurations")
    seen = set()
    for config in configs:
        require(isinstance(config, dict) and set(config) == {"id", "surface_types"}, f"invalid {api} EGL configuration")
        require(type(config["id"]) is int and config["id"] > 0 and config["id"] not in seen, f"duplicate or invalid {api} EGL config ID")
        seen.add(config["id"])
        surfaces = config["surface_types"]
        require(isinstance(surfaces, list) and bool(surfaces) and all(value in {"window", "pbuffer", "pixmap"} for value in surfaces) and len(surfaces) == len(set(surfaces)), f"invalid {api} EGL surface list")


def expand_runs(manifest: dict[str, Any], configs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    validate_manifest(manifest)
    validate_egl_configs(configs, "OpenGL")
    runs = []

    def append(configuration: dict[str, Any], config_index: int) -> None:
        config = configs[config_index]
        surface = "fbo" if uses_fbo_surface(configuration) else next(
            value for value in ("window", "pbuffer", "pixmap") if value in config["surface_types"]
        )
        runs.append({"index": len(runs), "configuration_index": configuration["index"],
                     "egl_config_index": config_index, "egl_config_id": config["id"],
                     "surface_type": surface, "leaf_id": configuration["leaf_id"]})

    rows = manifest["configurations"]
    for row in rows:
        if row["collection"] != "main":
            append(row, 0)
    for index in range(len(configs)):
        for row in rows:
            if row["collection"] == "main" and row["use_for_first_egl_config"] == (index == 0):
                append(row, index)
    return runs


def runner_arguments(manifest: dict[str, Any], run: dict[str, Any], caselist_path: str) -> list[str]:
    row = manifest["configurations"][run["configuration_index"]]
    arguments = [
        f"--deqp-caselist-file={caselist_path}",
        "--deqp-screen-rotation=unspecified",
        f"--deqp-surface-width={row['surface_width']}",
        f"--deqp-surface-height={row['surface_height']}",
        f"--deqp-base-seed={row['base_seed']}",
        "--deqp-watchdog=disable",
    ]
    if uses_fbo_surface(row):
        arguments.append(f"--deqp-gl-config-name={row['fbo_config']}")
    arguments.extend((f"--deqp-surface-type={run['surface_type']}",
                      f"--deqp-gl-config-id={run['egl_config_id']}",
                      "--deqp-gl-context-type=egl"))
    return arguments
