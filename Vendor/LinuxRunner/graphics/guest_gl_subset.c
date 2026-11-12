// Experimental opt-in EGL/GLES fixed-function vertical slice. Not a Mesa
// replacement, conformant GL implementation, or a driver for existing apps.
#define LR_GPU_GUEST 1
#include "guest_gl_subset.h"
#include "guest_gpu.h"
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <stdatomic.h>
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "guest_uniforms.h"
#include "guest_mapping.h"
#define DISPLAY ((void *)1)
#define CONFIG ((void *)2)
#define MAX_OBJECTS 16
struct surface { uintptr_t handle;unsigned width,height;Window window;Display *display;GC gc;XImage *image;unsigned char *pixels; };
struct lr_immediate_vertex {float position[4],color[4];};
struct lr_vertex_attribute {int enabled,size,type,stride,normalized,integer,value_kind;unsigned buffer,divisor;const char *pointer;float value[4];};
struct lr_vertex_array {unsigned reserved,bound,element;struct lr_vertex_attribute attribute[LR_GL_MAX_ATTRIBUTES];};
struct context {
 uintptr_t handle;int fd,client_version,gpu_created,desktop;long owner;EGLSurface surface;
 struct {unsigned name,stage,compiled,deleted,refs;char *source;char log[LR_GL_LOG_SIZE];} shader[LR_GL_MAX_SHADERS];
 struct {unsigned name,vertex,fragment,deleted,can_draw;struct lr_gl_link link,executable;struct lr_gl_blocks *blocks;struct lr_gl_storages storages;unsigned storage_binding[LR_GL_MAX_STORAGE_BLOCKS],storage_valid;struct {unsigned slot;uint64_t serial;size_t offset,bytes;} storage_stamp[LR_GL_MAX_STORAGE_BLOCKS];unsigned block_binding[LR_GL_MAX_BLOCKS],ubo_valid;struct {unsigned slot;uint64_t serial;size_t offset;} ubo_stamp[LR_GL_MAX_BLOCKS];struct lr_gl_variable bindings[LR_GL_MAX_ATTRIBUTES];unsigned binding_count;struct lr_gl_uniform_extents extents;unsigned char *array_values[LR_GL_MAX_UNIFORMS];float values[LR_GL_MAX_UNIFORMS][16];GLint sampler_unit[LR_GL_MAX_UNIFORMS];} program[LR_GL_MAX_PROGRAMS];
 struct lr_vertex_attribute attribute[LR_GL_MAX_ATTRIBUTES];
 struct lr_vertex_array vao[17];unsigned vertex_array;
 struct {unsigned reserved,bound,width,height,format,min_filter,mag_filter,wrap_s,wrap_t;} textures[LR_GPU_MAX_TEXTURES+1];
 unsigned immediate_count;GLenum immediate_mode;int immediate;float immediate_color[4];struct lr_immediate_vertex *immediate_data;
 unsigned renderbuffer;struct {unsigned reserved,bound;} renderbuffers[LR_GPU_MAX_RENDERBUFFERS+1];
 unsigned framebuffer;struct {unsigned reserved,bound;} framebuffers[LR_GPU_MAX_FRAMEBUFFERS+1];
 unsigned texture_unit,bound_texture[LR_GPU_TEXTURE_UNITS],unpack_alignment,pack_alignment,texture_bytes;
 struct lr_gpu_rectangle viewport;unsigned char *read_scratch;size_t read_capacity;
 uint64_t buffer_serial;
 unsigned bound_uniform,bound_storage;struct {unsigned slot,range;size_t offset,bytes;} uniform_binding[LR_GL_MAX_BLOCK_BINDINGS],storage_binding[LR_GL_MAX_BLOCK_BINDINGS];struct lr_gl_storage_data *storage_scratch;struct lr_gl_block_data *uniform_scratch;
 unsigned active_program,bound_element;struct lr_gl_draw *program_scratch;
 struct lr_gpu_depth_state depth_state;struct lr_gpu_blend blend;unsigned instance_live,base_instance;
 GLfloat clear[4];const char *vertex,*color;GLint vertex_size;GLsizei vertex_stride,color_stride;
 int vertex_enabled,color_enabled;GLenum error;
 GLfloat matrix[2][16][16], normal[3],material[4],light[4]; unsigned top[2];int matrix_mode,lighting,light0,normalize,depth,cull,flat;
 const char *normal_array;GLsizei normal_stride;int normal_enabled;
 GLenum vertex_type,color_type;unsigned vertex_buffer,color_buffer,normal_buffer,bound_buffer;
 struct {unsigned char *data;size_t size;uint64_t serial;unsigned name,occupied,deleted,mapped,map_access,usage;size_t map_offset,map_length;} buffers[32];
 struct lr_gpu_vertex *scratch;
};
static struct surface surfaces[MAX_OBJECTS];static struct context contexts[MAX_OBJECTS];
static pthread_mutex_t objects=PTHREAD_MUTEX_INITIALIZER;
static Display *native_display;
static void identity(float *m){memset(m,0,16*sizeof(float));for(int i=0;i<4;i++)m[i*5]=1;}
static uintptr_t next_handle=16;static atomic_int initialized;
static _Thread_local EGLint egl_error=EGL_SUCCESS;
static _Thread_local struct context *active;
static _Thread_local GLenum no_context_error;
static EGLBoolean fail(EGLint e) {egl_error=e;return EGL_FALSE;}
static int valid(EGLDisplay d) {if(d!=DISPLAY){fail(EGL_BAD_DISPLAY);return 0;}if(!initialized){fail(EGL_NOT_INITIALIZED);return 0;}return 1;}
static struct context *find_context(EGLContext h) {for(int i=0;i<MAX_OBJECTS;i++)if(contexts[i].handle && contexts[i].handle==(uintptr_t)h)return &contexts[i];return NULL;}
static struct surface *find_surface(EGLSurface h) {for(int i=0;i<MAX_OBJECTS;i++)if(surfaces[i].handle && surfaces[i].handle==(uintptr_t)h)return &surfaces[i];return NULL;}
static int resize_surface(struct context *,struct surface *,unsigned,unsigned);
static int framebuffer_ready(unsigned *,unsigned *);
static int program_index(GLuint);
static void buffer_changed(unsigned);
static void draw_program(GLenum,GLint,GLsizei,GLenum,const void *);
static void draw_program_instances(GLenum,GLint,GLsizei,GLenum,const void *,unsigned);
void glDrawArraysInstanced(GLenum,GLint,GLsizei,GLsizei);
void glDrawElementsInstanced(GLenum,GLsizei,GLenum,const void *,GLsizei);
static void gl_error(GLenum error) {GLenum *p=active?&active->error:&no_context_error;if(*p==GL_NO_ERROR)*p=error;}
static int submit(struct lr_gpu_request *r) {if(!active || active->owner!=syscall(SYS_gettid)){gl_error(GL_INVALID_OPERATION);return 0;}r->version=LR_GPU_VERSION;if(ioctl(active->fd,LR_GPU_IOCTL,r)<0){gl_error(errno==ENOMEM?GL_OUT_OF_MEMORY:GL_INVALID_OPERATION);return 0;}return 1;}
EGLDisplay eglGetDisplay(void *d) {native_display=(Display *)d;return DISPLAY;}
EGLBoolean eglInitialize(EGLDisplay d,EGLint *major,EGLint *minor) {
 if(d!=DISPLAY)return fail(EGL_BAD_DISPLAY);
 int fd=open("/dev/linuxrunner-gpu",O_RDWR|O_CLOEXEC);if(fd<0)return fail(EGL_NOT_INITIALIZED);close(fd);
 pthread_mutex_lock(&objects);initialized=1;pthread_mutex_unlock(&objects);
 if(major)*major=1;if(minor)*minor=1;return EGL_TRUE;
}
EGLBoolean eglChooseConfig(EGLDisplay d,const EGLint *attributes,EGLConfig *result,EGLint size,EGLint *count) {
 if(!valid(d))return EGL_FALSE;if(!count||size<0)return fail(EGL_BAD_PARAMETER);
 if(attributes)for(int i=0;attributes[i]!=EGL_NONE;i+=2){if(i>=64)return fail(EGL_BAD_ATTRIBUTE);int a=attributes[i],v=attributes[i+1];
  if(a==EGL_SURFACE_TYPE && (v&~(EGL_PBUFFER_BIT|EGL_WINDOW_BIT)))return fail(EGL_BAD_MATCH);
  else if(a==EGL_RENDERABLE_TYPE && (!v || (v&~(EGL_OPENGL_ES_BIT|EGL_OPENGL_ES2_BIT))))return fail(EGL_BAD_MATCH);
  else if(a==EGL_DEPTH_SIZE){if(v<0||v>24)return fail(EGL_BAD_MATCH);}
  else if(a==EGL_RED_SIZE||a==EGL_GREEN_SIZE||a==EGL_BLUE_SIZE||a==EGL_ALPHA_SIZE){if(v<0||v>8)return fail(EGL_BAD_MATCH);}
  else if(a!=EGL_SURFACE_TYPE&&a!=EGL_RENDERABLE_TYPE)return fail(EGL_BAD_ATTRIBUTE);
 }
 *count=1;if(result&&size)result[0]=CONFIG;return EGL_TRUE;
}
EGLBoolean eglBindAPI(EGLenum api) {return api==EGL_OPENGL_ES_API?EGL_TRUE:fail(EGL_BAD_PARAMETER);}
EGLSurface eglCreatePbufferSurface(EGLDisplay d,EGLConfig config,const EGLint *attributes) {
 if(!valid(d))return EGL_NO_SURFACE;if(config!=CONFIG){fail(EGL_BAD_CONFIG);return EGL_NO_SURFACE;}
 unsigned w=0,h=0;if(attributes)for(int i=0;attributes[i]!=EGL_NONE;i+=2){if(i>=64){fail(EGL_BAD_ATTRIBUTE);return EGL_NO_SURFACE;}
  if(attributes[i]==EGL_WIDTH)w=attributes[i+1];else if(attributes[i]==EGL_HEIGHT)h=attributes[i+1];else{fail(EGL_BAD_ATTRIBUTE);return EGL_NO_SURFACE;}}
 if(!w||!h||w>LR_GPU_MAX_DIMENSION||h>LR_GPU_MAX_DIMENSION){fail(EGL_BAD_PARAMETER);return EGL_NO_SURFACE;}
 pthread_mutex_lock(&objects);EGLSurface result=EGL_NO_SURFACE;
 for(int i=0;i<MAX_OBJECTS;i++)if(!surfaces[i].handle){surfaces[i]=(struct surface){next_handle++,w,h};result=(void *)surfaces[i].handle;break;}
 pthread_mutex_unlock(&objects);if(!result)fail(EGL_BAD_ALLOC);return result;
}
EGLContext eglCreateContext(EGLDisplay d,EGLConfig config,EGLContext shared,const EGLint *attributes) {
 if(!valid(d))return EGL_NO_CONTEXT;if(config!=CONFIG){fail(EGL_BAD_CONFIG);return EGL_NO_CONTEXT;}
 if(shared){fail(EGL_BAD_MATCH);return EGL_NO_CONTEXT;}
 int client_version=1;
 if(attributes)for(int i=0;attributes[i]!=EGL_NONE;i+=2){if(i>=64 || attributes[i]!=EGL_CONTEXT_CLIENT_VERSION || (attributes[i+1]!=1&&attributes[i+1]!=2)){fail(EGL_BAD_ATTRIBUTE);return EGL_NO_CONTEXT;}client_version=attributes[i+1];}
 int fd=open("/dev/linuxrunner-gpu",O_RDWR|O_CLOEXEC);if(fd<0){fail(EGL_BAD_ALLOC);return EGL_NO_CONTEXT;}
 pthread_mutex_lock(&objects);EGLContext result=EGL_NO_CONTEXT;
 for(int i=0;i<MAX_OBJECTS;i++)if(!contexts[i].handle){contexts[i]=(struct context){.handle=next_handle++,.fd=fd,.client_version=client_version,.unpack_alignment=4,.pack_alignment=4,.vertex_size=4,.vertex_type=GL_FLOAT,.color_type=GL_FLOAT,.normal={0,0,1},.depth_state={.function=0x0201,.write_mask=1,.clear_depth=1},.blend={.src_rgb=1,.src_alpha=1,.equation_rgb=0x8006,.equation_alpha=0x8006},.immediate_color={1,1,1,1},.material={.8,.8,.8,1},.light={0,0,1,0}};identity(contexts[i].matrix[0][0]);identity(contexts[i].matrix[1][0]);for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++){contexts[i].attribute[a].size=4;contexts[i].attribute[a].type=GL_FLOAT;contexts[i].attribute[a].stride=16;contexts[i].attribute[a].value[3]=1;}result=(void *)contexts[i].handle;break;}
 pthread_mutex_unlock(&objects);if(!result){close(fd);fail(EGL_BAD_ALLOC);}return result;
}
// Query only real provider state; unsupported attributes report EGL_BAD_ATTRIBUTE.
EGLBoolean eglQueryContext(EGLDisplay d,EGLContext handle,EGLint property,EGLint *out){
 if(!valid(d))return EGL_FALSE;if(!out)return fail(EGL_BAD_PARAMETER);
 pthread_mutex_lock(&objects);struct context *c=find_context(handle);
 if(!c){pthread_mutex_unlock(&objects);return fail(EGL_BAD_CONTEXT);}
 EGLBoolean result=EGL_TRUE;
 switch(property){case EGL_CONFIG_ID:*out=1;break;case EGL_CONTEXT_CLIENT_TYPE:*out=EGL_OPENGL_ES_API;break;case EGL_CONTEXT_CLIENT_VERSION:*out=c->client_version;break;
 case EGL_RENDER_BUFFER:{struct surface *s=find_surface(c->surface);*out=c->owner&&s?(s->window?EGL_BACK_BUFFER:EGL_SINGLE_BUFFER):EGL_NONE;break;}
 default:result=fail(EGL_BAD_ATTRIBUTE);}
 pthread_mutex_unlock(&objects);return result;
}
EGLBoolean eglQuerySurface(EGLDisplay d,EGLSurface handle,EGLint property,EGLint *out){
 if(!valid(d))return EGL_FALSE;if(!out)return fail(EGL_BAD_PARAMETER);
 pthread_mutex_lock(&objects);struct surface *s=find_surface(handle);
 if(!s){pthread_mutex_unlock(&objects);return fail(EGL_BAD_SURFACE);}
 EGLBoolean result=EGL_TRUE;
 switch(property){case EGL_CONFIG_ID:*out=1;break;case EGL_WIDTH:*out=s->width;break;case EGL_HEIGHT:*out=s->height;break;case EGL_RENDER_BUFFER:*out=s->window?EGL_BACK_BUFFER:EGL_SINGLE_BUFFER;break;
 default:result=fail(EGL_BAD_ATTRIBUTE);}
 pthread_mutex_unlock(&objects);return result;
}
EGLBoolean eglMakeCurrent(EGLDisplay d,EGLSurface draw,EGLSurface read,EGLContext handle) {
 if(!valid(d))return EGL_FALSE;pthread_mutex_lock(&objects);EGLBoolean result=EGL_FALSE;long tid=syscall(SYS_gettid);
 if(!handle){if(draw||read){fail(EGL_BAD_MATCH);goto done;}if(active)active->owner=0;active=NULL;result=EGL_TRUE;goto done;}
 struct context *c=find_context(handle);struct surface *s=find_surface(draw);
 if(!c){fail(EGL_BAD_CONTEXT);goto done;}if(!s){fail(EGL_BAD_SURFACE);goto done;}
 if(draw!=read){fail(EGL_BAD_MATCH);goto done;}
 for(int i=0;i<MAX_OBJECTS;i++)if(contexts[i].handle&&contexts[i].surface==draw&&contexts[i].owner&&contexts[i].owner!=tid){fail(EGL_BAD_ACCESS);goto done;}
 if(c->owner&&c->owner!=tid){fail(EGL_BAD_ACCESS);goto done;}
 if(c->surface && c->surface!=draw){fail(EGL_BAD_MATCH);goto done;}
 if(!c->surface){struct lr_gpu_request request={.version=LR_GPU_VERSION,.operation=c->gpu_created?LR_GPU_RESIZE:LR_GPU_CREATE,.width=s->width,.height=s->height};if(ioctl(c->fd,LR_GPU_IOCTL,&request)<0){fail(EGL_BAD_ALLOC);goto done;}if(!c->gpu_created)c->viewport=(struct lr_gpu_rectangle){0,0,s->width,s->height};c->gpu_created=1;c->surface=draw;}
 if(active&&active!=c)active->owner=0;c->owner=tid;active=c;result=EGL_TRUE;
 done:pthread_mutex_unlock(&objects);return result;
}
EGLBoolean eglDestroyContext(EGLDisplay d,EGLContext handle) {
 if(!valid(d))return EGL_FALSE;pthread_mutex_lock(&objects);struct context *c=find_context(handle);EGLBoolean result=EGL_TRUE;
 if(!c)result=fail(EGL_BAD_CONTEXT);else if(c->owner)result=fail(EGL_BAD_ACCESS);else{close(c->fd);free(c->immediate_data);free(c->scratch);free(c->program_scratch);free(c->uniform_scratch);free(c->storage_scratch);free(c->read_scratch);for(unsigned p=0;p<LR_GL_MAX_PROGRAMS;p++){free(c->program[p].blocks);for(unsigned u=0;u<LR_GL_MAX_UNIFORMS;u++)free(c->program[p].array_values[u]);}for(unsigned i=0;i<LR_GL_MAX_SHADERS;i++)free(c->shader[i].source);for(int i=0;i<32;i++)free(c->buffers[i].data);memset(c,0,sizeof *c);}
 pthread_mutex_unlock(&objects);return result;
}
EGLBoolean eglDestroySurface(EGLDisplay d,EGLSurface handle) {
 if(!valid(d))return EGL_FALSE;pthread_mutex_lock(&objects);struct surface *s=find_surface(handle);EGLBoolean result=EGL_TRUE;
 if(!s)result=fail(EGL_BAD_SURFACE);
 else {
  for(int i=0;i<MAX_OBJECTS;i++)if(contexts[i].handle&&contexts[i].surface==handle&&contexts[i].owner){result=fail(EGL_BAD_ACCESS);break;}
  if(result){
   for(int i=0;i<MAX_OBJECTS;i++)if(contexts[i].handle&&contexts[i].surface==handle){contexts[i].surface=NULL;}
   if(s->image)XDestroyImage(s->image);if(s->gc)XFreeGC(s->display,s->gc);free(s->pixels);memset(s,0,sizeof *s);
  }
 }
 pthread_mutex_unlock(&objects);return result;
}
EGLBoolean eglTerminate(EGLDisplay d) {
 if(!valid(d))return EGL_FALSE;pthread_mutex_lock(&objects);
 for(int i=0;i<MAX_OBJECTS;i++)if(contexts[i].handle||surfaces[i].handle){pthread_mutex_unlock(&objects);return fail(EGL_BAD_ACCESS);}
 initialized=0;pthread_mutex_unlock(&objects);return EGL_TRUE;
}
EGLint eglGetError(void){EGLint e=egl_error;egl_error=EGL_SUCCESS;return e;}
void glClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){if(!active){gl_error(GL_INVALID_OPERATION);return;}GLfloat v[]={r,g,b,a};for(int i=0;i<4;i++){if(!isfinite(v[i])){gl_error(GL_INVALID_VALUE);return;}active->clear[i]=fmaxf(0,fminf(1,v[i]));}}
void glClear(GLbitfield bits){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(bits&~(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT)){gl_error(GL_INVALID_VALUE);return;}if(!active->depth_state.write_mask)bits&=~GL_DEPTH_BUFFER_BIT;if(!bits)return;if(!framebuffer_ready(NULL,NULL))return;struct lr_gpu_request r={.operation=LR_GPU_DRAW,.flags=((bits&GL_COLOR_BUFFER_BIT)?LR_GPU_CLEAR_BIT:0)|((bits&GL_DEPTH_BUFFER_BIT)?LR_GPU_DEPTH_CLEAR_BIT:0)};memcpy(r.clear,active->clear,sizeof r.clear);submit(&r);}
void glViewport(GLint x,GLint y,GLsizei w,GLsizei h){
 if(!active){gl_error(GL_INVALID_OPERATION);return;}if(w<0||h<0){gl_error(GL_INVALID_VALUE);return;}
 struct surface *s=find_surface(active->surface);if(!s){gl_error(GL_INVALID_OPERATION);return;}
 // EGLUT resize events already changed the X11 window. Preserve that backing
 // surface before applying the independently specified GL viewport.
 if(!x&&!y&&w>0&&h>0&&(w!=(int)s->width||h!=(int)s->height)&&s->window&&!active->framebuffer){XWindowAttributes a;if(XGetWindowAttributes(s->display,s->window,&a)&&w==a.width&&h==a.height&&!resize_surface(active,s,w,h)){gl_error(GL_OUT_OF_MEMORY);return;}}
 struct lr_gpu_rectangle area={x,y,w>LR_GPU_MAX_DIMENSION?LR_GPU_MAX_DIMENSION:w,h>LR_GPU_MAX_DIMENSION?LR_GPU_MAX_DIMENSION:h};
 struct lr_gpu_request r={.operation=LR_GPU_VIEWPORT,.address=(uintptr_t)&area,.bytes=sizeof area};if(submit(&r))active->viewport=area;
}
void glVertexPointer(GLint size,GLenum type,GLsizei stride,const void *p){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(type!=GL_FLOAT&&type!=GL_FIXED){gl_error(GL_INVALID_ENUM);return;}if(size<2||size>4||stride<0){gl_error(GL_INVALID_VALUE);return;}active->vertex_type=type;active->vertex_buffer=active->bound_buffer;active->vertex=p;active->vertex_size=size;active->vertex_stride=stride?stride:size*sizeof(float);}
void glColorPointer(GLint size,GLenum type,GLsizei stride,const void *p){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(type!=GL_FLOAT&&type!=GL_FIXED){gl_error(GL_INVALID_ENUM);return;}if(size!=4||stride<0){gl_error(GL_INVALID_VALUE);return;}active->color_type=type;active->color_buffer=active->bound_buffer;active->color=p;active->color_stride=stride?stride:4*sizeof(float);}
void glEnableClientState(GLenum array){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(array==GL_VERTEX_ARRAY)active->vertex_enabled=1;else if(array==GL_COLOR_ARRAY)active->color_enabled=1;else if(array==GL_NORMAL_ARRAY)active->normal_enabled=1;else gl_error(GL_INVALID_ENUM);}
void glDisableClientState(GLenum array){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(array==GL_VERTEX_ARRAY)active->vertex_enabled=0;else if(array==GL_COLOR_ARRAY)active->color_enabled=0;else if(array==GL_NORMAL_ARRAY)active->normal_enabled=0;else gl_error(GL_INVALID_ENUM);}
void glReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,void *pixels){
 if(!active){gl_error(GL_INVALID_OPERATION);return;}if(format!=GL_RGBA||type!=GL_UNSIGNED_BYTE){gl_error(GL_INVALID_ENUM);return;}
 if(w<0||h<0){gl_error(GL_INVALID_VALUE);return;}if(!w||!h)return;if(!pixels){gl_error(GL_INVALID_VALUE);return;}
 if(w>LR_GPU_MAX_DIMENSION||h>LR_GPU_MAX_DIMENSION){gl_error(GL_OUT_OF_MEMORY);return;}
 struct surface *s=find_surface(active->surface);if(!s){gl_error(GL_INVALID_OPERATION);return;}
 unsigned width=s->width,height=s->height;if(!framebuffer_ready(&width,&height))return;
 int64_t left=x<0?0:x,bottom=y<0?0:y,right=(int64_t)x+w,top=(int64_t)y+h;
 if(right>width)right=width;if(top>height)top=height;
 size_t stride=((size_t)w*4+active->pack_alignment-1)&~(size_t)(active->pack_alignment-1);
 // Out-of-framebuffer pixel values are undefined by ES2. Choose zero without
 // changing the error state or leaking host bytes; preserve row padding.
 if(right<=left||top<=bottom){for(int row=0;row<h;row++)memset((char*)pixels+row*stride,0,(size_t)w*4);return;}
 unsigned rw=right-left,rh=top-bottom;size_t bytes=sizeof(struct lr_gpu_rectangle)+(size_t)rw*rh*4;
 if(active->read_capacity<bytes){void *p=realloc(active->read_scratch,bytes);if(!p){gl_error(GL_OUT_OF_MEMORY);return;}active->read_scratch=p;active->read_capacity=bytes;}
 struct lr_gpu_rectangle *area=(void*)active->read_scratch;*area=(struct lr_gpu_rectangle){left,bottom,rw,rh};
 struct lr_gpu_request r={.operation=LR_GPU_READ_REGION,.width=rw,.height=rh,.address=(uintptr_t)area,.bytes=bytes};if(!submit(&r))return;
 for(int row=0;row<h;row++)memset((char*)pixels+row*stride,0,(size_t)w*4);
 for(unsigned row=0;row<rh;row++)memcpy((char*)pixels+(row+bottom-y)*stride+(left-x)*4,(char*)(area+1)+(size_t)row*rw*4,(size_t)rw*4);
}
void glFinish(void){struct lr_gpu_request r={.operation=LR_GPU_FINISH};submit(&r);}
void glFlush(void){glFinish();}
GLenum glGetError(void){GLenum *p=active?&active->error:&no_context_error;GLenum error=*p;*p=0;return error;}
const GLubyte *glGetString(GLenum name){if(!active){gl_error(GL_INVALID_OPERATION);return NULL;}switch(name){case GL_VENDOR:return (const GLubyte *)"LinuxRunner";case GL_RENDERER:return (const GLubyte *)"Apple Metal / experimental guest bridge";case GL_VERSION:if(active->desktop)return (const GLubyte *)"LinuxRunner desktop OpenGL subset (not conformant)";return (const GLubyte *)(active->client_version==2?"LinuxRunner GLES2 shader subset (not conformant)":"LinuxRunner GLES subset v1 (not conformant)");case GL_SHADING_LANGUAGE_VERSION:if(active->desktop)return (const GLubyte *)"LinuxRunner GLSL vertex/fragment subset (not conformant)";return (const GLubyte *)"OpenGL ES GLSL ES 1.00 / limited resource subset";case GL_EXTENSIONS:return (const GLubyte *)"";default:gl_error(GL_INVALID_ENUM);return NULL;}}

