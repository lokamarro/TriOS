#ifndef LINUXRUNNER_GL_SUBSET_H
#define LINUXRUNNER_GL_SUBSET_H
// Public EGL/GLES function signatures for this explicitly limited prototype.
// These constants/signatures follow Khronos EGL 1.5 / GLES 1.1 APIs.
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void *EGLDisplay;typedef void *EGLConfig;typedef void *EGLContext;typedef void *EGLSurface;
typedef int32_t EGLint;typedef uint32_t EGLBoolean;typedef uint32_t EGLenum;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_SUCCESS 0x3000
#define EGL_BAD_ACCESS 0x3002
#define EGL_BAD_ALLOC 0x3003
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_CONFIG 0x3005
#define EGL_BAD_CONTEXT 0x3006
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_MATCH 0x3009
#define EGL_BAD_PARAMETER 0x300c
#define EGL_BAD_SURFACE 0x300d
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30a0
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 1
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES_BIT 1
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
EGLDisplay eglGetDisplay(void *);
EGLBoolean eglInitialize(EGLDisplay,EGLint *,EGLint *);
EGLBoolean eglChooseConfig(EGLDisplay,const EGLint *,EGLConfig *,EGLint,EGLint *);
EGLBoolean eglBindAPI(EGLenum);
EGLSurface eglCreatePbufferSurface(EGLDisplay,EGLConfig,const EGLint *);
EGLContext eglCreateContext(EGLDisplay,EGLConfig,EGLContext,const EGLint *);
EGLBoolean eglMakeCurrent(EGLDisplay,EGLSurface,EGLSurface,EGLContext);
EGLBoolean eglDestroyContext(EGLDisplay,EGLContext);
EGLBoolean eglDestroySurface(EGLDisplay,EGLSurface);
EGLBoolean eglTerminate(EGLDisplay);
EGLint eglGetError(void);
typedef uint32_t GLenum;typedef uint32_t GLbitfield;typedef int32_t GLint;typedef int32_t GLsizei;typedef float GLfloat;typedef unsigned char GLubyte;
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY 0x0505
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TRIANGLES 0x0004
#define GL_RGBA 0x1908
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_VERTEX_ARRAY 0x8074
#define GL_COLOR_ARRAY 0x8076
#define GL_VENDOR 0x1f00
#define GL_RENDERER 0x1f01
#define GL_VERSION 0x1f02
#define GL_EXTENSIONS 0x1f03
void glClearColor(GLfloat,GLfloat,GLfloat,GLfloat);
void glClear(GLbitfield);
void glDepthFunc(GLenum);
void glDepthMask(unsigned char);
void glClearDepthf(GLfloat);
void glClearDepth(double);
void glGetBooleanv(GLenum,unsigned char *);
void glGetFloatv(GLenum,GLfloat *);
void glGetDoublev(GLenum,double *);
void glViewport(GLint,GLint,GLsizei,GLsizei);
void glVertexPointer(GLint,GLenum,GLsizei,const void *);
void glColorPointer(GLint,GLenum,GLsizei,const void *);
void glEnableClientState(GLenum);
void glDisableClientState(GLenum);
void glDrawArrays(GLenum,GLint,GLsizei);
void glReadPixels(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void *);
void glFinish(void);
void glFlush(void);
GLenum glGetError(void);
const GLubyte *glGetString(GLenum);
// Prototype diagnostics; not an OpenGL extension.
int lrGPUGetStatistics(uint64_t *,uint64_t *,uint64_t *);
#ifdef __cplusplus
}
#endif
#endif
