// Read-only SSBOs use ordinary GL buffer objects and indexed context bindings.
// Upload only changed storage; native buffers are never shared across contexts.
static int storage_program(GLuint name,GLenum interface){int p=program_index(name);if(p<0||!uniform_context())return -1;if(interface!=0x92e6){gl_error(GL_INVALID_ENUM);return -1;}if(!active->program[p].link.status){gl_error(GL_INVALID_OPERATION);return -1;}return p;}
GLuint glGetProgramResourceIndex(GLuint name,GLenum interface,const GLchar *resource){int p=storage_program(name,interface);if(p<0)return UINT32_MAX;if(!resource){gl_error(GL_INVALID_VALUE);return UINT32_MAX;}for(unsigned i=0;i<active->program[p].storages.count;i++)if(!strcmp(resource,active->program[p].storages.block[i].name))return i;return UINT32_MAX;}
void glShaderStorageBlockBinding(GLuint name,GLuint index,GLuint binding){int p=storage_program(name,0x92e6);if(p<0)return;if(index>=active->program[p].storages.count||binding>=LR_GL_MAX_BLOCK_BINDINGS){gl_error(GL_INVALID_VALUE);return;}active->program[p].storage_binding[index]=binding;}
void glGetProgramInterfaceiv(GLuint name,GLenum interface,GLenum property,GLint *out){int p=storage_program(name,interface);if(p<0)return;if(!out){gl_error(GL_INVALID_VALUE);return;}if(property==0x92f5)*out=active->program[p].storages.count;else if(property==0x92f6){*out=0;for(unsigned i=0;i<active->program[p].storages.count;i++){GLint n=strlen(active->program[p].storages.block[i].name)+1;if(n>*out)*out=n;}}else gl_error(GL_INVALID_ENUM);}
void glGetProgramResourceName(GLuint name,GLenum interface,GLuint index,GLsizei capacity,GLsizei *length,GLchar *out){int p=storage_program(name,interface);if(p<0)return;if(index>=active->program[p].storages.count||capacity<0||(!out&&capacity)){gl_error(GL_INVALID_VALUE);return;}const char *s=active->program[p].storages.block[index].name;size_t n=capacity?(size_t)capacity-1:0;if(n>strlen(s))n=strlen(s);if(capacity){memcpy(out,s,n);out[n]=0;}if(length)*length=n;}
void glGetProgramResourceiv(GLuint name,GLenum interface,GLuint index,GLsizei count,const GLenum *properties,GLsizei capacity,GLsizei *length,GLint *out){int p=storage_program(name,interface);if(p<0)return;if(index>=active->program[p].storages.count||count<0||count>32||capacity<0||(!out&&capacity)||(!properties&&count)){gl_error(GL_INVALID_VALUE);return;}
 const struct lr_gl_storage *b=&active->program[p].storages.block[index];GLint values[32];
 for(int i=0;i<count;i++)switch(properties[i]){case 0x92f9:values[i]=strlen(b->name)+1;break;case 0x9302:values[i]=active->program[p].storage_binding[index];break;case 0x9303:values[i]=b->min_bytes;break;case 0x9306:values[i]=b->vertex_binding!=UINT32_MAX;break;case 0x930a:values[i]=b->fragment_binding!=UINT32_MAX;break;default:gl_error(GL_INVALID_ENUM);return;}
 GLsizei take=count<capacity?count:capacity;if(take)memcpy(out,values,(size_t)take*4);if(length)*length=take;
}
static int upload_storage_blocks(unsigned p){
 const struct lr_gl_storages *s=&active->program[p].storages;if(!s->count)return 1;int changed=active->program[p].storage_valid!=(1u<<s->count)-1;size_t sizes[LR_GL_MAX_STORAGE_BLOCKS],bytes=sizeof(struct lr_gl_storage_data);
 for(unsigned i=0;i<s->count;i++){unsigned binding=active->program[p].storage_binding[i],slot=active->storage_binding[binding].slot;size_t offset=active->storage_binding[binding].offset;
  if(!slot||active->buffers[slot-1].mapped||offset>active->buffers[slot-1].size){gl_error(GL_INVALID_OPERATION);return 0;}
  sizes[i]=active->storage_binding[binding].range?active->storage_binding[binding].bytes:active->buffers[slot-1].size-offset;
  if(sizes[i]<s->block[i].min_bytes||sizes[i]>LR_GL_MAX_STORAGE_BYTES||sizes[i]>active->buffers[slot-1].size-offset){gl_error(GL_INVALID_OPERATION);return 0;}
  if(s->block[i].runtime_stride&&(sizes[i]-s->block[i].runtime_offset)%s->block[i].runtime_stride){gl_error(GL_INVALID_OPERATION);return 0;}
  if(active->program[p].storage_stamp[i].slot!=slot||active->program[p].storage_stamp[i].serial!=active->buffers[slot-1].serial||active->program[p].storage_stamp[i].offset!=offset||active->program[p].storage_stamp[i].bytes!=sizes[i])changed=1;bytes+=sizes[i];
 }
 if(!changed)return 1;
 if(!active->storage_scratch){active->storage_scratch=malloc(sizeof(struct lr_gl_storage_data)+LR_GL_MAX_STORAGE_BLOCKS*LR_GL_MAX_STORAGE_BYTES);if(!active->storage_scratch){gl_error(GL_OUT_OF_MEMORY);return 0;}}
 struct lr_gl_storage_data *packet=active->storage_scratch;packet->program=p+1;packet->count=s->count;size_t at=sizeof *packet;memset(packet->block,0,sizeof packet->block);
 for(unsigned i=0;i<s->count;i++){unsigned binding=active->program[p].storage_binding[i],slot=active->storage_binding[binding].slot;packet->block[i].offset=at;packet->block[i].bytes=sizes[i];memcpy((char *)packet+at,active->buffers[slot-1].data+active->storage_binding[binding].offset,sizes[i]);at+=sizes[i];}
 struct lr_gpu_request r={.operation=LR_GPU_STORAGE_DATA,.address=(uintptr_t)packet,.bytes=bytes};if(!submit(&r))return 0;
 for(unsigned i=0;i<s->count;i++){unsigned binding=active->program[p].storage_binding[i],slot=active->storage_binding[binding].slot;active->program[p].storage_stamp[i].slot=slot;active->program[p].storage_stamp[i].serial=active->buffers[slot-1].serial;active->program[p].storage_stamp[i].offset=active->storage_binding[binding].offset;active->program[p].storage_stamp[i].bytes=sizes[i];}active->program[p].storage_valid=(1u<<s->count)-1;return 1;
}
