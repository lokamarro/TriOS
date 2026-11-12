#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive the public OpenGL ES 3.2 runner scope from authenticated upstream XML.

The collection order and API predicates match getTestRunsForES in the pinned
glcTestRunner.cpp, which for an ES 3.2 context visits the NoContext, Single
Config and Khronos ES must-pass collections in that order and then walks the
EGL configuration list. Text files not referenced by these XML documents do
not define executable coverage. EGL configurations remain an ordered runtime
input.

The two AOSP collections it also visits (dEQP-EGL and dEQP-GLES2/3/31) name a
GL configuration instead of an EGL configuration id. The runner resolves that
name against ``ConfigList::aospConfigs``, so this module reconstructs that list
from the EGL attribute table the inventory records and matches the name's bits
the way isGLConfigCompatible does. The reconstruction is checked against the
``CTS-Configs`` list it must reproduce.

The collection order here is not the runner's emission order: the AOSP
collections come last so that adding them never renumbers an existing run.
Runs are independent guest boots, so only the set of runs and each run's own
command line are observable.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import shlex
from typing import Any, Callable
from xml.etree import ElementTree

# The OpenGL plan owns the helpers both plans share, and the runner source
# both are derived from.
from opengl_cts_plan import (
    RUNNER_SOURCE,
    PlanError,
    file_identity,
    require,
    uses_fbo_surface,
    validate_egl_configs,
)


ROOT = "/opt/vk-gl-cts/openglcts/gl_cts/data/mustpass"
API = "gles32"
# The run table selects a row when the context supports the row's API
# ("supported"), except the single-configuration collection, which the runner
# admits only on an exact API match ("exact"). The AOSP collections select the
# same way ("aosp-supported") or unconditionally ("aosp-any", the EGL run,
# whose records carry no API at all).
COLLECTIONS = (
    ("no-context", "gles/khronos_mustpass_noctx", "Khronos Mustpass ES NoContext", "supported"),
    ("single", "gles/khronos_mustpass_single", "Khronos Mustpass ES Single Config", "exact"),
    ("main", "gles/khronos_mustpass", "Khronos Mustpass ES", "supported"),
    ("aosp-es", "gles/aosp_mustpass", "AOSP Mustpass ES", "aosp-supported"),
    ("aosp-egl", "egl/aosp_mustpass", "AOSP Mustpass EGL", "aosp-any"),
)
AOSP_PREDICATES = frozenset({"aosp-supported", "aosp-any"})
# ES caselist stems name a version without a separator: 3 is ES 3.0, 31 is
# ES 3.1 and 32 is ES 3.2.
ES_VERSIONS = {"2": (2, 0), "3": (3, 0), "31": (3, 1), "32": (3, 2)}
TARGET_VERSION = ES_VERSIONS["32"]
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
CASELISTS = {
    "supported": re.compile(r"gles(2|3|31|32)-(khr-[a-z0-9-]+)\.txt\Z"),
    "exact": re.compile(r"gles(2|3|31|32)-(khr-[a-z0-9-]+)\.txt\Z"),
    "aosp-supported": re.compile(r"gles(2|3|31|32)-([a-z0-9-]+)\.txt\Z"),
    "aosp-any": re.compile(r"(egl)-([a-z0-9-]+)\.txt\Z"),
}
# parseConfigBitsFromName, as the runner spells the AOSP configuration names.
COLOR_BITS = {"rgba8888": (8, 8, 8, 8), "rgb565": (5, 6, 5, 0)}
DEPTH_BITS = {"d0": 0, "d24": 24}
STENCIL_BITS = {"s0": 0, "s8": 8}
SAMPLE_BITS = {"ms0": 0, "ms4": 4}
SURFACE_BITS = (("EGL_WINDOW_BIT", "window"), ("EGL_PIXMAP_BIT", "pixmap"),
                ("EGL_PBUFFER_BIT", "pbuffer"))
# The rotated AOSP records exist only for Android builds, which this one is
# not; they are parsed so a changed document is still rejected, then dropped.
ROTATIONS = ("unspecified", "0", "90", "180", "270")
ES3_BIT = "EGL_OPENGL_ES3_BIT_KHR"
ATTRIBUTE_KEYS = {"id", "red", "green", "blue", "alpha", "depth", "stencil",
                  "samples", "renderable", "conformant", "surface_types"}
SCHEMA = 1
LEAF_KEYS = {"id", "path", "size", "case_count", "sha256"}


