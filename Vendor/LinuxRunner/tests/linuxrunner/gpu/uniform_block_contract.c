#define LR_GPU_GUEST 1
#include "guest_gpu.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int lrGPUGetStatistics(uint64_t*,uint64_t*,uint64_t*);
static GLuint shader(GLenum type,const char *text){GLuint s=glCreateShader(type);glShaderSource(s,1,&text,NULL);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof log,NULL,log);fprintf(stderr,"UBO_SHADER_FAIL %s\n",log);}assert(ok);return s;}
static GLuint program(void){
 const char *block="layout(std140) uniform Scene {mat4 transform;vec4 tint;float weights[2];};\n";
 char vertex[512],fragment[768];snprintf(vertex,sizeof vertex,"#version 330 core\n%s layout(location=0) in vec2 position;void main(){gl_Position=transform*vec4(position,0,1);}",block);
 snprintf(fragment,sizeof fragment,"#version 430 core\n%s layout(std140,binding=7) uniform Palette {vec4 colors[2];int choice;};out vec4 color;void main(){color=colors[choice]*tint*weights[0];}",block);
 GLuint v=shader(GL_VERTEX_SHADER,vertex),f=shader(GL_FRAGMENT_SHADER,fragment),p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char log[2048];glGetProgramInfoLog(p,sizeof log,NULL,log);fprintf(stderr,"UBO_LINK_FAIL %s\n",log);}assert(ok);glDeleteShader(v);glDeleteShader(f);return p;
}
static void pixel(int red,int green,int blue){unsigned char p[4];glReadPixels(32,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);if(abs(p[0]-red)>2||abs(p[1]-green)>2||abs(p[2]-blue)>2)fprintf(stderr,"UBO_PIXEL %u %u %u expected %d %d %d\n",p[0],p[1],p[2],red,green,blue);assert(abs(p[0]-red)<=2&&abs(p[1]-green)<=2&&abs(p[2]-blue)<=2);}
int main(void){setbuf(stdout,NULL);Display *d=XOpenDisplay(NULL);assert(d);XVisualInfo *v=glXChooseVisual(d,DefaultScreen(d),NULL);assert(v);XSetWindowAttributes a={.colormap=XCreateColormap(d,RootWindow(d,v->screen),v->visual,AllocNone)};Window w=XCreateWindow(d,RootWindow(d,v->screen),0,0,64,64,0,v->depth,InputOutput,v->visual,CWColormap,&a);XMapWindow(d,w);XSync(d,False);GLXContext c=glXCreateContext(d,v,NULL,True);assert(c&&glXMakeCurrent(d,w,c));
 GLuint p=program();glUseProgram(p);GLint count,size;glGetProgramiv(p,GL_ACTIVE_UNIFORM_BLOCKS,&count);assert(count==2);GLuint scene=glGetUniformBlockIndex(p,"Scene"),palette=glGetUniformBlockIndex(p,"Palette");assert(scene!=GL_INVALID_INDEX&&palette!=GL_INVALID_INDEX&&scene!=palette);assert(glGetUniformBlockIndex(p,"absent")==GL_INVALID_INDEX);glGetActiveUniformBlockiv(p,scene,GL_UNIFORM_BLOCK_DATA_SIZE,&size);assert(size==112);glGetActiveUniformBlockiv(p,palette,GL_UNIFORM_BLOCK_DATA_SIZE,&size);assert(size==48);glGetActiveUniformBlockiv(p,palette,GL_UNIFORM_BLOCK_BINDING,&size);assert(size==7);
 const char *names[]={"transform","tint","weights[0]","colors[0]","choice"};GLuint indices[5];GLint offsets[5],stride[5];glGetUniformIndices(p,5,names,indices);for(int i=0;i<5;i++)assert(indices[i]!=GL_INVALID_INDEX);glGetActiveUniformsiv(p,5,indices,GL_UNIFORM_OFFSET,offsets);assert(offsets[0]==0&&offsets[1]==64&&offsets[2]==80&&offsets[3]==0&&offsets[4]==32);glGetActiveUniformsiv(p,5,indices,GL_UNIFORM_ARRAY_STRIDE,stride);assert(stride[2]==16&&stride[3]==16);glGetActiveUniformsiv(p,1,indices,GL_UNIFORM_MATRIX_STRIDE,stride);assert(stride[0]==16);glGetActiveUniformBlockiv(p,scene,GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER,&size);assert(size);glGetActiveUniformBlockiv(p,scene,GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER,&size);assert(size);glGetProgramiv(p,GL_ACTIVE_UNIFORMS,&count);assert(count==5);char name[64];GLenum type;glGetActiveUniform(p,indices[2],sizeof name,NULL,&size,&type,name);assert(!strcmp(name,"weights[0]")&&size==2&&type==GL_FLOAT);assert(glGetUniformLocation(p,"tint")==-1);
 GLuint buffers[3];glGenBuffers(3,buffers);float vertices[]={-1,-1,1,-1,0,1};glBindBuffer(GL_ARRAY_BUFFER,buffers[0]);glBufferData(GL_ARRAY_BUFFER,sizeof vertices,vertices,GL_STATIC_DRAW);glVertexAttribPointer(0,2,GL_FLOAT,False,0,0);glEnableVertexAttribArray(0);
 unsigned char bytes[192]={0};float *matrix=(void*)(bytes+32);for(int i=0;i<4;i++)matrix[i*5]=1;float *tint=(void*)(bytes+96);for(int i=0;i<4;i++)tint[i]=1;float one=1;memcpy(bytes+112,&one,4);float colors[]={0,1,0,1,0,0,1,1};memcpy(bytes+144,colors,sizeof colors);glBindBuffer(GL_UNIFORM_BUFFER,buffers[1]);glBufferData(GL_UNIFORM_BUFFER,sizeof bytes,bytes,GL_DYNAMIC_DRAW);glUniformBlockBinding(p,scene,1);glBindBufferRange(GL_UNIFORM_BUFFER,1,buffers[1],32,112);glBindBufferRange(GL_UNIFORM_BUFFER,7,buffers[1],144,48);glViewport(0,0,64,64);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,255,0);
 // Same data store, nonzero ranges, indexed state query and updates.
 GLint binding;glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING,7,&binding);assert(binding==buffers[1]);int64_t start;glGetInteger64i_v(GL_UNIFORM_BUFFER_START,7,&start);assert(start==144);int choice=1;glBufferSubData(GL_UNIFORM_BUFFER,176,4,&choice);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,0,255);
 glBindBufferRange(GL_UNIFORM_BUFFER,7,buffers[1],145,47);assert(glGetError()==GL_INVALID_VALUE);glBindBufferRange(GL_UNIFORM_BUFFER,7,buffers[1],144,16);assert(glGetError()==GL_NO_ERROR);glDrawArrays(GL_TRIANGLES,0,3);assert(glGetError()==GL_INVALID_OPERATION);glBindBufferRange(GL_UNIFORM_BUFFER,7,buffers[1],144,48);
 // Draw snapshots must survive a subsequent update before Finish/readback.
 choice=0;glBufferSubData(GL_UNIFORM_BUFFER,176,4,&choice);glDrawArrays(GL_TRIANGLES,0,3);choice=1;glBufferSubData(GL_UNIFORM_BUFFER,176,4,&choice);pixel(0,255,0);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,0,255);
 // Pipeline-cache reuse must not share mutable block values between programs.
 GLuint q=program();glUseProgram(q);glUniformBlockBinding(q,glGetUniformBlockIndex(q,"Scene"),1);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,0,255);glUseProgram(p);
 // Deleting a bound UBO detaches indexed and generic bindings. Names can be
 // reused without exposing old storage, and quotas recover after teardown.
 glDeleteBuffers(1,&buffers[1]);glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING,7,&binding);assert(!binding);glDrawArrays(GL_TRIANGLES,0,3);assert(glGetError()==GL_INVALID_OPERATION);
 glBindBuffer(GL_UNIFORM_BUFFER,buffers[1]);glBufferData(GL_UNIFORM_BUFFER,112,bytes+32,GL_STATIC_DRAW);glBindBufferBase(GL_UNIFORM_BUFFER,1,buffers[1]);glBindBuffer(GL_UNIFORM_BUFFER,buffers[2]);glBufferData(GL_UNIFORM_BUFFER,48,bytes+144,GL_STATIC_DRAW);glBindBufferBase(GL_UNIFORM_BUFFER,7,buffers[2]);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,255,0);glXSwapBuffers(d,w);assert(glGetError()==GL_NO_ERROR);
 // Mapped UBO writes use guest storage and become GPU snapshots at draw time.
 glBindBuffer(GL_UNIFORM_BUFFER,buffers[2]);float *mapped=glMapBufferRange(GL_UNIFORM_BUFFER,0,48,GL_MAP_WRITE_BIT|GL_MAP_FLUSH_EXPLICIT_BIT);assert(mapped);mapped[0]=0;mapped[1]=0;mapped[2]=1;mapped[3]=1;
 GLint untouched=123;glGetBufferParameteriv(GL_UNIFORM_BUFFER,0,&untouched);assert(glGetError()==GL_INVALID_ENUM&&untouched==123);
 void *pointer=NULL;glGetBufferPointerv(GL_UNIFORM_BUFFER,GL_BUFFER_MAP_POINTER,&pointer);assert(pointer==mapped);glGetBufferParameteriv(GL_UNIFORM_BUFFER,GL_BUFFER_MAPPED,&binding);assert(binding);glGetBufferParameteriv(GL_UNIFORM_BUFFER,GL_BUFFER_MAP_LENGTH,&binding);assert(binding==48);
 glDrawArrays(GL_TRIANGLES,0,3);assert(glGetError()==GL_INVALID_OPERATION);glBufferData(GL_UNIFORM_BUFFER,48,NULL,GL_STATIC_DRAW);assert(glGetError()==GL_INVALID_OPERATION);glBufferSubData(GL_UNIFORM_BUFFER,0,4,&one);assert(glGetError()==GL_INVALID_OPERATION);
 glFlushMappedBufferRange(GL_UNIFORM_BUFFER,0,48);assert(glUnmapBuffer(GL_UNIFORM_BUFFER));glGetBufferPointerv(GL_UNIFORM_BUFFER,GL_BUFFER_MAP_POINTER,&pointer);assert(!pointer);glDrawArrays(GL_TRIANGLES,0,3);pixel(0,0,255);
 mapped=glMapBuffer(GL_UNIFORM_BUFFER,GL_READ_ONLY);assert(mapped&&mapped[2]==1);assert(!glMapBufferRange(GL_UNIFORM_BUFFER,0,48,GL_MAP_READ_BIT));assert(glGetError()==GL_INVALID_OPERATION);assert(glUnmapBuffer(GL_UNIFORM_BUFFER));assert(!glUnmapBuffer(GL_UNIFORM_BUFFER));assert(glGetError()==GL_INVALID_OPERATION);
 assert(!glMapBufferRange(GL_UNIFORM_BUFFER,0,0,GL_MAP_READ_BIT));assert(glGetError()==GL_INVALID_OPERATION);assert(!glMapBufferRange(GL_UNIFORM_BUFFER,48,1,GL_MAP_WRITE_BIT));assert(glGetError()==GL_INVALID_VALUE);assert(!glMapBufferRange(GL_UNIFORM_BUFFER,0,4,GL_MAP_READ_BIT|GL_MAP_INVALIDATE_BUFFER_BIT));assert(glGetError()==GL_INVALID_OPERATION);
 // Versioned ABI rejects malformed ranges/counts and bad guest addresses.
 int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);struct lr_gpu_request request={.version=LR_GPU_VERSION,.operation=LR_GPU_CREATE,.width=8,.height=8};assert(ioctl(fd,LR_GPU_IOCTL,&request)==0);
 struct lr_gl_block_data malformed={.program=1,.count=LR_GL_MAX_BLOCKS+1};request=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_UNIFORM_BLOCK_DATA,.address=(uintptr_t)&malformed,.bytes=sizeof malformed};assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EINVAL);malformed.count=1;malformed.block[0].offset=UINT32_MAX;malformed.block[0].bytes=16;assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EINVAL);request.address=1;assert(ioctl(fd,LR_GPU_IOCTL,&request)<0&&errno==EFAULT);close(fd);
 uint64_t draws,reads,ns;assert(lrGPUGetStatistics(&draws,&reads,&ns)&&draws&&ns);printf("UBO_METAL_COUNTERS operations=%llu readbacks=%llu gpu_ns=%llu\n",(unsigned long long)draws,(unsigned long long)reads,(unsigned long long)ns);
 glUseProgram(0);glDeleteProgram(q);glDeleteProgram(p);glDeleteBuffers(3,buffers);glXMakeCurrent(d,None,NULL);glXDestroyContext(d,c);XDestroyWindow(d,w);XFree(v);XCloseDisplay(d);puts("UNIFORM_BLOCK_CONTRACT_PASS std140=PASS reflection=PASS indexed_range=PASS updates=PASS GPU_pixels=PASS draw_snapshot=PASS cache_isolation=PASS deletion=PASS mapped_updates=PASS malformed_ABI=PASS");return 0;}
