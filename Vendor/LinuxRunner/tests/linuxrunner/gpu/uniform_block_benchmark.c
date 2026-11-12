#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
extern int lrGPUGetUniformStatistics(uint64_t*,uint64_t*,uint64_t*);
static double clock_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
static GLuint shader(GLenum type,const char *text){GLuint s=glCreateShader(type);glShaderSource(s,1,&text,NULL);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);assert(ok);return s;}
int main(void){setbuf(stdout,NULL);Display *d=XOpenDisplay(NULL);assert(d);XVisualInfo *v=glXChooseVisual(d,DefaultScreen(d),NULL);assert(v);XSetWindowAttributes a={.colormap=XCreateColormap(d,RootWindow(d,v->screen),v->visual,AllocNone)};Window w=XCreateWindow(d,RootWindow(d,v->screen),0,0,64,64,0,v->depth,InputOutput,v->visual,CWColormap,&a);GLXContext c=glXCreateContext(d,v,NULL,True);assert(c&&glXMakeCurrent(d,w,c));GLuint vs=shader(GL_VERTEX_SHADER,"#version 330 core\nin vec2 position;void main(){gl_Position=vec4(position,0,1);}"),fs=shader(GL_FRAGMENT_SHADER,"#version 330 core\nlayout(std140) uniform Frame {vec4 color;};out vec4 result;void main(){result=color;}");GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glBindAttribLocation(p,0,"position");glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(p);float vertices[]={-1,-1,1,-1,0,1};glVertexAttribPointer(0,2,GL_FLOAT,False,0,vertices);glEnableVertexAttribArray(0);GLuint buffer;glGenBuffers(1,&buffer);glBindBuffer(GL_UNIFORM_BUFFER,buffer);float color[]={0,1,0,1};glBufferData(GL_UNIFORM_BUFFER,sizeof color,color,GL_DYNAMIC_DRAW);glBindBufferBase(GL_UNIFORM_BUFFER,0,buffer);glViewport(0,0,64,64);glClear(GL_COLOR_BUFFER_BIT);
 // Pipeline warmup excludes shader/context initialization. Same draws and
 // native backend for baseline/optimized providers; readback verifies output.
 for(int i=0;i<32;i++)glDrawArrays(GL_TRIANGLES,0,3);glFinish();
 uint64_t calls,bytes,snapshots,after_calls,after_bytes,after_snapshots;assert(lrGPUGetUniformStatistics(&calls,&bytes,&snapshots));double start=clock_ns();for(int i=0;i<2000;i++)glDrawArrays(GL_TRIANGLES,0,3);glFinish();double elapsed=clock_ns()-start;assert(lrGPUGetUniformStatistics(&after_calls,&after_bytes,&after_snapshots));unsigned char pixel[4];glReadPixels(32,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(!glGetError()&&pixel[1]>253&&pixel[0]<2&&pixel[2]<2);
 printf("UBO_BENCH draws=2000 milliseconds=%.3f upload_calls=%llu guest_payload_bytes=%llu native_snapshot_bytes=%llu pixel=PASS\n",elapsed/1e6,(unsigned long long)(after_calls-calls),(unsigned long long)(after_bytes-bytes),(unsigned long long)(after_snapshots-snapshots));glUseProgram(0);glDeleteProgram(p);glDeleteShader(vs);glDeleteShader(fs);glDeleteBuffers(1,&buffer);glXMakeCurrent(d,None,NULL);glXDestroyContext(d,c);XDestroyWindow(d,w);XFree(v);XCloseDisplay(d);return 0;}
