#ifndef LR_GUEST_UNIFORMS_H
#define LR_GUEST_UNIFORMS_H
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90d2
#endif
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER 0x8a11
#endif
GLuint glGetUniformBlockIndex(GLuint,const GLchar *);
void glUniformBlockBinding(GLuint,GLuint,GLuint);
void glGetActiveUniformBlockiv(GLuint,GLuint,GLenum,GLint *);
void glGetActiveUniformBlockName(GLuint,GLuint,GLsizei,GLsizei *,GLchar *);
void glGetUniformIndices(GLuint,GLsizei,const GLchar *const *,GLuint *);
void glGetActiveUniformsiv(GLuint,GLsizei,const GLuint *,GLenum,GLint *);
void glBindBufferBase(GLenum,GLuint,GLuint);
void glBindBufferRange(GLenum,GLuint,GLuint,GLintptr,GLsizeiptr);
void glGetIntegeri_v(GLenum,GLuint,GLint *);
void glGetInteger64i_v(GLenum,GLuint,int64_t *);
#endif

void glShaderStorageBlockBinding(GLuint,GLuint,GLuint);
GLuint glGetProgramResourceIndex(GLuint,GLenum,const GLchar *);
void glGetProgramInterfaceiv(GLuint,GLenum,GLenum,GLint *);
void glGetProgramResourceName(GLuint,GLenum,GLuint,GLsizei,GLsizei *,GLchar *);
void glGetProgramResourceiv(GLuint,GLenum,GLuint,GLsizei,const GLenum *,GLsizei,GLsizei *,GLint *);
