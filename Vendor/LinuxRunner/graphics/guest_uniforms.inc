// A context-wide monotonic generation prevents stale cache hits when an
// internal buffer slot is freed and reused under the same public GL name.
static void buffer_changed(unsigned slot){if(!++active->buffer_serial){active->buffer_serial=1;for(unsigned p=0;p<LR_GL_MAX_PROGRAMS;p++){active->program[p].ubo_valid=0;active->program[p].storage_valid=0;}}active->buffers[slot].serial=active->buffer_serial;}
// std140 UBOs are reflected by the reference GLSL/SPIR-V compiler. They use
// ordinary GL buffer objects; indexed bindings belong to the context, not VAOs.
static int uniform_context(void){if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return 0;}return 1;}
static const struct lr_gl_block_member *block_member(unsigned p,unsigned index,unsigned *block){
 unsigned at=active->program[p].link.uniforms;const struct lr_gl_blocks *b=active->program[p].blocks;if(!b)return NULL;
 for(unsigned i=0;i<b->count;i++){if(index>=at&&index-at<b->block[i].members){if(block)*block=i;return &b->block[i].member[index-at];}at+=b->block[i].members;}return NULL;
}
static unsigned block_member_count(unsigned p){if(!active->program[p].blocks)return 0;unsigned n=0;for(unsigned i=0;i<active->program[p].blocks->count;i++)n+=active->program[p].blocks->block[i].members;return n;}
static unsigned block_member_name_max(unsigned p){if(!active->program[p].blocks)return 0;unsigned n=0;for(unsigned i=0;i<active->program[p].blocks->count;i++)for(unsigned j=0;j<active->program[p].blocks->block[i].members;j++){unsigned k=strlen(active->program[p].blocks->block[i].member[j].name)+1;if(k>n)n=k;}return n;}
static int block_index(GLuint program,GLuint index){int p=program_index(program);if(p<0)return -1;if(!uniform_context())return -1;if(!active->program[p].link.status){gl_error(GL_INVALID_OPERATION);return -1;}if(index>=active->program[p].blocks->count){gl_error(GL_INVALID_VALUE);return -1;}return p;}
GLuint glGetUniformBlockIndex(GLuint program,const GLchar *name){int p=program_index(program);if(p<0||!uniform_context())return UINT32_MAX;if(!name){gl_error(GL_INVALID_VALUE);return UINT32_MAX;}if(!active->program[p].link.status){gl_error(GL_INVALID_OPERATION);return UINT32_MAX;}for(unsigned i=0;i<active->program[p].blocks->count;i++)if(!strcmp(name,active->program[p].blocks->block[i].name))return i;return UINT32_MAX;}
void glUniformBlockBinding(GLuint program,GLuint index,GLuint binding){int p=block_index(program,index);if(p<0)return;if(binding>=LR_GL_MAX_BLOCK_BINDINGS){gl_error(GL_INVALID_VALUE);return;}active->program[p].block_binding[index]=binding;}
void glGetActiveUniformBlockName(GLuint program,GLuint index,GLsizei capacity,GLsizei *length,GLchar *out){int p=block_index(program,index);if(p<0)return;if(capacity<0||(!out&&capacity)){gl_error(GL_INVALID_VALUE);return;}const char *s=active->program[p].blocks->block[index].name;size_t n=capacity?(size_t)capacity-1:0;if(n>strlen(s))n=strlen(s);if(capacity){memcpy(out,s,n);out[n]=0;}if(length)*length=n;}
void glGetActiveUniformBlockiv(GLuint program,GLuint index,GLenum property,GLint *out){int p=block_index(program,index);if(p<0)return;if(!out){gl_error(GL_INVALID_VALUE);return;}const struct lr_gl_block *b=&active->program[p].blocks->block[index];switch(property){
 case 0x8a3f:*out=active->program[p].block_binding[index];break; // BINDING
 case 0x8a40:*out=b->bytes;break;case 0x8a41:*out=strlen(b->name)+1;break;
 case 0x8a42:*out=b->members;break;
 case 0x8a43:{unsigned at=active->program[p].link.uniforms;for(unsigned i=0;i<index;i++)at+=active->program[p].blocks->block[i].members;for(unsigned i=0;i<b->members;i++)out[i]=at+i;break;}
 case 0x8a44:*out=b->vertex_binding!=UINT32_MAX;break;case 0x8a46:*out=b->fragment_binding!=UINT32_MAX;break;default:gl_error(GL_INVALID_ENUM);}}