int lrGPUGetCopyStatistics(uint64_t *compute,uint64_t *rows,uint64_t *bytes){if(!active||!compute||!rows||!bytes)return 0;struct lr_gpu_request r={.operation=LR_GPU_COPY_INFO};if(!submit(&r))return 0;*compute=r.submissions;*rows=r.readbacks;*bytes=r.bytes;return 1;}

int lrGPUGetUniformStatistics(uint64_t *uploads,uint64_t *bytes,uint64_t *snapshots){if(!active||!uploads||!bytes||!snapshots)return 0;struct lr_gpu_request r={.operation=LR_GPU_UNIFORM_BLOCK_INFO};if(!submit(&r))return 0;*uploads=r.submissions;*bytes=r.bytes;*snapshots=r.gpu_nanoseconds;return 1;}

int lrGPUGetStatistics(uint64_t *submissions,uint64_t *readbacks,uint64_t *gpu_ns) {
 if(!active || !submissions || !readbacks || !gpu_ns)return 0;
 struct lr_gpu_request request={.operation=LR_GPU_INFO};
 if(!submit(&request))return 0;
 *submissions=request.submissions;*readbacks=request.readbacks;*gpu_ns=request.gpu_nanoseconds;return 1;
}

#include "guest_typed.h"
#include "guest_framebuffers.inc"
#include "guest_textures.inc"
#include "guest_blend.inc"
#include "guest_depth.inc"
void glVertexAttribDivisor(GLuint,GLuint);
void glGetVertexAttribiv(GLuint,GLenum,GLint *);
#include "guest_gles1.inc"
#include "guest_mapping.inc"
#include "guest_uniforms.inc"
#include "guest_storage.inc"
#include "guest_gles2.inc"
#include "guest_typed.inc"

#ifdef LR_GLX_PROVIDER
#include "guest_glx.inc"
#endif
