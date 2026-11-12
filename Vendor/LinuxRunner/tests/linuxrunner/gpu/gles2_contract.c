// Actual ARM64 guest -> compiler -> Metal pipeline pixel contract.
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static GLuint shader(GLenum stage,const char *text){GLuint s=glCreateShader(stage);glShaderSource(s,1,&text,0);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof log,0,log);puts(log);}assert(ok);return s;}
int main(void){EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLConfig cfg;EGLint n;
 assert(eglInitialize(d,0,0)&&eglChooseConfig(d,(EGLint[]){EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE},&cfg,1,&n));
 EGLContext c=eglCreateContext(d,cfg,0,(EGLint[]){EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE});EGLSurface surface=eglCreatePbufferSurface(d,cfg,(EGLint[]){EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE});assert(c&&surface&&eglMakeCurrent(d,surface,surface,c));
 EGLint query;assert(eglGetConfigAttrib(d,cfg,EGL_CONFIG_ID,&query)&&query==1);assert(eglQueryContext(d,c,EGL_CONTEXT_CLIENT_VERSION,&query)&&query==2);assert(eglQueryContext(d,c,EGL_CONTEXT_CLIENT_TYPE,&query)&&query==EGL_OPENGL_ES_API);
 assert(eglQueryContext(d,c,EGL_RENDER_BUFFER,&query)&&query==EGL_SINGLE_BUFFER);assert(eglQuerySurface(d,surface,EGL_WIDTH,&query)&&query==32);assert(eglQuerySurface(d,surface,EGL_HEIGHT,&query)&&query==32);
 assert(!eglQuerySurface(d,(EGLSurface)999999,EGL_WIDTH,&query)&&eglGetError()==EGL_BAD_SURFACE);assert(!eglQueryContext(d,(EGLContext)999999,EGL_CONTEXT_CLIENT_VERSION,&query)&&eglGetError()==EGL_BAD_CONTEXT);
 assert(!eglQuerySurface(d,surface,0,&query)&&eglGetError()==EGL_BAD_ATTRIBUTE);assert(eglGetProcAddress("eglQueryContext")&&eglGetProcAddress("eglQuerySurface"));
 GLuint invalid=glCreateShader(GL_VERTEX_SHADER);const char *bad="this is invalid GLSL";glShaderSource(invalid,1,&bad,0);glCompileShader(invalid);GLint ok;glGetShaderiv(invalid,GL_COMPILE_STATUS,&ok);assert(!ok);char log[2048];glGetShaderInfoLog(invalid,sizeof log,0,log);assert(strlen(log));glDeleteShader(invalid);
 GLuint v=shader(GL_VERTEX_SHADER,"attribute vec3 position; uniform mat4 matrix; void main(){gl_Position=matrix*vec4(position,1.0);}"),f=shader(GL_FRAGMENT_SHADER,"precision mediump float; uniform vec4 color; void main(){gl_FragColor=color;}");
 GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glBindAttribLocation(p,3,"position");glLinkProgram(p);glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){glGetProgramInfoLog(p,sizeof log,0,log);puts(log);}assert(ok);assert(glGetAttribLocation(p,"position")==3);glUseProgram(p);
 GLint resources,max_name,size;GLenum type;GLsizei length;char name[64];GLuint attached[2];
 glGetProgramiv(p,GL_ATTACHED_SHADERS,&resources);assert(resources==2);
 glGetAttachedShaders(p,1,&length,attached);assert(length==1&&attached[0]==v);
 glGetAttachedShaders(p,2,&length,attached);assert(length==2&&attached[1]==f);
 glGetActiveAttrib(p,0,sizeof name,&length,&size,&type,name);assert(!strcmp(name,"position")&&length==8&&size==1&&type==GL_FLOAT_VEC3);
 glGetActiveAttrib(p,0,4,&length,&size,&type,name);assert(!strcmp(name,"pos")&&length==3);
 glGetActiveAttrib(p,0,0,&length,&size,&type,NULL);assert(length==0);
 glGetProgramiv(p,GL_ACTIVE_ATTRIBUTE_MAX_LENGTH,&max_name);assert(max_name==9);
 glGetProgramiv(p,GL_ACTIVE_UNIFORM_MAX_LENGTH,&max_name);assert(max_name==7);
 for(unsigned i=0;i<2;i++){glGetActiveUniform(p,i,sizeof name,&length,&size,&type,name);assert(size==1);assert((!strcmp(name,"matrix")&&type==GL_FLOAT_MAT4)||(!strcmp(name,"color")&&type==GL_FLOAT_VEC4));}
 glGetActiveUniform(p,2,sizeof name,&length,&size,&type,name);assert(glGetError()==GL_INVALID_VALUE);
 glGetShaderSource(v,4,&length,name);assert(length==3&&!strcmp(name,"att"));
 assert(eglGetProcAddress("glGetActiveUniform")&&eglGetProcAddress("glGetAttachedShaders"));
 GLfloat matrix[]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};GLint m=glGetUniformLocation(p,"matrix"),color=glGetUniformLocation(p,"color");assert(m>=0&&color>=0);glUniformMatrix4fv(m,1,0,matrix);glUniform4f(color,1,0,0,1);
 GLfloat retrieved[16];glGetUniformfv(p,m,retrieved);assert(!memcmp(retrieved,matrix,sizeof matrix));glGetUniformfv(p,color,retrieved);assert(retrieved[0]==1&&retrieved[1]==0&&retrieved[3]==1);glGetUniformfv(p,-1,retrieved);assert(glGetError()==GL_INVALID_OPERATION);assert(eglGetProcAddress("glGetUniformfv"));
 GLfloat vertex[]={-.9f,-.9f,-.5f,.9f,-.9f,-.5f,0,.9f,-.5f};GLushort indices[]={0,1,2};GLuint b[2];glGenBuffers(2,b);glBindBuffer(GL_ARRAY_BUFFER,b[0]);glBufferData(GL_ARRAY_BUFFER,sizeof vertex,vertex,GL_DYNAMIC_DRAW);glVertexAttribPointer(3,3,GL_FLOAT,0,0,0);glEnableVertexAttribArray(3);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,b[1]);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof indices,indices,GL_STATIC_DRAW);
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glEnable(GL_DEPTH_TEST);glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_SHORT,0);unsigned char pixels[4096];glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError());assert(pixels[(10*32+16)*4]>250&&pixels[(10*32+16)*4+1]<5);
 matrix[14]=1;glUniformMatrix4fv(m,1,0,matrix);glUniform4f(color,0,1,0,1);glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(pixels[(10*32+16)*4]>250); // GPU matrix and depth
 glDisable(GL_DEPTH_TEST);glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(pixels[(10*32+16)*4+1]>250);
 glBufferSubData(GL_ARRAY_BUFFER,0,sizeof vertex,vertex);glDeleteShader(v);glDeleteShader(f);glLinkProgram(p);glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(ok); // attached deleted shaders survive; successful relink resets uniforms
 glUniformMatrix4fv(m,1,0,matrix);glUniform4f(color,0,1,0,1);
 // A failed relink must keep the executable and uniforms in the current
 // context; surfaces must not own shader/program lifetime.
 glDetachShader(p,f);glLinkProgram(p);glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(!ok);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);assert(!glGetError()&&pixels[(10*32+16)*4+1]>250);
 assert(eglGetProcAddress("glCreateShader")&& eglGetProcAddress("glFramebufferTexture2D"));
 assert(eglMakeCurrent(d,0,0,0)&&eglDestroySurface(d,surface));
 surface=eglCreatePbufferSurface(d,cfg,(EGLint[]){EGL_WIDTH,48,EGL_HEIGHT,48,EGL_NONE});assert(surface&&eglMakeCurrent(d,surface,surface,c));
 GLint viewport[4];glGetIntegerv(GL_VIEWPORT,viewport);assert(viewport[2]==32&&viewport[3]==32); // GL state survives surface replacement
 glViewport(0,0,48,48);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);unsigned char large[48*48*4];glReadPixels(0,0,48,48,GL_RGBA,GL_UNSIGNED_BYTE,large);assert(!glGetError()&&large[(15*48+24)*4+1]>250);
 glUseProgram(0);glDeleteProgram(p);glDeleteBuffers(2,b);assert(!glGetError());assert(eglMakeCurrent(d,0,0,0)&&eglDestroyContext(d,c)&&eglDestroySurface(d,surface)&&eglTerminate(d));
 puts("GLES2_CONTRACT_PASS reflection=PASS compile_errors=PASS attribute_remap=PASS GPU_uniform_matrix=PASS fragment_uniform=PASS VBO_EBO=PASS depth=PASS attached_shader_lifetime=PASS failed_relink=PASS surface_rebind=PASS dynamic_dispatch=PASS");
}
