// Opt-in Linux diagnostic interposer. No replacement contexts or capabilities.
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <EGL/egl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <pthread.h>
// Blender and other loaders use dlsym on RTLD_LOCAL libGL handles. Preserve
// the exact resolved provider pointer instead of assuming the global scope.
static void *original(const char *);
static void *(*real_dlsym)(void *,const char *);
static pthread_once_t resolver_once=PTHREAD_ONCE_INIT;
static void resolve_dlsym(void){real_dlsym=dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.17");}
static void *lookup(void *handle,const char *name){pthread_once(&resolver_once,resolve_dlsym);return real_dlsym?real_dlsym(handle,name):NULL;}
static atomic_uint remaining=512;
static atomic_uint shader_dump_count;
static void trace(const char *fmt,...) {
 unsigned old=atomic_load(&remaining);
 while(old && !atomic_compare_exchange_weak(&remaining,&old,old-1)){}
 if(!old)return;
 va_list args;va_start(args,fmt);flockfile(stderr);
 fprintf(stderr,"LR_GRAPHICS_PATH pid=%ld ",(long)getpid());vfprintf(stderr,fmt,args);fputc('\n',stderr);
 funlockfile(stderr);va_end(args);
}
static void attributes(const char *api,const int *a,int end) {
 // API callers already owe valid, terminated attribute lists. Bound diagnostics.
 if(!a){trace("%s attributes=default",api);return;}
 for(unsigned i=0;i<32;i+=2){if(a[i]==end)return;trace("%s attribute=0x%x value=0x%x",api,a[i],a[i+1]);}
 trace("%s attribute_trace_truncated",api);
}
Bool glXQueryExtension(Display *d,int *e,int *v) {
 Bool(*fn)(Display*,int*,int*)=original("glXQueryExtension");
 Bool r=fn?fn(d,e,v):False;trace("glXQueryExtension present=%d",r);return r;
}
const char *glXGetClientString(Display *d,int n) {
 const char *(*fn)(Display*,int)=original("glXGetClientString");
 const char *r=fn?fn(d,n):NULL;trace("glXGetClientString name=0x%x value=%.512s",n,r?r:"NULL");return r;
}
const char *glXQueryExtensionsString(Display *d,int screen) {
 const char *(*fn)(Display*,int)=original("glXQueryExtensionsString");
 const char *r=fn?fn(d,screen):NULL;trace("glXQueryExtensionsString value=%.512s",r?r:"NULL");return r;
}
GLXContext glXCreateContextAttribsARB(Display *d,GLXFBConfig c,GLXContext s,Bool direct,const int *a) {
 GLXContext(*fn)(Display*,GLXFBConfig,GLXContext,Bool,const int*)=original("glXCreateContextAttribsARB");
 attributes("GLX",a,0);GLXContext r=fn?fn(d,c,s,direct,a):NULL;
 trace("glXCreateContextAttribsARB shared=%d direct=%d success=%d",s!=NULL,direct,r!=NULL);return r;
}
EGLBoolean eglBindAPI(EGLenum a) {
 EGLBoolean(*fn)(EGLenum)=original("eglBindAPI");
 EGLBoolean r=fn?fn(a):EGL_FALSE;trace("eglBindAPI api=0x%x success=%d",a,r);return r;
}
EGLContext eglCreateContext(EGLDisplay d,EGLConfig c,EGLContext s,const EGLint *a) {
 EGLContext(*fn)(EGLDisplay,EGLConfig,EGLContext,const EGLint*)=original("eglCreateContext");
 attributes("EGL",a,EGL_NONE);EGLContext r=fn?fn(d,c,s,a):EGL_NO_CONTEXT;
 trace("eglCreateContext shared=%d success=%d",s!=EGL_NO_CONTEXT,r!=EGL_NO_CONTEXT);return r;
}
const GLubyte *glGetString(GLenum n) {
 const GLubyte *(*fn)(GLenum)=original("glGetString");
 const GLubyte *r=fn?fn(n):NULL;
 if(n==GL_VENDOR||n==GL_RENDERER||n==GL_VERSION)trace("glGetString name=0x%x value=%.256s",n,r?(const char*)r:"NULL");
 return r;
}
// Optional bounded shader capture for comparing a working Mesa workload with
// bridge compiler support. It never alters source, context attributes or calls.
void glShaderSource(GLuint shader,GLsizei count,const GLchar *const *source,const GLint *lengths){
 void(*fn)(GLuint,GLsizei,const GLchar *const *,const GLint*)=original("glShaderSource");
 trace("glShaderSource shader=%u strings=%d",shader,count);
 const char *directory=getenv("LR_GRAPHICS_SHADER_DIR");unsigned number=directory?atomic_fetch_add(&shader_dump_count,1):32;
 if(directory&&number<32&&count>0&&count<=64&&source){size_t bytes=0;int valid=1;for(int i=0;i<count;i++){if(!source[i]){valid=0;break;}size_t n=lengths&&lengths[i]>=0?(size_t)lengths[i]:strnlen(source[i],262145);if(n>262144-bytes){valid=0;break;}bytes+=n;}
  if(valid){char path[1024];int n=snprintf(path,sizeof path,"%s/%ld-%u-%u.glsl",directory,(long)getpid(),number,shader);if(n>0&&(size_t)n<sizeof path){FILE *file=fopen(path,"wb");if(file){for(int i=0;i<count;i++){size_t size=lengths&&lengths[i]>=0?(size_t)lengths[i]:strlen(source[i]);fwrite(source[i],1,size,file);}fclose(file);trace("shader_capture shader=%u bytes=%zu path=%s",shader,bytes,path);}}}
 }
 if(fn)fn(shader,count,source,lengths);
}
void glCompileShader(GLuint shader){void(*fn)(GLuint)=original("glCompileShader");if(fn)fn(shader);void(*query)(GLuint,GLenum,GLint*)=original("glGetShaderiv");GLint ok=0;if(query)query(shader,GL_COMPILE_STATUS,&ok);trace("glCompileShader shader=%u success=%d",shader,ok);}
void glLinkProgram(GLuint program){void(*fn)(GLuint)=original("glLinkProgram");if(fn)fn(program);void(*query)(GLuint,GLenum,GLint*)=original("glGetProgramiv");GLint ok=0;if(query)query(program,GL_LINK_STATUS,&ok);trace("glLinkProgram program=%u success=%d",program,ok);}
void glBindBufferBase(GLenum target,GLuint index,GLuint buffer){void(*fn)(GLenum,GLuint,GLuint)=original("glBindBufferBase");trace("glBindBufferBase target=0x%x index=%u buffer=%u",target,index,buffer);if(fn)fn(target,index,buffer);}
void glDispatchCompute(GLuint x,GLuint y,GLuint z){void(*fn)(GLuint,GLuint,GLuint)=original("glDispatchCompute");trace("glDispatchCompute groups=%u,%u,%u",x,y,z);if(fn)fn(x,y,z);}
static void *interposed(const char *,void *);
__GLXextFuncPtr glXGetProcAddress(const GLubyte *n) {
 __GLXextFuncPtr(*fn)(const GLubyte*)=original("glXGetProcAddress");
 void *r=fn?(void*)fn(n):NULL;trace("glXGetProcAddress name=%.96s present=%d",n,r!=NULL);
 return (__GLXextFuncPtr)interposed((const char*)n,r);
}
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *n) {
 __GLXextFuncPtr(*fn)(const GLubyte*)=original("glXGetProcAddressARB");
 void *r=fn?(void*)fn(n):NULL;trace("glXGetProcAddressARB name=%.96s present=%d",n,r!=NULL);
 return (__GLXextFuncPtr)interposed((const char*)n,r);
}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *n) {
 __eglMustCastToProperFunctionPointerType(*fn)(const char*)=original("eglGetProcAddress");
 void *r=fn?(void*)fn(n):NULL;trace("eglGetProcAddress name=%.96s present=%d",n,r!=NULL);
 return (__eglMustCastToProperFunctionPointerType)interposed(n,r);
}

