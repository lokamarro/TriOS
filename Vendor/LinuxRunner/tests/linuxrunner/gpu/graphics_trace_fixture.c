#include <GL/glx.h>
#include <EGL/egl.h>
#include <string.h>
Bool glXQueryExtension(Display*d,int*e,int*v){(void)d;(void)e;(void)v;return False;}
const char *glXGetClientString(Display*d,int n){(void)d;(void)n;return "fixture extensions";}
GLXContext glXCreateContextAttribsARB(Display*d,GLXFBConfig c,GLXContext s,Bool b,const int*a){(void)d;(void)c;(void)s;(void)b;(void)a;return NULL;}
EGLBoolean eglBindAPI(EGLenum a){(void)a;return EGL_FALSE;}
EGLContext eglCreateContext(EGLDisplay d,EGLConfig c,EGLContext s,const EGLint*a){(void)d;(void)c;(void)s;(void)a;return EGL_NO_CONTEXT;}
const GLubyte *glGetString(GLenum n){(void)n;return (const GLubyte*)"fixture renderer";}
__GLXextFuncPtr glXGetProcAddress(const GLubyte*n){return !strcmp((const char*)n,"glXCreateContextAttribsARB")?(__GLXextFuncPtr)glXCreateContextAttribsARB:NULL;}
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte*n){return glXGetProcAddress(n);}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char*n){return !strcmp(n,"eglCreateContext")?(__eglMustCastToProperFunctionPointerType)eglCreateContext:NULL;}
