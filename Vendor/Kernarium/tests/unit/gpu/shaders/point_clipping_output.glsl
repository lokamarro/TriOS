// SPDX-License-Identifier: GPL-2.0-or-later
// Shared final-stage outputs and optional pre-clipping application effects.
#if EFFECTS
layout(set = 0, binding = 0, std430) buffer Data { uint words[]; } data;
#else
layout(set = 0, binding = 0, std430) readonly buffer Data { uint words[]; } data;
#endif
#if CAPTURE
layout(xfb_buffer = 0, xfb_stride = 24) out gl_PerVertex {
    layout(xfb_offset = 0) vec4 gl_Position;
    layout(xfb_offset = 16) float gl_PointSize;
    layout(xfb_offset = 20) float gl_ClipDistance[1];
};
#else
out gl_PerVertex {
    vec4 gl_Position;
    float gl_PointSize;
#if CLIP_PLANES
    float gl_ClipDistance[CLIP_PLANES];
#endif
#if CULL_PLANE
    float gl_CullDistance[1];
#endif
};
#endif

void write_point(uint base)
{
#if PARTIAL
    gl_Position = vec4(0.0);
    if (data.words[33] == 0u) {
        gl_Position.xy = uintBitsToFloat(uvec2(data.words[base], data.words[base + 1u]));
        gl_Position.z = uintBitsToFloat(data.words[base + 2u]);
    } else {
        gl_Position.xyz = uintBitsToFloat(uvec3(data.words[base + 16u],
                                               data.words[base + 17u], data.words[base + 18u]));
    }
    // The final homogeneous divisor is a separate component store, after the
    // conditional XYZ assignments. It must be used by point-center clipping.
    gl_Position.w = uintBitsToFloat(data.words[base + 3u]);
#else
    gl_Position = uintBitsToFloat(uvec4(data.words[base], data.words[base + 1u],
                                      data.words[base + 2u], data.words[base + 3u]));
#endif
    gl_PointSize = uintBitsToFloat(data.words[base + 4u]);
#if CLIP_PLANES
    for (uint plane = 0; plane < CLIP_PLANES; ++plane)
        gl_ClipDistance[plane] = uintBitsToFloat(data.words[base + 5u + plane]);
#endif
#if PARTIAL
    if (data.words[33] != 0u)
        gl_ClipDistance[0] = uintBitsToFloat(data.words[base + 21u]);
#endif
#if CULL_PLANE
    gl_CullDistance[0] = uintBitsToFloat(data.words[base + 13u]);
#endif
#if EFFECTS
    atomicAdd(data.words[32], 1u);
#endif
}