def command_values(arguments: list[str], predicate: str) -> dict[str, Any]:
    """The run parameters one XML record carries.

    A Khronos record always seeds itself and names a configuration only for the
    framework framebuffer object; an AOSP record never seeds itself and always
    names the GL configuration its EGL configuration is resolved from.
    """
    aosp = predicate in AOSP_PREDICATES
    allowed = {
        "deqp-screen-rotation", "deqp-surface-width", "deqp-surface-height",
        "deqp-base-seed", "deqp-gl-config-name", "deqp-surface-type",
        "deqp-watchdog",
    }
    values: dict[str, str] = {}
    for argument in arguments:
        match = re.fullmatch(r"--([a-z0-9-]+)=([^\s]+)", argument)
        require(match is not None, "malformed OpenGL ES must-pass argument")
        key, value = match.groups()
        require(key in allowed and key not in values,
                "unknown or duplicate OpenGL ES must-pass argument")
        values[key] = value
    required = {"deqp-screen-rotation", "deqp-surface-width",
                "deqp-surface-height", "deqp-watchdog"}
    required |= {"deqp-gl-config-name"} if aosp else {"deqp-base-seed"}
    require(required.issubset(values), "incomplete OpenGL ES must-pass command")
    require(aosp != ("deqp-base-seed" in values),
            "OpenGL ES AOSP records carry no base seed")
    require(not aosp or "deqp-surface-type" not in values,
            "an OpenGL ES AOSP record never names its surface type")
    require(values["deqp-screen-rotation"] in ROTATIONS, "unsupported OpenGL ES rotation")
    require(values["deqp-watchdog"] == "disable", "unsupported OpenGL ES watchdog policy")
    try:
        width = int(values["deqp-surface-width"])
        height = int(values["deqp-surface-height"])
        seed = None if aosp else int(values["deqp-base-seed"])
    except ValueError as error:
        raise PlanError("invalid OpenGL ES must-pass numeric argument") from error
    require(width == -1 or 0 < width <= 0x7FFFFFFF, "invalid OpenGL ES surface width")
    require(height == -1 or 0 < height <= 0x7FFFFFFF, "invalid OpenGL ES surface height")
    require(seed is None or 0 <= seed <= 0xFFFFFFFF, "invalid OpenGL ES base seed")
    config = values.get("deqp-gl-config-name")
    surface = values.get("deqp-surface-type")
    require(config is None or re.fullmatch(r"[a-z0-9]+", config) is not None,
            "invalid OpenGL ES named config")
    require(surface is None or surface == "fbo", "unsupported OpenGL ES XML surface type")
    require(aosp or (config is None) == (surface is None),
            "incomplete OpenGL ES FBO configuration")
    if aosp:
        parse_config_bits(config)
    return {"screen_rotation": values["deqp-screen-rotation"],
            "surface_width": width, "surface_height": height,
            "base_seed": seed,
            "fbo_config": None if aosp else config,
            "aosp_config": config if aosp else None}


def parse_config_bits(name: str) -> tuple[int, ...]:
    """The seven-component configuration a GL configuration name selects.

    parseConfigBitsFromName consumes the colour, depth, stencil and sample
    prefixes in that order; a name it cannot consume entirely never matches a
    configuration, so refuse it instead of matching on uninitialised bits.
    """
    rest = name
    bits: list[int] = []
    for table in (COLOR_BITS, DEPTH_BITS, STENCIL_BITS, SAMPLE_BITS):
        prefix = next((key for key in table if rest.startswith(key)), None)
        require(prefix is not None, f"unsupported OpenGL ES config name: {name}")
        value = table[prefix]
        bits.extend(value if isinstance(value, tuple) else (value,))
        rest = rest[len(prefix):]
    require(not rest, f"unsupported OpenGL ES config name: {name}")
    return tuple(bits)


