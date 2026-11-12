// Genuine ARM64 Linux EGL/GLES client. GPU readback is independently checked,
// then shown through a real X11 window. No CPU triangle rasterizer exists here.
#include "guest_gl_subset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#define W 256
static void check_pixel(const unsigned char *p,int x,int y,int r,int g,int b){p+=(y*W+x)*4;assert(abs(p[0]-r)<3&&abs(p[1]-g)<3&&abs(p[2]-b)<3&&p[3]>252);}
int main(int argc,char **argv){
 EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLint major,minor,count;EGLConfig config;
 assert(eglInitialize(display,&major,&minor));assert(eglBindAPI(EGL_OPENGL_ES_API));
 EGLint config_attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES_BIT,EGL_NONE};
 assert(eglChooseConfig(display,config_attrs,&config,1,&count)&&count==1);
 EGLint attrs[]={EGL_WIDTH,W,EGL_HEIGHT,W,EGL_NONE},ca[]={EGL_CONTEXT_CLIENT_VERSION,1,EGL_NONE};
 EGLSurface surface=eglCreatePbufferSurface(display,config,attrs);EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);
 assert(surface&&context&&eglMakeCurrent(display,surface,surface,context));
 printf("GPU_BRIDGE renderer=%s api=%s EGL=%d.%d\n",glGetString(GL_RENDERER),glGetString(GL_VERSION),major,minor);
 glViewport(0,0,W,W);GLfloat vertices[]={-.75f,-.75f,.75f,-.75f,0,.75f};GLfloat colors[]={1,0,0,1,1,0,0,1,1,0,0,1};
 glVertexPointer(2,GL_FLOAT,0,vertices);glColorPointer(4,GL_FLOAT,0,colors);glEnableClientState(GL_VERTEX_ARRAY);glEnableClientState(GL_COLOR_ARRAY);
 unsigned char *pixels=malloc(W*W*4);assert(pixels);
 glClearColor(.02f,.04f,.10f,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,W,W,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(glGetError()==GL_NO_ERROR);
 check_pixel(pixels,128,100,255,0,0);check_pixel(pixels,5,5,5,10,26);check_pixel(pixels,128,230,5,10,26);
 // Move the triangle: a previously covered pixel becomes background, while a
 // new region becomes green. Proves guest vertex/color changes affect the GPU.
 for(int i=0;i<3;i++){vertices[i*2]+=.3f;colors[i*4]=0;colors[i*4+1]=1;}
 glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,W,W,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(glGetError()==GL_NO_ERROR);
 check_pixel(pixels,180,100,0,255,0);check_pixel(pixels,45,50,5,10,26);
 glDrawArrays(0xffff,0,3);assert(glGetError()==GL_INVALID_ENUM);
 glClear(0xffff);assert(glGetError()==GL_INVALID_VALUE);
 assert(!eglCreateContext(display,config,EGL_NO_CONTEXT,(EGLint[]){EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE}));assert(eglGetError()==EGL_BAD_ATTRIBUTE);
 // Rebuild an interpolated image for the actual Linux display.
 GLfloat rgb[]={1,.1f,.15f,1,.1f,.8f,1,1,.5f,.2f,1,1};memcpy(colors,rgb,sizeof colors);
 vertices[0]=-.75f;vertices[2]=.75f;vertices[4]=0;
 glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,W,W,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(glGetError()==0);
 uint64_t submits,reads,gpu_ns;assert(lrGPUGetStatistics(&submits,&reads,&gpu_ns)&&submits==6&&reads==3);printf("GPU_BRIDGE_COUNTERS completed_submissions=%llu readbacks=%llu gpu_ns=%llu\n",(unsigned long long)submits,(unsigned long long)reads,(unsigned long long)gpu_ns);
 puts("GPU_BRIDGE_PIXEL_PASS rendering=Metal-GPU guest=Linux-ARM64 triangles=3 readbacks=3 unsupported=PASS");fflush(stdout);
 if(argc>1&&!strcmp(argv[1],"--headless"))goto finish;
 Display *x=XOpenDisplay(NULL);assert(x);int screen=DefaultScreen(x);
 Window window=XCreateSimpleWindow(x,RootWindow(x,screen),30,35,W,W+45,0,0,0x050a1a);
 XStoreName(x,window,"LinuxRunner - Linux EGL/GLES to Metal GPU");
 XSelectInput(x,window,ExposureMask|KeyPressMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask|EnterWindowMask|LeaveWindowMask|FocusChangeMask|StructureNotifyMask);XMapWindow(x,window);
 unsigned char *image_data=calloc(W*W,4);assert(image_data);
 for(int y=0;y<W;y++)for(int ix=0;ix<W;ix++){unsigned char *s=pixels+((W-1-y)*W+ix)*4,*d=image_data+(y*W+ix)*4;d[0]=s[2];d[1]=s[1];d[2]=s[0];}
 XImage *image=XCreateImage(x,DefaultVisual(x,screen),DefaultDepth(x,screen),ZPixmap,0,(char *)image_data,W,W,32,0);assert(image);
 GC gc=XCreateGC(x,window,0,NULL);Atom close=XInternAtom(x,"WM_DELETE_WINDOW",False);XSetWMProtocols(x,window,&close,1);
 unsigned events[36]={0}; unsigned redraws=0; const char *exit_reason="unknown";
 for(;;){XEvent event;XNextEvent(x,&event);if(event.type>=0&&event.type<36&&++events[event.type]<=4){printf("GPU_X11_EVENT type=%d count=%u\n",event.type,events[event.type]);fflush(stdout);}if(event.type==Expose){redraws++;XPutImage(x,window,gc,image,0,0,0,0,W,W);XSetForeground(x,gc,0xffffff);XDrawString(x,window,gc,8,W+25,"ARM64 Linux -> Metal GPU",24);XFlush(x);}if(event.type==KeyPress&&XLookupKeysym(&event.xkey,0)==XK_Escape){exit_reason="escape";break;}
 if(event.type==ClientMessage&&event.xclient.message_type==XInternAtom(x,"WM_PROTOCOLS",False)&&(Atom)event.xclient.data.l[0]==close){exit_reason="wm-delete";break;}
 if(event.type==DestroyNotify){exit_reason="window-destroy";break;}}
 printf("GPU_EXIT reason=%s motion=%u enter=%u leave=%u buttons=%u redraws=%u\n",exit_reason,events[MotionNotify],events[EnterNotify],events[LeaveNotify],events[ButtonPress],redraws);fflush(stdout);
 XDestroyImage(image);XFreeGC(x,gc);XDestroyWindow(x,window);XCloseDisplay(x);
 finish:free(pixels);assert(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT));assert(eglDestroyContext(display,context));assert(eglDestroySurface(display,surface));assert(eglTerminate(display));puts("GPU_BRIDGE_CLEANUP_PASS");return 0;
}
