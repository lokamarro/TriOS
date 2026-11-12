// Guest GLES2 offscreen draw -> Metal -> sample that texture in a second pass.
#include "guest_gl_subset.h"
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
static GLuint shader(GLenum kind,const char *src){GLuint s=glCreateShader(kind);glShaderSource(s,1,&src,NULL);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);assert(ok);return s;}
static GLuint program(const char *fragment){GLuint p=glCreateProgram(),v=shader(GL_VERTEX_SHADER,"attribute vec2 pos; varying vec2 uv; void main(){uv=(pos+1.0)*0.5;gl_Position=vec4(pos,0.0,1.0);}"),f=shader(GL_FRAGMENT_SHADER,fragment);glAttachShader(p,v);glAttachShader(p,f);glBindAttribLocation(p,0,"pos");glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(ok);glDeleteShader(v);glDeleteShader(f);return p;}
static void pixel(int x,int y,int r,int g,int b){unsigned char p[4];glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);assert(!glGetError());if(p[0]!=r||p[1]!=g||p[2]!=b){fprintf(stderr,"FBO_PIXEL %d,%d got=%u,%u,%u expected=%d,%d,%d\n",x,y,p[0],p[1],p[2],r,g,b);assert(0);}}
static void storage(GLuint t,unsigned w,unsigned h){glBindTexture(GL_TEXTURE_2D,t);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);assert(!glGetError());}
int main(void){EGLDisplay d=eglGetDisplay(0);EGLConfig cfg;EGLint n;assert(eglInitialize(d,0,0)&&eglChooseConfig(d,(EGLint[]){EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE},&cfg,1,&n));EGLContext c=eglCreateContext(d,cfg,0,(EGLint[]){EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE});EGLSurface s=eglCreatePbufferSurface(d,cfg,(EGLint[]){EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE});assert(c&&s&&eglMakeCurrent(d,s,s,c));
 GLuint f,t;glGenFramebuffers(1,&f);assert(!glIsFramebuffer(f));glBindFramebuffer(GL_FRAMEBUFFER,f);assert(glIsFramebuffer(f));assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);glClear(GL_COLOR_BUFFER_BIT);assert(glGetError()==GL_INVALID_FRAMEBUFFER_OPERATION);
 glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t,0);assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);storage(t,16,16);assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
 GLuint draw=program("precision mediump float; varying vec2 uv; void main(){gl_FragColor=uv.y<0.5?vec4(1,0,0,1):vec4(0,0,1,1);}"),sample=program("precision mediump float; varying vec2 uv; uniform sampler2D image; void main(){gl_FragColor=texture2D(image,uv);}");
 const GLfloat v[]={-1,-1,1,-1,-1,1,-1,1,1,-1,1,1};glVertexAttribPointer(0,2,GL_FLOAT,0,0,v);glEnableVertexAttribArray(0);glViewport(0,0,16,16);glUseProgram(draw);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,6);pixel(8,2,255,0,0);pixel(8,14,0,0,255);
 // A second pass samples the actual offscreen GPU output, preserving GL origin.
 glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,32,32);glUseProgram(sample);glUniform1i(glGetUniformLocation(sample,"image"),0);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,6);pixel(16,4,255,0,0);pixel(16,28,0,0,255);
 // Feedback is rejected explicitly rather than causing Metal validation failure.
 glBindFramebuffer(GL_FRAMEBUFFER,f);glDrawArrays(GL_TRIANGLES,0,6);assert(glGetError()==GL_INVALID_OPERATION);
 // Deleted attachment survives while the FBO is unbound, and name reuse must
 // not replace it. Deleting the currently bound attachment detaches it.
 glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteTextures(1,&t);GLuint replacement;glGenTextures(1,&replacement);assert(replacement==t);storage(replacement,4,4);glBindFramebuffer(GL_FRAMEBUFFER,f);assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);pixel(8,14,0,0,255);
 glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,replacement,0);glViewport(0,0,4,4);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);pixel(2,2,0,255,0);
 // Respecification keeps attachment identity, changes dimensions, zero storage
 // makes the FBO incomplete, and ALPHA storage is not color-renderable.
 storage(replacement,8,8);assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);glClear(GL_COLOR_BUFFER_BIT);pixel(7,7,0,255,0);
 glTexImage2D(GL_TEXTURE_2D,0,GL_ALPHA,8,8,0,GL_ALPHA,GL_UNSIGNED_BYTE,0);assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_UNSUPPORTED);glClear(GL_COLOR_BUFFER_BIT);assert(glGetError()==GL_INVALID_FRAMEBUFFER_OPERATION);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,0,0,0,GL_RGBA,GL_UNSIGNED_BYTE,0);assert(!glGetError()&&glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);storage(replacement,8,8);
 glDeleteTextures(1,&replacement);assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);glDeleteFramebuffers(1,&f);GLint bound;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&bound);assert(!bound);
 glBindFramebuffer(GL_FRAMEBUFFER,999);assert(glGetError()==GL_INVALID_VALUE);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,0,0);assert(glGetError()==GL_INVALID_ENUM);
 uint64_t submissions,reads,ns;assert(lrGPUGetStatistics(&submissions,&reads,&ns)&&submissions>=4&&reads>=4);printf("FRAMEBUFFER_CONTRACT_PASS offscreen=PASS sampled_pixels=PASS orientation=PASS feedback=PASS resize=PASS deletion=PASS retained_identity=PASS unsupported=PASS submissions=%llu readbacks=%llu gpu_ns=%llu\n",(unsigned long long)submissions,(unsigned long long)reads,(unsigned long long)ns);
 int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);struct lr_gpu_request r={.version=LR_GPU_VERSION,.operation=LR_GPU_CREATE,.width=4,.height=4};assert(!ioctl(fd,LR_GPU_IOCTL,&r));struct lr_gpu_framebuffer packet={.command=LR_GPU_FB_BIND,.name=17};r=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_FRAMEBUFFER,.address=(uintptr_t)&packet,.bytes=sizeof packet};assert(ioctl(fd,LR_GPU_IOCTL,&r)<0&&errno==EINVAL);r.address=1;assert(ioctl(fd,LR_GPU_IOCTL,&r)<0&&errno==EFAULT);close(fd);
 // Deleted unbound attachments remain charged to the host quota. Three
 // 512x512 mipmapped images plus render storage fit; a fourth must fail.
 GLuint retained[4],images[4];glGenFramebuffers(4,retained);glGenTextures(4,images);
 unsigned accepted=0;for(unsigned i=0;i<4;i++){
  glBindFramebuffer(GL_FRAMEBUFFER,0);glBindTexture(GL_TEXTURE_2D,images[i]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,512,512,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
  GLenum allocation=glGetError();if(allocation==GL_OUT_OF_MEMORY)break;assert(!allocation);glBindFramebuffer(GL_FRAMEBUFFER,retained[i]);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,images[i],0);
  GLenum error=glGetError();if(error==GL_OUT_OF_MEMORY)break;assert(!error);accepted++;
  glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteTextures(1,&images[i]);
 }
 assert(accepted==3);glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(4,retained);glDeleteTextures(4,images);
 GLuint recovered;glGenTextures(1,&recovered);storage(recovered,512,512);glDeleteTextures(1,&recovered);
 puts("FRAMEBUFFER_QUOTA_RECOVERY_PASS retained_deleted_attachments=PASS reclaim=PASS");
 glUseProgram(0);glDeleteProgram(draw);glDeleteProgram(sample);assert(!glGetError());assert(eglMakeCurrent(d,0,0,0)&&eglDestroyContext(d,c)&&eglDestroySurface(d,s)&&eglTerminate(d));
}
