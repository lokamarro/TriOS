#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode the pinned Virgl v1/v2 capability ABI into stable JSON values."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import math
import struct
from typing import Any


CAPSET_V1_SIZE = 308
CAPSET_V2_SIZE = 1408

BOOL_SET1 = (
    "indep_blend_enable",
    "indep_blend_func",
    "cube_map_array",
    "shader_stencil_export",
    "conditional_render",
    "start_instance",
    "primitive_restart",
    "blend_eq_sep",
    "instanceid",
    "vertex_element_instance_divisor",
    "seamless_cube_map",
    "occlusion_query",
    "timer_query",
    "streamout_pause_resume",
    "texture_multisample",
    "fragment_coord_conventions",
    "depth_clip_disable",
    "seamless_cube_map_per_texture",
    "ubo",
    "color_clamping",
    "poly_stipple",
    "mirror_clamp",
    "texture_query_lod",
    "has_fp64",
    "has_tessellation_shaders",
    "has_indirect_draw",
    "has_sample_shading",
    "has_cull",
    "conditional_render_inverted",
    "derivative_control",
    "polygon_offset_clamp",
    "transform_feedback_overflow_query",
)

CAPABILITY_BITS = (
    "tgsi_invariant",
    "texture_view",
    "set_min_samples",
    "copy_image",
    "tgsi_precise",
    "txqs",
    "memory_barrier",
    "compute_shader",
    "fb_no_attach",
    "robust_buffer_access",
    "tgsi_fbfetch",
    "shader_clock",
    "texture_barrier",
    "tgsi_components",
    "guest_may_init_log",
    "srgb_write_control",
    "qbo",
    "transfer",
    "fbo_mixed_color_formats",
    "host_is_gles",
    "bind_command_args",
    "multi_draw_indirect",
    "indirect_params",
    "transform_feedback3",
    "3d_astc",
    "indirect_input_addr",
    "copy_transfer",
    "clip_halfz",
    "app_tweak_support",
    "bgra_srgb_is_emulated",
    "clear_texture",
    "arb_buffer_storage",
)

CAPABILITY_BITS_V2 = (
    "blend_equation",
    "untyped_resource",
    "video_memory",
    "meminfo",
    "string_marker",
    "different_gpu",
    "implicit_msaa",
    "copy_transfer_both_directions",
    "scanout_uses_gbm",
    "sso",
    "texture_shadow_lod",
    "vs_vertex_layer",
    "vs_viewport_index",
    "pipeline_statistics_query",
    "draw_parameters",
    "group_vote",
    "mirror_clamp_to_edge",
    "mirror_clamp",
    "resource_layout",
)

V1_SCALARS = (
    "glsl_level",
    "max_texture_array_layers",
    "max_streamout_buffers",
    "max_dual_source_render_targets",
    "max_render_targets",
    "max_samples",
    "prim_mask",
    "max_tbo_size",
    "max_uniform_blocks",
    "max_viewports",
    "max_texture_gather_components",
)


class Decoder:
    def __init__(self, payload: bytes):
        self.payload = payload
        self.offset = 0

    def take(self, format_: str) -> tuple[Any, ...]:
        size = struct.calcsize(format_)
        if size > len(self.payload) - self.offset:
            raise ValueError("Virgl capset ended inside a declared field")
        result = struct.unpack_from(format_, self.payload, self.offset)
        self.offset += size
        return result

    def u32(self) -> int:
        return int(self.take("<I")[0])

    def i32(self) -> int:
        return int(self.take("<i")[0])

    def f32(self) -> float:
        value = float(self.take("<f")[0])
        if not math.isfinite(value):
            raise ValueError("Virgl capset contains a non-finite limit")
        return value

    def u32_array(self, count: int) -> list[int]:
        return [self.u32() for _ in range(count)]

    def bytes(self, count: int) -> bytes:
        if count > len(self.payload) - self.offset:
            raise ValueError("Virgl capset ended inside a byte array")
        result = self.payload[self.offset : self.offset + count]
        self.offset += count
        return result


def decode_bits(value: int, names: tuple[str, ...]) -> dict[str, Any]:
    known_mask = (1 << len(names)) - 1
    return {
        "raw": value,
        "enabled": [name for bit, name in enumerate(names) if value & (1 << bit)],
        "unknown_mask": value & ~known_mask,
    }


def decode_format_mask(decoder: Decoder) -> list[int]:
    words = decoder.u32_array(16)
    return [
        word_index * 32 + bit
        for word_index, word in enumerate(words)
        for bit in range(32)
        if word & (1 << bit)
    ]


def decode_v1(decoder: Decoder) -> dict[str, Any]:
    result: dict[str, Any] = {"max_version": decoder.u32()}
    for name in ("sampler", "render", "depthstencil", "vertexbuffer"):
        result[f"{name}_formats"] = decode_format_mask(decoder)
    result["features"] = decode_bits(decoder.u32(), BOOL_SET1)
    for name in V1_SCALARS:
        result[name] = decoder.u32()
    return result


def decode_renderer(storage: bytes) -> str:
    value, separator, trailing = storage.partition(b"\0")
    if separator and any(trailing):
        raise ValueError("Virgl renderer field has nonzero bytes after its terminator")
    try:
        renderer = value.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ValueError("Virgl renderer field is not UTF-8") from error
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in renderer):
        raise ValueError("Virgl renderer field contains control data")
    return renderer


