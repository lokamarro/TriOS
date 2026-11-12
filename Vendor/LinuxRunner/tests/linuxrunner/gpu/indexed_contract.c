#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define LR_GPU_GUEST
#include "guest_gpu.h"
extern int lrGPUGetStatistics(uint64_t*,uint64_t*,uint64_t*);
static GLuint shader(GLenum stage,const char *text){GLuint s=glCreateShader(stage);glShaderSource(s,1,&text,NULL);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);assert(ok);return s;}
static void pixel(int green){unsigned char p[4];glReadPixels(32,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);printf("INDEXED_PIXEL=%u,%u,%u expected_green=%d\n",p[0],p[1],p[2],green);assert(p[green?1:0]>253&&p[green?0:1]<2&&p[2]<2&&!glGetError());}
int main(void){setbuf(stdout,NULL);Display *d=XOpenDisplay(NULL);assert(d);int attr[]={GLX_RGBA,GLX_DOUBLEBUFFER,GLX_DEPTH_SIZE,24,None};XVisualInfo *v=glXChooseVisual(d,DefaultScreen(d),attr);assert(v);XSetWindowAttributes a={.colormap=XCreateColormap(d,RootWindow(d,v->screen),v->visual,AllocNone)};Window w=XCreateWindow(d,RootWindow(d,v->screen),0,0,64,64,0,v->depth,InputOutput,v->visual,CWColormap,&a);GLXContext c=glXCreateContext(d,v,NULL,True);assert(c&&glXMakeCurrent(d,w,c));
 GLuint vs=shader(GL_VERTEX_SHADER,"#version 330 core\nflat out vec4 color;void main(){const vec2 p[6]=vec2[6](vec2(0),vec2(0),vec2(0),vec2(-1,-1),vec2(1,-1),vec2(0,1));gl_Position=vec4(p[gl_VertexID],0,1);color=gl_VertexID==5?vec4(0,1,0,1):vec4(1,0,0,1);}"),fs=shader(GL_FRAGMENT_SHADER,"#version 330 core\nflat in vec4 color;out vec4 result;void main(){result=color;}");GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok){char log[2048];glGetProgramInfoLog(program,sizeof log,NULL,log);fprintf(stderr,"INDEXED_LINK_FAIL %s\n",log);}assert(ok);glUseProgram(program);glViewport(0,0,64,64);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,3,3);pixel(1);
 uint32_t ids[]={3,4,5};glClear(GL_COLOR_BUFFER_BIT);glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_INT,ids);pixel(1);glClear(GL_COLOR_BUFFER_BIT);glDrawElementsInstanced(GL_TRIANGLES,3,GL_UNSIGNED_INT,ids,2);pixel(1);
 GLuint buffer;glGenBuffers(1,&buffer);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffer);uint32_t data[]={999,3,4,5};glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof data,data,GL_STATIC_DRAW);glClear(GL_COLOR_BUFFER_BIT);glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_INT,(void*)4);pixel(1);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);glDeleteBuffers(1,&buffer);
 uint32_t rotated[]={4,5,3};glClear(GL_COLOR_BUFFER_BIT);glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_INT,rotated);pixel(0);
 ids[2]=UINT32_MAX;glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_INT,ids);assert(glGetError()==GL_OUT_OF_MEMORY);
 int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);struct lr_gpu_request request={.version=LR_GPU_VERSION,.operation=LR_GPU_CREATE,.width=8,.height=8};assert(!ioctl(fd,LR_GPU_IOCTL,&request));struct {uint32_t program,count;float uniforms[LR_GL_MAX_UNIFORMS][16];struct lr_gl_vertex vertex;uint32_t indices[3];} packet={.program=1,.count=1,.indices={0,0,1}};request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_DRAW_INDEXED,.width=1,.height=3,.count=1,.address=(uintptr_t)&packet,.bytes=sizeof packet};assert(ioctl(fd,LR_GPU_IOCTL,&request)==-1&&errno==EINVAL);packet.indices[2]=0;request.address=1;assert(ioctl(fd,LR_GPU_IOCTL,&request)==-1&&errno==EFAULT);request.height=UINT32_MAX;assert(ioctl(fd,LR_GPU_IOCTL,&request)==-1&&errno==EINVAL);close(fd);
 uint64_t draws,reads,ns;assert(lrGPUGetStatistics(&draws,&reads,&ns)&&draws&&ns);printf("INDEXED_METAL_COUNTERS operations=%llu readbacks=%llu gpu_ns=%llu\n",(unsigned long long)draws,(unsigned long long)reads,(unsigned long long)ns);glUseProgram(0);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);glXMakeCurrent(d,None,NULL);glXDestroyContext(d,c);XDestroyWindow(d,w);XFree(v);XCloseDisplay(d);puts("INDEXED_CONTRACT_PASS VertexID=PASS nonzero_first=PASS uint_indices=PASS EBO_offset=PASS flat_provoking=PASS instanced=PASS GPU_pixels=PASS malformed=PASS");}