def aosp_config_list(attributes: Any) -> list[dict[str, Any]]:
    """ConfigList::aospConfigs for an ES 3.2 context, in enumeration order.

    getDefaultEglConfigList keeps every configuration that both renders and
    conforms to ES 3, with its EGL surface bits; the multisample, float and
    YUV exclusions apply to the conformant list only, never to this one.
    """
    require(isinstance(attributes, list) and bool(attributes),
            "OpenGL ES run plan has no EGL configuration attributes")
    result = []
    seen: set[int] = set()
    for row in attributes:
        require(isinstance(row, dict) and set(row) == ATTRIBUTE_KEYS,
                "invalid OpenGL ES EGL configuration attributes")
        require(type(row["id"]) is int and row["id"] > 0 and row["id"] not in seen,
                "duplicate or invalid OpenGL ES EGL config ID")
        seen.add(row["id"])
        require(all(type(row[key]) is int and row[key] >= 0 for key in
                    ("red", "green", "blue", "alpha", "depth", "stencil", "samples")),
                "invalid OpenGL ES EGL configuration attribute")
        surfaces = row["surface_types"]
        require(isinstance(surfaces, list)
                and all(value in {"window", "pbuffer", "pixmap"} for value in surfaces)
                and len(surfaces) == len(set(surfaces)),
                "invalid OpenGL ES EGL surface list")
        require(surfaces == [value for _, value in SURFACE_BITS if value in surfaces],
                "OpenGL ES EGL surface list is out of order")
        for key in ("renderable", "conformant"):
            require(isinstance(row[key], list)
                    and all(isinstance(value, str) and value for value in row[key]),
                    "invalid OpenGL ES EGL API bits")
        if ES3_BIT in row["renderable"] and ES3_BIT in row["conformant"]:
            result.append({"id": row["id"], "surface_types": list(surfaces),
                           "bits": tuple(row[key] for key in
                                         ("red", "green", "blue", "alpha",
                                          "depth", "stencil", "samples"))})
    require(bool(result), "no EGL configuration renders and conforms to ES 3")
    return result


def selected(version: str, predicate: str) -> bool:
    if predicate == "aosp-any":
        return version == "egl"
    if predicate == "exact":
        return ES_VERSIONS[version] == TARGET_VERSION
    return ES_VERSIONS[version] <= TARGET_VERSION


def parse_documents(documents: Any) -> list[dict[str, Any]]:
    require(isinstance(documents, list) and len(documents) == len(COLLECTIONS),
            "incomplete OpenGL ES XML collection")
    configurations: list[dict[str, Any]] = []
    attributes = {"caseListFile", "commandLine", "name", "os", "useForFirstEGLConfig"}
    for document, (collection, directory, package_name, predicate) in zip(documents, COLLECTIONS):
        path = f"{ROOT}/{directory}/main/mustpass.xml"
        require(isinstance(document, dict)
                and set(document) == {"collection", "path", "size", "sha256", "text"},
                "invalid OpenGL ES XML record")
        require(document["collection"] == collection and document["path"] == path,
                "OpenGL ES XML collection order/path changed")
        require(isinstance(document["text"], str), "invalid OpenGL ES XML payload")
        payload = document["text"].encode("utf-8")
        require(type(document["size"]) is int and 0 < document["size"] <= 64 * 1024,
                "invalid OpenGL ES XML size")
        require(file_identity(path, payload)
                == {key: document[key] for key in ("path", "size", "sha256")},
                "OpenGL ES XML identity changed")
        require(b"<!DOCTYPE" not in payload and b"<!ENTITY" not in payload,
                "unsupported OpenGL ES XML declarations")
        try:
            root = ElementTree.fromstring(payload)
        except ElementTree.ParseError as error:
            raise PlanError("malformed OpenGL ES must-pass XML") from error
        require(root.tag == "Mustpass" and root.attrib == {"version": "main"},
                "unsupported OpenGL ES XML document")
        require(len(root) == 1, "unexpected OpenGL ES XML package set")
        package = root[0]
        require(package.tag == "TestPackage" and package.attrib == {"name": package_name},
                "unexpected OpenGL ES XML package")
        chosen = 0
        for xml_index, element in enumerate(package):
            require(element.tag == "Configuration"
                    and set(element.attrib) == attributes and not list(element),
                    "invalid OpenGL ES XML configuration")
            require(not (element.text or "").strip(),
                    "unexpected OpenGL ES configuration content")
            row = element.attrib
            aosp = predicate in AOSP_PREDICATES
            require(row["os"] == "any" or (aosp and row["os"] == "android"),
                    "unsupported OpenGL ES configuration scope")
            require(row["useForFirstEGLConfig"] in {"True", "False"},
                    "unsupported OpenGL ES configuration scope")
            match = CASELISTS[predicate].fullmatch(row["caseListFile"])
            require(match is not None, "unsafe or unsupported OpenGL ES caselist reference")
            version, name = match.groups()
            require(row["name"] == name, "OpenGL ES configuration name differs from caselist")
            try:
                arguments = shlex.split(row["commandLine"])
            except ValueError as error:
                raise PlanError("malformed OpenGL ES must-pass command") from error
            values = command_values(arguments, predicate)
            # The generated RunParams tables guard every non-"any" record with
            # the Android build condition, so this build never emits them.
            if row["os"] != "any" or not selected(version, predicate):
                continue
            first = row["useForFirstEGLConfig"] == "True"
            require(collection == "main" or first,
                    "one-time OpenGL ES collection has an other-config row")
            require(not aosp or values["aosp_config"] is not None,
                    "an OpenGL ES AOSP record names no GL configuration")
            require(values.pop("screen_rotation") == "unspecified",
                    "unsupported OpenGL ES rotation")
            configurations.append({
                "index": len(configurations), "collection": collection,
                "xml_index": xml_index, "leaf_id": row["caseListFile"][:-4],
                "leaf_path": f"{ROOT}/{directory}/main/{row['caseListFile']}",
                "name": name, "use_for_first_egl_config": first,
                "xml_arguments": arguments, **values,
            })
            chosen += 1
        require(chosen > 0, f"OpenGL ES XML collection has no ES 3.2 runs: {collection}")
    main = [row for row in configurations if row["collection"] == "main"]
    require(any(row["use_for_first_egl_config"] for row in main)
            and any(not row["use_for_first_egl_config"] for row in main),
            "OpenGL ES main XML lacks first/other roles")
    return configurations