void glGetUniformIndices(GLuint program,GLsizei count,const GLchar *const *names,GLuint *out){int p=program_index(program);if(p<0||!uniform_context())return;if(count<0||(!names&&count)||(!out&&count)){gl_error(GL_INVALID_VALUE);return;}if(!active->program[p].link.status){gl_error(GL_INVALID_OPERATION);return;}
 unsigned n=active->program[p].link.uniforms+block_member_count(p);for(int k=0;k<count;k++){out[k]=UINT32_MAX;if(!names[k]){gl_error(GL_INVALID_VALUE);return;}for(unsigned i=0;i<n;i++){const struct lr_gl_block_member *m=block_member(p,i,NULL);const char *name=m?m->name:active->program[p].link.uniform[i].name;if(!strcmp(names[k],name)){out[k]=i;break;}size_t len=strlen(name);if(!m&&active->program[p].extents.uniform[i].elements>1&&strlen(names[k])==len+3&&!strncmp(names[k],name,len)&&!strcmp(names[k]+len,"[0]")){out[k]=i;break;}if(m&&m->array_stride&&len>3&&!strcmp(name+len-3,"[0]")&&strlen(names[k])==len-3&&!strncmp(names[k],name,len-3)){out[k]=i;break;}}}
}
void glGetActiveUniformsiv(GLuint program,GLsizei count,const GLuint *indices,GLenum property,GLint *out){int p=program_index(program);if(p<0||!uniform_context())return;if(count<0||(!indices&&count)||(!out&&count)){gl_error(GL_INVALID_VALUE);return;}if(!active->program[p].link.status){gl_error(GL_INVALID_OPERATION);return;}
 // Validate the whole query before writing output.
 unsigned n=active->program[p].link.uniforms+block_member_count(p);for(int k=0;k<count;k++)if(indices[k]>=n){gl_error(GL_INVALID_VALUE);return;}
 for(int k=0;k<count;k++){unsigned block=0;const struct lr_gl_block_member *m=block_member(p,indices[k],&block);const struct lr_gl_variable *v=m?NULL:&active->program[p].link.uniform[indices[k]];
 switch(property){case 0x8a37:out[k]=m?m->type:v->type;break;case 0x8a38:out[k]=m?m->size:active->program[p].extents.uniform[indices[k]].elements;break;case 0x8a39:out[k]=strlen(m?m->name:v->name)+1;break;case 0x8a3a:out[k]=m?(GLint)block:-1;break;case 0x8a3b:out[k]=m?(GLint)m->offset:-1;break;case 0x8a3c:out[k]=m?m->array_stride:0;break;case 0x8a3d:out[k]=m?m->matrix_stride:0;break;case 0x8a3e:out[k]=m?m->row_major:0;break;default:gl_error(GL_INVALID_ENUM);return;}}
}
static void bind_uniform_buffer(GLenum target,GLuint index,GLuint name,GLintptr offset,GLsizeiptr bytes,int range){
 if(!uniform_context())return;if(target!=GL_UNIFORM_BUFFER&&target!=GL_SHADER_STORAGE_BUFFER){gl_error(GL_INVALID_ENUM);return;}if(index>=LR_GL_MAX_BLOCK_BINDINGS){gl_error(GL_INVALID_VALUE);return;}
 if(name&&range&&(offset<0||bytes<=0||offset%16)){gl_error(GL_INVALID_VALUE);return;}int slot=name?buffer_slot(name):-1;
 if(name&&slot<0){gl_error(GL_INVALID_OPERATION);return;}
 if(name&&range&&((size_t)offset>active->buffers[slot].size||(size_t)bytes>active->buffers[slot].size-offset)){gl_error(GL_INVALID_VALUE);return;}
 if(target==GL_SHADER_STORAGE_BUFFER){active->bound_storage=name?slot+1:0;active->storage_binding[index].slot=active->bound_storage;active->storage_binding[index].offset=name&&range?offset:0;active->storage_binding[index].bytes=name&&range?bytes:0;active->storage_binding[index].range=range;collect_buffers();return;}
 active->bound_uniform=name?slot+1:0;active->uniform_binding[index].slot=active->bound_uniform;active->uniform_binding[index].offset=name&&range?offset:0;active->uniform_binding[index].bytes=name&&range?bytes:0;active->uniform_binding[index].range=range;collect_buffers();
}
void glBindBufferBase(GLenum target,GLuint index,GLuint buffer){bind_uniform_buffer(target,index,buffer,0,0,0);}
void glBindBufferRange(GLenum target,GLuint index,GLuint buffer,GLintptr offset,GLsizeiptr bytes){bind_uniform_buffer(target,index,buffer,offset,bytes,1);}
static int indexed_uniform_query(GLenum property,GLuint index,int64_t *out){if(!uniform_context())return 0;if(index>=LR_GL_MAX_BLOCK_BINDINGS||!out){gl_error(GL_INVALID_VALUE);return 0;}if(property>=0x90d3&&property<=0x90d5){unsigned s=active->storage_binding[index].slot;*out=property==0x90d3?(s?active->buffers[s-1].name:0):property==0x90d4?active->storage_binding[index].offset:s?(active->storage_binding[index].range?active->storage_binding[index].bytes:active->buffers[s-1].size):0;return 1;}unsigned slot=active->uniform_binding[index].slot;switch(property){case 0x8a28:*out=slot?active->buffers[slot-1].name:0;break;case 0x8a29:*out=active->uniform_binding[index].offset;break;case 0x8a2a:*out=slot?(active->uniform_binding[index].range?active->uniform_binding[index].bytes:active->buffers[slot-1].size):0;break;default:gl_error(GL_INVALID_ENUM);return 0;}return 1;}
void glGetIntegeri_v(GLenum property,GLuint index,GLint *out){int64_t value;if(indexed_uniform_query(property,index,&value)&&out)*out=value;else if(!out)gl_error(GL_INVALID_VALUE);}
void glGetInteger64i_v(GLenum property,GLuint index,int64_t *out){indexed_uniform_query(property,index,out);}
static int upload_uniform_blocks(unsigned program){
 const struct lr_gl_blocks *b=active->program[program].blocks;if(!b->count)return 1;
 int changed=active->program[program].ubo_valid!=(1u<<b->count)-1;
 // Validate even cached bindings: mapping or resizing can make them invalid.
 for(unsigned i=0;i<b->count;i++){unsigned binding=active->program[program].block_binding[i],slot=active->uniform_binding[binding].slot;size_t offset=active->uniform_binding[binding].offset,size=slot?active->buffers[slot-1].size:0;
  if(!slot||active->buffers[slot-1].mapped||offset>size||b->block[i].bytes>size-offset||(active->uniform_binding[binding].range&&b->block[i].bytes>active->uniform_binding[binding].bytes)){gl_error(GL_INVALID_OPERATION);return 0;}
  if(active->program[program].ubo_stamp[i].slot!=slot||active->program[program].ubo_stamp[i].offset!=offset||active->program[program].ubo_stamp[i].serial!=active->buffers[slot-1].serial)changed=1;
 }
#ifdef LR_UNIFORM_UPLOAD_CACHE_DISABLED
 changed=1; // Controlled component benchmark; absent in packaged providers.
#endif
 if(!changed)return 1;
 if(!active->uniform_scratch){active->uniform_scratch=malloc(sizeof(struct lr_gl_block_data)+LR_GL_MAX_BLOCKS*LR_GL_MAX_BLOCK_BYTES);if(!active->uniform_scratch){gl_error(GL_OUT_OF_MEMORY);return 0;}}
 struct lr_gl_block_data *packet=active->uniform_scratch;packet->program=program+1;packet->count=b->count;size_t bytes=sizeof(*packet);memset(packet->block,0,sizeof packet->block);
 for(unsigned i=0;i<b->count;i++){unsigned binding=active->program[program].block_binding[i],slot=active->uniform_binding[binding].slot;size_t offset=active->uniform_binding[binding].offset;
  packet->block[i].offset=bytes;packet->block[i].bytes=b->block[i].bytes;memcpy((char *)packet+bytes,active->buffers[slot-1].data+offset,b->block[i].bytes);bytes+=b->block[i].bytes;
 }
 struct lr_gpu_request r={.operation=LR_GPU_UNIFORM_BLOCK_DATA,.address=(uintptr_t)packet,.bytes=bytes};if(!submit(&r))return 0;
 for(unsigned i=0;i<b->count;i++){unsigned binding=active->program[program].block_binding[i],slot=active->uniform_binding[binding].slot;active->program[program].ubo_stamp[i].slot=slot;active->program[program].ubo_stamp[i].offset=active->uniform_binding[binding].offset;active->program[program].ubo_stamp[i].serial=active->buffers[slot-1].serial;}
 active->program[program].ubo_valid=(1u<<b->count)-1;return 1;
}
