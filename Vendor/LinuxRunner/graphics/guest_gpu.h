#ifndef LINUXRUNNER_GUEST_GPU_H
#define LINUXRUNNER_GUEST_GPU_H
#include <stdint.h>
#include <stddef.h>
#include "shader_compiler.h"
// Experimental, versioned Linux character-device ABI. Never replaces Mesa.
#define LR_GPU_MAJOR 241
#define LR_GPU_IOCTL 0x4c520101u
#define LR_GPU_VERSION 1u
#define LR_GPU_MAX_DIMENSION 1024u
#define LR_GPU_MAX_VERTICES 4096u
#define LR_GPU_CREATE 1u
#define LR_GPU_DRAW 2u
#define LR_GPU_READ 3u
#define LR_GPU_INFO 4u
#define LR_GPU_FINISH 5u
#define LR_GPU_READ_X11 6u
#define LR_GPU_COMPILE_SHADER 8u
#define LR_GPU_COMPILE_SHADER_EXTENDED 34u
#define LR_GPU_DRAW_INDEXED_BASE 33u // existing indexed payload plus one trailing uint32 base instance
#define LR_GPU_LINK_PROGRAM 9u
#define LR_GPU_DELETE_SHADER 10u
#define LR_GPU_DELETE_PROGRAM 11u
#define LR_GPU_DRAW_PROGRAM 12u
#define LR_GPU_RESIZE 13u
#define LR_GPU_TEXTURE 14u
#define LR_GPU_DELETE_TEXTURE 15u
#define LR_GPU_GENERATE_MIPMAP 16u
#define LR_GPU_RESOURCE_INFO 17u // count=vertex allocations; bytes=vertex uploads; submissions=texture allocations; readbacks=texture uploads; gpu_nanoseconds=texture storage bytes (not timing)
#define LR_GPU_READ_REGION 18u
#define LR_GPU_VIEWPORT 19u
#define LR_GPU_FRAMEBUFFER 20u
#define LR_GPU_INSTANCE_ATTRIBUTES 29u
struct lr_gpu_instance_attributes {uint32_t instances,divisor[LR_GL_MAX_ATTRIBUTES],count[LR_GL_MAX_ATTRIBUTES];float value[LR_GL_MAX_ATTRIBUTES][64][4];};
#define LR_GPU_TYPED_DATA_BIT 32u // indexed/instance packets carry reflected int/uint bits; backend validates float fields
#define LR_GPU_DRAW_INDEXED 28u // width=instances, height=index count; draw vertices followed by uint32 indices
#define LR_GPU_COPY_INFO 27u // submissions=compute copies, readbacks=row blit commands, bytes=GPU copy bytes
#define LR_GPU_RENDERBUFFER 26u
#define LR_GPU_MAX_RENDERBUFFERS 16u
#define LR_GPU_RENDERBUFFER_BUDGET (8u*1024u*1024u)
#define LR_GPU_RB_BIND 1u
#define LR_GPU_RB_STORAGE 2u
#define LR_GPU_RB_DELETE 3u
#define LR_GPU_RB_QUERY 4u
struct lr_gpu_renderbuffer {uint32_t command,name,width,height,format,depth_bits;};
#define LR_GPU_UNIFORM_BLOCK_INFO 25u // submissions=upload calls, bytes=upload payload bytes, gpu_nanoseconds=arena snapshot bytes (not time)
#define LR_GPU_DRAW_INSTANCED 24u // width=instance count, existing draw payload unchanged
#define LR_GPU_MAX_INSTANCES 64u
#define LR_GPU_BLEND 23u
struct lr_gpu_blend {uint32_t enabled,src_rgb,dst_rgb,src_alpha,dst_alpha,equation_rgb,equation_alpha;float color[4];};
#define LR_GPU_QUERY_BLOCKS 21u
#define LR_GPU_QUERY_BLOCKS_EXTENDED 30u
#define LR_GPU_QUERY_UNIFORM_EXTENTS 31u
#define LR_GPU_UNIFORM_ARRAY_DATA 32u
#define LR_GPU_QUERY_STORAGES 35u
#define LR_GPU_STORAGE_DATA 36u
#define LR_GPU_DEPTH_STATE 37u
#define LR_GPU_IMPORT_COLOR 38u // tightly packed bottom-left RGBA8, default target only
#define LR_GPU_RASTER_STATE 39u
#define LR_GPU_IMPORT_FRAME 40u // flags=GL_RGBA8/GL_RGBA16F, bottom-left native pixels
#define LR_GPU_READ_FRAME 41u
#define LR_GPU_IMPORT_DEPTH 42u
#define LR_GPU_READ_DEPTH 43u
#define LR_GPU_IMPORT_MRT 44u
#define LR_GPU_READ_MRT 45u
#define LR_GPU_MAX_COLOR_TARGETS 4u
struct lr_gpu_mrt {uint32_t count;struct {uint32_t format,mask,offset,bytes;} color[LR_GPU_MAX_COLOR_TARGETS];unsigned char pixels[];};
struct lr_gpu_raster_state {uint32_t scissor,color_mask,cull_face,front_face;int32_t x,y;uint32_t width,height,framebuffer_srgb,depth_bits;
 // Appended (requests of LR_GPU_RASTER_V1_BYTES leave these zero): polygon offset
 // (glPolygonOffset factor/units, GL_POLYGON_OFFSET_FILL) and glDepthRange.
 uint32_t extensions;float offset_factor,offset_units,depth_near,depth_far;};
