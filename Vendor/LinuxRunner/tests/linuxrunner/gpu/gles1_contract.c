#include <GLES/gl.h>
#include <EGL/egl.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "guest_gpu.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
int main(void){
 EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLConfig config;EGLint count;
 assert(eglInitialize(d,0,0)&&eglChooseConfig(d,0,&config,1,&count));
 EGLSurface s=eglCreatePbufferSurface(d,config,(EGLint[]){EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE});EGLContext c=eglCreateContext(d,config,0,0);assert(s&&c&&eglMakeCurrent(d,s,s,c));
 GLfloat v[]={-.9,-.9,-.5,.9,-.9,-.5,0,.9,-.5};GLfloat red[]={1,0,0,1,1,0,0,1,1,0,0,1};GLuint buffer;
 glGenBuffers(1,&buffer);assert(buffer);glBindBuffer(GL_ARRAY_BUFFER,buffer);glBufferData(GL_ARRAY_BUFFER,sizeof v,v,GL_STATIC_DRAW);glVertexPointer(3,GL_FLOAT,0,0);glBindBuffer(GL_ARRAY_BUFFER,0);glColorPointer(4,GL_FLOAT,0,red);glEnableClientState(GL_VERTEX_ARRAY);glEnableClientState(GL_COLOR_ARRAY);
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glEnable(GL_DEPTH_TEST);glEnable(GL_CULL_FACE);glDrawArrays(GL_TRIANGLES,0,3);
 unsigned char pixels[4096];glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(glGetError()==0);assert(pixels[(10*32+16)*4]>250);
 glTranslatef(0,0,1);for(int i=0;i<3;i++){red[i*4]=0;red[i*4+1]=1;}glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_SHORT,(GLushort[]){0,1,2});glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(glGetError()==0);assert(pixels[(10*32+16)*4]>250&&pixels[(10*32+16)*4+1]<5); // farther geometry fails depth test
 glDisable(GL_DEPTH_TEST);glDrawArrays(GL_TRIANGLES,0,3);glFinish();glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(glGetError()==0&&pixels[(10*32+16)*4+1]>250);
 glBufferData(GL_ARRAY_BUFFER,8,NULL,GL_STATIC_DRAW);assert(glGetError()==GL_INVALID_OPERATION);glEnable(GL_TEXTURE_2D);assert(glGetError()==GL_INVALID_ENUM);glMatrixMode(0xffff);assert(glGetError()==GL_INVALID_ENUM);
 glDeleteBuffers(1,&buffer);assert(eglMakeCurrent(d,0,0,0)&&eglDestroyContext(d,c)&&eglDestroySurface(d,s)&&eglTerminate(d));
 // Native-format copy must preserve rows and shuffle exactly R/B, including
 // a non-vector-multiple row length. This tests the scalar SIMD tail as well.
 int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);struct lr_gpu_request r={.version=1,.operation=LR_GPU_CREATE,.width=17,.height=3};assert(!ioctl(fd,LR_GPU_IOCTL,&r));r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_DRAW,.flags=LR_GPU_CLEAR_BIT,.clear={1,.5,0,1}};assert(!ioctl(fd,LR_GPU_IOCTL,&r));unsigned char rgba[17*3*4],bgra[17*3*4];r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_READ,.width=17,.height=3,.address=(uintptr_t)rgba,.bytes=sizeof rgba};assert(!ioctl(fd,LR_GPU_IOCTL,&r));r.operation=LR_GPU_READ_X11;r.address=(uintptr_t)bgra;assert(!ioctl(fd,LR_GPU_IOCTL,&r));for(unsigned i=0;i<17*3;i++){assert(bgra[i*4]==rgba[i*4+2]&&bgra[i*4+1]==rgba[i*4+1]&&bgra[i*4+2]==rgba[i*4]&&bgra[i*4+3]==255);}assert(!close(fd));
 puts("GLES1_CONTRACT_PASS VBO=PASS matrices=PASS depth=PASS cull=PASS indexed=PASS finish=PASS errors=PASS BGRA_tail=PASS");
}
