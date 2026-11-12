// Real Linux ARM64 EGL/GLES2 -> device ABI -> Metal texture pixel contract.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#define LR_GPU_GUEST 1
#include "guest_gpu.h"
static const GLfloat position[]={-1,-1,1,-1,-1,1,-1,1,1,-1,1,1};
static const GLfloat coords[]={0,0,1,0,0,1,0,1,1,0,1,1};
static GLuint compile(GLenum kind,const char *source){GLuint s=glCreateShader(kind);glShaderSource(s,1,&source,NULL);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof log,NULL,log);puts(log);}assert(ok);return s;}
static GLuint program(const char *fragment){GLuint v=compile(GL_VERTEX_SHADER,"attribute vec2 position; attribute vec2 coord; varying vec2 uv; void main(){uv=coord;gl_Position=vec4(position,0.0,1.0);}"),f=compile(GL_FRAGMENT_SHADER,fragment),p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glBindAttribLocation(p,0,"position");glBindAttribLocation(p,1,"coord");glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char log[2048];glGetProgramInfoLog(p,sizeof log,NULL,log);puts(log);}assert(ok);glDeleteShader(v);glDeleteShader(f);return p;}
static void draw(unsigned char *pixels){glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,6);glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(glGetError()==GL_NO_ERROR);}
static void color(const unsigned char *p,unsigned x,unsigned y,int r,int g,int b,int a){const unsigned char *v=p+(y*32+x)*4;if(v[0]!=r||v[1]!=g||v[2]!=b||v[3]!=a){fprintf(stderr,"pixel %u,%u actual=%u,%u,%u,%u expected=%d,%d,%d,%d\n",x,y,v[0],v[1],v[2],v[3],r,g,b,a);}assert(v[0]==r&&v[1]==g&&v[2]==b&&v[3]==a);}
static void nearest(void){glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);}
int main(void){EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLConfig config;EGLint n;assert(eglInitialize(d,NULL,NULL)&&eglChooseConfig(d,(EGLint[]){EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE},&config,1,&n));EGLContext c=eglCreateContext(d,config,0,(EGLint[]){EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE});EGLSurface surface=eglCreatePbufferSurface(d,config,(EGLint[]){EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE});assert(c&&surface&&eglMakeCurrent(d,surface,surface,c));
 GLuint p=program("precision mediump float; varying vec2 uv; uniform sampler2D image; void main(){gl_FragColor=texture2D(image,uv);}");glUseProgram(p);GLint location=glGetUniformLocation(p,"image");assert(location>=0);glUniform1i(location,0);
 glVertexAttribPointer(0,2,GL_FLOAT,0,0,position);glVertexAttribPointer(1,2,GL_FLOAT,0,0,coords);glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
 unsigned char pixels[4096];GLuint t;glGenTextures(1,&t);assert(!glIsTexture(t));glBindTexture(GL_TEXTURE_2D,t);assert(glIsTexture(t));nearest();
 const unsigned char checker[]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,checker);draw(pixels);color(pixels,8,8,255,0,0,255);color(pixels,24,8,0,255,0,255);color(pixels,8,24,0,0,255,255);color(pixels,24,24,255,255,255,255);

 // Read only the requested rectangle; pack padding is left untouched.
 unsigned char tiny[16];memset(tiny,0xa5,sizeof tiny);glPixelStorei(GL_PACK_ALIGNMENT,8);glReadPixels(8,8,1,2,GL_RGBA,GL_UNSIGNED_BYTE,tiny);assert(!glGetError());assert(tiny[0]==255&&tiny[1]==0&&tiny[8]==255&&tiny[9]==0&&tiny[4]==0xa5&&tiny[12]==0xa5);glPixelStorei(GL_PACK_ALIGNMENT,4);
 glReadPixels(0,0,0,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);assert(!glGetError());glReadPixels(32,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,tiny);assert(!glGetError()&&tiny[0]==0);
 glReadPixels(-1,-1,2,2,GL_RGBA,GL_UNSIGNED_BYTE,tiny);assert(!glGetError()&&tiny[0]==0&&tiny[12]==255&&tiny[13]==0);
 glViewport(8,8,16,16);draw(pixels);color(pixels,12,12,255,0,0,255);color(pixels,4,4,0,0,0,255);color(pixels,20,20,255,255,255,255);
 GLint viewport[4];glGetIntegerv(GL_VIEWPORT,viewport);assert(viewport[0]==8&&viewport[1]==8&&viewport[2]==16&&viewport[3]==16);
 glViewport(0,0,0,0);draw(pixels);color(pixels,12,12,0,0,0,255);glViewport(-8,0,32,32);draw(pixels);color(pixels,28,8,0,0,0,255);glViewport(INT32_MAX,INT32_MIN,32,32);draw(pixels);color(pixels,12,12,0,0,0,255);glViewport(0,0,32,32);
 int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);struct lr_gpu_request request={.version=LR_GPU_VERSION,.operation=LR_GPU_CREATE,.width=1,.height=1};assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_TEXTURE,.width=1,.height=1,.bytes=sizeof(struct lr_gpu_texture)+4,.address=1};assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EFAULT);
 struct {struct lr_gpu_texture h;unsigned char p[4];} packet={.h={.name=1,.width=1,.height=1,.min_filter=GL_NEAREST,.mag_filter=GL_LINEAR,.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT},.p={1,2,3,255}};
 packet.h.min_filter=0;request.address=(uintptr_t)&packet;assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EINVAL);packet.h.min_filter=GL_NEAREST;assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_RESOURCE_INFO};assert(!ioctl(fd,LR_GPU_IOCTL,&request)&&request.gpu_nanoseconds==4);
 // Native RGBA16F storage uses eight-byte texels and survives a rejected four-byte subimage.
 struct {struct lr_gpu_texture h;uint16_t p[16];} hdr={.h={.name=1,.width=2,.height=2,.min_filter=GL_NEAREST,.mag_filter=GL_LINEAR,.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT},.p={0x4000,0x3800,0x3400,0x3c00}};
 for(unsigned i=1;i<4;i++){hdr.p[i*4]=0x4000;hdr.p[i*4+1]=0x3800;hdr.p[i*4+2]=0x3400;hdr.p[i*4+3]=0x3c00;}
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_TEXTURE,.count=LR_GPU_TEXTURE_FORMAT_RGBA16F,.width=2,.height=2,.address=(uintptr_t)&hdr,.bytes=sizeof hdr};assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 struct {struct lr_gpu_texture h;unsigned char p[4];} malformed={.h={.name=1,.width=1,.height=1,.min_filter=GL_NEAREST,.mag_filter=GL_LINEAR,.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT},.p={9,8,7,255}};
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_TEXTURE,.count=GL_RGBA,.flags=LR_GPU_TEXTURE_SUBIMAGE,.width=1,.height=1,.address=(uintptr_t)&malformed,.bytes=sizeof malformed};assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EINVAL);
 struct {struct lr_gpu_texture h;uint16_t p[4];} update={.h={.name=1,.width=1,.height=1,.min_filter=GL_NEAREST,.mag_filter=GL_LINEAR,.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT},.p={0x4200,0x3c00,0x3800,0x3c00}};
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_TEXTURE,.count=LR_GPU_TEXTURE_FORMAT_RGBA16F,.flags=LR_GPU_TEXTURE_SUBIMAGE,.width=1,.height=1,.address=(uintptr_t)&update,.bytes=sizeof update};assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 struct {struct lr_gpu_texture h;unsigned char p[16];} srgb={.h={.name=2,.width=2,.height=2,.min_filter=GL_NEAREST,.mag_filter=GL_LINEAR,.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT},.p={128,64,32,200,128,64,32,200,128,64,32,200,128,64,32,200}};
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_TEXTURE,.count=LR_GPU_TEXTURE_FORMAT_SRGB8_ALPHA8,.width=2,.height=2,.address=(uintptr_t)&srgb,.bytes=sizeof srgb};assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 malformed.h.name=2;
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_TEXTURE,.count=LR_GPU_TEXTURE_FORMAT_SRGB8_ALPHA8,.flags=LR_GPU_TEXTURE_SUBIMAGE,.width=1,.height=1,.address=(uintptr_t)&malformed,.bytes=sizeof malformed};assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_TEXTURE,.count=GL_RGBA,.flags=LR_GPU_TEXTURE_SUBIMAGE,.width=1,.height=1,.address=(uintptr_t)&malformed,.bytes=sizeof malformed};assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EINVAL);
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_RESOURCE_INFO};assert(!ioctl(fd,LR_GPU_IOCTL,&request)&&request.readbacks==4+32+8+16+4);
 struct {struct lr_gpu_texture h;} clear={.h={.name=1,.min_filter=GL_NEAREST,.mag_filter=GL_LINEAR,.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT}};
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_TEXTURE,.count=GL_RGBA,.flags=LR_GPU_TEXTURE_STORAGE_CLEAR,.address=(uintptr_t)&clear,.bytes=sizeof clear};assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 clear.h.name=2;request.address=(uintptr_t)&clear;assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_RESOURCE_INFO};assert(!ioctl(fd,LR_GPU_IOCTL,&request)&&request.gpu_nanoseconds==0);
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_TEXTURE,.count=LR_GPU_TEXTURE_FORMAT_RGBA16F,.width=2,.height=2,.address=(uintptr_t)&hdr,.bytes=sizeof hdr};assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_RESOURCE_INFO};assert(!ioctl(fd,LR_GPU_IOCTL,&request)&&request.gpu_nanoseconds>0);
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_VIEWPORT,.bytes=sizeof(struct lr_gpu_rectangle),.address=1};assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EFAULT);
 struct lr_gpu_rectangle area={0,0,960,540};request.address=(uintptr_t)&area;assert(!ioctl(fd,LR_GPU_IOCTL,&request));
 area.width=LR_GPU_MAX_DIMENSION+1;area.height=1;request.address=(uintptr_t)&area;assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EINVAL);
 request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_READ_REGION,.width=1,.height=1,.bytes=sizeof area+4,.address=(uintptr_t)&area};assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EINVAL);close(fd); // isolated texture allocation never changes EGL context
 const unsigned char yellow[]={255,255,0,255};glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,yellow);draw(pixels);color(pixels,8,8,255,255,0,255);color(pixels,24,8,0,255,0,255);
 glTexSubImage2D(GL_TEXTURE_2D,0,2,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,yellow);assert(glGetError()==GL_INVALID_VALUE);glPixelStorei(GL_UNPACK_ALIGNMENT,3);assert(glGetError()==GL_INVALID_VALUE);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_NEAREST);draw(pixels);color(pixels,8,8,0,0,0,255);glGenerateMipmap(GL_TEXTURE_2D);draw(pixels);color(pixels,8,8,255,255,0,255);
 // NPOT uses core ES2 completeness rules, and padded RGB rows are unpacked.
 const unsigned char rgb[]={255,0,0,255,0,0,255,0,0,99,99,99,0,0,255,0,0,255,0,0,255,99,99,99};
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,3,2,0,GL_RGB,GL_UNSIGNED_BYTE,rgb);draw(pixels);color(pixels,8,8,0,0,0,255);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);draw(pixels);color(pixels,8,8,255,0,0,255);color(pixels,8,24,0,0,255,255);glGenerateMipmap(GL_TEXTURE_2D);assert(glGetError()==GL_INVALID_OPERATION);
 unsigned char luminance=42;glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,1,1,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,&luminance);glGenerateMipmap(GL_TEXTURE_2D);draw(pixels);color(pixels,8,8,42,42,42,255);
 unsigned char alpha=128;glTexImage2D(GL_TEXTURE_2D,0,GL_ALPHA,1,1,0,GL_ALPHA,GL_UNSIGNED_BYTE,&alpha);draw(pixels);color(pixels,8,8,0,0,0,128);
 GLuint q=program("precision mediump float; varying vec2 uv; uniform sampler2D first; uniform sampler2D second; void main(){gl_FragColor=texture2D(first,uv)*texture2D(second,uv);}");GLuint second;glGenTextures(1,&second);const unsigned char a[]={255,128,0,255},b[]={128,255,0,255};glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,t);nearest();glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,a);glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,second);nearest();glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,b);glUseProgram(q);glUniform1i(glGetUniformLocation(q,"first"),0);GLint second_location=glGetUniformLocation(q,"second");glUniform1i(second_location,1);GLint unit=-1;glGetUniformiv(q,second_location,&unit);assert(unit==1);draw(pixels);color(pixels,8,8,128,128,0,255);
 glUniform1i(second_location,99);glDrawArrays(GL_TRIANGLES,0,6);assert(glGetError()==GL_INVALID_OPERATION);glUniform1i(second_location,1);glDeleteTextures(1,&second);draw(pixels);color(pixels,8,8,0,0,0,255);assert(!glIsTexture(second));
 glUseProgram(p);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,0);nearest();glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,a);draw(pixels);color(pixels,8,8,255,128,0,255);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,0,0,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);draw(pixels);color(pixels,8,8,0,0,0,255);
 GLuint big[7];glGenTextures(7,big);unsigned accepted=0;for(unsigned i=0;i<7;i++){glBindTexture(GL_TEXTURE_2D,big[i]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,512,512,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);GLenum error=glGetError();if(error==GL_OUT_OF_MEMORY)break;assert(error==GL_NO_ERROR);accepted++;}assert(accepted>0&&accepted<7);glDeleteTextures(7,big);glGenTextures(1,big);glBindTexture(GL_TEXTURE_2D,big[0]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,512,512,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);assert(glGetError()==GL_NO_ERROR);glDeleteTextures(1,big);glDeleteTextures(1,&t);
 assert(eglGetProcAddress("glTexImage2D")&&eglGetProcAddress("glGenerateMipmap")&&eglGetProcAddress("glFramebufferTexture2D"));glUseProgram(0);glDeleteProgram(p);glDeleteProgram(q);assert(glGetError()==GL_NO_ERROR);assert(eglMakeCurrent(d,0,0,0)&&eglDestroyContext(d,c)&&eglDestroySurface(d,surface)&&eglTerminate(d));
 puts("TEXTURE_CONTRACT_PASS sampler_shader=PASS RGBA_pixels=PASS subimage=PASS RGB_alignment=PASS ALPHA_LUMINANCE=PASS incomplete_NPOT=PASS mipmaps=PASS multi_unit=PASS deletion=PASS quota_recovery=PASS malformed_request=PASS isolation=PASS viewport=PASS partial_read_pack=PASS");
}