#define LR_GPU_RASTER_V1_BYTES 40u
#define LR_GPU_RASTER_POLYGON_OFFSET 1u
#define LR_GPU_RASTER_DEPTH_RANGE 2u
// Additive v1 operations: old raster, draw and request layouts are unchanged.
// Stencil transfers are bottom-up, tightly packed uint8 rows of the companion
// default target. State.clear performs an explicit full-target initialization
// (not a masked/scissored GL clear); normal GL clears remain Mesa-owned.
// GPU-resident textures (additive). The Mesa bridge keeps GL render targets
// and sampled textures in Metal across draws and passes, transferring pixels
// only at GL coherence points. Ids are bridge-assigned per device file. The
// request address holds struct lr_gpu_resident; for UPLOAD/READ the full image
// follows it in GL row order (bottom row first), sized by the defined format.
#define LR_GPU_RESIDENT 49u
#define LR_GPU_MAX_RESIDENT 1024u
#define LR_GPU_RESIDENT_SAMPLER_BASE 64u   // draw sampler uniform = base + id
#define LR_GPU_RESIDENT_BUDGET (192u*1024u*1024u)
// Resident textures may exceed the per-draw upload limits (font/icon atlases).
#define LR_GPU_RESIDENT_MAX_DIMENSION 16384u   // Metal 2D limit on Apple3+ GPUs; creation failure falls back
#define LR_GPU_RESIDENT_MAX_IMAGE (64u*1024u*1024u)
#define LR_GPU_RES_DEFINE 1u   // id,width,height,format: (re)allocate; colour (0,0,0,1), depth 1, stencil 0
#define LR_GPU_RES_UPLOAD 2u
#define LR_GPU_RES_READ 3u
#define LR_GPU_RES_BIND 4u     // count colour ids (+masks), depth, stencil (0 = none); count 0 restores the companion
#define LR_GPU_RES_CLEAR 5u    // whole image: clear[] (float formats), clear_bits[] (integer), clear_depth, clear_stencil
#define LR_GPU_RES_DELETE 6u
#define LR_GPU_RES_BLIT 8u     // id=source, color[0]=destination, rect=src x0,y0,x1,y1 dst x0,y0,x1,y1 (GL coords), min_filter, REGION=scissor
#define LR_GPU_RES_SAMPLER 7u  // filters/wraps; refreshes the sampled copy after rendering
#define LR_GPU_RES_FORMAT_DEPTH 0x8cacu    // GL_DEPTH_COMPONENT32F -> Depth32Float
#define LR_GPU_RES_FORMAT_R8 0x8229u       // R8Unorm (samples as r,0,0,1 like GL)
#define LR_GPU_RES_FORMAT_RG8 0x822bu      // RG8Unorm
#define LR_GPU_RES_FORMAT_SRGB8_A8 0x8c43u // RGBA8Unorm storage, sRGB views for encode/decode
#define LR_GPU_RES_FLAG_SRGB 1u            // BIND/CLEAR: linear->sRGB encode on sRGB targets
#define LR_GPU_RES_FLAG_RAW 2u             // SAMPLER: GL_SKIP_DECODE_EXT, sample sRGB bytes raw
#define LR_GPU_RES_FLAG_REGION 4u          // CLEAR/READ/UPLOAD/BLIT: x,y,width,height (GL lower-left origin) bound it; CLEAR: masks[0] colour mask, masks[1] stencil mask
#define LR_GPU_RES_FORMAT_STENCIL 0x8d48u  // GL_STENCIL_INDEX8 -> Stencil8
struct lr_gpu_resident {uint32_t command,id,width,height,format,count,color[4],masks[4],depth,stencil,min_filter,mag_filter,wrap_s,wrap_t;float clear[4];uint32_t clear_bits[4];float clear_depth;uint32_t clear_stencil,flags,x,y;int32_t rect[8];};
static inline unsigned lr_gpu_resident_pixel_bytes(unsigned format){
 switch(format){case 0x8058u:case LR_GPU_RES_FORMAT_SRGB8_A8:return 4;case LR_GPU_RES_FORMAT_R8:return 1;case LR_GPU_RES_FORMAT_RG8:return 2;case 0x881au:case 0x8d76u:case 0x8d88u:return 8;case 0x8d70u:case 0x8d82u:return 16;
  case LR_GPU_RES_FORMAT_DEPTH:return 4;case LR_GPU_RES_FORMAT_STENCIL:return 1;default:return 0;}
}
#define LR_GPU_STENCIL_STATE 46u
#define LR_GPU_IMPORT_STENCIL 47u
#define LR_GPU_READ_STENCIL 48u
struct lr_gpu_stencil_face {uint32_t function,reference,read_mask,write_mask,fail,depth_fail,depth_pass;};
struct lr_gpu_stencil_state {uint32_t enabled,clear,clear_value;struct lr_gpu_stencil_face front,back;};
static inline int lr_gpu_stencil_op_valid(unsigned op){return op==0||op==0x1e00||op==0x1e01||op==0x1e02||op==0x1e03||op==0x150a||op==0x8507||op==0x8508;}
static inline int lr_gpu_stencil_state_valid(const struct lr_gpu_stencil_state *s){
 if(s->enabled>1||s->clear>1||s->clear_value>255)return 0;
 const struct lr_gpu_stencil_face *faces[2]={&s->front,&s->back};
 for(unsigned i=0;i<2;i++){const struct lr_gpu_stencil_face *f=faces[i];if(f->function<0x0200||f->function>0x0207||f->reference>255||f->read_mask>255||f->write_mask>255||!lr_gpu_stencil_op_valid(f->fail)||!lr_gpu_stencil_op_valid(f->depth_fail)||!lr_gpu_stencil_op_valid(f->depth_pass))return 0;}return 1;
}
struct lr_gpu_depth_state {uint32_t function,write_mask;float clear_depth;};
#define LR_GPU_UNIFORM_BLOCK_DATA 22u
#define LR_GPU_MAX_FRAMEBUFFERS 16u
#define LR_GPU_FB_BIND 1u
#define LR_GPU_FB_ATTACH 2u
#define LR_GPU_FB_QUERY 3u
#define LR_GPU_FB_DELETE 4u
#define LR_GPU_FB_DEPTH_RENDERBUFFER 5u
// RGBA color texture level zero only. Status values are GLES framebuffer enums.
struct lr_gpu_framebuffer { uint32_t command,name,texture,format,status,width,height; };
struct lr_gpu_rectangle { int32_t x,y; uint32_t width,height; };
#define LR_GPU_MAX_TEXTURES 32u
#define LR_GPU_TEXTURE_UNITS 4u
#define LR_GPU_TEXTURE_BUDGET (8u*1024u*1024u)
#define LR_GPU_TEXTURE_SUBIMAGE 1u
#define LR_GPU_TEXTURE_STATE 2u
#define LR_GPU_TEXTURE_STORAGE_CLEAR 3u
// Legacy source-format markers; all eight-bit uploads carry four RGBA bytes.
#define LR_GPU_TEXTURE_FORMAT_R8 0x1903u
#define LR_GPU_TEXTURE_FORMAT_RG8 0x8227u
#define LR_GPU_TEXTURE_FORMAT_RGB8 0x1907u
#define LR_GPU_TEXTURE_FORMAT_RGBA8 0x1908u
#define LR_GPU_TEXTURE_FORMAT_ALPHA8 0x1906u
#define LR_GPU_TEXTURE_FORMAT_LUMINANCE8 0x1909u
#define LR_GPU_TEXTURE_FORMAT_LUMINANCE_ALPHA8 0x190au
#define LR_GPU_TEXTURE_FORMAT_DEPTH_COMPONENT 0x1902u
#define LR_GPU_TEXTURE_FORMAT_RGBA16F 0x881au
#define LR_GPU_TEXTURE_FORMAT_SRGB8_ALPHA8 0x8c43u
static inline unsigned lr_gpu_texture_pixel_bytes(unsigned format){
 switch(format){
  case LR_GPU_TEXTURE_FORMAT_RGBA16F:return 8;
  case LR_GPU_TEXTURE_FORMAT_R8:case LR_GPU_TEXTURE_FORMAT_RG8:case LR_GPU_TEXTURE_FORMAT_RGB8:
  case LR_GPU_TEXTURE_FORMAT_RGBA8:case LR_GPU_TEXTURE_FORMAT_ALPHA8:case LR_GPU_TEXTURE_FORMAT_LUMINANCE8:
  case LR_GPU_TEXTURE_FORMAT_LUMINANCE_ALPHA8:case LR_GPU_TEXTURE_FORMAT_DEPTH_COMPONENT:
  case LR_GPU_TEXTURE_FORMAT_SRGB8_ALPHA8:return 4;
  default:return 0;
 }
}
static inline int lr_gpu_texture_format_valid(unsigned format){return !format||lr_gpu_texture_pixel_bytes(format)!=0;}
// New operations preserve the 88-byte v1 request and existing draw/link layouts.
// For sampler uniforms a draw packet's value[0] is a validated texture object ID.
// LR_GPU_TEXTURE count carries the source GL format (zero preserves v1 RGBA).
// Backend callback flags encode format in bits 8+, operation in bits 0..7.
struct lr_gpu_texture {
 uint32_t name,width,height,x,y,min_filter,mag_filter,wrap_s,wrap_t;
 unsigned char pixels[];
};
#define LR_GPU_TRANSFER_INFO 7u // count=CPU allocation growths, bytes=retained capacity
#define LR_GPU_CLEAR_BIT 1u
#define LR_GPU_TRIANGLES_BIT 2u
#define LR_GPU_DEPTH_CLEAR_BIT 4u
#define LR_GPU_DEPTH_TEST_BIT 8u
#define LR_GPU_CULL_BIT 16u
#define LR_GPU_LINES_BIT 64u // indexed draws: index pairs rasterised as one-pixel lines
#define LR_GPU_POINTS_BIT 128u // indexed draws: each index rasterised as a point (vertex shader writes gl_PointSize)
struct lr_gpu_vertex { float position[4], color[4]; };
struct lr_gpu_request {
    uint32_t version, operation, width, height;
    uint32_t count, flags, reserved[2];
    uint64_t address, bytes;
    float clear[4];
    uint64_t submissions, readbacks, gpu_nanoseconds;
};
#ifdef __cplusplus
static_assert(sizeof(struct lr_gpu_vertex)==32,"vertex ABI");
static_assert(sizeof(struct lr_gpu_request)==88,"request ABI");
extern "C" {
#else
_Static_assert(sizeof(struct lr_gpu_vertex)==32,"vertex ABI");
_Static_assert(sizeof(struct lr_gpu_request)==88,"request ABI");
#endif
#ifndef LR_GPU_GUEST
struct lr_gpu_backend {
    void *(*create)(unsigned width, unsigned height);
    int (*draw)(void *, const struct lr_gpu_vertex *, unsigned count, unsigned flags, const float clear[4]);
    int (*read)(void *, void *, unsigned width, unsigned height, unsigned x11_bgra);
    int (*info)(void *, uint64_t *, uint64_t *, uint64_t *);
    void (*destroy)(void *);
    int (*finish)(void *);
    int (*compile_shader)(void *,struct lr_gl_compile *);
    int (*link_program)(void *,struct lr_gl_link *);
    int (*delete_object)(void *,unsigned operation,unsigned name);
    int (*draw_program)(void *,const struct lr_gl_draw *,unsigned flags);
    int (*resize)(void *,unsigned width,unsigned height);
    int (*texture)(void *,const struct lr_gpu_texture *,unsigned flags);
    int (*mipmap)(void *,unsigned name);
    int (*resource_info)(void *,struct lr_gpu_request *);
    int (*read_region)(void *,void *,const struct lr_gpu_rectangle *);
    int (*viewport)(void *,const struct lr_gpu_rectangle *);
    int (*framebuffer)(void *,struct lr_gpu_framebuffer *);
    int (*query_blocks)(void *,struct lr_gl_blocks *);
    int (*uniform_blocks)(void *,const struct lr_gl_block_data *,size_t);
    int (*blend)(void *,const struct lr_gpu_blend *);
    int (*draw_instanced)(void *,const struct lr_gl_draw *,unsigned,unsigned);
    int (*uniform_info)(void *,struct lr_gpu_request *);
    int (*renderbuffer)(void *,struct lr_gpu_renderbuffer *);
    int (*copy_info)(void *,struct lr_gpu_request *);
    int (*draw_indexed)(void *,const struct lr_gl_draw *,const uint32_t *,unsigned,unsigned,unsigned);
    int (*instance_attributes)(void *,const struct lr_gpu_instance_attributes *);
    int (*uniform_extents)(void *,struct lr_gl_uniform_extents *);
    int (*uniform_array)(void *,const struct lr_gl_uniform_array_data *,size_t);
    int (*draw_indexed_base)(void *,const struct lr_gl_draw *,const uint32_t *,unsigned,unsigned,unsigned,unsigned);
    int (*query_storages)(void *,struct lr_gl_storages *);
    int (*storage_data)(void *,const struct lr_gl_storage_data *,size_t);
    int (*depth_state)(void *,const struct lr_gpu_depth_state *);
    int (*import_color)(void *,const void *,unsigned,unsigned);
    int (*raster_state)(void *,const struct lr_gpu_raster_state *);
    int (*import_frame)(void *,const void *,unsigned,unsigned,unsigned);
    int (*read_frame)(void *,void *,unsigned,unsigned,unsigned);
    int (*depth_transfer)(void *,void *,unsigned,unsigned,unsigned);
    int (*import_mrt)(void *,const struct lr_gpu_mrt *,size_t,unsigned,unsigned);
    int (*read_mrt)(void *,void *,unsigned,unsigned,unsigned,unsigned);
    int (*stencil_state)(void *,const struct lr_gpu_stencil_state *);
    int (*stencil_transfer)(void *,void *,unsigned,unsigned,unsigned);
    int (*resident)(void *,const struct lr_gpu_resident *,void *pixels,size_t pixel_bytes);
};
// Register once, before guest execution. NULL leaves the device unavailable.
void lr_gpu_install(const struct lr_gpu_backend *backend);
#endif
#ifdef __cplusplus
}
#endif
#endif
