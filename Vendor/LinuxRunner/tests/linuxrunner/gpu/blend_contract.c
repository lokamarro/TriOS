#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
extern int lrGPUGetStatistics(uint64_t*,uint64_t*,uint64_t*);
static GLuint shader(GLenum type,const char *text){GLuint s=glCreateShader(type);glShaderSource(s,1,&text,NULL);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);assert(ok);return s;}
static void pixel(int r,int g,int b,int a){unsigned char p[4];glReadPixels(32,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);fprintf(stdout,"BLEND_PIXEL=%u,%u,%u,%u\n",p[0],p[1],p[2],p[3]);assert(abs(p[0]-r)<=2&&abs(p[1]-g)<=2&&abs(p[2]-b)<=2&&abs(p[3]-a)<=2);}
int main(void){setbuf(stdout,NULL);Display *d=XOpenDisplay(NULL);assert(d);XVisualInfo *v=glXChooseVisual(d,DefaultScreen(d),NULL);assert(v);XSetWindowAttributes a={.colormap=XCreateColormap(d,RootWindow(d,v->screen),v->visual,AllocNone)};Window w=XCreateWindow(d,RootWindow(d,v->screen),0,0,64,64,0,v->depth,InputOutput,v->visual,CWColormap,&a);GLXContext c=glXCreateContext(d,v,NULL,True);assert(c&&glXMakeCurrent(d,w,c));
 GLuint vs=shader(GL_VERTEX_SHADER,"#version 330 core\nin vec2 position;void main(){gl_Position=vec4(position,0,1);}"),fs=shader(GL_FRAGMENT_SHADER,"#version 330 core\nuniform vec4 color;out vec4 result;void main(){result=color;}");GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glBindAttribLocation(p,0,"position");glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(p);glUniform4f(glGetUniformLocation(p,"color"),0,1,0,.5);float vertices[]={-1,-1,1,-1,0,1};glVertexAttribPointer(0,2,GL_FLOAT,False,0,vertices);glEnableVertexAttribArray(0);glClearColor(1,0,0,1);glViewport(0,0,64,64);
 assert(!glIsEnabled(GL_BLEND));glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glEnable(GL_BLEND);assert(glIsEnabled(GL_BLEND));glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);pixel(128,128,0,191);
 glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);pixel(128,128,0,255);
 glBlendColor(.25,.5,.75,.5);glBlendFunc(GL_CONSTANT_COLOR,GL_ZERO);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,128,0,64);
 glBlendFunc(GL_ONE,GL_ONE);glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT,GL_FUNC_ADD);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);pixel(255,0,0,255);
 glBlendEquation(GL_MIN);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,0,0,128);glBlendEquation(GL_MAX);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);pixel(255,255,0,255);
 glBlendFunc(GL_ONE,GL_SRC_ALPHA_SATURATE);assert(glGetError()==GL_INVALID_ENUM);glBlendEquation(0xdead);assert(glGetError()==GL_INVALID_ENUM);
 glDisable(GL_BLEND);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,255,0,128);
 // The legacy path must use the same correctly translated blend state.
 glUseProgram(0);glBlendEquation(GL_FUNC_ADD);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glEnable(GL_BLEND);glClear(GL_COLOR_BUFFER_BIT);glColor4f(0,1,0,.5);glBegin(GL_TRIANGLES);glVertex2f(-1,-1);glVertex2f(1,-1);glVertex2f(0,1);glEnd();pixel(128,128,0,191);glDisable(GL_BLEND);assert(glGetError()==GL_NO_ERROR);
 uint64_t draws,reads,ns;assert(lrGPUGetStatistics(&draws,&reads,&ns)&&draws&&ns);printf("BLEND_METAL_COUNTERS operations=%llu readbacks=%llu gpu_ns=%llu\n",(unsigned long long)draws,(unsigned long long)reads,(unsigned long long)ns);glDeleteProgram(p);glDeleteShader(vs);glDeleteShader(fs);glXMakeCurrent(d,None,NULL);glXDestroyContext(d,c);XDestroyWindow(d,w);XFree(v);XCloseDisplay(d);puts("BLEND_CONTRACT_PASS alpha=PASS separate=PASS constant=PASS equation=PASS min_max=PASS disable=PASS legacy=PASS GPU_pixels=PASS invalid=PASS");return 0;}
