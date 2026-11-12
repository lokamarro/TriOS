// Actual system Mesa GLX/core-context probe. Never overrides GL versions or
// substitutes the experimental GLES provider for requested desktop OpenGL.
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>
static int xerror;
static int error(Display *d,XErrorEvent *e){(void)d;xerror=e->error_code;return 0;}
static GLuint shader(GLenum type,const char *text){GLuint s=glCreateShader(type);glShaderSource(s,1,&text,NULL);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof log,NULL,log);fprintf(stderr,"GLX_SHADER_ERROR %s\n",log);}assert(ok);return s;}
int main(void){
 setbuf(stdout,NULL);Display *d=XOpenDisplay(NULL);if(!d){fprintf(stderr,"GLX_DIAGNOSTIC_FAIL stage=XOpenDisplay\n");return 2;}XSetErrorHandler(error);
 printf("GLX_ENV LIBGL_ALWAYS_SOFTWARE=%s GALLIUM_DRIVER=%s LD_LIBRARY_PATH=%s\n",getenv("LIBGL_ALWAYS_SOFTWARE")?:"unset",getenv("GALLIUM_DRIVER")?:"unset",getenv("LD_LIBRARY_PATH")?:"unset");
 Dl_info library;if(dladdr((void *)glXQueryVersion,&library))printf("GLX_LOADER libGL=%s\n",library.dli_fname);
 int major,minor,eb,ev;if(!glXQueryExtension(d,&eb,&ev)){fprintf(stderr,"GLX_DIAGNOSTIC_FAIL stage=server-extension GLX-disabled; restart display with LINUXRUNNER_GLX=software\n");XCloseDisplay(d);return 3;}assert(glXQueryVersion(d,&major,&minor));
 printf("GLX_SERVER version=%d.%d error_base=%d event_base=%d\n",major,minor,eb,ev);
 const char *extensions=glXQueryExtensionsString(d,DefaultScreen(d));printf("GLX_EXTENSIONS %s\n",extensions?extensions:"NULL");
 int attrs[]={GLX_X_RENDERABLE,True,GLX_DRAWABLE_TYPE,GLX_WINDOW_BIT,GLX_RENDER_TYPE,GLX_RGBA_BIT,GLX_RED_SIZE,8,GLX_GREEN_SIZE,8,GLX_BLUE_SIZE,8,GLX_DEPTH_SIZE,24,GLX_DOUBLEBUFFER,True,None};
 int count=0;GLXFBConfig *configs=glXChooseFBConfig(d,DefaultScreen(d),attrs,&count);printf("GLX_CONFIGS matching=%d\n",count);if(!configs||!count){fprintf(stderr,"GLX_DIAGNOSTIC_FAIL stage=framebuffer-config\n");return 4;}XVisualInfo *v=glXGetVisualFromFBConfig(d,configs[0]);assert(v);
 XSetWindowAttributes wa={.colormap=XCreateColormap(d,RootWindow(d,v->screen),v->visual,AllocNone),.event_mask=StructureNotifyMask};
 Window w=XCreateWindow(d,RootWindow(d,v->screen),0,0,64,64,0,v->depth,InputOutput,v->visual,CWColormap|CWEventMask,&wa);XStoreName(d,w,"LinuxRunner real Mesa GLX 4.3 contract");XMapWindow(d,w);XSync(d,False);
 PFNGLXCREATECONTEXTATTRIBSARBPROC create=(void*)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");assert(create);
 int ca[]={GLX_CONTEXT_MAJOR_VERSION_ARB,4,GLX_CONTEXT_MINOR_VERSION_ARB,3,GLX_CONTEXT_PROFILE_MASK_ARB,GLX_CONTEXT_CORE_PROFILE_BIT_ARB,None};
 GLXContext c=create(d,configs[0],NULL,True,ca);XSync(d,False);if(!c||xerror){fprintf(stderr,"GLX_CONTEXT_FAIL requested=4.3-core xerror=%d\n",xerror);return 1;}
 assert(glXMakeContextCurrent(d,w,w,c));assert(glXGetCurrentContext()==c&&glXGetCurrentDrawable()==w&&glXGetCurrentReadDrawable()==w);
 printf("GLX_ACTUAL vendor=%s renderer=%s version=%s GLSL=%s direct=%d\n",glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION),glGetString(GL_SHADING_LANGUAGE_VERSION),glXIsDirect(d,c));
 FILE *maps=fopen("/proc/self/maps","r");if(maps){char line[1024];while(fgets(line,sizeof line,maps))if(strstr(line,"/libGL")||strstr(line,"/libEGL")||strstr(line,"/libLLVM")||strstr(line,"_dri.so")||strstr(line,"/libgallium"))printf("GLX_LIBRARY %s",line);fclose(maps);}
 GLuint vs=shader(GL_VERTEX_SHADER,"#version 330 core\nconst vec2 p[3]=vec2[3](vec2(-1,-1),vec2(1,-1),vec2(0,1));void main(){gl_Position=vec4(p[gl_VertexID],0,1);}");
 GLuint fs=shader(GL_FRAGMENT_SHADER,"#version 330 core\nout vec4 color;void main(){color=vec4(0,1,0,1);}");
 GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);GLint linked;glGetProgramiv(program,GL_LINK_STATUS,&linked);assert(linked);
 GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);glUseProgram(program);glViewport(0,0,64,64);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);
 unsigned char pixel[4];glReadPixels(32,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[1]>250&&pixel[0]<5);glReadPixels(0,63,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]>250&&pixel[1]<5);glXSwapBuffers(d,w);glFinish();assert(glGetError()==GL_NO_ERROR);
 // A shared context uses the same program but has independent VAO state.
 GLXContext shared=create(d,configs[0],c,True,ca);assert(shared);assert(glXMakeCurrent(d,w,shared));assert(glIsProgram(program));assert(!glIsVertexArray(vao));assert(glXMakeCurrent(d,w,c));
 glDeleteVertexArrays(1,&vao);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);assert(glXMakeCurrent(d,None,NULL));assert(glXGetCurrentContext()==NULL);
 glXDestroyContext(d,shared);glXDestroyContext(d,c);XDestroyWindow(d,w);XFreeColormap(d,wa.colormap);XFree(v);XFree(configs);XCloseDisplay(d);
 puts("GLX_CONTEXT_PASS requested_4_3_core=PASS modern_shaders=PASS VAO=PASS pixels=PASS swap=PASS sharing=PASS cleanup=PASS backend=system-Mesa-not-Metal");return 0;
}
