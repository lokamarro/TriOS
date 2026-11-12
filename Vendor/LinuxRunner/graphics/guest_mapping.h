#ifndef LR_GUEST_MAPPING_H
#define LR_GUEST_MAPPING_H
void *glMapBufferRange(GLenum,GLintptr,GLsizeiptr,GLbitfield);
void *glMapBuffer(GLenum,GLenum);
GLboolean glUnmapBuffer(GLenum);
void glFlushMappedBufferRange(GLenum,GLintptr,GLsizeiptr);
void glGetBufferParameteriv(GLenum,GLenum,GLint *);
void glGetBufferParameteri64v(GLenum,GLenum,int64_t *);
void glGetBufferPointerv(GLenum,GLenum,void **);
#endif
