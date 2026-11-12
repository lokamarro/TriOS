#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "fs/dev.h"
#include "graphics/guest_gpu.h"
static _Atomic(const struct lr_gpu_backend *) provider;
static atomic_uint active_contexts;
// Default color + depth + optional stencil targets are additional to per-context texture/RB quotas.
// Reserve before Metal allocation; failed create/resize leaves accounting intact.
static atomic_uint surface_storage;
#define LR_GPU_SURFACE_BUDGET (64u*1024u*1024u)
static int reserve_surface(unsigned bytes){unsigned old=atomic_load(&surface_storage);do{if(bytes>LR_GPU_SURFACE_BUDGET-old)return _ENOMEM;}while(!atomic_compare_exchange_weak(&surface_storage,&old,old+bytes));return 0;}
static atomic_uint gpu_errors;
#define LR_GPU_MAX_CONTEXTS 32
void lr_gpu_install(const struct lr_gpu_backend *backend) {
    const struct lr_gpu_backend *expected=NULL;
    // Never swap a provider underneath live file descriptors.
    if (backend) atomic_compare_exchange_strong(&provider, &expected, backend);
}
struct gpu_file {
    const struct lr_gpu_backend *backend;
    void *context;
    unsigned width,height,draw_width,draw_height,surface_bytes,surface_bpp,stencil_bpp,mrt_count;
    // Serialized by fd->lock; dup/fork share the buffer with this open.
    // Backends must copy draw input before returning (as with the former malloc/free).
    void *transfer;
    size_t transfer_capacity;
    unsigned transfer_allocations;
};
static int gpu_reserve_transfer(struct gpu_file *file, size_t bytes) {
    if (bytes <= file->transfer_capacity) return 0;
    // Requests have already been bounded and validated by their operation.
    // Keep the old allocation intact on failure; never expose it across opens.
    void *buffer = realloc(file->transfer, bytes);
    if (!buffer) return _ENOMEM;
    file->transfer = buffer;
    file->transfer_capacity = bytes;
    file->transfer_allocations++;
    return 0;
}
static int gpu_open(int major, int minor, struct fd *fd) {
    (void)major;
    const struct lr_gpu_backend *backend=atomic_load(&provider);
    if (minor || !backend) return _ENODEV;
    struct gpu_file *file=calloc(1,sizeof *file);
    if (!file) return _ENOMEM;
    file->backend=backend;file->stencil_bpp=backend->stencil_transfer?1:0;fd->data=file;return 0;
}
static ssize_t gpu_ioctl_size(int cmd) { return (uint32_t)cmd==LR_GPU_IOCTL?sizeof(struct lr_gpu_request):-1; }
static int gpu_ioctl(struct fd *fd, int cmd, void *arg) {
    if ((uint32_t)cmd!=LR_GPU_IOCTL) return _ENOTTY;
    struct lr_gpu_request *r=arg;
    if (r->version!=LR_GPU_VERSION || r->reserved[0] || r->reserved[1]) return _EINVAL;
    struct gpu_file *file=fd->data;
    if (!file) return _ENODEV;
    // Each open owns an isolated context. dup/fork share that open's context,
    // serialized through its fd lock; unrelated contexts do not serialize.
    lock(&fd->lock);
    int result=0;
    switch(r->operation) {
    case LR_GPU_CREATE:
        if (file->context) { result=_EBUSY;break; }
        if (!r->width || !r->height || r->width>LR_GPU_MAX_DIMENSION || r->height>LR_GPU_MAX_DIMENSION || r->flags) {result=_EINVAL;break;}
        unsigned active=atomic_load(&active_contexts);
        do { if(active>=LR_GPU_MAX_CONTEXTS) {result=_ENOMEM;break;} }
        while(!atomic_compare_exchange_weak(&active_contexts,&active,active+1));
        if(result)break;
        unsigned storage=r->width*r->height*(8+file->stencil_bpp);
        result=reserve_surface(storage);if(result){atomic_fetch_sub(&active_contexts,1);break;}
        file->context=file->backend->create(r->width,r->height);
        if (!file->context) {atomic_fetch_sub(&surface_storage,storage);atomic_fetch_sub(&active_contexts,1);result=_EIO;break;}
        file->surface_bytes=storage;file->surface_bpp=8;
        file->width=file->draw_width=r->width;file->height=file->draw_height=r->height;break;
    case LR_GPU_DRAW: {
        if (!file->context) {result=_ENODEV;break;}
        if (r->flags & ~(LR_GPU_CLEAR_BIT|LR_GPU_TRIANGLES_BIT|LR_GPU_DEPTH_CLEAR_BIT|LR_GPU_DEPTH_TEST_BIT|LR_GPU_CULL_BIT) || !(r->flags&(LR_GPU_CLEAR_BIT|LR_GPU_TRIANGLES_BIT|LR_GPU_DEPTH_CLEAR_BIT)) || r->count>LR_GPU_MAX_VERTICES || r->count%3 ||
            ((r->flags&LR_GPU_TRIANGLES_BIT) && !r->count) || (!(r->flags&LR_GPU_TRIANGLES_BIT) && r->count)) {result=_EINVAL;break;}
        for(unsigned i=0;i<4;i++) if(!isfinite(r->clear[i])) result=_EINVAL;
        if(result) break;
        size_t bytes=(size_t)r->count*sizeof(struct lr_gpu_vertex);
        if(r->bytes!=bytes) {result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,bytes);
        if(result)break;
        struct lr_gpu_vertex *vertices=bytes?file->transfer:NULL;
        if(bytes && user_read(r->address,vertices,bytes)) result=_EFAULT;
        if(!result) for(unsigned i=0;i<r->count;i++) for(unsigned j=0;j<4;j++)
            if(!isfinite(vertices[i].position[j]) || !isfinite(vertices[i].color[j])) result=_EINVAL;
        if(!result) result=file->backend->draw(file->context,vertices,r->count,r->flags,r->clear);
        break;
    }
    case LR_GPU_READ:
    case LR_GPU_READ_X11: {
        if(file->surface_bpp!=8){result=_EOPNOTSUPP;break;}
        if (!file->context) {result=_ENODEV;break;}
        unsigned w=r->operation==LR_GPU_READ_X11?file->width:file->draw_width,h=r->operation==LR_GPU_READ_X11?file->height:file->draw_height;
        size_t bytes=(size_t)w*h*4;
        if(r->bytes!=bytes || r->width!=w || r->height!=h || r->flags) {result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,bytes);
        if(result)break;
        void *pixels=file->transfer;
        result=file->backend->read(file->context,pixels,w,h,r->operation==LR_GPU_READ_X11);
        if(!result && user_write(r->address,pixels,bytes)) result=_EFAULT;
        break;
    }
    case LR_GPU_RESIZE: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->resize){result=_EOPNOTSUPP;break;}
        if(r->flags||!r->width||!r->height||r->width>LR_GPU_MAX_DIMENSION||r->height>LR_GPU_MAX_DIMENSION){result=_EINVAL;break;}
        unsigned storage=r->width*r->height*(file->surface_bpp+file->stencil_bpp),growth=storage>file->surface_bytes?storage-file->surface_bytes:0;
        if(growth&&(result=reserve_surface(growth)))break;
        result=file->backend->resize(file->context,r->width,r->height);
        if(result){if(growth)atomic_fetch_sub(&surface_storage,growth);break;}
        if(storage<file->surface_bytes)atomic_fetch_sub(&surface_storage,file->surface_bytes-storage);file->surface_bytes=storage;
        if(!result){file->width=r->width;file->height=r->height;
            if(file->backend->framebuffer){struct lr_gpu_framebuffer p={.command=LR_GPU_FB_QUERY};result=file->backend->framebuffer(file->context,&p);file->draw_width=p.width;file->draw_height=p.height;}else{file->draw_width=r->width;file->draw_height=r->height;}}break;
    }
    case LR_GPU_COMPILE_SHADER:
    case LR_GPU_COMPILE_SHADER_EXTENDED:
    case LR_GPU_LINK_PROGRAM: {
        if(!file->context){result=_ENODEV;break;}
        int compile=r->operation!=LR_GPU_LINK_PROGRAM,legacy=r->operation==LR_GPU_COMPILE_SHADER;
        size_t bytes=compile?(legacy?sizeof(struct lr_gl_compile_v1):sizeof(struct lr_gl_compile)):sizeof(struct lr_gl_link);
        if(r->bytes!=bytes||r->flags){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,compile?sizeof(struct lr_gl_compile)+sizeof(struct lr_gl_compile_v1):bytes);if(result)break;
        if(legacy){struct lr_gl_compile *p=file->transfer;struct lr_gl_compile_v1 *old=(void *)(p+1);if(user_read(r->address,old,bytes)){result=_EFAULT;break;}memset(p,0,sizeof(*p));memcpy(p,old,16);memcpy(p->source,old->source,LR_GL_LEGACY_SOURCE);}
        else if(user_read(r->address,file->transfer,bytes)){result=_EFAULT;break;}
        if(compile){struct lr_gl_compile *p=file->transfer;
            if(!file->backend->compile_shader){result=_EOPNOTSUPP;break;}
            if(!p->name||p->name>LR_GL_MAX_SHADERS||p->length>=(legacy?LR_GL_LEGACY_SOURCE:LR_GL_MAX_SOURCE)||p->source[p->length]||memchr(p->source,0,p->length)||
               (p->stage!=0x8b31&&p->stage!=0x8b30)){result=_EINVAL;break;}
            result=file->backend->compile_shader(file->context,p);
        }else{struct lr_gl_link *p=file->transfer;
            if(!file->backend->link_program){result=_EOPNOTSUPP;break;}
            if(!p->name||p->name>LR_GL_MAX_PROGRAMS||!p->vertex||p->vertex>LR_GL_MAX_SHADERS||!p->fragment||p->fragment>LR_GL_MAX_SHADERS||p->attributes>LR_GL_MAX_ATTRIBUTES){result=_EINVAL;break;}
            for(unsigned i=0;i<p->attributes;i++)if(!memchr(p->attribute[i].name,0,64)||p->attribute[i].location>=LR_GL_MAX_ATTRIBUTES)result=_EINVAL;
            if(!result)result=file->backend->link_program(file->context,p);
        }
        if(!result&&legacy){struct lr_gl_compile *p=file->transfer;struct lr_gl_compile_v1 *old=(void *)(p+1);old->status=p->status;memcpy(old->log,p->log,sizeof old->log);if(user_write(r->address,old,bytes))result=_EFAULT;}
        else if(!result&&user_write(r->address,file->transfer,bytes))result=_EFAULT;
        break;
    }
    case LR_GPU_IMPORT_MRT: {
        if(!file->context){result=_ENODEV;break;}if(!file->backend->import_mrt){result=_EOPNOTSUPP;break;}
        if(r->flags||r->width!=file->width||r->height!=file->height||r->bytes<sizeof(struct lr_gpu_mrt)||r->bytes>LR_GPU_SURFACE_BUDGET){result=_EINVAL;break;}
        struct lr_gpu_mrt header;if(user_read(r->address,&header,sizeof header)){result=_EFAULT;break;}
        if(!header.count||header.count>LR_GPU_MAX_COLOR_TARGETS){result=_EINVAL;break;}
        size_t at=sizeof header;unsigned bpp=4;
        for(unsigned i=0;i<header.count;i++){unsigned pixel=header.color[i].format==0x8058?4:(header.color[i].format==0x881a||header.color[i].format==0x8d76||header.color[i].format==0x8d88)?8:(header.color[i].format==0x8d70||header.color[i].format==0x8d82)?16:0;
            size_t bytes=(size_t)file->width*file->height*pixel;if(!pixel||header.color[i].mask>15||header.color[i].offset!=at||header.color[i].bytes!=bytes||bytes>r->bytes-at){result=_EINVAL;break;}at+=bytes;bpp+=pixel;}
        if(result)break;if(at!=r->bytes){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,r->bytes);if(result)break;if(user_read(r->address,file->transfer,r->bytes)){result=_EFAULT;break;}
        // Prevent a concurrent guest edit from changing the validated offsets.
        if(memcmp(file->transfer,&header,sizeof header)){result=_EINVAL;break;}
        unsigned storage=file->width*file->height*(bpp+file->stencil_bpp),growth=storage>file->surface_bytes?storage-file->surface_bytes:0;
        if(growth&&(result=reserve_surface(growth)))break;result=file->backend->import_mrt(file->context,file->transfer,r->bytes,file->width,file->height);
        if(result){if(growth)atomic_fetch_sub(&surface_storage,growth);break;}if(storage<file->surface_bytes)atomic_fetch_sub(&surface_storage,file->surface_bytes-storage);file->surface_bytes=storage;file->surface_bpp=bpp;file->mrt_count=header.count;break;
    }
    case LR_GPU_READ_MRT: {
        if(!file->context){result=_ENODEV;break;}if(!file->backend->read_mrt){result=_EOPNOTSUPP;break;}
        unsigned pixel=r->flags==0x8058?4:(r->flags==0x881a||r->flags==0x8d76||r->flags==0x8d88)?8:(r->flags==0x8d70||r->flags==0x8d82)?16:0;
        if(!pixel||r->width!=file->width||r->height!=file->height||r->count>=file->mrt_count||r->bytes!=(size_t)file->width*file->height*pixel){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,r->bytes);if(result)break;result=file->backend->read_mrt(file->context,file->transfer,r->count,file->width,file->height,r->flags);if(!result&&user_write(r->address,file->transfer,r->bytes))result=_EFAULT;break;
    }
    case LR_GPU_RESIDENT: {
        // GPU-resident textures: header, then (UPLOAD/READ) the whole image.
        // The backend validates the image size against the defined texture.
        if(!file->context){result=_ENODEV;break;}if(!file->backend->resident){result=_EOPNOTSUPP;break;}
        const size_t header_bytes=sizeof(struct lr_gpu_resident),image_limit=(size_t)LR_GPU_RESIDENT_MAX_IMAGE;
        if(r->flags||r->bytes<header_bytes||r->bytes-header_bytes>image_limit){result=_EINVAL;break;}
        struct lr_gpu_resident header;if(user_read(r->address,&header,header_bytes)){result=_EFAULT;break;}
        int moves_pixels=header.command==LR_GPU_RES_UPLOAD||header.command==LR_GPU_RES_READ;
        if(header.command<LR_GPU_RES_DEFINE||header.command>LR_GPU_RES_BLIT||(!moves_pixels&&r->bytes!=header_bytes)){result=_EINVAL;break;}
        size_t pixel_bytes=r->bytes-header_bytes;
        result=gpu_reserve_transfer(file,r->bytes);if(result)break;
        memcpy(file->transfer,&header,header_bytes);
        if(header.command==LR_GPU_RES_UPLOAD&&user_read(r->address+header_bytes,(char *)file->transfer+header_bytes,pixel_bytes)){result=_EFAULT;break;}
        result=file->backend->resident(file->context,file->transfer,(char *)file->transfer+header_bytes,pixel_bytes);
        if(!result&&header.command==LR_GPU_RES_READ&&user_write(r->address+header_bytes,(char *)file->transfer+header_bytes,pixel_bytes))result=_EFAULT;
        break;
    }
    case LR_GPU_IMPORT_DEPTH:
    case LR_GPU_READ_DEPTH: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->depth_transfer){result=_EOPNOTSUPP;break;}
        size_t bytes=(size_t)file->width*file->height*4;
        if(r->flags||r->width!=file->width||r->height!=file->height||r->bytes!=bytes){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,bytes);if(result)break;
        if(r->operation==LR_GPU_IMPORT_DEPTH){if(user_read(r->address,file->transfer,bytes)){result=_EFAULT;break;}float *p=file->transfer;for(size_t i=0;i<bytes/4;i++)if(!isfinite(p[i])||p[i]<0||p[i]>1){result=_EINVAL;break;}}
        if(result)break;
        result=file->backend->depth_transfer(file->context,file->transfer,file->width,file->height,r->operation==LR_GPU_READ_DEPTH);
        if(!result&&r->operation==LR_GPU_READ_DEPTH&&user_write(r->address,file->transfer,bytes))result=_EFAULT;
        break;
    }
    case LR_GPU_IMPORT_FRAME:
    case LR_GPU_READ_FRAME: {
        if(!file->context){result=_ENODEV;break;}
        unsigned bpp=r->flags==0x8058?4:(r->flags==0x881a||r->flags==0x8d76||r->flags==0x8d88)?8:0;
        if(!bpp||r->width!=file->width||r->height!=file->height||r->bytes!=(size_t)file->width*file->height*bpp){result=_EINVAL;break;}
        if(r->operation==LR_GPU_READ_FRAME&&file->surface_bpp!=bpp+4){result=_EINVAL;break;}
        if(r->operation==LR_GPU_IMPORT_FRAME?!file->backend->import_frame:!file->backend->read_frame){result=_EOPNOTSUPP;break;}
        result=gpu_reserve_transfer(file,r->bytes);if(result)break;
        if(r->operation==LR_GPU_IMPORT_FRAME){
            if(user_read(r->address,file->transfer,r->bytes)){result=_EFAULT;break;}
            unsigned storage=file->width*file->height*(bpp+4+file->stencil_bpp),growth=storage>file->surface_bytes?storage-file->surface_bytes:0;
            if(growth&&(result=reserve_surface(growth)))break;
            result=file->backend->import_frame(file->context,file->transfer,file->width,file->height,r->flags);
            if(result){if(growth)atomic_fetch_sub(&surface_storage,growth);break;}
            if(storage<file->surface_bytes)atomic_fetch_sub(&surface_storage,file->surface_bytes-storage);
            file->surface_bytes=storage;file->surface_bpp=bpp+4;file->mrt_count=0;
        }else{result=file->backend->read_frame(file->context,file->transfer,file->width,file->height,r->flags);if(!result&&user_write(r->address,file->transfer,r->bytes))result=_EFAULT;}
        break;
    }
    case LR_GPU_STENCIL_STATE: {
        if(!file->context||!file->backend->stencil_state){result=file->context?_EOPNOTSUPP:_ENODEV;break;}
        struct lr_gpu_stencil_state p;if(r->flags||r->bytes!=sizeof p){result=_EINVAL;break;}if(user_read(r->address,&p,sizeof p)){result=_EFAULT;break;}if(!lr_gpu_stencil_state_valid(&p)){result=_EINVAL;break;}result=file->backend->stencil_state(file->context,&p);break;
    }
    case LR_GPU_IMPORT_STENCIL:
    case LR_GPU_READ_STENCIL: {
        if(!file->context||!file->backend->stencil_transfer){result=file->context?_EOPNOTSUPP:_ENODEV;break;}size_t bytes=(size_t)file->width*file->height;if(r->flags||r->width!=file->width||r->height!=file->height||r->bytes!=bytes){result=_EINVAL;break;}result=gpu_reserve_transfer(file,bytes);if(result)break;if(r->operation==LR_GPU_IMPORT_STENCIL&&user_read(r->address,file->transfer,bytes)){result=_EFAULT;break;}result=file->backend->stencil_transfer(file->context,file->transfer,file->width,file->height,r->operation==LR_GPU_READ_STENCIL);if(!result&&r->operation==LR_GPU_READ_STENCIL&&user_write(r->address,file->transfer,bytes))result=_EFAULT;break;
    }
    case LR_GPU_IMPORT_COLOR: {
        if(file->surface_bpp!=8){result=_EOPNOTSUPP;break;}
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->import_color){result=_EOPNOTSUPP;break;}
        size_t bytes=(size_t)file->width*file->height*4;
        if(r->flags||r->width!=file->width||r->height!=file->height||r->bytes!=bytes){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,bytes);if(result)break;
        if(user_read(r->address,file->transfer,bytes)){result=_EFAULT;break;}
        result=file->backend->import_color(file->context,file->transfer,file->width,file->height);break;
    }
    case LR_GPU_RASTER_STATE: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->raster_state){result=_EOPNOTSUPP;break;}
        struct lr_gpu_raster_state p;memset(&p,0,sizeof p);
        if(r->flags||(r->bytes!=sizeof p&&r->bytes!=LR_GPU_RASTER_V1_BYTES)){result=_EINVAL;break;}
        if(user_read(r->address,&p,r->bytes)){result=_EFAULT;break;}
        if(p.extensions&~(LR_GPU_RASTER_POLYGON_OFFSET|LR_GPU_RASTER_DEPTH_RANGE)||!isfinite(p.offset_factor)||!isfinite(p.offset_units)||
           ((p.extensions&LR_GPU_RASTER_DEPTH_RANGE)&&!(p.depth_near>=0&&p.depth_near<=1&&p.depth_far>=0&&p.depth_far<=1))){result=_EINVAL;break;}
        if(p.framebuffer_srgb>1||p.scissor>1||p.color_mask>15||p.width>INT32_MAX||p.height>INT32_MAX||
           (p.cull_face!=0x0404&&p.cull_face!=0x0405&&p.cull_face!=0x0408)||
           (p.front_face!=0x0900&&p.front_face!=0x0901)){result=_EINVAL;break;}
        result=file->backend->raster_state(file->context,&p);break;
    }
    case LR_GPU_DEPTH_STATE: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->depth_state){result=_EOPNOTSUPP;break;}
        struct lr_gpu_depth_state p;
        if(r->flags||r->bytes!=sizeof p){result=_EINVAL;break;}
        if(user_read(r->address,&p,sizeof p)){result=_EFAULT;break;}
        if(p.function<0x0200||p.function>0x0207||p.write_mask>1||!isfinite(p.clear_depth)||p.clear_depth<0||p.clear_depth>1){result=_EINVAL;break;}
        result=file->backend->depth_state(file->context,&p);break;
    }
    case LR_GPU_BLEND: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->blend){result=_EOPNOTSUPP;break;}
        struct lr_gpu_blend p;
        if(r->flags||r->bytes!=sizeof(p)){result=_EINVAL;break;}
        if(user_read(r->address,&p,sizeof(p))){result=_EFAULT;break;}
        if(p.enabled>1){result=_EINVAL;break;}
        for(unsigned i=0;i<4;i++)if(!isfinite(p.color[i])||p.color[i]<0||p.color[i]>1)result=_EINVAL;
        if(!result)result=file->backend->blend(file->context,&p);break;
    }
    case LR_GPU_QUERY_BLOCKS:
    case LR_GPU_QUERY_BLOCKS_EXTENDED: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->query_blocks){result=_EOPNOTSUPP;break;}
        int legacy=r->operation==LR_GPU_QUERY_BLOCKS;
        size_t output=legacy?sizeof(struct lr_gl_blocks_v1):sizeof(struct lr_gl_blocks);
        if(r->bytes!=output||r->flags){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,sizeof(struct lr_gl_blocks)+sizeof(struct lr_gl_blocks_v1));if(result)break;
        struct lr_gl_blocks *p=file->transfer;memset(p,0,sizeof(*p));
        if(user_read(r->address,&p->program,sizeof(p->program))){result=_EFAULT;break;}
        if(!p->program||p->program>LR_GL_MAX_PROGRAMS){result=_EINVAL;break;}
        result=file->backend->query_blocks(file->context,p);if(result)break;
        if(legacy){struct lr_gl_blocks_v1 *old=(void *)(p+1);memset(old,0,sizeof(*old));old->program=p->program;old->count=p->count;
            for(unsigned i=0;i<p->count;i++){if(p->block[i].members>LR_GL_LEGACY_BLOCK_MEMBERS){result=_EOVERFLOW;break;}memcpy(&old->block[i],&p->block[i],sizeof(old->block[i]));}
            if(!result&&user_write(r->address,old,output))result=_EFAULT;
        }else if(user_write(r->address,p,output))result=_EFAULT;break;
    }
    case LR_GPU_QUERY_STORAGES: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->query_storages){result=_EOPNOTSUPP;break;}
        struct lr_gl_storages p={0};if(r->flags||r->bytes!=sizeof(p)){result=_EINVAL;break;}
        if(user_read(r->address,&p.program,sizeof(p.program))){result=_EFAULT;break;}
        if(!p.program||p.program>LR_GL_MAX_PROGRAMS){result=_EINVAL;break;}
        result=file->backend->query_storages(file->context,&p);if(!result&&user_write(r->address,&p,sizeof(p)))result=_EFAULT;break;
    }
    case LR_GPU_STORAGE_DATA: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->storage_data){result=_EOPNOTSUPP;break;}
        if(r->flags||r->bytes<sizeof(struct lr_gl_storage_data)||r->bytes>sizeof(struct lr_gl_storage_data)+LR_GL_MAX_STORAGE_BLOCKS*LR_GL_MAX_STORAGE_BYTES){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,r->bytes);if(result)break;
        if(user_read(r->address,file->transfer,r->bytes)){result=_EFAULT;break;}
        struct lr_gl_storage_data *p=file->transfer;if(!p->program||p->program>LR_GL_MAX_PROGRAMS||p->count>LR_GL_MAX_STORAGE_BLOCKS){result=_EINVAL;break;}
        for(unsigned i=0;i<p->count;i++)if(p->block[i].offset<sizeof(*p)||p->block[i].offset>r->bytes||!p->block[i].bytes||p->block[i].bytes>LR_GL_MAX_STORAGE_BYTES||p->block[i].bytes>r->bytes-p->block[i].offset)result=_EINVAL;
        if(!result)result=file->backend->storage_data(file->context,p,r->bytes);break;
    }
    case LR_GPU_QUERY_UNIFORM_EXTENTS: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->uniform_extents){result=_EOPNOTSUPP;break;}
        struct lr_gl_uniform_extents p={0};
        if(r->flags||r->bytes!=sizeof(p)){result=_EINVAL;break;}
        if(user_read(r->address,&p.program,sizeof(p.program))){result=_EFAULT;break;}
        if(!p.program||p.program>LR_GL_MAX_PROGRAMS){result=_EINVAL;break;}
        result=file->backend->uniform_extents(file->context,&p);
        if(!result&&user_write(r->address,&p,sizeof(p)))result=_EFAULT;break;
    }
    case LR_GPU_UNIFORM_ARRAY_DATA: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->uniform_array){result=_EOPNOTSUPP;break;}
        if(r->flags||r->bytes<sizeof(struct lr_gl_uniform_array_data)||r->bytes>sizeof(struct lr_gl_uniform_array_data)+LR_GL_MAX_UNIFORM_ARRAY_BYTES){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,r->bytes);if(result)break;
        if(user_read(r->address,file->transfer,r->bytes)){result=_EFAULT;break;}
        struct lr_gl_uniform_array_data *p=file->transfer;
        if(!p->program||p->program>LR_GL_MAX_PROGRAMS||p->index>=LR_GL_MAX_UNIFORMS||!p->count||p->count>LR_GL_MAX_UNIFORM_ELEMENTS||p->first>=LR_GL_MAX_UNIFORM_ELEMENTS){result=_EINVAL;break;}
        result=file->backend->uniform_array(file->context,p,r->bytes);break;
    }
    case LR_GPU_UNIFORM_BLOCK_DATA: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->uniform_blocks){result=_EOPNOTSUPP;break;}
        if(r->flags||r->bytes<sizeof(struct lr_gl_block_data)||r->bytes>sizeof(struct lr_gl_block_data)+LR_GL_MAX_BLOCKS*LR_GL_MAX_BLOCK_BYTES){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,r->bytes);if(result)break;
        if(user_read(r->address,file->transfer,r->bytes)){result=_EFAULT;break;}
        struct lr_gl_block_data *p=file->transfer;
        if(!p->program||p->program>LR_GL_MAX_PROGRAMS||p->count>LR_GL_MAX_BLOCKS){result=_EINVAL;break;}
        for(unsigned i=0;i<p->count;i++)if(p->block[i].offset<sizeof(*p)||p->block[i].offset>r->bytes||!p->block[i].bytes||p->block[i].bytes>LR_GL_MAX_BLOCK_BYTES||p->block[i].bytes>r->bytes-p->block[i].offset)result=_EINVAL;
        if(!result)result=file->backend->uniform_blocks(file->context,p,r->bytes);break;
    }
    case LR_GPU_DELETE_SHADER:
    case LR_GPU_DELETE_PROGRAM:
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->delete_object){result=_EOPNOTSUPP;break;}
        if(r->flags||!r->count||r->count>(r->operation==LR_GPU_DELETE_SHADER?LR_GL_MAX_SHADERS:LR_GL_MAX_PROGRAMS)){result=_EINVAL;break;}
        result=file->backend->delete_object(file->context,r->operation,r->count);break;
    case LR_GPU_INSTANCE_ATTRIBUTES: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->instance_attributes){result=_EOPNOTSUPP;break;}
        if((r->flags&~LR_GPU_TYPED_DATA_BIT)||r->bytes!=sizeof(struct lr_gpu_instance_attributes)){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,r->bytes);if(result)break;
        struct lr_gpu_instance_attributes *p=file->transfer;if(user_read(r->address,p,r->bytes)){result=_EFAULT;break;}
        if(!p->instances||p->instances>LR_GPU_MAX_INSTANCES){result=_EINVAL;break;}
        for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++){uint64_t n=p->divisor[a]?((uint64_t)p->instances+p->divisor[a]-1)/p->divisor[a]:0;if(p->count[a]!=n)result=_EINVAL;else if(!(r->flags&LR_GPU_TYPED_DATA_BIT))for(unsigned i=0;i<n;i++)for(unsigned j=0;j<4;j++)if(!isfinite(p->value[a][i][j]))result=_EINVAL;}
        if(!result)result=file->backend->instance_attributes(file->context,p);break;
    }
    case LR_GPU_DRAW_INDEXED:
    case LR_GPU_DRAW_INDEXED_BASE: {
        int baseDraw=r->operation==LR_GPU_DRAW_INDEXED_BASE;
        if(!file->context){result=_ENODEV;break;}
        if(baseDraw?!file->backend->draw_indexed_base:!file->backend->draw_indexed){result=_EOPNOTSUPP;break;}
        if(r->flags&~(LR_GPU_DEPTH_TEST_BIT|LR_GPU_CULL_BIT|LR_GPU_TYPED_DATA_BIT|LR_GPU_LINES_BIT|LR_GPU_POINTS_BIT)||((r->flags&(LR_GPU_LINES_BIT|LR_GPU_POINTS_BIT))&&(r->flags&LR_GPU_CULL_BIT))||((r->flags&LR_GPU_LINES_BIT)&&(r->flags&LR_GPU_POINTS_BIT))||!r->count||r->count>LR_GPU_MAX_VERTICES||!r->width||r->width>LR_GPU_MAX_INSTANCES||!r->height||r->height>LR_GPU_MAX_VERTICES||r->height%((r->flags&LR_GPU_POINTS_BIT)?1:(r->flags&LR_GPU_LINES_BIT)?2:3)){result=_EINVAL;break;}
        size_t vertexBytes=sizeof(struct lr_gl_draw)+(size_t)r->count*sizeof(struct lr_gl_vertex);
        size_t bytes=vertexBytes+(size_t)r->height*sizeof(uint32_t)+(baseDraw?4:0);
        if(r->bytes!=bytes){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,bytes);if(result)break;
        if(user_read(r->address,file->transfer,bytes)){result=_EFAULT;break;}
        struct lr_gl_draw *p=file->transfer;const uint32_t *indices=(const void *)((char *)p+vertexBytes);
        if(p->count!=r->count||!p->program||p->program>LR_GL_MAX_PROGRAMS){result=_EINVAL;break;}
        for(unsigned i=0;i<r->height;i++)if(indices[i]>=r->count)result=_EINVAL;
        if(!(r->flags&LR_GPU_TYPED_DATA_BIT))for(unsigned i=0;i<LR_GL_MAX_UNIFORMS;i++)for(unsigned j=0;j<16;j++)if(!isfinite(p->uniform[i][j]))result=_EINVAL;
        if(!(r->flags&LR_GPU_TYPED_DATA_BIT))for(unsigned i=0;i<p->count;i++)for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)for(unsigned j=0;j<4;j++)if(!isfinite(p->vertex[i].attribute[a][j]))result=_EINVAL;
        unsigned base=0;if(baseDraw){memcpy(&base,(const char *)file->transfer+bytes-4,4);if(base>UINT32_MAX-r->width)result=_EINVAL;}
        if(!result)result=baseDraw?file->backend->draw_indexed_base(file->context,p,indices,r->height,r->flags,r->width,base):file->backend->draw_indexed(file->context,p,indices,r->height,r->flags,r->width);break;
    }
    case LR_GPU_DRAW_INSTANCED:
    case LR_GPU_DRAW_PROGRAM: {
        if(!file->context){result=_ENODEV;break;}
        if(r->operation==LR_GPU_DRAW_INSTANCED){if(!file->backend->draw_instanced){result=_EOPNOTSUPP;break;}if(!r->width||r->width>LR_GPU_MAX_INSTANCES){result=_EINVAL;break;}}
        else if(!file->backend->draw_program){result=_EOPNOTSUPP;break;}
        if(r->flags&~(LR_GPU_DEPTH_TEST_BIT|LR_GPU_CULL_BIT)||!r->count||r->count>LR_GPU_MAX_VERTICES||r->count%3){result=_EINVAL;break;}
        size_t bytes=sizeof(struct lr_gl_draw)+(size_t)r->count*sizeof(struct lr_gl_vertex);
        if(r->bytes!=bytes){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,bytes);if(result)break;
        if(user_read(r->address,file->transfer,bytes)){result=_EFAULT;break;}
        struct lr_gl_draw *p=file->transfer;
        if(p->count!=r->count||!p->program||p->program>LR_GL_MAX_PROGRAMS){result=_EINVAL;break;}
        for(unsigned i=0;i<LR_GL_MAX_UNIFORMS;i++)for(unsigned j=0;j<16;j++)if(!isfinite(p->uniform[i][j]))result=_EINVAL;
        for(unsigned i=0;i<p->count;i++)for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)for(unsigned j=0;j<4;j++)if(!isfinite(p->vertex[i].attribute[a][j]))result=_EINVAL;
        if(!result)result=r->operation==LR_GPU_DRAW_INSTANCED?file->backend->draw_instanced(file->context,p,r->flags,r->width):file->backend->draw_program(file->context,p,r->flags);break;
    }
    case LR_GPU_TEXTURE: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->texture){result=_EOPNOTSUPP;break;}
        if(r->flags!=0&&r->flags!=LR_GPU_TEXTURE_SUBIMAGE&&r->flags!=LR_GPU_TEXTURE_STATE&&r->flags!=LR_GPU_TEXTURE_STORAGE_CLEAR){result=_EINVAL;break;}
        if((r->flags==LR_GPU_TEXTURE_STORAGE_CLEAR?(r->width||r->height):(!r->width||!r->height))||r->width>LR_GPU_MAX_DIMENSION||r->height>LR_GPU_MAX_DIMENSION){result=_EINVAL;break;}
        unsigned pixel_bytes=lr_gpu_texture_pixel_bytes(r->count?r->count:LR_GPU_TEXTURE_FORMAT_RGBA8);
        if(!lr_gpu_texture_format_valid(r->count)){result=_EINVAL;break;}
        size_t bytes=sizeof(struct lr_gpu_texture)+((r->flags==LR_GPU_TEXTURE_STATE||r->flags==LR_GPU_TEXTURE_STORAGE_CLEAR)?0:(size_t)r->width*r->height*pixel_bytes);
        if(r->bytes!=bytes){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,bytes);if(result)break;
        if(user_read(r->address,file->transfer,bytes)){result=_EFAULT;break;}
        struct lr_gpu_texture *p=file->transfer;
        if(p->name>LR_GPU_MAX_TEXTURES||p->width!=r->width||p->height!=r->height||p->x>LR_GPU_MAX_DIMENSION||p->y>LR_GPU_MAX_DIMENSION||
           !(p->min_filter==0x2600||p->min_filter==0x2601||(p->min_filter>=0x2700&&p->min_filter<=0x2703))||
           !(p->mag_filter==0x2600||p->mag_filter==0x2601)||
           !(p->wrap_s==0x2901||p->wrap_s==0x812f||p->wrap_s==0x8370)||!(p->wrap_t==0x2901||p->wrap_t==0x812f||p->wrap_t==0x8370)||
           (r->flags!=LR_GPU_TEXTURE_SUBIMAGE&&(p->x||p->y))){result=_EINVAL;break;}
        if(r->count==LR_GPU_TEXTURE_FORMAT_DEPTH_COMPONENT&&r->flags!=LR_GPU_TEXTURE_STATE&&r->flags!=LR_GPU_TEXTURE_STORAGE_CLEAR){const float *depth=(const void *)p->pixels;for(size_t i=0;i<(size_t)p->width*p->height;i++)if(!isfinite(depth[i])||depth[i]<0||depth[i]>1){result=_EINVAL;break;}}if(!result)result=file->backend->texture(file->context,p,r->flags|(r->count<<8));break;
    }
    case LR_GPU_DELETE_TEXTURE:
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->delete_object){result=_EOPNOTSUPP;break;}
        if(r->flags||r->count>LR_GPU_MAX_TEXTURES){result=_EINVAL;break;}
        result=file->backend->delete_object(file->context,r->operation,r->count);break;
    case LR_GPU_GENERATE_MIPMAP:
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->mipmap){result=_EOPNOTSUPP;break;}
        if(r->flags||r->count>LR_GPU_MAX_TEXTURES){result=_EINVAL;break;}
        result=file->backend->mipmap(file->context,r->count);break;
    case LR_GPU_VIEWPORT:
    case LR_GPU_READ_REGION: {
        if(r->operation==LR_GPU_READ_REGION&&file->surface_bpp!=8){result=_EOPNOTSUPP;break;}
        if(!file->context){result=_ENODEV;break;}
        if(r->flags){result=_EINVAL;break;}
        size_t bytes=sizeof(struct lr_gpu_rectangle);
        if(r->operation==LR_GPU_READ_REGION){
            if(!file->backend->read_region){result=_EOPNOTSUPP;break;}
            if(!r->width||!r->height||r->width>file->draw_width||r->height>file->draw_height){result=_EINVAL;break;}
            bytes+=(size_t)r->width*r->height*4;
        }else if(!file->backend->viewport){result=_EOPNOTSUPP;break;}
        if(r->bytes!=bytes){result=_EINVAL;break;}
        result=gpu_reserve_transfer(file,bytes);if(result)break;
        struct lr_gpu_rectangle *area=file->transfer;
        if(user_read(r->address,area,sizeof *area)){result=_EFAULT;break;}
        if(area->width>LR_GPU_MAX_DIMENSION||area->height>LR_GPU_MAX_DIMENSION){result=_EINVAL;break;}
        if(r->operation==LR_GPU_VIEWPORT)result=file->backend->viewport(file->context,area);
        else {
            if(area->x<0||area->y<0||area->width!=r->width||area->height!=r->height||
               (unsigned)area->x>file->draw_width-r->width||(unsigned)area->y>file->draw_height-r->height){result=_EINVAL;break;}
            void *pixels=area+1;
            result=file->backend->read_region(file->context,pixels,area);
            if(!result&&(r->address>UINT64_MAX-sizeof *area||user_write(r->address+sizeof *area,pixels,bytes-sizeof *area)))result=_EFAULT;
        }
        break;
    }
    case LR_GPU_FRAMEBUFFER: {
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->framebuffer){result=_EOPNOTSUPP;break;}
        struct lr_gpu_framebuffer p;
        if(r->flags||r->bytes!=sizeof p){result=_EINVAL;break;}
        if(user_read(r->address,&p,sizeof p)){result=_EFAULT;break;}
        if(p.command<LR_GPU_FB_BIND||p.command>LR_GPU_FB_DEPTH_RENDERBUFFER||p.name>LR_GPU_MAX_FRAMEBUFFERS||p.texture>LR_GPU_MAX_TEXTURES||
           (p.command==LR_GPU_FB_ATTACH&&(!p.name||(p.format!=0x1908&&p.format!=0x1907)))||
           (p.command==LR_GPU_FB_DEPTH_RENDERBUFFER&&(!p.name||p.texture>LR_GPU_MAX_RENDERBUFFERS||p.format!=0x8d00))||
           (p.command==LR_GPU_FB_QUERY&&(p.texture||(p.format&&p.format!=0x8d00)))||
           (p.command!=LR_GPU_FB_ATTACH&&p.command!=LR_GPU_FB_DEPTH_RENDERBUFFER&&p.command!=LR_GPU_FB_QUERY&&(p.texture||p.format))){result=_EINVAL;break;}
        result=file->backend->framebuffer(file->context,&p);
        if(!result){file->draw_width=p.width;file->draw_height=p.height;
            if(user_write(r->address,&p,sizeof p))result=_EFAULT;}
        break;
    }
    case LR_GPU_RENDERBUFFER: {
        if(!file->context){result=_ENODEV;break;}if(!file->backend->renderbuffer){result=_EOPNOTSUPP;break;}
        struct lr_gpu_renderbuffer p;if(r->flags||r->bytes!=sizeof(p)){result=_EINVAL;break;}if(user_read(r->address,&p,sizeof(p))){result=_EFAULT;break;}
        if(!p.name||p.name>LR_GPU_MAX_RENDERBUFFERS||p.command<LR_GPU_RB_BIND||p.command>LR_GPU_RB_QUERY||p.width>LR_GPU_MAX_DIMENSION||p.height>LR_GPU_MAX_DIMENSION||(p.command==LR_GPU_RB_STORAGE&&p.format!=0x81a5&&p.format!=0x81a6&&p.format!=0x8cac)){result=_EINVAL;break;}
        result=file->backend->renderbuffer(file->context,&p);if(!result){struct lr_gpu_framebuffer fb={.command=LR_GPU_FB_QUERY};if(file->backend->framebuffer){result=file->backend->framebuffer(file->context,&fb);file->draw_width=fb.width;file->draw_height=fb.height;}if(!result&&user_write(r->address,&p,sizeof(p)))result=_EFAULT;}break;
    }
    case LR_GPU_COPY_INFO:
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->copy_info){result=_EOPNOTSUPP;break;}
        if(r->flags){result=_EINVAL;break;}
        result=file->backend->copy_info(file->context,r);break;
    case LR_GPU_UNIFORM_BLOCK_INFO:
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->uniform_info){result=_EOPNOTSUPP;break;}
        if(r->flags){result=_EINVAL;break;}
        result=file->backend->uniform_info(file->context,r);break;
    case LR_GPU_RESOURCE_INFO:
        if(!file->context){result=_ENODEV;break;}
        if(!file->backend->resource_info){result=_EOPNOTSUPP;break;}
        if(r->flags){result=_EINVAL;break;}
        result=file->backend->resource_info(file->context,r);break;
    case LR_GPU_FINISH:
        if(r->flags){result=_EINVAL;break;}
        if(!file->context){result=_ENODEV;break;}
        result=file->backend->finish(file->context);break;
    case LR_GPU_TRANSFER_INFO:
        if(r->flags){result=_EINVAL;break;}
        if(!file->context){result=_ENODEV;break;}
        // Per-open cumulative growth count and retained CPU transfer bytes.
        // These are allocations, not GPU submissions or rendered frames.
        r->count=file->transfer_allocations;r->bytes=file->transfer_capacity;
        break;
    case LR_GPU_INFO:
        if(r->flags) {result=_EINVAL;break;}
        if (!file->context) {result=_ENODEV;break;}
        r->width=file->width;r->height=file->height;
        result=file->backend->info(file->context,&r->submissions,&r->readbacks,&r->gpu_nanoseconds);break;
    default: result=_EOPNOTSUPP;
    }
    unlock(&fd->lock);
    if(result && atomic_fetch_add(&gpu_errors,1)<32)
        printk("GPU_DEVICE_ERROR operation=%u flags=%u count=%u dimensions=%ux%u linux_errno=%d\n",r->operation,r->flags,r->count,r->width,r->height,-result);
    return result;
}
static int gpu_close(struct fd *fd) {
    struct gpu_file *file=fd->data;
    if(file) {if(file->context){file->backend->destroy(file->context);atomic_fetch_sub(&surface_storage,file->surface_bytes);atomic_fetch_sub(&active_contexts,1);}free(file->transfer);free(file);fd->data=NULL;}
    return 0;
}
struct dev_ops gpu_dev={.open=gpu_open,.fd={.ioctl_size=gpu_ioctl_size,.ioctl=gpu_ioctl,.close=gpu_close}};
