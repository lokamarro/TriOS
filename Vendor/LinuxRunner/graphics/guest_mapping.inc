// Mappings point to bounded guest-side GL buffer storage, never host pointers.
// The GPU reads immutable draw snapshots, so completed writes need no host wait.
static int bound_buffer_index(GLenum target){if(!active){gl_error(GL_INVALID_OPERATION);return -1;}unsigned slot;
 if(target==GL_ARRAY_BUFFER)slot=active->bound_buffer;else if(target==GL_ELEMENT_ARRAY_BUFFER)slot=active->bound_element;else if(active->desktop&&target==GL_UNIFORM_BUFFER)slot=active->bound_uniform;else if(active->desktop&&target==GL_SHADER_STORAGE_BUFFER)slot=active->bound_storage;else{gl_error(GL_INVALID_ENUM);return -1;}
 if(!slot){gl_error(GL_INVALID_OPERATION);return -1;}return slot-1;
}
void *glMapBufferRange(GLenum target,GLintptr offset,GLsizeiptr length,GLbitfield access){int b=bound_buffer_index(target);if(b<0)return NULL;if(!active->desktop){gl_error(GL_INVALID_OPERATION);return NULL;}
 if(access&~0x3fu){gl_error(GL_INVALID_VALUE);return NULL;}
 if(!(access&3)||((access&1)&&(access&(4|8|32)))||(!(access&2)&&(access&16))||active->buffers[b].mapped||!length){gl_error(GL_INVALID_OPERATION);return NULL;}
 if(offset<0||length<0||(size_t)offset>active->buffers[b].size||(size_t)length>active->buffers[b].size-offset){gl_error(GL_INVALID_VALUE);return NULL;}
 active->buffers[b].mapped=1;active->buffers[b].map_offset=offset;active->buffers[b].map_length=length;active->buffers[b].map_access=access;return active->buffers[b].data+offset;
}
void *glMapBuffer(GLenum target,GLenum access){int b=bound_buffer_index(target);if(b<0)return NULL;unsigned bits=access==0x88b8?1:access==0x88b9?2:access==0x88ba?3:0;if(!bits){gl_error(GL_INVALID_ENUM);return NULL;}return glMapBufferRange(target,0,active->buffers[b].size,bits);}
GLboolean glUnmapBuffer(GLenum target){int b=bound_buffer_index(target);if(b<0)return GL_FALSE;if(!active->buffers[b].mapped){gl_error(GL_INVALID_OPERATION);return GL_FALSE;}if(active->buffers[b].map_access&2)buffer_changed(b);active->buffers[b].mapped=0;active->buffers[b].map_offset=active->buffers[b].map_length=active->buffers[b].map_access=0;return GL_TRUE;}
void glFlushMappedBufferRange(GLenum target,GLintptr offset,GLsizeiptr length){int b=bound_buffer_index(target);if(b<0)return;if(!active->buffers[b].mapped||!(active->buffers[b].map_access&16)){gl_error(GL_INVALID_OPERATION);return;}if(offset<0||length<0||(size_t)offset>active->buffers[b].map_length||(size_t)length>active->buffers[b].map_length-offset){gl_error(GL_INVALID_VALUE);return;}
 // CPU guest storage is coherent; draws are prohibited until unmap. With
 // FLUSH_EXPLICIT, unflushed modified bytes have undefined GL values.
}
static int query_buffer_parameter(GLenum target,GLenum property,int64_t *out){int b=bound_buffer_index(target);if(b<0)return 0;if(!out){gl_error(GL_INVALID_VALUE);return 0;}switch(property){case 0x8764:*out=active->buffers[b].size;break;case 0x8765:*out=active->buffers[b].usage;break;case 0x88bc:*out=active->buffers[b].mapped;break;case 0x88bb:*out=active->buffers[b].mapped?(active->buffers[b].map_access&3)==1?0x88b8:(active->buffers[b].map_access&3)==2?0x88b9:0x88ba:0x88ba;break;case 0x911f:*out=active->buffers[b].map_access;break;case 0x9120:*out=active->buffers[b].map_length;break;case 0x9121:*out=active->buffers[b].map_offset;break;default:gl_error(GL_INVALID_ENUM);return 0;}return 1;}
void glGetBufferParameteri64v(GLenum target,GLenum property,int64_t *out){query_buffer_parameter(target,property,out);}
void glGetBufferParameteriv(GLenum target,GLenum property,GLint *out){int64_t value=0;if(!out){gl_error(GL_INVALID_VALUE);return;}if(query_buffer_parameter(target,property,&value))*out=value;}
void glGetBufferPointerv(GLenum target,GLenum property,void **out){int b=bound_buffer_index(target);if(b<0)return;if(property!=0x88bd){gl_error(GL_INVALID_ENUM);return;}if(!out){gl_error(GL_INVALID_VALUE);return;}*out=active->buffers[b].mapped?active->buffers[b].data+active->buffers[b].map_offset:NULL;}
