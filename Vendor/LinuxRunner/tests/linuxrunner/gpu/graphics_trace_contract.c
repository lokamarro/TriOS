#include <assert.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <EGL/egl.h>
int main(void) {
 assert(glXQueryExtension(NULL,NULL,NULL)==False);
 assert(!strcmp(glXGetClientString(NULL,GLX_EXTENSIONS),"fixture extensions"));
 assert(!glXGetProcAddress((const GLubyte*)"unknown"));
 PFNGLXCREATECONTEXTATTRIBSARBPROC create=(void*)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
 assert(create);
 int a[]={GLX_CONTEXT_MAJOR_VERSION_ARB,4,GLX_CONTEXT_MINOR_VERSION_ARB,3,GLX_CONTEXT_PROFILE_MASK_ARB,GLX_CONTEXT_CORE_PROFILE_BIT_ARB,0};
 assert(!create(NULL,NULL,NULL,True,a));
 assert(!eglGetProcAddress("unknown"));
 int e[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
 assert(eglCreateContext(NULL,NULL,NULL,e)==EGL_NO_CONTEXT);
 assert(eglBindAPI(EGL_OPENGL_API)==EGL_FALSE);
 assert(!strcmp((const char*)glGetString(GL_RENDERER),"fixture renderer"));
 for(int i=0;i<300;i++)assert(!glXQueryExtension(NULL,NULL,NULL));
 void *library=dlopen("./libgraphics-trace-fixture.so",RTLD_NOW|RTLD_LOCAL);assert(library);PFNGLXCREATECONTEXTATTRIBSARBPROC resolved=dlsym(library,"glXCreateContextAttribsARB");assert(resolved&&!resolved(NULL,NULL,NULL,True,a));assert(!dlsym(library,"unsupported_graphics_function"));dlclose(library);
 puts("GRAPHICS_TRACE_CONTRACT_PASS explicit_library_handle=PASS");
}