struct traced_entry {const char *name;void *wrapper;_Atomic(void *) resolved;};
#define ENTRY(name) {#name,(void *)name,NULL}
static struct traced_entry entries[]={
 ENTRY(glXQueryExtension),ENTRY(glXGetClientString),ENTRY(glXQueryExtensionsString),ENTRY(glXCreateContextAttribsARB),ENTRY(eglBindAPI),ENTRY(eglCreateContext),ENTRY(glGetString),ENTRY(glShaderSource),ENTRY(glCompileShader),ENTRY(glLinkProgram),ENTRY(glBindBufferBase),ENTRY(glDispatchCompute),ENTRY(glXGetProcAddress),ENTRY(glXGetProcAddressARB),ENTRY(eglGetProcAddress)
};
#undef ENTRY
static void *original(const char *name){for(unsigned i=0;i<sizeof entries/sizeof *entries;i++)if(!strcmp(name,entries[i].name)){void *p=atomic_load(&entries[i].resolved);if(p)return p;break;}return lookup(RTLD_NEXT,name);}
static void *interposed(const char *name,void *pointer){if(!pointer)return NULL;for(unsigned i=0;i<sizeof entries/sizeof *entries;i++)if(!strcmp(name,entries[i].name)){
 struct traced_entry *e=&entries[i];if(pointer==e->wrapper)return pointer;void *expected=NULL;if(!atomic_compare_exchange_strong(&e->resolved,&expected,pointer)&&expected!=pointer){trace("provider_changed name=%s tracing_skipped",name);return pointer;}return e->wrapper;
 }return pointer;}
void *dlsym(void *handle,const char *name){return interposed(name,lookup(handle,name));}