def build_manifest(
    read_document: Callable[[str], bytes],
    leaf_identity: Callable[[str], dict[str, Any]],
) -> dict[str, Any]:
    """Assemble the ES 3.2 manifest from repository XML and pinned leaves.

    ``read_document`` returns the bytes of one collection's ``mustpass.xml``
    and ``leaf_identity`` the pinned identity of one caselist; the caselists
    themselves live only in the guest image, where the shard re-checks the
    digest this manifest carries.
    """
    documents = []
    for collection, directory, _, _ in COLLECTIONS:
        path = f"{ROOT}/{directory}/main/mustpass.xml"
        payload = read_document(collection)
        documents.append({"collection": collection, **file_identity(path, payload),
                          "text": payload.decode("utf-8", errors="strict")})
    configurations = parse_documents(documents)
    leaves: dict[str, dict[str, Any]] = {}
    count = 0
    for row in configurations:
        if row["leaf_id"] in leaves:
            require(leaves[row["leaf_id"]]["path"] == row["leaf_path"],
                    "OpenGL ES leaf identifier names multiple files")
            continue
        leaf = dict(leaf_identity(row["leaf_path"]))
        require(set(leaf) == LEAF_KEYS, "invalid OpenGL ES leaf identity")
        require(leaf["path"] == row["leaf_path"], "OpenGL ES leaf identity names another file")
        leaf["id"] = row["leaf_id"]
        leaves[row["leaf_id"]] = leaf
        count += leaf["case_count"]
    return {"schema_version": SCHEMA, "api": API,
            "runner_source": dict(RUNNER_SOURCE), "documents": documents,
            "configurations": configurations, "leaves": list(leaves.values()),
            "leaf_count": len(leaves), "case_count": count}


def validate_manifest(manifest: Any) -> dict[str, Any]:
    require(isinstance(manifest, dict) and set(manifest) == {
        "schema_version", "api", "runner_source", "documents", "configurations",
        "leaves", "leaf_count", "case_count",
    }, "invalid OpenGL ES must-pass manifest")
    require(type(manifest["schema_version"]) is int
            and manifest["schema_version"] == SCHEMA and manifest["api"] == API,
            "unsupported OpenGL ES must-pass manifest")
    require(manifest["runner_source"] == RUNNER_SOURCE, "OpenGL ES runner scope source changed")
    configurations = parse_documents(manifest["documents"])
    require(manifest["configurations"] == configurations,
            "OpenGL ES run records differ from authenticated XML")
    expected: dict[str, str] = {}
    for row in configurations:
        expected.setdefault(row["leaf_id"], row["leaf_path"])
    leaves = manifest["leaves"]
    require(isinstance(leaves, list) and len(leaves) == len(expected),
            "incomplete OpenGL ES leaf set")
    count = 0
    for leaf, (identifier, path) in zip(leaves, expected.items()):
        require(isinstance(leaf, dict) and set(leaf) == LEAF_KEYS,
                "invalid OpenGL ES leaf identity")
        require(leaf["id"] == identifier and leaf["path"] == path,
                "OpenGL ES leaf order/path differs from XML")
        require(type(leaf["case_count"]) is int and leaf["case_count"] > 0
                and type(leaf["size"]) is int and leaf["size"] > 0,
                "invalid OpenGL ES leaf size/case count")
        require(isinstance(leaf["sha256"], str) and SHA256.fullmatch(leaf["sha256"]) is not None,
                "invalid OpenGL ES leaf digest")
        count += leaf["case_count"]
    for key in ("leaf_count", "case_count"):
        require(type(manifest[key]) is int, "invalid OpenGL ES coverage count")
    require(manifest["leaf_count"] == len(leaves) and manifest["case_count"] == count,
            "OpenGL ES coverage count mismatch")
    return manifest


