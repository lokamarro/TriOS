#define GLX_GLXEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
extern int lrGPUGetStatistics(uint64_t*,uint64_t*,uint64_t*);
static Display *display;static Window window;static GLXContext context;
static void *contender(void *unused){(void)unused;assert(!glXMakeCurrent(display,window,context));assert(!glXGetCurrentContext());return NULL;}
static GLuint shader(GLenum type,const char *source){GLuint s=glCreateShader(type);glShaderSource(s,1,&source,NULL);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof log,NULL,log);fprintf(stderr,"GLX_SHADER_FAIL %s\n",log);}assert(ok);return s;}
static Window create(Display *d,XVisualInfo *v){XSetWindowAttributes a={.colormap=XCreateColormap(d,RootWindow(d,v->screen),v->visual,AllocNone)};Window w=XCreateWindow(d,RootWindow(d,v->screen),0,0,96,96,0,v->depth,InputOutput,v->visual,CWColormap,&a);XMapWindow(d,w);XSync(d,False);return w;}
int main(void){setbuf(stdout,NULL);XInitThreads();display=XOpenDisplay(NULL);assert(display);int n=0;GLXFBConfig *configs=glXChooseFBConfig(display,DefaultScreen(display),NULL,&n);assert(configs&&n==1);XVisualInfo *v=glXGetVisualFromFBConfig(display,configs[0]);assert(v);window=create(display,v);XStoreName(display,window,"LinuxRunner GLX Metal shader contract");
 int attrs[]={GLX_CONTEXT_MAJOR_VERSION_ARB,4,GLX_CONTEXT_MINOR_VERSION_ARB,3,GLX_CONTEXT_PROFILE_MASK_ARB,GLX_CONTEXT_CORE_PROFILE_BIT_ARB,None};
 assert(!glXCreateContextAttribsARB(display,configs[0],NULL,True,attrs));assert(!strstr(glXQueryExtensionsString(display,DefaultScreen(display)),"GLX_ARB_create_context"));assert(!glXGetProcAddress((const GLubyte*)"glDispatchCompute"));
 context=glXCreateContext(display,v,NULL,True);assert(context);assert(!glXCreateContext(display,v,context,True));assert(glXMakeCurrent(display,window,context));assert(glXGetCurrentContext()==context&&glXGetCurrentDrawable()==window&&glXGetCurrentDisplay()==display);
 pthread_t worker;assert(!pthread_create(&worker,NULL,contender,NULL));pthread_join(worker,NULL);
 printf("GLX_METAL_ACTUAL vendor=%s renderer=%s version=%s\n",glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION));
 GLuint vs=shader(GL_VERTEX_SHADER,"#version 120\nattribute vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);}");
 GLuint fs=shader(GL_FRAGMENT_SHADER,"#version 120\nuniform vec4 color;void main(){gl_FragColor=color;}");
 GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glBindAttribLocation(p,0,"position");glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char log[2048];glGetProgramInfoLog(p,sizeof log,NULL,log);fprintf(stderr,"GLX_LINK_FAIL %s\n",log);}assert(ok);glUseProgram(p);glUniform4f(glGetUniformLocation(p,"color"),0,1,0,1);
 GLuint vao;glGenVertexArrays(1,&vao);assert(!glIsVertexArray(vao));glBindVertexArray(vao);assert(glIsVertexArray(vao));
 float vertices[]={-1,-1,1,-1,0,1};GLuint b;glGenBuffers(1,&b);glBindBuffer(GL_ARRAY_BUFFER,b);glBufferData(GL_ARRAY_BUFFER,sizeof vertices,vertices,GL_STATIC_DRAW);glVertexAttribPointer(0,2,GL_FLOAT,False,0,0);glEnableVertexAttribArray(0);glViewport(0,0,96,96);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);
 unsigned char pixel[4];glReadPixels(48,24,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[1]>250&&pixel[0]<5);glReadPixels(0,95,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]>250&&pixel[1]<5);glXSwapBuffers(display,window);assert(glGetError()==GL_NO_ERROR);
 uint64_t submissions,reads,ns;assert(lrGPUGetStatistics(&submissions,&reads,&ns)&&submissions&&ns);printf("GLX_METAL_COUNTERS completed_render_operations=%llu readbacks=%llu gpu_ns=%llu\n",(unsigned long long)submissions,(unsigned long long)reads,(unsigned long long)ns);
 // An unbound VAO retains a deleted VBO object. Reusing the name must not
 // overwrite that retained object's storage.
 glBindVertexArray(0);glDeleteBuffers(1,&b);glBindBuffer(GL_ARRAY_BUFFER,b);float bad[]={0,0,0,0,0,0};glBufferData(GL_ARRAY_BUFFER,sizeof bad,bad,GL_STATIC_DRAW);glBindVertexArray(vao);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(48,24,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[1]>250&&pixel[0]<5);
 // Modern vertex/fragment syntax is tested independently of capability
 // negotiation: a shader subset does not confer desktop GL 3.3 conformance.
 GLuint mv=shader(GL_VERTEX_SHADER,"#version 330 core\nin vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);}");
 GLuint mf=shader(GL_FRAGMENT_SHADER,"#version 330 core\nout vec4 outputColor;void main(){outputColor=vec4(0,0,1,1);}");
 GLuint mp=glCreateProgram();glAttachShader(mp,mv);glAttachShader(mp,mf);glBindAttribLocation(mp,0,"position");glLinkProgram(mp);glGetProgramiv(mp,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(mp);glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(48,24,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[2]>250&&pixel[0]<5&&pixel[1]<5);glUseProgram(p);glDeleteProgram(mp);glDeleteShader(mv);glDeleteShader(mf);
 // Second context owns independent GL names/state. Rebinding a drawable and
 // resizing must continue to use the original context's resources.
 GLXContext second=glXCreateNewContext(display,configs[0],GLX_RGBA_TYPE,NULL,True);assert(second);Window other=create(display,v);assert(glXMakeCurrent(display,other,second));assert(glCreateShader(GL_VERTEX_SHADER)==1);assert(glXMakeCurrent(display,window,context));XResizeWindow(display,window,960,540);XSync(display,False);XWindowAttributes wa;for(int tries=0;tries<100;tries++){XGetWindowAttributes(display,window,&wa);if(wa.width==960&&wa.height==540)break;usleep(10000);}assert(wa.width==960&&wa.height==540);glViewport(0,0,960,540);glDrawArrays(GL_TRIANGLES,0,3);glXSwapBuffers(display,window);glReadPixels(480,100,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[1]>250&&pixel[0]<2&&pixel[2]<2&&glGetError()==GL_NO_ERROR);
 assert(!glXMakeContextCurrent(display,window,other,context));assert(glXGetCurrentContext()==context);
 glDeleteVertexArrays(1,&vao);glDeleteBuffers(1,&b);glDeleteProgram(p);glDeleteShader(vs);glDeleteShader(fs);
 glXDestroyContext(display,context);assert(glXGetCurrentContext()==context);assert(glXMakeCurrent(display,None,NULL));assert(!glXMakeCurrent(display,window,context));glXDestroyContext(display,second);
 // Bounded slots recover after deferred destruction and repeated shutdown.
 for(int i=0;i<24;i++){GLXContext c=glXCreateContext(display,v,NULL,True);assert(c);glXDestroyContext(display,c);}
 XDestroyWindow(display,window);XDestroyWindow(display,other);XFree(v);XFree(configs);XCloseDisplay(display);puts("GLX_METAL_CONTRACT_PASS desktop_GLSL120=PASS GLSL330_vertex_fragment=PASS VBO=PASS VAO=PASS deleted_VBO_retention=PASS pixels=PASS GPU=PASS ownership=PASS wide_960x540=PASS resize=PASS cleanup=PASS unsupported43=PASS");return 0;
}
