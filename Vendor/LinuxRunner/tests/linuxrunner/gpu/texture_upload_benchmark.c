#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(void){EGLDisplay d=eglGetDisplay(0);EGLConfig cfg;EGLint n;assert(eglInitialize(d,0,0)&&eglChooseConfig(d,(EGLint[]){EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE},&cfg,1,&n));EGLContext c=eglCreateContext(d,cfg,0,(EGLint[]){EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE});EGLSurface s=eglCreatePbufferSurface(d,cfg,(EGLint[]){EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE});assert(eglMakeCurrent(d,s,s,c));unsigned char *pixels=malloc(256*256*4);assert(pixels);for(int i=0;i<256*256*4;i++)pixels[i]=i;GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,256,256,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());double start=now();for(int i=0;i<128;i++){pixels[0]=i;glTexSubImage2D(GL_TEXTURE_2D,0,0,0,256,256,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());}glFinish();printf("TEXTURE_UPLOAD_BENCH_PASS elapsed_seconds=%.6f updates=128 RGBA_bytes=33554432\n",now()-start);glDeleteTextures(1,&texture);free(pixels);assert(eglMakeCurrent(d,0,0,0)&&eglDestroyContext(d,c)&&eglDestroySurface(d,s)&&eglTerminate(d));}
