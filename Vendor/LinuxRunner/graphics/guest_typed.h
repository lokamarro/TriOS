// Shared declarations: EGL resolver appears before GLES2 implementation.
static void uniform_integer(GLint,GLsizei,const void *,unsigned,unsigned);
static void uniform_query(GLuint,GLint,void *,unsigned);
void glUniform2iv(GLint,GLsizei,const GLint *);void glUniform3iv(GLint,GLsizei,const GLint *);void glUniform4iv(GLint,GLsizei,const GLint *);
void glUniform2i(GLint,GLint,GLint);void glUniform3i(GLint,GLint,GLint,GLint);void glUniform4i(GLint,GLint,GLint,GLint,GLint);
void glUniform1uiv(GLint,GLsizei,const GLuint *);void glUniform2uiv(GLint,GLsizei,const GLuint *);void glUniform3uiv(GLint,GLsizei,const GLuint *);void glUniform4uiv(GLint,GLsizei,const GLuint *);
void glUniform1ui(GLint,GLuint);void glUniform2ui(GLint,GLuint,GLuint);void glUniform3ui(GLint,GLuint,GLuint,GLuint);void glUniform4ui(GLint,GLuint,GLuint,GLuint,GLuint);
void glGetUniformuiv(GLuint,GLint,GLuint *);
void glVertexAttribIPointer(GLuint,GLint,GLenum,GLsizei,const void *);
void glVertexAttribI4i(GLuint,GLint,GLint,GLint,GLint);void glVertexAttribI4ui(GLuint,GLuint,GLuint,GLuint,GLuint);
void glVertexAttribI4iv(GLuint,const GLint *);void glVertexAttribI4uiv(GLuint,const GLuint *);
void glGetVertexAttribIiv(GLuint,GLenum,GLint *);void glGetVertexAttribIuiv(GLuint,GLenum,GLuint *);

void glDrawArraysInstancedBaseInstance(GLenum,GLint,GLsizei,GLsizei,GLuint);
void glDrawElementsInstancedBaseInstance(GLenum,GLsizei,GLenum,const void *,GLsizei,GLuint);
