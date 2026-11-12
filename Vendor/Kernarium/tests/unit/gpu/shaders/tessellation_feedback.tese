// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#if CLOCKWISE
#define WINDING cw
#else
#define WINDING ccw
#endif
#if PRIMITIVE == 0
layout(triangles, equal_spacing, ccw, point_mode) in;
#elif PRIMITIVE == 1
layout(isolines, equal_spacing) in;
#elif PRIMITIVE == 2
layout(triangles, equal_spacing, WINDING) in;
#else
layout(quads, equal_spacing, WINDING) in;
#endif
layout(location = 0) flat in uint instance_id[];
layout(set = 0, binding = 0, std430) buffer Data { uint words[]; } data;
#if CAPTURE && !PACKED
layout(xfb_buffer = 0, xfb_stride = 64) out gl_PerVertex {
    layout(xfb_offset = 0) vec4 gl_Position;
    layout(xfb_offset = 16) float gl_PointSize;
    layout(xfb_offset = 20) float gl_ClipDistance[2];
    layout(xfb_offset = 28) float gl_CullDistance[2];
};
layout(location = 0, xfb_buffer = 0, xfb_offset = 36, xfb_stride = 64) out vec3 coords;
layout(location = 1, xfb_buffer = 0, xfb_offset = 48, xfb_stride = 64) flat out uvec3 ids;
#else
out gl_PerVertex {
    vec4 gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[2];
    float gl_CullDistance[2];
};
#if CAPTURE && PACKED
layout(location = 0, xfb_buffer = 0, xfb_offset = 4, xfb_stride = 28) out vec3 coords;
layout(location = 1, xfb_buffer = 1, xfb_offset = 8, xfb_stride = 44) flat out uvec3 ids;
layout(location = 2, component = 1, xfb_buffer = 0, xfb_offset = 20, xfb_stride = 28) out float packed_u;
layout(location = 2, component = 3, xfb_buffer = 1, xfb_offset = 28, xfb_stride = 44) out float packed_v;
#else
layout(location = 0) out vec3 coords;
layout(location = 1) flat out uvec3 ids;
#endif
#endif
void main()
{
    uint patch_id = uint(gl_PrimitiveID);
    uint instance = instance_id[0];
    uint slot = instance * data.words[1] + patch_id;
    // Independent slots expose duplicate/lost effects without assigning an
    // implementation-dependent atomic-return order to the captured vertices.
    atomicAdd(data.words[16 + slot], 1);
    coords = gl_TessCoord;
    uint domain = uint(gl_TessCoord.x * 64.0) + uint(gl_TessCoord.y * 64.0) * 128u;
    ids = uvec3(patch_id, instance, 0x12340000u | domain);
#if CAPTURE && PACKED
    packed_u = gl_TessCoord.x + 0.25;
    packed_v = gl_TessCoord.y + 0.5;
#endif
    gl_Position = vec4(gl_TessCoord.xy - vec2(0.5), uintBitsToFloat(data.words[2]), 1);
    gl_PointSize = 5.0;
    gl_ClipDistance[0] = 1.0;
    gl_ClipDistance[1] = uintBitsToFloat(data.words[3]);
    gl_CullDistance[0] = 1.0;
    gl_CullDistance[1] = uintBitsToFloat(data.words[4]);
}