def decode_video_cap(decoder: Decoder) -> dict[str, Any]:
    identity = decoder.u32()
    dimensions = decoder.u32()
    format_and_blocks = decoder.u32()
    features = decoder.u32()
    return {
        "profile": identity & 0xFF,
        "entrypoint": (identity >> 8) & 0xFF,
        "max_level": (identity >> 16) & 0xFF,
        "stacked_frames": (identity >> 24) & 0xFF,
        "max_width": dimensions & 0xFFFF,
        "max_height": (dimensions >> 16) & 0xFFFF,
        "preferred_format": format_and_blocks & 0xFFFF,
        "max_macroblocks": (format_and_blocks >> 16) & 0xFFFF,
        "npot_texture": bool(features & 1),
        "supports_progressive": bool(features & 2),
        "supports_interlaced": bool(features & 4),
        "prefers_interlaced": bool(features & 8),
        "max_temporal_layers": (features >> 4) & 0xFF,
        "reserved": features >> 12,
    }


def decode_capset(identifier: int, payload: bytes) -> dict[str, Any]:
    if identifier == 1 and len(payload) != CAPSET_V1_SIZE:
        raise ValueError(
            f"Virgl capset 1 has size {len(payload)}, expected {CAPSET_V1_SIZE}"
        )
    if identifier == 2 and len(payload) < CAPSET_V2_SIZE:
        raise ValueError(
            f"Virgl capset 2 has size {len(payload)}, expected at least "
            f"{CAPSET_V2_SIZE}"
        )
    if identifier not in (1, 2):
        raise ValueError(f"unsupported Virgl capset identifier: {identifier}")

    decoder = Decoder(payload)
    result: dict[str, Any] = {"v1": decode_v1(decoder)}
    if identifier == 1:
        if decoder.offset != CAPSET_V1_SIZE:
            raise ValueError("Virgl capset 1 decoder consumed the wrong size")
        return result

    v2: dict[str, Any] = {}
    for name in (
        "min_aliased_point_size",
        "max_aliased_point_size",
        "min_smooth_point_size",
        "max_smooth_point_size",
        "min_aliased_line_width",
        "max_aliased_line_width",
        "min_smooth_line_width",
        "max_smooth_line_width",
        "max_texture_lod_bias",
    ):
        v2[name] = decoder.f32()
    for name in (
        "max_geom_output_vertices",
        "max_geom_total_output_components",
        "max_vertex_outputs",
        "max_vertex_attribs",
        "max_shader_patch_varyings",
    ):
        v2[name] = decoder.u32()
    for name in (
        "min_texel_offset",
        "max_texel_offset",
        "min_texture_gather_offset",
        "max_texture_gather_offset",
    ):
        v2[name] = decoder.i32()
    for name in (
        "texture_buffer_offset_alignment",
        "uniform_buffer_offset_alignment",
        "shader_buffer_offset_alignment",
    ):
        v2[name] = decoder.u32()
    v2["capabilities"] = decode_bits(decoder.u32(), CAPABILITY_BITS)
    v2["sample_locations"] = decoder.u32_array(8)
    for name in (
        "max_vertex_attrib_stride",
        "max_shader_buffer_frag_compute",
        "max_shader_buffer_other_stages",
        "max_shader_image_frag_compute",
        "max_shader_image_other_stages",
        "max_image_samples",
        "max_compute_work_group_invocations",
        "max_compute_shared_memory_size",
    ):
        v2[name] = decoder.u32()
    v2["max_compute_grid_size"] = decoder.u32_array(3)
    v2["max_compute_block_size"] = decoder.u32_array(3)
    for name in (
        "max_texture_2d_size",
        "max_texture_3d_size",
        "max_texture_cube_size",
        "max_combined_shader_buffers",
    ):
        v2[name] = decoder.u32()
    v2["max_atomic_counters"] = decoder.u32_array(6)
    v2["max_atomic_counter_buffers"] = decoder.u32_array(6)
    for name in (
        "max_combined_atomic_counters",
        "max_combined_atomic_counter_buffers",
        "host_feature_check_version",
    ):
        v2[name] = decoder.u32()
    v2["supported_readback_formats"] = decode_format_mask(decoder)
    v2["scanout_formats"] = decode_format_mask(decoder)
    v2["capabilities_v2"] = decode_bits(decoder.u32(), CAPABILITY_BITS_V2)
    v2["max_video_memory"] = decoder.u32()
    v2["renderer"] = decode_renderer(decoder.bytes(64))
    v2["max_anisotropy"] = decoder.f32()
    v2["max_texture_samplers"] = decoder.u32()
    v2["supported_multisample_formats"] = decode_format_mask(decoder)
    v2["max_const_buffer_size"] = decoder.u32_array(6)
    v2["num_video_caps"] = decoder.u32()
    if v2["num_video_caps"] > 32:
        raise ValueError("Virgl capset declares more than 32 video capabilities")
    v2["video_caps"] = [decode_video_cap(decoder) for _ in range(32)]
    for name in (
        "max_uniform_block_size",
        "max_tcs_outputs",
        "max_tes_outputs",
    ):
        v2[name] = decoder.u32()
    v2["max_shader_storage_blocks"] = decoder.u32_array(6)
    if decoder.offset != CAPSET_V2_SIZE:
        raise ValueError("Virgl capset 2 decoder consumed the wrong size")
    result["v2"] = v2
    result["known_size"] = CAPSET_V2_SIZE
    result["trailing_hex"] = payload[CAPSET_V2_SIZE:].hex()
    return result