def expand_runs(manifest: dict[str, Any], configs: list[dict[str, Any]],
                attributes: Any = None) -> list[dict[str, Any]]:
    """Expand the manifest over the ordered EGL configuration list.

    getTestRunsForES emits the NoContext and Single Config collections against
    the first configuration only, then walks every configuration and emits the
    Khronos ES rows whose first/other role matches. The AOSP collections are
    appended last, each record on the first configuration whose bits its GL
    configuration name selects: the ES records fail closed when none matches
    (TCU_FAIL) and the EGL record is simply omitted.
    """
    validate_manifest(manifest)
    validate_egl_configs(configs, "OpenGL ES")
    runs: list[dict[str, Any]] = []

    def append(configuration: dict[str, Any], config_index: int) -> None:
        config = configs[config_index]
        surface = "fbo" if uses_fbo_surface(configuration) else next(
            value for value in ("window", "pbuffer", "pixmap") if value in config["surface_types"]
        )
        runs.append({"index": len(runs), "configuration_index": configuration["index"],
                     "egl_config_index": config_index, "egl_config_id": config["id"],
                     "surface_type": surface, "leaf_id": configuration["leaf_id"],
                     "config_scope": "conformant"})

    rows = manifest["configurations"]
    for row in rows:
        if row["collection"] not in {"main", "aosp-es", "aosp-egl"}:
            append(row, 0)
    for index in range(len(configs)):
        for row in rows:
            if row["collection"] == "main" and row["use_for_first_egl_config"] == (index == 0):
                append(row, index)
    if not any(row["collection"] in {"aosp-es", "aosp-egl"} for row in rows):
        return runs
    aosp = aosp_config_list(attributes)
    conformant = [
        row for row in aosp
        if row["bits"][6] == 0 and any(
            row["id"] == config["id"] for config in configs)
    ]
    require([(row["id"], set(row["surface_types"])) for row in conformant]
            == [(row["id"], set(row["surface_types"])) for row in configs],
            "EGL configuration attributes do not reproduce the conformant list")
    for row in rows:
        if row["collection"] not in {"aosp-es", "aosp-egl"}:
            continue
        wanted = parse_config_bits(row["aosp_config"])
        index = next((position for position, config in enumerate(aosp)
                      if config["bits"] == wanted), None)
        if index is None:
            # getTestRunsForAOSPES fails the whole run list; the EGL record is
            # skipped instead.
            require(row["collection"] == "aosp-egl",
                    f"no EGL configuration matches {row['aosp_config']}")
            continue
        config = aosp[index]
        # The EGL record names no surface type at all, and an ES record whose
        # configuration offers none of the three named types names none either.
        surface = "none" if row["collection"] == "aosp-egl" else next(
            (value for value in ("window", "pbuffer", "pixmap")
             if value in config["surface_types"]), "none")
        runs.append({"index": len(runs), "configuration_index": row["index"],
                     "egl_config_index": index, "egl_config_id": config["id"],
                     "surface_type": surface, "leaf_id": row["leaf_id"],
                     "config_scope": "aosp"})
    return runs


def runner_arguments(manifest: dict[str, Any], run: dict[str, Any], caselist_path: str) -> list[str]:
    """The official command line for one run, in getBaseOptions order."""
    row = manifest["configurations"][run["configuration_index"]]
    arguments = [
        f"--deqp-caselist-file={caselist_path}",
        "--deqp-screen-rotation=unspecified",
        f"--deqp-surface-width={row['surface_width']}",
        f"--deqp-surface-height={row['surface_height']}",
        "--deqp-watchdog=disable",
    ]
    if row["aosp_config"] is not None:
        # An AOSP record names its GL configuration and, outside the EGL run,
        # a surface type its resolved configuration offers. It never names an
        # EGL configuration id, a context type or a base seed.
        arguments.append(f"--deqp-gl-config-name={row['aosp_config']}")
        if run["surface_type"] != "none":
            arguments.append(f"--deqp-surface-type={run['surface_type']}")
        return arguments
    arguments.append(f"--deqp-base-seed={row['base_seed']}")
    if uses_fbo_surface(row):
        arguments.extend((f"--deqp-gl-config-name={row['fbo_config']}",
                          "--deqp-surface-type=fbo"))
    else:
        arguments.append(f"--deqp-surface-type={run['surface_type']}")
    arguments.extend((f"--deqp-gl-config-id={run['egl_config_id']}",
                      "--deqp-gl-context-type=egl"))
    return arguments
